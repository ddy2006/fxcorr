#include "xmac.h"

#include <algorithm>
#include <cassert>
#include <cstdlib>
#include <iostream>

using namespace std;

XmacEngine::XmacEngine(Configuration *conf, int cindex, Model *m, int sc) :
	config(conf), model(m), scan(sc), configindex(cindex),
	numphasecentres(model->getNumPhaseCentres(scan)),
	pulsarbin(config->pulsarBinOn(configindex)),
	scrunchoutput(pulsarbin && config->scrunchOutputOn(configindex)),
	numpulsarbins(pulsarbin ? config->getNumPulsarBins(configindex) : 0),
	threadbinloop(pulsarbin ? config->getNumPulsarBins(configindex) : 1),
	corebinloop((pulsarbin && !config->scrunchOutputOn(configindex)) ? config->getNumPulsarBins(configindex) : 1),
	freqtablelength(0), numbaselines(0), numdatastreams(0),
	numBufferedFFTs(0), blockspersend(0), xmacstridelength(0),
	blockns(0.0),
	threadresultlength(0), threadcrosscorrs(0), baselineweight(0), baselineshiftdecorr(0), conjbuf(0),
	bins(0), pulsarscratchspace(0), pulsaraccumspace(0),
	maxchan(0), maxrotatestrideplussteplength(0),
	chanfreqs(0), rotator(0), rotated(0), argument(0), shifterrorcount(0)
{
	freqtablelength = config->getFreqTableLength();
	numbaselines = config->getNumBaselines();
	numdatastreams = config->getNumDataStreams();
	numBufferedFFTs = config->getNumBufferedFFTs(configindex);
	blockspersend = config->getBlocksPerSend(configindex);
	xmacstridelength = config->getXmacStrideLength(configindex);
	blockns = double(config->getSubintNS(configindex))/double(blockspersend);
	threadresultlength = config->getMaxThreadResultLength();

	// P3: per-thread scratch copies (main.cpp sets the thread count before
	// construction; omp_get_max_threads() is 1 when OMP_NUM_THREADS is unset)
	nthreads = omp_get_max_threads();
	threadcrosscorrs = vectorAlloc_cf32(threadresultlength);

	// conjbuf sized for the widest freq in the table
	maxchan = config->getMaxNumChannels();
	conjbuf = new cf32*[nthreads];
	for(int t=0;t<nthreads;t++)
		conjbuf[t] = vectorAlloc_cf32(maxchan);

	// pulsar scratchspace (core.cpp:407-453, 1940-2064): bins per buffered
	// FFT, xmac multiply buffer, and (scrunch only) per-bin accumulation
	if(pulsarbin)
	{
		pulsarscratchspace = new cf32*[nthreads];
		for(int t=0;t<nthreads;t++)
			pulsarscratchspace[t] = vectorAlloc_cf32(xmacstridelength);
		bins = new s32**[numBufferedFFTs];
		for(int i=0;i<numBufferedFFTs;i++)
		{
			bins[i] = new s32*[freqtablelength];
			for(int f=0;f<freqtablelength;f++)
			{
				if(config->isFrequencyUsed(configindex, f))
					bins[i][f] = vectorAlloc_s32(config->getFNumChannels(f));
				else
					bins[i][f] = 0;
			}
		}
		if(scrunchoutput)
		{
			// createPulsarVaryingSpace (core.cpp:2019-2062), forced to a
			// single pulsar ephemeris (source slot 0) like the upstream
			pulsaraccumspace = new cf32******[freqtablelength];
			for(int f=0;f<freqtablelength;f++)
			{
				if(!config->isFrequencyUsed(configindex, f))
				{
					pulsaraccumspace[f] = 0;
					continue;
				}
				pulsaraccumspace[f] = new cf32*****[config->getNumXmacStrides(configindex, f)];
				for(int x=0;x<config->getNumXmacStrides(configindex, f);x++)
				{
					pulsaraccumspace[f][x] = new cf32****[numbaselines];
					for(int i=0;i<numbaselines;i++)
					{
						int localfreqindex = localFreqIndex(i, f);
						if(localfreqindex < 0)
						{
							pulsaraccumspace[f][x][i] = 0;
							continue;
						}
						pulsaraccumspace[f][x][i] = new cf32***[1]; //just 1 source for now!
						pulsaraccumspace[f][x][i][0] = new cf32**[config->getBNumPolProducts(configindex, i, localfreqindex)];
						for(int j=0;j<config->getBNumPolProducts(configindex, i, localfreqindex);j++)
						{
							pulsaraccumspace[f][x][i][0][j] = new cf32*[numpulsarbins];
							for(int k=0;k<numpulsarbins;k++)
							{
								pulsaraccumspace[f][x][i][0][j][k] = vectorAlloc_cf32(xmacstridelength);
								vectorZero_cf32(pulsaraccumspace[f][x][i][0][j][k], xmacstridelength);
							}
						}
					}
				}
			}
		}
	}

	// multi phase centre scratchspace (core.cpp:416-433), sized over all
	// configs exactly like the upstream allocation
	{
		int slen = config->getRotateStrideLength(0);
		maxrotatestrideplussteplength = slen + maxchan/slen;
		for(int i=1;i<config->getNumConfigs();i++)
		{
			slen = config->getRotateStrideLength(i);
			int strideplussteplen = slen + maxchan/slen;
			if(strideplussteplen > maxrotatestrideplussteplength)
				maxrotatestrideplussteplength = strideplussteplen;
		}
		chanfreqs = new f64*[nthreads];
		rotator = new cf32*[nthreads];
		rotated = new cf32*[nthreads];
		argument = new f32*[nthreads];
		for(int t=0;t<nthreads;t++)
		{
			chanfreqs[t] = vectorAlloc_f64(maxrotatestrideplussteplength);
			rotator[t] = vectorAlloc_cf32(maxrotatestrideplussteplength);
			rotated[t] = vectorAlloc_cf32(maxchan);
			argument[t] = vectorAlloc_f32(3*maxrotatestrideplussteplength);
		}
	}

	// baselineweight[f][b][j][p] (b = pulsar bin, single slot otherwise),
	// allocated per numpolproducts like allocateConfigSpecificThreadArrays
	baselineweight = new f32***[freqtablelength];
	baselineshiftdecorr = new f32**[freqtablelength];
	for(int i=0;i<freqtablelength;i++)
	{
		baselineweight[i] = new f32**[corebinloop];
		for(int b=0;b<corebinloop;b++)
		{
			baselineweight[i][b] = new f32*[numbaselines];
			for(int j=0;j<numbaselines;j++)
			{
				int localfreqindex = localFreqIndex(j, i);
				if(localfreqindex >= 0)
					baselineweight[i][b][j] = new f32[config->getBNumPolProducts(configindex, j, localfreqindex)];
				else
					baselineweight[i][b][j] = 0;
			}
		}
		if(config->isFrequencyUsed(configindex, i) && config->getMaxPhaseCentres(configindex) > 1)
		{
			// allocateConfigSpecificThreadArrays (core.cpp:2135-2148)
			baselineshiftdecorr[i] = new f32*[numbaselines];
			for(int j=0;j<numbaselines;j++)
			{
				int localfreqindex = localFreqIndex(j, i);
				if(localfreqindex >= 0)
					baselineshiftdecorr[i][j] = vectorAlloc_f32(config->getMaxPhaseCentres(configindex));
				else
					baselineshiftdecorr[i][j] = 0;
			}
		}
		else
			baselineshiftdecorr[i] = 0;
	}
}

