#include "pcalwriter.h"

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <iostream>

using namespace std;

// pcal.bin layout follows fxcorr/data-spec.md 5.3; header and per-subint
// blocks are byte-compatible with fxcorr-f's FEngineWriter (the file format
// does not change with V9, only the writer does)

PcalWriter::PcalWriter(const string &outdir, Configuration *conf, int confindex,
                       int ds, int nsubs) :
	config(conf), configindex(confindex), dsindex(ds), nsubints(nsubs),
	nrecordedbands(0), file(0), finalised(false)
{
	nrecordedbands = config->getDNumRecordedBands(configindex, dsindex);

	path = outdir + "/pcal.bin";
	file = fopen(path.c_str(), "wb");
	if(file == NULL)
	{
		cerr << "PcalWriter: cannot open " << path << " for writing" << endl;
		exit(EXIT_FAILURE);
	}
	writeHeader();
}

PcalWriter::~PcalWriter()
{
	// silent: finalise() is the one that must notice a failed write
	if(file != NULL)
		fclose(file);
}

void PcalWriter::writeHeader()
{
	// magic + version + counts (FEngineWriter::writePcalHeader verbatim)
	fwrite("FXCPC\0", 1, 6, file);
	u32 version = 1;  fwrite(&version, 1, 4, file);
	u32 nsub = nsubints;  fwrite(&nsub, 1, 4, file);
	u32 ntones = config->getDMaxRecordedPCalTones(configindex, dsindex);  fwrite(&ntones, 1, 4, file);
	u32 nbands = nrecordedbands;  fwrite(&nbands, 1, 4, file);

	// per-band tone tables (tones concatenated in band order)
	for(int j=0;j<nrecordedbands;j++)
	{
		u32 bandindex = j;  fwrite(&bandindex, 1, 4, file);
		int localfreqindex = config->getDLocalRecordedFreqIndex(configindex, dsindex, j);
		int ntonesband = config->getDRecordedFreqNumPCalTones(configindex, dsindex, localfreqindex);
		u32 nt = ntonesband;  fwrite(&nt, 1, 4, file);
		char pol = config->getDRecordedBandPol(configindex, dsindex, j);
		for(int t=0;t<ntonesband;t++)
		{
			f64 tonefreq = config->getDRecordedFreqPCalToneFreqHz(configindex, dsindex, localfreqindex, t)/1.0e6;
			fwrite(&tonefreq, 1, 8, file);
			fwrite(&pol, 1, 1, file);
		}
	}
}

void PcalWriter::writeSubint(Mode *mode)
{
	// per subint: finalise the accumulated tones, then write them band by
	// band (FEngineWriter::writePcal)
	mode->finalisepcal();
	for(int j=0;j<nrecordedbands;j++)
	{
		int localfreqindex = config->getDLocalRecordedFreqIndex(configindex, dsindex, j);
		int ntonesband = config->getDRecordedFreqNumPCalTones(configindex, dsindex, localfreqindex);
		for(int t=0;t<ntonesband;t++)
		{
			cf32 tone = mode->getPcal(j, t);
			fwrite(&tone, sizeof(cf32), 1, file);
		}
	}
}

bool PcalWriter::finalise()
{
	if(finalised)
		return true;
	finalised = true;

	bool ok = true;
	if(file != NULL)
	{
		if(fflush(file) != 0 || ferror(file))
		{
			cerr << "PcalWriter: write to " << path << " failed: " << strerror(errno) << endl;
			ok = false;
		}
		if(fclose(file) != 0)
		{
			cerr << "PcalWriter: close of " << path << " failed: " << strerror(errno) << endl;
			ok = false;
		}
		file = NULL;
	}
	return ok;
}
