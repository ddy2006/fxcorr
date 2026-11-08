#ifndef INTEGRATE_H
#define INTEGRATE_H

#include <string>
#include <vector>

#include <fxcorrcommon/architecture.h>
#include <fxcorrcommon/configuration.h>
#include <fxcorrcommon/visibility.h>

class DifxMonitor;

/**
 * One autocorrelation averaging batch as fxcorr-xpp's station stage produced
 * it -- the in-memory equivalent of one autocorr.bin record (fxcorr/v9-plan.md
 * R4; the file itself is gone in the v9 chain, D7).
 *
 * ac/acx hold the per-band autocorrelations concatenated in datastream-total
 * band order (recorded + zoom, zoom already in its sliced form inside Mode);
 * w/wx carry one weight per total band, zoom bands already mapped to their
 * parent recorded band's weight -- exactly the values fxcorr-f's
 * writeAutocorrelationBatch used to write into autocorr.bin.  acx/wx stay
 * empty unless the cross-pol section is configured (WRITE AUTOCORRS &&
 * maxproducts > 2).
 */
struct AutocorrBatch
{
	std::vector<cf32> ac;	// parallel section, total-band order
	std::vector<f32> w;	// one weight per total band
	std::vector<cf32> acx;	// cross-pol section (empty when off)
	std::vector<f32> wx;
};

/**
 * @class Integrator
 * @brief Single-Visibility long-term integration for fxcorr-x: the serial
 *        replacement of FxManager's visbuffer ring + write thread
 *        (fxmanager.cpp:168-185, 650-698).
 *
 * Subint results are accumulated with Visibility::addData(); when an
 * integration period completes, writedata() emits the SWIN record and
 * increment() moves the Visibility to the next period.  Autocorrelations
 * arrive in memory from fxcorr-xpp's own station stage (v9: autocorr.bin no
 * longer exists, fxcorr/v9-plan.md D7).
 *
 * When a monitor is attached, each completed integration also emits a
 * DIFX_STATE_RUNNING DifxMessage (algo-plan.md P1, upstream
 * Visibility::multicastweights visibility.cpp:1100-1146).
 */
class Integrator {
public:
	/**
	 * @param configindex configuration index of the batch's scan
	 * @param eseconds total correlation length of this batch, seconds
	 * @param scan     0 in V1 (single scan)
	 * @param startsec seconds of the batch start relative to the scan start
	 * @param startns  nanoseconds remainder of the batch start
	 * @param monitor  optional DifxMessage emitter for RUNNING status
	 *
	 * There is no output-directory parameter: SWIN writes go to the .input
	 * OUTPUT FILENAME directory (created if missing), which Configuration
	 * resolves against FXCORR_VIS_ROOT (v5-plan.md Q10 - batch.json's
	 * difx_dir was never read by anything).
	 */
	Integrator(Configuration *config, int configindex, int eseconds,
		int scan, int startsec, int startns, DifxMonitor *monitor = 0);
	~Integrator();

	Visibility *visibility() const { return vis_; }

	/**
	 * Accumulates one subint into the Visibility; when the integration period
	 * completes, writes the SWIN record and advances.
	 * @return true if an integration was completed (and written)
	 */
	bool addSubint(cf32 *subintresults);

	/**
	 * Accumulates one subint of autocorrelations into the autocorr / acweight
	 * sections of subintresults (core.cpp:1260-1370).  v9: the records arrive
	 * in memory, produced by the station stage out of its own Modes, instead
	 * of being read from autocorr.bin; the numerical path -- accumulation
	 * order, weight sums, used-frequency gate, cross-pol continuation -- is
	 * unchanged from the fxcorr-x file-reading form (see integrate.cpp for
	 * the locked-in detail comments).
	 * @param records one record list per datastream; a datastream outside the
	 *        shard has an empty list and is skipped silently
	 */
	void addAutocorrs(const std::vector<std::vector<AutocorrBatch> > &records,
	                  cf32 *subintresults);

private:
	// visibility.cpp multicastweights 1100-1146: per-station band-averaged
	// autocorr weights + integration-centre MJD, sent as RUNNING
	void sendRunning();

	Configuration *config;
	int configindex;
	Visibility *vis_;
	char *todiskbuffer_;
	DifxMonitor *monitor_;
};

#endif