// -1 for a baseline outside this shard, otherwise the normal lookup: every
// caller already treats -1 as "this baseline has no data at this frequency"
int XmacEngine::localFreqIndex(int baseline, int freq) const
{
	if(!activebaselines.empty() && activebaselines[baseline] == 0)
		return -1;
	return config->getBLocalFreqIndex(configindex, baseline, freq);
}

void XmacEngine::setActiveBaselines(const std::vector<char> &active)
{
	activebaselines = active;
}

XmacEngine::~XmacEngine()
{
	vectorFree(threadcrosscorrs);
	for(int t=0;t<nthreads;t++)
		vectorFree(conjbuf[t]);
	delete [] conjbuf;
	for(int t=0;t<nthreads;t++)
	{
		vectorFree(chanfreqs[t]);
		vectorFree(rotator[t]);
		vectorFree(rotated[t]);
		vectorFree(argument[t]);
	}
	delete [] chanfreqs;
	delete [] rotator;
	delete [] rotated;
	delete [] argument;
	if(pulsarbin)
	{
		for(int t=0;t<nthreads;t++)
			vectorFree(pulsarscratchspace[t]);
		delete [] pulsarscratchspace;
		for(int i=0;i<numBufferedFFTs;i++)
		{
			for(int f=0;f<freqtablelength;f++)
				if(bins[i][f])
					vectorFree(bins[i][f]);
			delete [] bins[i];
		}
		delete [] bins;
		if(scrunchoutput)
		{
			// reverse of createPulsarVaryingSpace's allocation (core.cpp:1969-1999)
			for(int f=0;f<freqtablelength;f++)
			{
				if(!pulsaraccumspace[f])
					continue;
				for(int x=0;x<config->getNumXmacStrides(configindex, f);x++)
				{
					for(int i=0;i<numbaselines;i++)
					{
						if(!pulsaraccumspace[f][x][i])
							continue;
						int localfreqindex = localFreqIndex(i, f);
						for(int j=0;j<config->getBNumPolProducts(configindex, i, localfreqindex);j++)
						{
							for(int k=0;k<numpulsarbins;k++)
								vectorFree(pulsaraccumspace[f][x][i][0][j][k]);
							delete [] pulsaraccumspace[f][x][i][0][j];
						}
						delete [] pulsaraccumspace[f][x][i][0];
						delete [] pulsaraccumspace[f][x][i];
					}
					delete [] pulsaraccumspace[f][x];
				}
				delete [] pulsaraccumspace[f];
			}
			delete [] pulsaraccumspace;
		}
	}
	for(int i=0;i<freqtablelength;i++)
	{
		for(int b=0;b<corebinloop;b++)
		{
			for(int j=0;j<numbaselines;j++)
				delete [] baselineweight[i][b][j];
			delete [] baselineweight[i][b];
		}
		for(int j=0;j<numbaselines;j++)
		{
			if(baselineshiftdecorr[i] && baselineshiftdecorr[i][j])
				vectorFree(baselineshiftdecorr[i][j]);
		}
		delete [] baselineweight[i];
		delete [] baselineshiftdecorr[i];
	}
	delete [] baselineweight;
	delete [] baselineshiftdecorr;
}

