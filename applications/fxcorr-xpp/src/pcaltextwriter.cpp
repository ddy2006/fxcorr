#include "pcaltextwriter.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>

using namespace std;

// Line and header format byte-compatible with mpifxcorr
// Visibility::writeSWIN (visibility.cpp:989-1051) and
// Visibility::initialisePcalFiles (visibility.cpp:107-136).
//
// One file per station, shared by that station's datastreams (upstream
// comments this explicitly in visibility.cpp:120-122): the datastream is
// distinguished by the 4th field of each data line, not by the filename.
// Each fxcorr-f job owns exactly one datastream, so the rerun-idempotent
// rewrite below must key on (datastream, timestamp) or the datastreams
// would delete each other's lines.

namespace {

// one data line of the PCAL file: <station> <mjd> <intTime> <dsindex> ...
struct PcalLine
{
	double mjd;
	int ds;
	std::string text;
};

// sort by timestamp, then by datastream so the per-intTime block of lines is
// stable across reruns (%17.11f round-trips to the same double for all
// datastreams of one intTime, so plain double comparison groups them)
bool pcalline_less(const PcalLine &a, const PcalLine &b)
{
	if(a.mjd != b.mjd)
		return a.mjd < b.mjd;
	return a.ds < b.ds;
}

}	// namespace

PcalTextWriter::PcalTextWriter(const string &pcaldir, Configuration *conf, int confindex, int ds) :
	config(conf), configindex(confindex), dsindex(ds), nrecordedbands(0), maxtones(0)
{
	nrecordedbands = config->getDNumRecordedBands(configindex, dsindex);
	maxtones = config->getDMaxRecordedPCalTones(configindex, dsindex);

	bandntones.resize(nrecordedbands);
	acwacc.resize(nrecordedbands, 0.0f);
	freqchannels.resize(nrecordedbands);
	int totaltones = 0;
	for(int j=0;j<nrecordedbands;j++)
	{
		int localfreqindex = config->getDLocalRecordedFreqIndex(configindex, dsindex, j);
		bandntones[j] = config->getDRecordedFreqNumPCalTones(configindex, dsindex, localfreqindex);
		totaltones += bandntones[j];
		int freqindex = config->getDRecordedFreqIndex(configindex, dsindex, localfreqindex);
		freqchannels[j] = config->getFNumChannels(freqindex)/config->getFChannelsToAverage(freqindex);
	}
	accum.resize(totaltones);	// value-initialised to zero
	chanstoavg = config->getFChannelsToAverage(config->getDRecordedFreqIndex(configindex, dsindex, config->getDLocalRecordedFreqIndex(configindex, dsindex, 0)));
	blockspersend = config->getBlocksPerSend(configindex);

	// One file per (station, datastream) -- the _ds<N> suffix keeps the
	// concurrent writers apart.  Before V7 P3 every datastream of a station
	// wrote the SAME file (upstream layout, shared by a station's
	// datastreams), which was fine while they ran one after another but
	// loses rows once they overlap: the update below is a read-modify-write
	// and the later writer drops the earlier one's new lines.  The suffix is
	// the GLOBAL datastream index (the same one visibility.cpp uses), not the
	// station-local one -- both sides must keep agreeing on it.
	// The single-file, timestamp-sorted `PCAL_%05d_%06d_%s` of the product
	// contract is to be recovered by a merge step (V7 P5, not implemented
	// yet), the same way vis-parts/*.part is merged into the SWIN.
	char filename[1024];
	snprintf(filename, sizeof(filename), "%s/PCAL_%05d_%06d_%s_ds%d",
	         pcaldir.c_str(), config->getStartMJD(), config->getStartSeconds(),
	         config->getDStationName(configindex, dsindex).c_str(), dsindex);
	filepath = filename;
}

PcalTextWriter::~PcalTextWriter()
{
}

