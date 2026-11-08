#include "memreader.h"

#include <cstring>

using namespace std;

MemReader::MemReader(Mode *m, Configuration *config, int configindex, int dsindex, int b) :
	mode(m), band(b), nchan(0), blockspersend(0), numbufferedffts(0),
	specbuf(0), wbuf(0)
{
	int freqindex = config->getDTotalFreqIndex(configindex, dsindex, band);
	nchan = config->getFNumChannels(freqindex);
	blockspersend = config->getBlocksPerSend(configindex);
	numbufferedffts = config->getNumBufferedFFTs(configindex);

	specbuf = new cf32[(long long)blockspersend*nchan];
	wbuf = new f32[blockspersend];
}

MemReader::~MemReader()
{
	delete [] specbuf;
	delete [] wbuf;
}

void MemReader::collectBand(int fftloop)
{
	// same fftloop slicing as fxcorr-f's writeSpectraBand: the batch covers
	// blocks [fftloop*numbufferedffts, ...+numffts), slot b of the Mode holds
	// global block fftloop*numbufferedffts+b
	int numffts = blockspersend - fftloop*numbufferedffts;
	if(numffts > numbufferedffts)
		numffts = numbufferedffts;

	for(int b=0;b<numffts;b++)
	{
		long long i = (long long)fftloop*numbufferedffts + b;
		const cf32 *spec = mode->getFreqs(band, b);
		memcpy(specbuf + i*nchan, spec, sizeof(cf32)*nchan);
		wbuf[i] = mode->getDataWeight(band, b);
	}
}