void XmacEngine::zeroSubint()
{
	// core.cpp:722-759
	vectorZero_cf32(threadcrosscorrs, threadresultlength);
	for(int i=0;i<freqtablelength;i++)
	{
		if(config->isFrequencyUsed(configindex, i))
		{
			for(int b=0;b<corebinloop;b++)
			{
				for(int j=0;j<numbaselines;j++)
				{
					int localfreqindex = localFreqIndex(j, i);
					if(localfreqindex >= 0)
						vectorZero_f32(baselineweight[i][b][j], config->getBNumPolProducts(configindex, j, localfreqindex));
				}
			}
			if(numphasecentres > 1)
			{
				// core.cpp:745-757
				for(int j=0;j<numbaselines;j++)
				{
					int localfreqindex = localFreqIndex(j, i);
					if(localfreqindex >= 0)
						vectorZero_f32(baselineshiftdecorr[i][j], numphasecentres);
				}
			}
		}
	}
}

// gcc 11's -O2 (the build default) does not auto-vectorise; the inner loops
// here are element-wise complex multiply-adds -- pure throughput work -- so
// this one function gets -O3.  Deliberately no -march/-ffast-math: the
// vectorisation stays SSE2 (the x86-64 baseline) with no FMA contraction,
// every element is rounded exactly as in the -O2 build, and the products
// stay bit-identical (checked against the SWIN reference).
__attribute__((optimize("O3")))
void XmacEngine::xmacBatch(int fftloop, const vector<vector<SpReader *> > &readers, Polyco *currentpolyco)
{
	// if necessary, work out the pulsar bins (core.cpp:803-812).  The FFT
	// index i starts from 0 (no upstream startblock); offsetmins is in the
	// subint-local minute base.
	if(pulsarbin)
	{
		for(int fftsubloop=0;fftsubloop<numBufferedFFTs;fftsubloop++)
		{
			int i = fftloop*numBufferedFFTs + fftsubloop;
			double offsetmins = ((double)i)*blockns/60000000000.0;
			currentpolyco->getBins(offsetmins, bins[fftsubloop]);
		}
	}

	// core.cpp:867-982; resultindex accumulation mirrors
	// Configuration::populateResultLengths() so threadcrosscorrs offsets agree
	// with getThreadResultFreqOffset/getThreadResultBaselineOffset.
	//
	// P3: collect the used (freq, xmac-pass) pairs, precomputing each pair's
	// base offset into threadcrosscorrs by replaying the serial resultindex
	// accumulation (f -> x -> baseline), then run the baseline loops of all
	// pairs in parallel.  Each baseline's write region (threadcrosscorrs,
	// baselineweight, pulsaraccumspace) is disjoint between baselines and its
	// accumulation sequence is unchanged, so results are bit-identical to the
	// serial form; conjbuf/pulsarscratchspace are per-thread copies.
	struct FXPass { int f, x, xmacstart, xmacstrideremain; long long baseoffset; };
	std::vector<FXPass> fxpasses;
	long long resultindex = 0;
	for(int f=0;f<freqtablelength;f++)
	{
		if(!config->isFrequencyUsed(configindex, f))
			continue;

		int freqchannels = config->getFNumChannels(f);
		int xmacpasses = config->getNumXmacStrides(configindex, f);
		for(int x=0;x<xmacpasses;x++)
		{
			int xmacstart = x*xmacstridelength;
			int xmacstrideremain = min(freqchannels-xmacstart, xmacstridelength);
			if(xmacstrideremain < 0)
				continue;

			FXPass pass = {f, x, xmacstart, xmacstrideremain, resultindex};
			fxpasses.push_back(pass);
			for(int j=0;j<numbaselines;j++)
			{
				// the layout must be the full one even in shard mode: the
				// offsets are a fixed function of the baseline number and
				// uvshiftAndAverage below reads the array through them
				int localfreqindex = config->getBLocalFreqIndex(configindex, j, f);
				if(localfreqindex >= 0)
				{
					// core.cpp:970-977: advance to next baseline's xmac slice
					if(pulsarbin && !scrunchoutput)
						resultindex += config->getBNumPolProducts(configindex, j, localfreqindex)*numpulsarbins*xmacstridelength;
					else
						resultindex += config->getBNumPolProducts(configindex, j, localfreqindex)*xmacstridelength;
				}
			}
		}
	}

	#pragma omp parallel for schedule(static)
	for(int fx=0;fx<(int)fxpasses.size();fx++)
	{
		const FXPass &pass = fxpasses[fx];
		int f = pass.f;
		int x = pass.x;
		int xmacstart = pass.xmacstart;
		int xmacstrideremain = pass.xmacstrideremain;
		int freqchannels = config->getFNumChannels(f);
		long long resultindex = pass.baseoffset;
		int status;
		int tid = omp_get_thread_num();
		cf32 *myconjbuf = conjbuf[tid];
		cf32 *mypulsarscratchspace = (pulsarbin) ? pulsarscratchspace[tid] : 0;

		for(int j=0;j<numbaselines;j++)
		{
			int localfreqindex = config->getBLocalFreqIndex(configindex, j, f);
			if(localfreqindex >= 0)
			{
				if(!baselineActive(j))
				{
					// Shard mode: this baseline is not ours, but its slice of
					// threadcrosscorrs must still be stepped over - the
					// offsets are a fixed function of the baseline number, so
					// skipping the step would shift every later baseline onto
					// the wrong data (this is what made a shard's baselines
					// come out as zeros)
					if(pulsarbin && !scrunchoutput)
						resultindex += config->getBNumPolProducts(configindex, j, localfreqindex)*numpulsarbins*xmacstridelength;
					else
						resultindex += config->getBNumPolProducts(configindex, j, localfreqindex)*xmacstridelength;
					continue;
				}
				int ds1index = config->getBOrderedDataStream1Index(configindex, j);
				int ds2index = config->getBOrderedDataStream2Index(configindex, j);

				for(int fftsubloop=0;fftsubloop<numBufferedFFTs;fftsubloop++)
				{
					int i = fftloop*numBufferedFFTs + fftsubloop;
					if(i >= blockspersend)
						break; //may not have to fully complete last fftloop

					for(int p=0;p<config->getBNumPolProducts(configindex, j, localfreqindex);p++)
					{
						int band1 = config->getBDataStream1BandIndex(configindex, j, localfreqindex, p);
						int band2 = config->getBDataStream2BandIndex(configindex, j, localfreqindex, p);
						const SpReader *r1 = readers[ds1index][band1];
						const SpReader *r2 = readers[ds2index][band2];

						// vis1 = getFreqs(...)[xmacstart]; vis2 = conjugated counterpart
						const cf32 *vis1 = r1->spectra() + (long long)i*r1->numChannels() + xmacstart;
						const cf32 *vis2 = r2->spectra() + (long long)i*r2->numChannels() + xmacstart;

						status = vectorConj_cf32(vis2, myconjbuf, xmacstrideremain);
						if(status != vecNoErr)
							cerr << "Error conjugating vis2, baseline " << j << ", status " << status << endl;

						if(pulsarbin)
						{
							// core.cpp:914-957: multiply into scratch space, then bin
							int ds1recordbandindex = config->getBDataStream1RecordBandIndex(configindex, j, localfreqindex, p);
							int ds2recordbandindex = config->getBDataStream2RecordBandIndex(configindex, j, localfreqindex, p);
							f32 weight1 = readers[ds1index][ds1recordbandindex]->weights()[i];
							f32 weight2 = readers[ds2index][ds2recordbandindex]->weights()[i];
							f32 bweight = weight1*weight2/freqchannels;

							status = vectorMul_cf32(vis1, myconjbuf, mypulsarscratchspace, xmacstrideremain);
							if(status != vecNoErr)
								cerr << "Error trying to xmac baseline " << j << " frequency " << localfreqindex << " polarisation product " << p << ", status " << status << endl;

							// if scrunching, add into temp accumulate space, otherwise add into normal space
							if(scrunchoutput)
							{
								f64 *binweights = currentpolyco->getBinWeights();
								int destchan = xmacstart;
								for(int l=0;l<xmacstrideremain;l++)
								{
									// the first zero (the source slot) is because we are limiting to one pulsar ephemeris for now
									int destbin = bins[fftsubloop][f][destchan];
									pulsaraccumspace[f][x][j][0][p][destbin][l].re += mypulsarscratchspace[l].re;
									pulsaraccumspace[f][x][j][0][p][destbin][l].im += mypulsarscratchspace[l].im;
									// Negative bin weights are generally used when scrunching to estimate and
									// remove slowly-time-varying signal; ignore them in the baseline weight
									if(binweights[destbin] > 0.0)
										baselineweight[f][0][j][p] += bweight*binweights[destbin];
									destchan++;
								}
							}
							else
							{
								int destchan = xmacstart;
								for(int l=0;l<xmacstrideremain;l++)
								{
									int destbin = bins[fftsubloop][f][destchan];
									int cindex = resultindex + (destbin*config->getBNumPolProducts(configindex, j, localfreqindex) + p)*xmacstridelength + l;
									threadcrosscorrs[cindex].re += mypulsarscratchspace[l].re;
									threadcrosscorrs[cindex].im += mypulsarscratchspace[l].im;
									baselineweight[f][destbin][j][p] += bweight;
									destchan++;
								}
							}
						}
						else
						{
							status = vectorAddProduct_cf32(vis1, myconjbuf, &(threadcrosscorrs[resultindex+p*xmacstridelength]), xmacstrideremain);
							if(status != vecNoErr)
								cerr << "Error trying to xmac baseline " << j << " frequency " << localfreqindex << " polarisation product " << p << ", status " << status << endl;
						}
					}
				}
				// core.cpp:970-977: advance to next baseline's xmac slice
				if(pulsarbin && !scrunchoutput)
					resultindex += config->getBNumPolProducts(configindex, j, localfreqindex)*numpulsarbins*xmacstridelength;
				else
					resultindex += config->getBNumPolProducts(configindex, j, localfreqindex)*xmacstridelength;
			}
		}
	}
}

