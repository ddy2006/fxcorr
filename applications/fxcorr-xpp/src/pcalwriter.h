#ifndef PCALWRITER_H
#define PCALWRITER_H

#include <cstdio>
#include <string>

#include <fxcorrcommon/configuration.h>
#include <fxcorrcommon/mode.h>

/**
 * @class PcalWriter
 * @brief Writes one datastream's pcal.bin in fxcorr-xpp (previously written by
 *        fxcorr-f's FEngineWriter; fxcorr/v9-plan.md D6/D7).
 *
 * Same file, same position (<fengine>/<batch>/<g>/<station>/ds_<N>/pcal.bin),
 * same layout (fxcorr/data-spec.md 5.3): header, then one per-subint block of
 * per-band finalised tones.  The tone content is lifted from FEngineWriter's
 * writePcalHeader/writePcal verbatim -- the tones now live in xp's own Mode
 * instead of arriving through the .sp pipeline.
 *
 * Write-error discipline follows fxcorr-f: finalise() flushes and closes with
 * the errors checked, so a full filesystem fails the task here (v7-plan 17.2).
 */
class PcalWriter {
public:
	/**
	 * @param outdir  fengine/<batch_id>/<g>/<station>/ds_<N>/ (must exist)
	 * @param nsubints  total subints expected in the batch (header)
	 */
	PcalWriter(const std::string &outdir, Configuration *config, int configindex,
	           int dsindex, int nsubints);
	~PcalWriter();

	/** Finalises the accumulated tones of one subint and appends them, band by band. */
	void writeSubint(Mode *mode);

	/** Flushes and closes the file, verifying the writes actually landed. */
	bool finalise();

private:
	void writeHeader();

	Configuration *config;
	int configindex;
	int dsindex;
	int nsubints;
	int nrecordedbands;
	std::string path;
	FILE *file;
	bool finalised;
};

#endif
