#ifndef MEMREADER_H
#define MEMREADER_H

#include <fxcorrcommon/architecture.h>	// cf32, f32
#include <fxcorrcommon/configuration.h>
#include <fxcorrcommon/mode.h>

#include "spreader.h"

/**
 * @class MemReader
 * @brief MemReader -- the SpReader of the v9 chain: one (datastream, band)
 *        spectrum view fed straight out of a Mode (fxcorr/v9-plan.md D9).
 *
 * The station stage of fxcorr-xpp runs Mode::process for every block of the
 * subint; after each fftloop (before the slots are reused) collectBand()
 * copies this band's spectra and per-block data weights out of the Mode's
 * buffered slots into a full-subint buffer, exactly the values
 * fxcorr-f's writeSpectraBand used to write into band_XX.sp
 * (getFreqs/getDataWeight, slot-indexed).  XmacEngine then consumes the
 * buffer through the SpReader interface with no code change of its own.
 *
 * band is the datastream-TOTAL band index (recorded + zoom), the same
 * indexing fxcorr-x used: Mode already stores a zoom band's fftoutputs as a
 * slice of its parent recorded band's array, so getFreqs(zoom, slot) is the
 * sliced view and no channel offset has to be applied here.
 */
class MemReader : public SpReader {
public:
	MemReader(Mode *mode, Configuration *config, int configindex, int dsindex, int band);
	~MemReader();

	/**
	 * Copies this fftloop's slice of the buffered slots (spectra + data
	 * weights) into the subint buffer, at the fftloop's global block
	 * positions.  Must run after the station stage processed the fftloop and
	 * before the next fftloop overwrites the slots.
	 *
	 * globalslots = true for the block-parallel form: the Mode's slots are
	 * global block indices (subloopCapacity() >= blockspersend), so slot i
	 * holds block i and fftloop only selects the range to copy.
	 */
	void collectBand(int fftloop, bool globalslots = false);

	// ---- SpReader interface ----
	int numChannels() const { return nchan; }
	const cf32 *spectra() const { return specbuf; }
	const f32 *weights() const { return wbuf; }

private:
	Mode *mode;
	int band;
	int nchan;
	int blockspersend;
	int numbufferedffts;
	cf32 *specbuf;	// blockspersend*nchan
	f32 *wbuf;	// blockspersend
};

#endif
