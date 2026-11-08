#include "integrate.h"

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <iostream>

#include <fxcorrcommon/visibility.h>
#include <fxcorrcommon/difxmonitor.h>

using namespace std;

static const string CIRCULAR_POL_NAMES[4] = {"RR", "LL", "RL", "LR"};
static const string LL_CIRCULAR_POL_NAMES[4] = {"LL", "RR", "LR", "RL"};
static const string LINEAR_POL_NAMES[4] = {"XX", "YY", "XY", "YX"};

// autocorr.bin header layout (FEngineWriter::writeAutocorrHeader):
// "FXCAC\0" + u32 version + u32 nsub + u32 nbands + per band (u32 bandindex, u32 nchan)

Integrator::Integrator(Configuration *conf, int cindex, int eseconds,
	int scan, int startsec, int startns, DifxMonitor *monitor) :
	config(conf), configindex(cindex), vis_(0), todiskbuffer_(0), monitor_(monitor)
{
	// todiskbuffer sizing follows fxmanager.cpp:114-133
	int resultlength = config->getMaxCoreResultLength();
	int todiskbufferlen = resultlength*8;
	for(int i=0;i<config->getNumConfigs();i++)
	{
		int confresultbytes = config->getCoreResultLength(i)*8;
		int minchans = 999999;
		for(int j=0;j<config->getFreqTableLength();j++)
		{
			if(config->isFrequencyOutput(i, j) && config->getFNumChannels(j)/config->getFChannelsToAverage(j) < minchans)
				minchans = config->getFNumChannels(j)/config->getFChannelsToAverage(j);
		}
		double headerbloatfactor = 1.0 + ((double)(Visibility::HEADER_BYTES))/(minchans*8);
		if(confresultbytes*headerbloatfactor > todiskbufferlen)
			todiskbufferlen = int(1.02*confresultbytes*headerbloatfactor); //a little extra margin to be sure
	}

	todiskbuffer_ = (char*)vectorAlloc_u8(todiskbufferlen);
	if(!todiskbuffer_)
	{
		cerr << "Failed to allocate " << todiskbufferlen << " bytes for the SWIN output buffer" << endl;
		exit(EXIT_FAILURE);
	}

	// writeSWIN appends into the .input OUTPUT FILENAME directory, so it must
	// exist beforehand (difx_dir in batch.json is metadata only)
	if(system(("mkdir -p " + config->getOutputFilename()).c_str()) != 0)
	{
		cerr << "Failed to create output directory " << config->getOutputFilename() << endl;
		exit(EXIT_FAILURE);
	}

	// polarisation names as in fxmanager.cpp:171-174
	const string *polnames;
	if(config->circularPolarisations())
		polnames = ((config->getMaxProducts() == 1)&&(config->getDRecordedBandPol(0, 0, 0) == 'L'))?LL_CIRCULAR_POL_NAMES:CIRCULAR_POL_NAMES;
	else
		polnames = LINEAR_POL_NAMES;

	vis_ = new Visibility(config, 0, 1, todiskbuffer_, todiskbufferlen, eseconds, scan, startsec, startns, polnames);
	if(!vis_->configuredOK())
	{
		cerr << "Visibility configuration failed" << endl;
		exit(EXIT_FAILURE);
	}
}

Integrator::~Integrator()
{
	delete vis_;
	vectorFree(todiskbuffer_);
}

bool Integrator::addSubint(cf32 *subintresults)
{
	bool done = vis_->addData(subintresults);
	if(done)
	{
		vis_->writedata();
		// RUNNING before increment(), which zeroes floatresults
		// (same order as FxManager::loopwrite: writedata, multicastweights)
		if(monitor_)
			sendRunning();
		vis_->increment();
	}
	return done;
}