// same -O3 rationale as xmacBatch above (element-wise loops, SSE2 baseline)
__attribute__((optimize("O3")))
void XmacEngine::accumulateWeights(int fftloop, const vector<vector<SpReader *> > &readers)
{
	if(pulsarbin)
		return; // weights are updated inside the XMAC pulsar branch (core.cpp:924/945/954)

	// core.cpp:1005-1052, non-pulsar branch.  P3: baseline loop outermost and
	// parallel; each baseline's accumulation sequence over (fftsubloop, f, p)
	// is unchanged (the loop exchange only reorders across baselines), so
	// baselineweight entries are bit-identical to the serial form.
	#pragma omp parallel for schedule(static)
	for(int j=0;j<numbaselines;j++)
	{
		for(int fftsubloop=0;fftsubloop<numBufferedFFTs;fftsubloop++)
		{
			int i = fftloop*numBufferedFFTs + fftsubloop;
			if(i >= blockspersend)
				break; //may not have to fully complete last fftloop

			for(int f=0;f<freqtablelength;f++)
			{
				if(config->isFrequencyUsed(configindex, f))
				{
					int localfreqindex = localFreqIndex(j, f);
					if(localfreqindex >= 0)
					{
						int ds1index = config->getBOrderedDataStream1Index(configindex, j);
						int ds2index = config->getBOrderedDataStream2Index(configindex, j);
						for(int p=0;p<config->getBNumPolProducts(configindex, j, localfreqindex);p++)
						{
							int ds1recordbandindex = config->getBDataStream1RecordBandIndex(configindex, j, localfreqindex, p);
							int ds2recordbandindex = config->getBDataStream2RecordBandIndex(configindex, j, localfreqindex, p);

							if(ds1recordbandindex < 0 || ds2recordbandindex < 0)
							{
								cerr << "Error: one of the record band indices could not be found: ds1recordbandindex = " << ds1recordbandindex << " ds2recordbandindex = " << ds2recordbandindex << endl;
							}
							else
							{
								f32 weight1 = readers[ds1index][ds1recordbandindex]->weights()[i];
								f32 weight2 = readers[ds2index][ds2recordbandindex]->weights()[i];
								baselineweight[f][0][j][p] += weight1*weight2;
							}
						}
					}
				}
			}
		}
	}
}

