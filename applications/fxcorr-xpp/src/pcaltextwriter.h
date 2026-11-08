#ifndef PCALTEXTWRITER_H
#define PCALTEXTWRITER_H

// fxcorr-xpp: byte-for-byte copy of fxcorr-f's pcaltextwriter.{h,cpp} (the
// writer moved to the x side in v9, fxcorr/v9-plan.md D6/D7 -- the PCAL text
// file itself is unchanged).  A fix to either copy must be applied to the
// other; the v8/v9 PCAL text comparison (v9 judgement 5) is the regression
// that catches a drift.

#include <string>
#include <vector>

#include <fxcorrcommon/configuration.h>
#include <fxcorrcommon/mode.h>
#include <fxcorrcommon/architecture.h>	// cf32

/**
 * @class PcalTextWriter
 * @brief Appends intTime-averaged phasecal tones to the experiment-level text
 *        file <pcaldir>/PCAL_<mjd>_<sec>_<station> (pcaldir = vis/<exp>.difx/,
 *        .input OUTPUT FILENAME), byte-compatible with mpifxcorr's
 *        Visibility::writeSWIN pcal section (visibility.cpp:989-1051) and
 *        header (visibility.cpp:107-136).  See algo-plan.md P0.
 *
 * One file per station, shared by that station's datastreams (upstream
 * visibility.cpp:120-122); the datastream is carried by the 4th field of each
 * data line, so jobs writing the same station never clash as long as they
 * only ever replace their own (datastream, timestamp) line.
 *
 * Per-subint tones (Mode::getPcal after finalisepcal) are accumulated in
 * subint order -- same summation order as Core::copyPCalTones
 * (core.cpp:1131-1153), so the f32 results are bit-identical to upstream.
 * Every intTime boundary the accumulated line is appended idempotently:
 * the previous line for this datastream at this timestamp is dropped while
 * every other line (other intTimes, other batches, sibling datastreams) is
 * kept, so rerunning a batch does not duplicate lines.
 */
class PcalTextWriter {
public:
	/**
	 * @param pcaldir  vis/<exp>.difx/ (must exist; created by caller)
	 */
	PcalTextWriter(const std::string &pcaldir, Configuration *config, int configindex, int dsindex);
	~PcalTextWriter();

	/** Adds one subint of finalised tones (call after FEngineWriter::writePcal). */
	void accumulate(Mode *mode);

	/**
	 * Accumulates the autocorrelation weights for the calibration scale.
	 * Call immediately before each zeroAutocorrelations() (the weight sums
	 * are cleared by it), mirroring core.cpp averageAndSendAutocorrs.
	 */
	void accumulateWeight(Mode *mode);

	/**
	 * Appends the accumulated intTime line (if any tone is nonzero, as
	 * upstream) and resets the accumulator.
	 *
	 * @param timestartsec / timestartns  start of this intTime, day-seconds
	 *        system (config start seconds + job-relative offset), i.e. the
	 *        same system as main.cpp's batchstartsec/batchstartns
	 */
	void flush(long long timestartsec, int timestartns);

	/** true if tones are accumulated but not yet flushed (batch length not an intTime multiple). */
	bool hasUnflushed() const;

private:
	Configuration *config;
	int configindex;
	int dsindex;
	int nrecordedbands;
	int maxtones;
	std::string filepath;
	std::vector<cf32> accum;	// band-major, per-band tone counts from bandntones
	std::vector<int> bandntones;	// per recorded band, actual tone count
	std::vector<f32> acwacc;	// per recorded band, intTime sum of Mode::getWeight
	std::vector<int> freqchannels;	// per recorded band, nchan/chanstoavg (visibility.cpp:656)
	int chanstoavg;		// datastream-wide (fenginewriter.cpp:29)
	int blockspersend;
};

#endif
