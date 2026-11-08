#ifndef XMAC_H
#define XMAC_H

#include <vector>

#include <fxcorrcommon/architecture.h>
#include <fxcorrcommon/configuration.h>
#include <fxcorrcommon/model.h>
#include <fxcorrcommon/polyco.h>

#include "spreader.h"
#include "ompcompat.h"

/**
 * @class XmacEngine
 * @brief Baseline-based cross multiplication and uvshift/averaging for one
 *        subint, extracted from Core::processdata() (core.cpp:814-1052,
 *        1431-1938) with the following removals: MPI/pthread, phased array,
 *        thread splitting.  Pulsar binning (P4c, core.cpp:803-812, 914-975,
 *        1438-1475, 1626-1631, 1781-1789) and multi phase centre rotation
 *        (P4b, core.cpp:1636-1725, 1746-1815, 1877-1923) are supported.
 *
 * Data source is the SpReader interface (fxcorr-xpp: MemReader -- per-station
 * per-band spectra and weights straight out of each datastream's Mode)
 * instead of Mode objects; vis2 is conjugated on the fly (equivalent of
 * Mode::getConjugatedFreqs()).
 */
class XmacEngine {
public:
	XmacEngine(Configuration *config, int configindex, Model *model, int scan);
	~XmacEngine();

	/** Zero threadcrosscorrs, baselineweights and baselineshiftdecorrs for a new subint (core.cpp:722-759). */
	void zeroSubint();

	/**
	 * One fftloop batch of cross multiplication into threadcrosscorrs
	 * (core.cpp:867-982, both pulsar and non-pulsar branches).
	 * readers[ds][band] hold the current subint (fxcorr-xpp: filled by the
	 * station stage's MemReader::collectBand before the XMAC stage).
	 * currentpolyco must be setTime()'d to the subint start (day-of-year
	 * seconds time base, main.cpp); it is 0 without pulsar binning, which
	 * fxcorr-xpp stage 1 refuses anyway.
	 */
	void xmacBatch(int fftloop, const std::vector<std::vector<SpReader *> > &readers, Polyco *currentpolyco);

	/** Accumulate this batch's baseline weights (core.cpp:1005-1052, non-pulsar only). */
	void accumulateWeights(int fftloop, const std::vector<std::vector<SpReader *> > &readers);

	/**
	 * Average threadcrosscorrs (with pulsar bin expansion, spectral
	 * averaging and multi phase centre rotation if configured) into
	 * subintresults, then zero threadcrosscorrs (core.cpp:1431-1603, single
	 * thread path).  Scrunch folding happens first (core.cpp:1438-1475).
	 * offsetsec is the subint start in scan-relative seconds (upstream
	 * offsets[1] + offsets[2]/1e9), nsoffset the mid-XMAC-window offset in ns.
	 */
	void uvshiftAndAverage(double offsetsec, double nsoffset, double nswidth, Polyco *currentpolyco, cf32 *subintresults);

	/** Copy accumulated baseline weights (and shift decorrs) into the floatresults section (core.cpp:1070-1107, no locks). */
	void copyBaselineWeights(f32 *floatresults);

	/**
	 * Restrict every baseline loop to the flagged baselines (fxcorr-x
	 * sharding, v5-plan.md P6): a shard holds only part of the datastreams,
	 * so only the baselines they form may be cross-multiplied.  Unset
	 * (default, empty vector) = all baselines, and every loop then runs
	 * exactly as it did before sharding existed.
	 */
	void setActiveBaselines(const std::vector<char> &active);

private:
	/**
	 * getBLocalFreqIndex, but -1 for a baseline outside this shard.  All
	 * baseline loops test ">= 0" to decide whether the baseline has data at
	 * this frequency, so masking here makes them skip shard-outsiders
	 * everywhere at once - including the index advancement that keeps the
	 * parallel pass above and the accumulation pass below in step.
	 */
	int localFreqIndex(int baseline, int freq) const;

	/// 1 when baseline j takes part in this shard (all of them by default)
	bool baselineActive(int j) const
	{ return activebaselines.empty() || activebaselines[j] != 0; }

	void uvshiftAndAverageBaselineFreq(double offsetsec, double nsoffset, double nswidth, Polyco *currentpolyco, int freqindex, int baseline, cf32 *subintresults);

	Configuration *config;
	Model *model;
	int scan;
	int configindex;
	int numphasecentres;
	bool pulsarbin, scrunchoutput;
	int numpulsarbins;		// config bins, 0 when no pulsar binning
	int threadbinloop;		// pulsarbin ? numpulsarbins : 1 (core.cpp:1626-1629)
	int corebinloop;		// pulsarbin && !scrunch ? numpulsarbins : 1 (core.cpp:1630-1631)
	int freqtablelength, numbaselines, numdatastreams;
	int numBufferedFFTs, blockspersend;
	// fxcorr-x sharding: empty = no restriction (see setActiveBaselines)
	std::vector<char> activebaselines;
	int xmacstridelength;
	double blockns;			// subintNS / blocksPerSend
	long long threadresultlength;
	cf32 *threadcrosscorrs;
	f32 ****baselineweight;	// [freqtablelength][corebinloop][numbaselines][numpolproducts]
	f32 ***baselineshiftdecorr; // [freqtablelength][numbaselines][numphasecentres], only if >1 phase centres
	int nthreads;		// omp_get_max_threads() at construction (P3)
	cf32 **conjbuf;		// scratch for vis2 conjugation, [nthreads] private copies
	// pulsar scratchspace (core.cpp:407-453, 1940-2064)
	s32 ***bins;		// [numbufferedffts][freqtablelength][freqchannels], only used freqs allocated
	cf32 **pulsarscratchspace;	// cf32[max xmacstridelength], [nthreads] private copies
	cf32 *******pulsaraccumspace; // [freq][xmacstride][baseline][1 source][polproduct][bin][chan], scrunch only
	// multi phase centre scratchspace (core.cpp:416-433), [nthreads] private copies
	int maxchan, maxrotatestrideplussteplength;
	f64 **chanfreqs;
	cf32 **rotator;
	cf32 **rotated;
	f32 **argument;
	int shifterrorcount;
};

#endif