void XmacEngine::uvshiftAndAverage(double offsetsec, double nsoffset, double nswidth, Polyco *currentpolyco, cf32 *subintresults)
{
	// first scale the pulsar data if necessary (core.cpp:1438-1475)
	if(pulsarbin && scrunchoutput)
	{
		f64 *binweights = currentpolyco->getBinWeights();

		for(int f=0;f<freqtablelength;f++)
		{
			if(config->isFrequencyUsed(configindex, f))
			{
				int freqchannels = config->getFNumChannels(f);
				int numxmacstrides = config->getNumXmacStrides(configindex, f);
				for(int x=0;x<numxmacstrides;x++)
				{
					int xmacstrideremain = min(freqchannels-x*xmacstridelength, xmacstridelength);
					for(int i=0;i<numbaselines;i++)
					{
						int localfreqindex = localFreqIndex(i, f);
						if(localfreqindex >= 0)
						{
							for(int s=0;s<1;s++) //forced to single pulsar ephemeris for now
							{
								for(int j=0;j<config->getBNumPolProducts(configindex, i, localfreqindex);j++)
								{
									for(int k=0;k<numpulsarbins;k++)
									{
										int status = vectorMulC_f32_I((f32)(binweights[k]), (f32*)(pulsaraccumspace[f][x][i][s][j][k]), 2*xmacstrideremain);
										if(status != vecNoErr)
											cerr << "Error trying to scale for scrunch!!! " << status << endl;
									}
								}
							}
						}
					}
				}
			}
		}
	}

	// core.cpp:1530-1563 with threadid==0 (startfreq/startbaseline both 0,
	// so every used (freq, baseline) pair is processed exactly once).
	// P3: (freq, baseline) pairs run in parallel; each pair writes a disjoint
	// slice of subintresults (core offset from getCoreResultBaselineOffset)
	// and its own baselineshiftdecorr entry, so results are bit-identical.
	#pragma omp parallel for collapse(2) schedule(static)
	for(int f=0;f<freqtablelength;f++)
	{
		for(int i=0;i<numbaselines;i++)
		{
			if(config->isFrequencyUsed(configindex, f))
				uvshiftAndAverageBaselineFreq(offsetsec, nsoffset, nswidth, currentpolyco, f, i, subintresults);
		}
	}

	// clear the cross-corr results for the next averaging period (core.cpp:1560-1563)
	vectorZero_cf32(threadcrosscorrs, threadresultlength);

	// clear the pulsar accumulation vector if necessary (core.cpp:1565-1602)
	if(pulsarbin && scrunchoutput)
	{
		for(int f=0;f<freqtablelength;f++)
		{
			if(config->isFrequencyUsed(configindex, f))
			{
				int freqchannels = config->getFNumChannels(f);
				int numxmacstrides = config->getNumXmacStrides(configindex, f);
				for(int x=0;x<numxmacstrides;x++)
				{
					int xmacstrideremain = min(freqchannels-x*xmacstridelength, xmacstridelength);
					for(int i=0;i<numbaselines;i++)
					{
						int localfreqindex = localFreqIndex(i, f);
						if(localfreqindex >= 0)
						{
							for(int s=0;s<1;s++) //forced to single pulsar ephemeris for now
							{
								for(int j=0;j<config->getBNumPolProducts(configindex, i, localfreqindex);j++)
								{
									for(int k=0;k<numpulsarbins;k++)
									{
										//zero the accumulation space for next time
										vectorZero_cf32(pulsaraccumspace[f][x][i][s][j][k], xmacstrideremain);
									}
								}
							}
						}
					}
				}
			}
		}
	}
}