void PcalTextWriter::accumulate(Mode *mode)
{
	// FEngineWriter::writePcal has already finalised the tones; add them to
	// the intTime accumulator in the same order as core.cpp:1145-1153
	int toneindex = 0;
	for(int j=0;j<nrecordedbands;j++)
	{
		for(int t=0;t<bandntones[j];t++)
		{
			cf32 tone = mode->getPcal(j, t);
			accum[toneindex].re += tone.re;
			accum[toneindex].im += tone.im;
			toneindex++;
		}
	}
}

void PcalTextWriter::accumulateWeight(Mode *mode)
{
	// Mirrors Core::averageAndSendAutocorrs: the per-band weights accumulated
	// since the last zeroAutocorrelations() must be read BEFORE they are
	// cleared, so the caller invokes this right before writeAutocorrelationBatch
	// + zeroAutocorrelations (f32 sums like core.cpp:1331)
	for(int j=0;j<nrecordedbands;j++)
		acwacc[j] += mode->getWeight(false, j);
}

void PcalTextWriter::flush(long long timestartsec, int timestartns)
{
	// calibration scale per band, byte-compatible with the pulse-cal
	// calibration in Visibility::writedata (visibility.cpp:652-676):
	//   acw = f32(intTime weight sum / fftsperintegration)
	//   scale = 1/(acw*meansubintsperintegration*((float)(blockspersend*2*freqchannels*chanstoavg)))
	double meansubints = config->getIntTime(configindex)/(((double)config->getSubintNS(configindex))/1000000000.0);
	double fftspi = meansubints*(double)blockspersend;
	std::vector<f32> bandscale(nrecordedbands, 1.0f);
	for(int j=0;j<nrecordedbands;j++)
	{
		if(acwacc[j] > 0.0f)
		{
			double acw = (double)acwacc[j]/fftspi;	// f32 storage upstream (visibility.cpp:455)
			if(acw == 0.0 && config->getDNumTotalBands(configindex, dsindex) > nrecordedbands)
				acw = 1.0;	// zoom band w/o matching autos (visibility.cpp:664)
			if(acw > 0.0)
				bandscale[j] = (f32)(1.0/(acw*meansubints*((float)(blockspersend*2*freqchannels[j]*chanstoavg))));
		}
	}

	// pcalmjd = intTime midpoint, same decomposition as visibility.cpp:983-989
	// (split whole days first so the f64 rounding matches upstream bit for bit)
	long long daysec = timestartsec;
	int dumpmjd = config->getStartMJD() + (int)(daysec/86400);
	double dumpseconds = (double)(daysec%86400) + (double)timestartns/1.0e9 + config->getIntTime(configindex)/2.0;
	double pcalmjd = (double)dumpmjd + dumpseconds/86400.0;

	// assemble the data line (visibility.cpp:994-1048)
	const char noToneAvailable[] = " -1 0 0 0";
	ostringstream line;
	char buf[256];
	snprintf(buf, sizeof(buf), "%s %17.11f %13.11f %d %d %d",
	         config->getDStationName(configindex, dsindex).c_str(), pcalmjd,
	         config->getIntTime(configindex)/86400.0, dsindex,
	         nrecordedbands, maxtones);
	line << buf;

	bool nonzero = false;	// visibility.cpp:1040: any tone with re != 0 and im != 0
	int toneindex = 0;
	for(int j=0;j<nrecordedbands;j++)
	{
		int localfreqindex = config->getDLocalRecordedFreqIndex(configindex, dsindex, j);
		for(int t=0;t<maxtones;t++)
		{
			if(t >= bandntones[j])
			{
				line << noToneAvailable;
				continue;
			}
			// upstream keeps tonefreq as float (visibility.cpp:1011); keep the
			// same type so %.12g prints identically
			float tonefreq = 1e-6*config->getDRecordedFreqPCalToneFreqHz(configindex, dsindex, localfreqindex, t);
			// calibrated values: f32*f32 like vectorMulC_f32_I (visibility.cpp:672)
			f32 re = accum[toneindex].re * bandscale[j];
			f32 im = accum[toneindex].im * bandscale[j];
			// sideband sign correction, byte-compatible with visibility.cpp:1019-1033:
			// LSB writes the extractor value as-is, USB flips the imaginary part
			if(config->getDRecordedLowerSideband(configindex, dsindex, localfreqindex))
			{
				snprintf(buf, sizeof(buf), " %.12g %c %12.5e %12.5e",
				         tonefreq, config->getDRecordedBandPol(configindex, dsindex, j),
				         re, im);
			}
			else
			{
				snprintf(buf, sizeof(buf), " %.12g %c %12.5e %12.5e",
				         tonefreq, config->getDRecordedBandPol(configindex, dsindex, j),
				         re, -im);
			}
			if(re != 0.0f && -im != 0.0f)
				nonzero = true;
			line << buf;
			toneindex++;
		}
	}

	if(!nonzero)
	{
		// no tone survived calibration: nothing to write, but reset like upstream
		memset(&accum[0], 0, sizeof(cf32)*accum.size());
		for(int j=0;j<nrecordedbands;j++)
			acwacc[j] = 0.0f;
		return;
	}

	// Idempotent append (algo-plan.md P0 strategy A): keep the comment header
	// and every line except this datastream's line at this intTime (rerunning
	// a batch rewrites each intTime line in place, leaving all other intTimes,
	// all other batches and the sibling datastreams of this station intact),
	// then rewrite all data lines sorted by (timestamp, datastream) so the
	// file stays in timestamp order even after a middle-batch rerun.  There is
	// one line per (datastream, intTime), so that pair is the exact
	// replacement key; tolerance absorbs the %17.11f -> strtod round-trip
	// error (~1e-11 days) and is far below half an intTime.
	double tol = 1e-8;	// days ~= 0.86 ms
	ostringstream header;
	vector<PcalLine> datalines;
	ifstream in(filepath.c_str());
	if(in.is_open())
	{
		string pline;
		while(getline(in, pline))
		{
			if(pline.empty())
				continue;
			if(pline[0] == '#')
			{
				header << pline << "\n";
				continue;
			}
			// fields: <station> <mjd> <intTime> <datastream> ...  a line that
			// does not parse is kept as-is (never another job's replacement)
			PcalLine old;
			old.mjd = 0.0;
			old.ds = -1;
			if(sscanf(pline.c_str(), "%*s %lf %*f %d", &old.mjd, &old.ds) == 2
			   && fabs(old.mjd - pcalmjd) < tol && old.ds == dsindex)
				continue;	// same datastream, same intTime: superseded by this rerun
			old.text = pline;
			datalines.push_back(old);
		}
		in.close();
	}
	if(header.str().empty())
	{
		// first write: comment header, format parsed by other software
		// (visibility.cpp:123-131)
		header << "# DiFX-derived pulse cal data\n"
		       << "# File version = 1\n"
		       << "# Start MJD = " << config->getStartMJD() << "\n"
		       << "# Start seconds = " << config->getStartSeconds() << "\n"
		       << "# Telescope name = " << config->getDStationName(configindex, dsindex) << "\n";
	}
	PcalLine current;
	current.mjd = pcalmjd;
	current.ds = dsindex;
	current.text = line.str();
	datalines.push_back(current);
	sort(datalines.begin(), datalines.end(), pcalline_less);

	ofstream out(filepath.c_str(), ios::trunc);
	if(!out.is_open())
	{
		cerr << "PcalTextWriter: cannot open " << filepath << " for writing" << endl;
		exit(EXIT_FAILURE);
	}
	out << header.str();
	for(vector<PcalLine>::const_iterator it = datalines.begin(); it != datalines.end(); ++it)
		out << it->text << "\n";
	out.close();

	// reset the accumulators for the next intTime
	memset(&accum[0], 0, sizeof(cf32)*accum.size());
	for(int j=0;j<nrecordedbands;j++)
		acwacc[j] = 0.0f;
}

bool PcalTextWriter::hasUnflushed() const
{
	for(int i=0;i<(int)accum.size();i++)
	{
		if(accum[i].re != 0.0f || accum[i].im != 0.0f)
			return true;
	}
	return false;
}
