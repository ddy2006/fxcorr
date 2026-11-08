#include "streamreader.h"

#include <cstring>
#include <iostream>

#include <fxcorrcommon/mpifxcorr.h>	// FLAGS_PER_INT

using namespace std;

// stream.bin layout (fxcorr/v9-plan.md section 4; written by fxcorr-fp's
// StreamWriter).  The static asserts pin the on-disk sizes against padding
// changes -- header field order must match streamwriter.cpp exactly.
struct StreamHeader {
	char magic[8];		// "FXCORR9S"
	u32 version;		// 1
	char batchid[32];
	char station[16];
	u32 dsindex;
	u32 subintns;
	u32 blockspersend;
	u32 numbufferedffts;
	u32 nsubints;
	u32 sendbytes;
	u32 framebytes;
	u32 framespersec;
};
static_assert(sizeof(StreamHeader) == 92, "stream.bin header must stay 92 bytes");

struct StreamRecordHead {
	u32 status;		// 0 = ok, 1 = invalid subint (validbytes == 0)
	u32 validbytes;
	u32 scan;
	s32 sec;
	u32 ns;
	u32 offsetsec;
	u32 offsetns;
};
static_assert(sizeof(StreamRecordHead) == 28, "stream.bin record head must stay 28 bytes");

// fixed-length strings in the header must compare as C strings; a full-length
// name without a terminator still compares equal, so a 32-char batch id works
static bool strfieldMatches(const char *field, size_t maxlen, const string &value)
{
	return strncmp(field, value.c_str(), maxlen) == 0;
}

StreamReader::StreamReader(const string &path, const string &batchid,
                           const string &station, int dsindex, int subintns,
                           int blockspersend, int numbufferedffts, int nsubints) :
	path_(path), file_(0), ok_(false), flagwords_(0), sendbytes_(0),
	nsubints_(0), recordbytes_(0), status_(0), validbytes_(0), ns_(0),
	offsetsec_(0), offsetns_(0), sec_(0), scan_(0)
{
	file_ = fopen(path.c_str(), "rb");
	if(file_ == 0)
	{
		cerr << "StreamReader: cannot open " << path << endl;
		return;
	}

	StreamHeader h;
	if(fread(&h, 1, sizeof(h), file_) != sizeof(h))
	{
		cerr << "StreamReader: short header in " << path << endl;
		return;
	}
	if(memcmp(h.magic, "FXCORR9S", 8) != 0 || h.version != 1)
	{
		cerr << "StreamReader: bad magic or version in " << path << endl;
		return;
	}

	// the header identity and geometry must be exactly what this run expects
	// (same idea as fxcorr-x's .sp header cross-check)
	if(!strfieldMatches(h.batchid, sizeof(h.batchid), batchid) ||
	   !strfieldMatches(h.station, sizeof(h.station), station) ||
	   (int)h.dsindex != dsindex ||
	   (int)h.subintns != subintns ||
	   (int)h.blockspersend != blockspersend ||
	   (int)h.numbufferedffts != numbufferedffts ||
	   (int)h.nsubints != nsubints)
	{
		cerr << "StreamReader: " << path << " header does not match the config"
		     << " (batch " << h.batchid << " vs " << batchid
		     << ", station " << h.station << " vs " << station
		     << ", ds " << h.dsindex << " vs " << dsindex
		     << ", subintns " << h.subintns << " vs " << subintns
		     << ", bps " << h.blockspersend << " vs " << blockspersend
		     << ", nbf " << h.numbufferedffts << " vs " << numbufferedffts
		     << ", nsub " << h.nsubints << " vs " << nsubints << ")" << endl;
		return;
	}

	flagwords_ = (blockspersend + FLAGS_PER_INT - 1)/FLAGS_PER_INT;
	sendbytes_ = (int)h.sendbytes;
	nsubints_ = h.nsubints;
	recordbytes_ = (long long)sizeof(StreamRecordHead) + 4LL*flagwords_ + sendbytes_;

	flagsbuf_.resize(flagwords_);
	databuf_.resize(sendbytes_);
	ok_ = true;
}

StreamReader::~StreamReader()
{
	if(file_ != 0)
		fclose(file_);
}

long long StreamReader::expectedSize() const
{
	return (long long)sizeof(StreamHeader) + (long long)nsubints_*recordbytes_;
}

bool StreamReader::readSubint(int s)
{
	if(!ok_ || s < 0 || (u32)s >= nsubints_)
		return false;

	if(fseeko(file_, (long long)sizeof(StreamHeader) + (long long)s*recordbytes_,
	          SEEK_SET) != 0)
		return false;

	StreamRecordHead rh;
	if(fread(&rh, 1, sizeof(rh), file_) != sizeof(rh))
		return false;
	if(fread(&flagsbuf_[0], 4, flagwords_, file_) != (size_t)flagwords_)
		return false;
	if(fread(&databuf_[0], 1, sendbytes_, file_) != (size_t)sendbytes_)
		return false;

	status_ = rh.status;
	validbytes_ = rh.validbytes;
	scan_ = (int)rh.scan;
	sec_ = rh.sec;
	ns_ = rh.ns;
	offsetsec_ = rh.offsetsec;
	offsetns_ = rh.offsetns;
	return true;
}
