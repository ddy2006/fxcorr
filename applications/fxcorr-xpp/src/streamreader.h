#ifndef STREAMREADER_H
#define STREAMREADER_H

#include <cstdio>
#include <string>
#include <vector>

#include <fxcorrcommon/architecture.h>	// u8 / s32 / u32

/**
 * @class StreamReader
 * @brief Sequential reader for fxcorr-fp's stream.bin (fxcorr/v9-plan.md
 *        section 4, data-spec 5.3.1) -- one datastream of one batch.
 *
 * The file is the per-subint product of the old fxcorr-f read loop: the
 * time-slot-ordered data bytes, the valid flags and both time stamps
 * (DataReader's sec/ns and the loop's offsetsec/offsetns).  fxcorr-xpp
 * replays setValidFlags/setData/setOffsets from each record verbatim, with
 * no re-alignment or gap handling of its own.
 *
 * The header is validated against the configuration the caller passes in
 * (same cross-check fxcorr-x applied to the .sp headers).  Records are
 * fixed length, so readSubint(s) seeks and reads one -- subints may be read
 * in any order.
 */
class StreamReader {
public:
	/**
	 * @param path  fengine/<batch_id>/<g>/<station>/ds_<N>/stream.bin
	 * @param batchid / station / dsindex   expected header identity
	 * @param subintns / blockspersend / numbufferedffts / nsubints
	 *        expected geometry from the .input (header must match)
	 */
	StreamReader(const std::string &path, const std::string &batchid,
	             const std::string &station, int dsindex, int subintns,
	             int blockspersend, int numbufferedffts, int nsubints);
	~StreamReader();

	/** True if the header was read and validated. */
	bool ok() const { return ok_; }

	/** Bytes per subint data area (from the header; the record's data length). */
	int sendBytes() const { return sendbytes_; }

	/** Subints the stream actually holds (>= the .input's count expected). */
	u32 numSubints() const { return nsubints_; }

	/** s32 words of validity flags per record (ceil(blockspersend/FLAGS_PER_INT)). */
	u32 flagWords() const { return (u32)flagwords_; }

	/** Total file size the header implies (records start after the header). */
	long long expectedSize() const;

	/**
	 * Reads subint s's record into the internal buffers.
	 * @return false on I/O error / beyond the data written.
	 */
	bool readSubint(int s);

	// record fields of the last-read subint, verbatim
	u32 status() const { return status_; }		// 0 = ok, 1 = invalid subint
	u32 validBytes() const { return validbytes_; }
	int scan() const { return scan_; }
	int sec() const { return sec_; }		// reader's data seconds (INVALID_SUBINT if status 1)
	int ns() const { return ns_; }
	int offsetSec() const { return offsetsec_; }	// the offsets for Mode::setOffsets
	int offsetNs() const { return offsetns_; }
	const s32 *flags() const { return flagsbuf_.empty() ? 0 : &flagsbuf_[0]; }
	const u8 *data() const { return databuf_.empty() ? 0 : &databuf_[0]; }

private:
	std::string path_;
	FILE *file_;
	bool ok_;
	int flagwords_;
	int sendbytes_;
	u32 nsubints_;
	long long recordbytes_;

	u32 status_, validbytes_, ns_, offsetsec_, offsetns_;
	s32 sec_;
	int scan_;
	std::vector<s32> flagsbuf_;
	std::vector<u8> databuf_;
};

#endif