void XmacEngine::uvshiftAndAverageBaselineFreq(double offsetsec, double nsoffset, double nswidth, Polyco *currentpolyco, int freqindex, int baseline, cf32 *subintresults)
{
	int localfreqindex, targetfreqindex, freqchannels, targetfreqchannels;
	int channelinc, targetchannelinc, stridestoaverage, averagesperstride, averagelength, numstrides;
	int xmacstrideremain, threadindex, threadstart, coreindex, coreoffset, coredest;
	int rotatestridelen, rotatesperstride, rotatorlength, status;
	double bandwidth, bandwidthoftarget, channelbandwidth, outchannelplacementpreavg;
	double stepbandwidth, lofrequency, applieddelay, delaywindow, edgeturns, turns;
	double maxphasechange, timesmeardecorr, delaydecorr;
	double pointingcentredelay1approx[2], pointingcentredelay2approx[2];
	double **phasecentredelay1 = 0, **phasecentredelay2 = 0, **differentialdelay = 0;

	// P3: per-thread scratch copies (index 0 when running serial)
	int tid = omp_get_thread_num();
	f64 *chanfreqs_t = chanfreqs[tid];
	f32 *argument_t = argument[tid];
	cf32 *rotator_t = rotator[tid];
	cf32 *rotated_t = rotated[tid];

	applieddelay = 0.0;
	delaywindow = config->getFNumChannels(freqindex)/(config->getFreqTableBandwidth(freqindex)); //max lag (plus and minus)
	localfreqindex = localFreqIndex(baseline, freqindex);
	rotatestridelen = config->getRotateStrideLength(configindex);

	if(localfreqindex < 0)
		return;

	// allocate space for the phase centre delays if necessary, and calculate pointing centre delays (core.cpp:1636-1672)
	if(numphasecentres > 1)
	{
		phasecentredelay1 = new double*[numphasecentres];
		phasecentredelay2 = new double*[numphasecentres];
		differentialdelay = new double*[numphasecentres];
		for(int i=0;i<numphasecentres;i++)
		{
			phasecentredelay1[i] = new double[2];
			phasecentredelay2[i] = new double[2];
			differentialdelay[i] = new double[2];
		}

		int antenna1index = config->getDModelFileIndex(configindex, config->getBDataStream1Index(configindex, baseline));
		int antenna2index = config->getDModelFileIndex(configindex, config->getBDataStream2Index(configindex, baseline));

		// get the pointing centre interpolator, validity range aribitrarily set to 1us (approximately the tangent)
		model->calculateDelayInterpolator(scan, offsetsec + nsoffset/1000000000.0, 0.000001, 1, antenna1index, 0, 1, pointingcentredelay1approx);
		model->calculateDelayInterpolator(scan, offsetsec + nsoffset/1000000000.0, 0.000001, 1, antenna2index, 0, 1, pointingcentredelay2approx);
		for(int s=0;s<numphasecentres;s++)
		{
			model->calculateDelayInterpolator(scan, offsetsec + nsoffset/1000000000.0, 0.000001, 1, antenna1index, s+1, 1, phasecentredelay1[s]);
			model->calculateDelayInterpolator(scan, offsetsec + nsoffset/1000000000.0, 0.000001, 1, antenna2index, s+1, 1, phasecentredelay2[s]);

			// work out the correct delay (and rate of delay) for this phase centre
			double applieddelay1 = phasecentredelay1[s][1] - pointingcentredelay1approx[1];
			double applieddelay2 = phasecentredelay2[s][1] - pointingcentredelay2approx[1];
			// make correction for geometric rate over the shifted sample range
			applieddelay1 += applieddelay1*pointingcentredelay1approx[0];
			applieddelay2 += applieddelay2*pointingcentredelay2approx[0];
			differentialdelay[s][1] = applieddelay2 - applieddelay1;
			differentialdelay[s][0] = phasecentredelay2[s][0] + pointingcentredelay1approx[0] - (phasecentredelay1[s][0] + pointingcentredelay2approx[0]);
		}
	}

	freqchannels = config->getFNumChannels(freqindex);
	channelinc = config->getFChannelsToAverage(freqindex);
	bandwidth = config->getFreqTableBandwidth(freqindex);
	targetfreqindex = config->getBTargetFreqIndex(configindex, baseline, localfreqindex);
	targetfreqchannels = config->getFNumChannels(targetfreqindex);
	targetchannelinc = config->getFChannelsToAverage(targetfreqindex);
	bandwidthoftarget = config->getFreqTableBandwidth(targetfreqindex);
	lofrequency = config->getFreqTableFreq(freqindex);
	stridestoaverage = channelinc/xmacstridelength;
	rotatesperstride = xmacstridelength/rotatestridelen;
	if(stridestoaverage == 0)
		stridestoaverage = 1;
	averagesperstride = xmacstridelength/channelinc;
	if(averagesperstride == 0)
		averagesperstride = 1;
	averagelength = xmacstridelength/averagesperstride;
	numstrides = freqchannels/xmacstridelength;
	channelbandwidth = bandwidth/double(freqchannels);
	outchannelplacementpreavg = fabs(config->getFreqTableFreqLowedge(freqindex) - config->getFreqTableFreqLowedge(targetfreqindex)) / channelbandwidth;

	assert(targetfreqchannels == (int)(0.5 + (bandwidthoftarget / bandwidth)*freqchannels));
	assert(targetchannelinc == channelinc);

	// fill chanfreqs for the rotator (core.cpp:1697-1717)
	rotatorlength = rotatestridelen+numstrides*rotatesperstride;
	stepbandwidth = rotatestridelen*channelbandwidth;
	if(numphasecentres > 1)
	{
		if(config->getFreqTableLowerSideband(freqindex))
		{
			for(int c=0;c<rotatestridelen;c++)
				chanfreqs_t[c] = -(rotatestridelen-(c+1))*channelbandwidth;
			for(int c=0;c<numstrides*rotatesperstride;c++)
				chanfreqs_t[rotatestridelen+c] = -(numstrides*rotatesperstride-(c+1))*stepbandwidth;
		}
		else
		{
			for(int c=0;c<rotatestridelen;c++)
				chanfreqs_t[c] = c*channelbandwidth;
			for(int c=0;c<numstrides*rotatesperstride;c++)
				chanfreqs_t[rotatestridelen+c] = c*stepbandwidth;
		}
	}

	// core.cpp:1725-1727: destination index into the core results array
	coreindex = config->getCoreResultBaselineOffset(configindex, freqindex, baseline);
	if(coreindex < 0)
		cerr << "Baseline " << baseline << " input freqId " << freqindex << " with destination freqId " << targetfreqindex << " is not mapped to any results[] array index!" << endl;

	// collect spectra data from threadcrosscorrs, do the multi-phasecenter rotation (if necessary),
	// spectral averaging (if necessary) and concatenation to the subint results (core.cpp:1729-1879)
	for(int s=0;s<numphasecentres;s++)
	{
		if(numphasecentres > 1)
		{
			// work out the correct rotator for this frequency and phase centre (core.cpp:1732-1768)
			applieddelay = differentialdelay[s][1];
			if(fabs(applieddelay) > 1.0e-20)
			{
				edgeturns = applieddelay*lofrequency;
				edgeturns -= floor(edgeturns);
				for(int r=0;r<rotatestridelen;r++)
				{
					turns = applieddelay*chanfreqs_t[r] + edgeturns;
					argument_t[r] = (turns-floor(turns))*TWO_PI;
				}
				for(int r=rotatestridelen;r<rotatorlength;r++)
				{
					turns = applieddelay*chanfreqs_t[r];
					argument_t[r] = (turns-floor(turns))*TWO_PI;
				}
				status = vectorSinCos_f32(argument_t, &(argument_t[rotatorlength]), &(argument_t[2*rotatorlength]), rotatorlength);
				if(status != vecNoErr)
					cerr << "Error in phase shift, sin/cos!!! " << status << endl;
				status = vectorRealToComplex_f32(&(argument_t[2*rotatorlength]), &(argument_t[rotatorlength]), rotator_t, rotatorlength);
				if(status != vecNoErr)
					cerr << "Error in phase shift, real to complex!!! " << status << endl;
			}
		}

		// core.cpp:1770: threadresults offset for this freq/baseline
		threadstart = config->getThreadResultFreqOffset(configindex, freqindex) + config->getThreadResultBaselineOffset(configindex, freqindex, baseline);

		for(int x=0;x<config->getNumXmacStrides(configindex, freqindex);x++)
		{
			threadindex = threadstart + x*config->getCompleteStrideLength(configindex, freqindex);
			xmacstrideremain = min(freqchannels-x*xmacstridelength, xmacstridelength);

			for(int b=0;b<threadbinloop;b++)
			{
				for(int k=0;k<config->getBNumPolProducts(configindex, baseline, localfreqindex);k++)
				{
					if(corebinloop > 1)
						coreoffset = ((b*config->getBNumPolProducts(configindex, baseline, localfreqindex)+k)*targetfreqchannels + x*xmacstridelength)/targetchannelinc;
					else
						coreoffset = (k*targetfreqchannels + x*xmacstridelength)/targetchannelinc;

					const cf32 *srcpointer;
					if(numphasecentres > 1 && fabs(applieddelay) > 1.0e-20)
					{
						// rotate into the scratch buffer (core.cpp:1789-1808)
						if(pulsarbin && scrunchoutput)
							srcpointer = pulsaraccumspace[freqindex][x][baseline][0][k][b];
						else
							srcpointer = &(threadcrosscorrs[threadindex]);
						for(int r=0;r<rotatesperstride;r++)
						{
							status = vectorMul_cf32(rotator_t, &(srcpointer[r*rotatestridelen]), &(rotated_t[r*rotatestridelen]), rotatestridelen);
							if(status != vecNoErr)
								cerr << "Error in phase shift, multiplication1!!! " << status << endl;
							status = vectorMulC_cf32_I(rotator_t[rotatestridelen+r+x*rotatesperstride], &(rotated_t[r*rotatestridelen]), rotatestridelen);
							if(status != vecNoErr)
								cerr << "Error in phase shift, multiplication2!!! " << status << endl;
						}
						srcpointer = rotated_t;
					}
					else
					{
						if(pulsarbin && scrunchoutput)
							srcpointer = pulsaraccumspace[freqindex][x][baseline][0][k][b];
						else
							srcpointer = &(threadcrosscorrs[threadindex]);
					}

				// spectrally average (or not) and accumulate into the subint results
				coredest = coreindex+coreoffset;
				if(channelinc == 1) //this frequency is not averaged
				{
					vectorAdd_cf32_I(srcpointer, &(subintresults[coredest]), xmacstrideremain);
				}
				else //this frequency *is* averaged - core.cpp:1829-1853
				{
					outchannelplacementpreavg = fabs(config->getFreqTableFreqLowedge(freqindex) - config->getFreqTableFreqLowedge(targetfreqindex)) / channelbandwidth;
					int virtualplacement = outchannelplacementpreavg + x*xmacstridelength;
					const cf32* psrc = srcpointer;
					const cf32* pend = psrc + xmacstrideremain;
					while (psrc < pend)
					{
						int valuesinbin = virtualplacement % averagelength;
						if(valuesinbin == 0)
							valuesinbin = averagelength;
						if(valuesinbin > xmacstrideremain)
							valuesinbin = xmacstrideremain;
						cf32 meanresult;
						vectorSum_cf32(psrc, valuesinbin, &meanresult, vecAlgHintFast);
						subintresults[coredest].re += meanresult.re/(stridestoaverage*averagelength);
						subintresults[coredest].im += meanresult.im/(stridestoaverage*averagelength);
						psrc += valuesinbin;
						virtualplacement += valuesinbin;
						coredest++;
					}
				}
				//advance to next xmac channel group
				threadindex += xmacstridelength;
			}
			}
		}

		// stride to next phase centre output area (core.cpp:1878); we stride by 'targetfreqchannels'>='freqchannels'
		// since current freq may be nested within wider freq
		coreindex += config->getBNumPolProducts(configindex, baseline, localfreqindex)*targetfreqchannels/targetchannelinc;
	}

	// calculate the decorrelation for each freq/baseline/source (core.cpp:1886-1923)
	if(numphasecentres > 1)
	{
		for(int s=0;s<numphasecentres;s++)
		{
			timesmeardecorr = 1.0;
			delaydecorr = 1.0;
			if(fabs(differentialdelay[s][0]) > 1e-18)
			{
				maxphasechange = TWO_PI*differentialdelay[s][0]*(nswidth/1000.0)*config->getFreqTableFreq(freqindex);
				timesmeardecorr = sin(maxphasechange/2.0) / (maxphasechange/2.0);
				if(timesmeardecorr < 0.0)
				{
					// use Brian Kernighan's bit counting trick to see if shifterrorcount is a power of two,
					// print only the first few and then increasingly less
					if(shifterrorcount < 10 || (shifterrorcount & (shifterrorcount-1)) == 0)
						cerr << "UV shift integration time far too long for baseline " << baseline << ", source " << s << "; no correlation! (errorcount now " << shifterrorcount << ")" << endl;
					timesmeardecorr = 0;
					#pragma omp atomic
					shifterrorcount++;
				}
			}
			if(fabs(differentialdelay[s][1]) > 1e-18)
			{
				delaydecorr = 1.0 - fabs(differentialdelay[s][1] / delaywindow);
				if(delaydecorr < 0.0)
				{
					if(shifterrorcount < 10 || (shifterrorcount & (shifterrorcount-1)) == 0)
						cerr << "FFT window is not wide enough for baseline " << baseline << ", source " << s << "; no correlation! (errorcount now " << shifterrorcount << ")" << endl;
					delaydecorr = 0;
					#pragma omp atomic
					shifterrorcount++;
				}
			}
			baselineshiftdecorr[freqindex][baseline][s] += nswidth*timesmeardecorr*delaydecorr;
		}
	}

	// free the phasecentredelay vectors if necessary (core.cpp:1925-1937)
	if(phasecentredelay1)
	{
		for(int i=0;i<numphasecentres;i++)
		{
			delete [] phasecentredelay1[i];
			delete [] phasecentredelay2[i];
			delete [] differentialdelay[i];
		}
		delete [] phasecentredelay1;
		delete [] phasecentredelay2;
		delete [] differentialdelay;
	}
}

