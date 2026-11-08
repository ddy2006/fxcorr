#include "acbatch.h"

using namespace std;

AcBatchCollector::AcBatchCollector(Configuration *conf, int confindex, int ds) :
	config(conf), configindex(confindex), dsindex(ds),
	nrecordedbands(0), ntotalbands(0), hascrosspol(false)
{
	nrecordedbands = config->getDNumRecordedBands(configindex, dsindex);
	ntotalbands = config->getDNumTotalBands(configindex, dsindex);

	// per-band autocorrelation width after averageFrequency() and the band
	// whose weight applies (FEngineWriter constructor / writeAutocorrelationBatch)
	acbandnchan.resize(ntotalbands);
	parentband.resize(ntotalbands);
	for(int k=0;k<ntotalbands;k++)
	{
		int freqindex = config->getDTotalFreqIndex(configindex, dsindex, k);
		acbandnchan[k] = config->getFNumChannels(freqindex)/config->getFChannelsToAverage(freqindex);
		parentband[k] = k;	// recorded band: its own weight
		if(k >= nrecordedbands)
		{
			// zoom band: the weight of its parent recorded band
			// (core.cpp:1324-1339; Mode::weights does not cover zoom bands)
			int localzoom = config->getDLocalZoomFreqIndex(configindex, dsindex, k-nrecordedbands);
			int parentfreqindex = config->getDZoomFreqParentFreqIndex(configindex, dsindex, localzoom);
			parentband[k] = -1;
			for(int l=0;l<nrecordedbands;l++)
			{
				if(config->getDLocalRecordedFreqIndex(configindex, dsindex, l) == parentfreqindex &&
				   config->getDRecordedBandPol(configindex, dsindex, l) == config->getDZoomBandPol(configindex, dsindex, k-nrecordedbands))
				{
					parentband[k] = l;
				}
			}
		}
	}

	// cross-polar section condition, same as core.cpp:1288 / FEngineWriter
	hascrosspol = config->writeAutoCorrs(configindex) && config->getMaxProducts() > 2;
}

AutocorrBatch AcBatchCollector::collect(Mode *mode, bool datastreamsaveraged)
{
	AutocorrBatch b;

	// fxcorr-f averaged the spectra in frequency first (writeAutocorrelationBatch);
	// P9: an STA dump above the post-average channel count pre-averages and the
	// average step here must then be skipped (core.cpp:1263-1268)
	if(!datastreamsaveraged)
		mode->averageFrequency();

	// parallel section, in the results layout's total-band order
	b.w.resize(ntotalbands);
	for(int j=0;j<ntotalbands;j++)
	{
		const cf32 *ac = mode->getAutocorrelation(false, j);
		b.ac.insert(b.ac.end(), ac, ac + acbandnchan[j]);
		b.w[j] = mode->getWeight(false, parentband[j]);
	}
	if(hascrosspol)
	{
		b.wx.resize(ntotalbands);
		for(int j=0;j<ntotalbands;j++)
		{
			const cf32 *ac = mode->getAutocorrelation(true, j);
			b.acx.insert(b.acx.end(), ac, ac + acbandnchan[j]);
			b.wx[j] = mode->getWeight(true, parentband[j]);
		}
	}
	return b;
}