// P1: RUNNING status, byte-identical semantics to
// Visibility::multicastweights (visibility.cpp:1100-1146)
void Integrator::sendRunning()
{
	int numdatastreams = config->getNumDataStreams();
	float *weight = new float[numdatastreams];
	int freqindex, weightcount;

	// per-station weights, averaged over recorded bands (only used
	// frequencies); the acweight section is indexed with the same
	// resultindex walk as visibility.cpp:445-460
	for(int i=0;i<numdatastreams;i++)
	{
		const int n = config->getDNumTotalBands(configindex, i);

		weight[i] = 0.0;
		weightcount = 0;
		if(n > 0)
		{
			int resultindex = config->getCoreResultACWeightOffset(configindex, i)*2;
			for(int j=0;j<n;j++)
			{
				freqindex = config->getDTotalFreqIndex(configindex, i, j);
				if(config->isFrequencyUsed(configindex, freqindex) || config->isEquivalentFrequencyUsed(configindex, freqindex))
				{
					// f32 truncation at storage, like upstream
					// autocorrweights (visibility.cpp:455)
					float acw = (float)(vis_->floatresults[resultindex]/vis_->fftsperintegration);
					weight[i] += acw;
					resultindex++;
					weightcount++;
				}
			}
			if(weightcount > 0)
				weight[i] /= weightcount;
		}
	}

	// integration centre time, visibility.cpp:1119-1132 (day-of-observation
	// decomposition; the integer/remainder order matters for f64 precision)
	int intsec = vis_->experseconds +
	             (int)config->getModel()->getScanStartSec(vis_->currentscan, vis_->expermjd, vis_->experseconds) +
	             vis_->currentstartseconds;
	int dumpmjd = vis_->expermjd + intsec/86400;
	double dumpseconds = double(intsec%86400) + ((double)vis_->currentstartns)/1000000000.0 +
	                     config->getIntTime(configindex)/2.0;
	if(dumpseconds > 86400.0)
	{
		dumpmjd++;
		dumpseconds -= 86400.0;
	}
	double mjd = dumpmjd + dumpseconds/86400.0;

	monitor_->status(DIFX_STATE_RUNNING, "", mjd, numdatastreams, weight,
	                 vis_->expermjd + vis_->experseconds/86400.0,
	                 vis_->expermjd + (vis_->experseconds + vis_->executeseconds)/86400.0);

	delete [] weight;
}

// v9 (fxcorr/v9-plan.md R4): the records used to be read from one
// autocorr.bin per datastream; they now arrive in memory from fxcorr-xpp's own
// station stage (autocorr.bin is gone, D7).  The accumulation below is the
// fxcorr-x file-reading form with only the source swapped -- the walk over
// total bands, the resultindex/weightindex recurrence, the cross-pol
// continuation and the used-frequency gate are unchanged, so the resulting
// subintresults bytes stay identical.
void Integrator::addAutocorrs(const vector<vector<AutocorrBatch> > &records, cf32 *subintresults)
{
	f32 *floatresults = (f32*)subintresults;
	int numdatastreams = config->getNumDataStreams();

	for(int ds=0;ds<numdatastreams;ds++)
	{
		// shard mode: a datastream outside this shard has no records (the
		// caller passes an empty list); skip it silently - the records it
		// would have produced belong to another shard
		if(ds >= (int)records.size() || records[ds].empty())
			continue;

		// core.cpp:1273-1302 / 1314-1339: every ac batch record of this subint
		// is accumulated, over total bands (recorded + zoom); zoom band weights
		// were already mapped to the parent recorded band by the station stage.
		// P7: the cross-pol section follows the parallel one and continues the
		// same resultindex/weightindex walk (core.cpp:1288-1301 / 1342-1369
		// concatenates the sections in the results layout)
		int ntotalbands = config->getDNumTotalBands(configindex, ds);
		for(size_t rec=0;rec<records[ds].size();rec++)
		{
			const AutocorrBatch &batch = records[ds][rec];
			int resultindex = config->getCoreResultAutocorrOffset(configindex, ds);
			int weightindex = config->getCoreResultACWeightOffset(configindex, ds)*2;
			// resultindex/weightindex continue across the parallel and
			// cross-pol sections (core.cpp concatenates them in the results
			// layout); offset is the section's own flat position and restarts
			// at 0 for the cross-pol section -- the file-reading form read
			// both sections from one continuous stream and had no equivalent
			auto readsection = [&](const vector<cf32> &ac, const vector<f32> &w)
			{
				int offset = 0;
				for(int k=0;k<ntotalbands;k++)
				{
					int freqindex = config->getDTotalFreqIndex(configindex, ds, k);
					int freqchannels = config->getFNumChannels(freqindex)/config->getFChannelsToAverage(freqindex);
					if(config->isFrequencyUsed(configindex, freqindex) || config->isEquivalentFrequencyUsed(configindex, freqindex))
					{
						vectorAdd_cf32_I(&ac[offset], &subintresults[resultindex], freqchannels);
						floatresults[weightindex] += w[k];
					}
					resultindex += freqchannels;
					weightindex++;
					offset += freqchannels;
				}
			};
			readsection(batch.ac, batch.w);
			if(!batch.acx.empty())
				readsection(batch.acx, batch.wx);
		}
	}
}