void XmacEngine::copyBaselineWeights(f32 *floatresults)
{
	// core.cpp:1070-1107, without locks
	for(int f=0;f<freqtablelength;f++)
	{
		if(config->isFrequencyUsed(configindex, f))
		{
			for(int i=0;i<numbaselines;i++)
			{
				int localfreqindex = localFreqIndex(i, f);
				if(localfreqindex >= 0)
				{
					int resultindex = config->getCoreResultBWeightOffset(configindex, f, i)*2;
					for(int b=0;b<corebinloop;b++)
					{
						for(int j=0;j<config->getBNumPolProducts(configindex, i, localfreqindex);j++)
						{
							floatresults[resultindex] += baselineweight[f][b][i][j];
							resultindex++;
						}
					}
				}
			}
			if(numphasecentres > 1)
			{
				// shift-decorr section (core.cpp:1090-1105), one f32 per phase centre
				for(int i=0;i<numbaselines;i++)
				{
					int localfreqindex = localFreqIndex(i, f);
					if(localfreqindex >= 0)
					{
						int resultindex = config->getCoreResultBShiftDecorrOffset(configindex, f, i)*2;
						for(int s=0;s<numphasecentres;s++)
						{
							floatresults[resultindex] += baselineshiftdecorr[f][i][s];
							resultindex++;
						}
					}
				}
			}
		}
	}
}
