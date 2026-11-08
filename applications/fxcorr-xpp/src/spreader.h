#ifndef SPREADER_H
#define SPREADER_H

#include <fxcorrcommon/architecture.h>	// cf32, f32

/**
 * @class SpReader
 * @brief The spectrum-source interface XmacEngine reads from (fxcorr-xpp).
 *
 * fxcorr-x's SpReader read one band_XX.sp file per (datastream, band).  In
 * the v9 chain the spectra never reach a file: fxcorr-xpp computes them itself
 * through Mode::process and MemReader hands the engine the per-subint slots
 * straight out of the Mode (fxcorr/v9-plan.md D9).  Only the three methods
 * XmacEngine actually calls are part of the interface, so xmac.cpp is
 * unchanged from fxcorr-x.
 *
 * One instance per (datastream, band) in datastream-total band order, the
 * same indexing fxcorr-x used: recorded bands carry their own spectra and
 * zoom bands are the parent recorded band's slice (inside Mode,
 * fftoutputs[zoom] already points into the parent array).
 */
class SpReader {
public:
	virtual ~SpReader() {}

	/** Channels of this view (sliced for zoom bands). */
	virtual int numChannels() const = 0;

	/** Spectra of the current subint, linear FFT order (blockspersend*numChannels cf32). */
	virtual const cf32 *spectra() const = 0;

	/** Per-FFT-block data weights of the current subint (blockspersend f32). */
	virtual const f32 *weights() const = 0;
};

#endif
