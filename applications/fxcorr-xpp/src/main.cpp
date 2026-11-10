// fxcorr-xpp -- the v9 chain's core side (fxcorr/v9-plan.md): fxcorr-x grown
// with the station stage.  It reads fxcorr-fp's stream.bin per datastream
// (1x raw slot data), replays the old fxcorr-f setData/setOffsets sequence
// into one Mode per datastream, and runs the same Mode::process then XMAC
// pipeline the two v8 tools ran separately -- so the SWIN it produces is
// byte-identical to the v8 chain's (v9 judgements 3/4).  The merge
// subcommands are unchanged.  Grown from fxcorr-x; see CLAUDE.md for the
// file-by-file origin map.

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <dirent.h>
#include <unistd.h>	// getpid (timing event lines)
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <utility>
#include <vector>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <functional>
#include <chrono>

#include <fxcorrcommon/configuration.h>
#include <fxcorrcommon/mode.h>
#include <fxcorrcommon/model.h>
#include <fxcorrcommon/architecture.h>
#include <fxcorrcommon/difxmonitor.h>
#include <fxcorrcommon/fxcorrpath.h>

#include "spreader.h"
#include "memreader.h"
#include "streamreader.h"
#include "acbatch.h"
#include "pcalwriter.h"
#include "pcaltextwriter.h"
#include "xmac.h"
#include "integrate.h"
#include "log.h"
#include "ompcompat.h"

using namespace std;

// ---- stage timing (FXCORR_TIMING; measurement only, no behaviour change) ----
// Same instrument as fxcorr-f (see its main.cpp for the full comment):
// FXCORR_TIMING=1 aggregates one TIMING line at exit, FXCORR_TIMING_FILE=<path>
// sends the output there instead of stderr, FXCORR_TIMING_TRACE=1 adds one
// TSV event line per completed stage
//   <epoch sec>\t<prog>\t<pid>\t<thread>\t<file:line>\t<label>\t<sec>
// Setting any of the three enables the timer; every nowsec() call is skipped
// when it is off.
static bool g_timing = false;
static bool g_timing_trace = false;
static bool g_timing_brief = false;
static FILE *g_timing_out = 0;
static double g_tt_station, g_tt_read, g_tt_zero, g_tt_xmac, g_tt_wacc,
              g_tt_uvshift, g_tt_cbw, g_tt_auto, g_tt_addsub, g_tt_start;

static inline double nowsec()
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (double)ts.tv_sec + (double)ts.tv_nsec*1e-9;
}

// wall clock for the event timestamps: they must be comparable across
// processes, so epoch seconds, not the monotonic clock the durations use
static inline double epochsec()
{
	struct timespec ts;
	clock_gettime(CLOCK_REALTIME, &ts);
	return (double)ts.tv_sec + (double)ts.tv_nsec*1e-9;
}

// ---------------------------------------------------------------- timing tiers
// Three tiers share the STAGE_LAP call sites; only the output path differs
// (tier comments at the initialisation below):
//   aggregate -- one summary line at exit, unchanged
//   brief     -- one TSV row per probe at exit (the table below drives it)
//   trace     -- every lap buffered in memory, written once at exit
struct TimingEvent
{
	double ts;		// epoch seconds at the lap
	double dur;		// seconds since the timer's previous lap
	int tid;
	int line;
	const char *file;	// basename of the lap site
	const char *label;
};
static std::vector<TimingEvent> g_trace_buf;

struct TimingProbe
{
	const char *label;
	const char *file;
	int line;
	double *total;		// the matching g_tt_* accumulator
	double first_ts;	// epoch seconds of the first lap (0 = not seen)
};
// order follows the pipeline (station -> read -> zero -> xmac -> ... -> addsub)
// (line numbers are filled in from the STAGE_LAP sites below; fxcorr-xpp's
// station lap covers the whole per-ds station stage the read lap used to
// feed, and read now only reads the stream.bin records)
static TimingProbe g_probes[] = {
	{"station", "main.cpp", 1866, &g_tt_station, 0},
	{"read",    "main.cpp", 1683, &g_tt_read,    0},
	{"zero",    "main.cpp", 1874, &g_tt_zero,    0},
	{"xmac",    "main.cpp", 1887, &g_tt_xmac,    0},
	{"wacc",    "main.cpp", 1889, &g_tt_wacc,    0},
	{"uvshift", "main.cpp", 1897, &g_tt_uvshift, 0},
	{"cbw",     "main.cpp", 1914, &g_tt_cbw,     0},
	{"auto",    "main.cpp", 1922, &g_tt_auto,    0},
	{"addsub",  "main.cpp", 1929, &g_tt_addsub,  0},
	{0, 0, 0, 0, 0}
};

static void timing_event(const char *label, const char *file, int line,
                         double dur, double ts)
{
	const char *base = strrchr(file, '/');
	base = (base != 0) ? base+1 : file;
	int thread = omp_get_thread_num();
	#pragma omp critical(fxcorr_timing)
	{
		if(g_timing_trace)
		{
			TimingEvent ev;
			ev.ts = ts;
			ev.dur = dur;
			ev.tid = thread;
			ev.line = line;
			ev.file = base;
			ev.label = label;
			g_trace_buf.push_back(ev);
		}
		else if(g_timing_brief)
		{
			for(TimingProbe *p = g_probes; p->label != 0; p++)
				if(strcmp(p->label, label) == 0)
				{
					if(p->first_ts == 0.0)
						p->first_ts = ts;
					break;
				}
		}
	}
}

// write the tier's product: brief = one row per probe (first timestamp +
// accumulated total), trace = the whole buffer.  Called on the normal exit
// path and from fail(), so an aborted task still leaves its events behind.
static void timing_finish(void)
{
	static bool done = false;
	if(done || !g_timing)
		return;
	done = true;
	if(g_timing_trace)
	{
		for(size_t i=0;i<g_trace_buf.size();i++)
		{
			const TimingEvent &e = g_trace_buf[i];
			fprintf(g_timing_out, "%.6f\tfxcorr-xpp\t%d\t%d\t%s:%d\t%s\t%.6f\n",
			        e.ts, (int)getpid(), e.tid, e.file, e.line, e.label, e.dur);
		}
		g_trace_buf.clear();
	}
	else if(g_timing_brief)
	{
		for(TimingProbe *p = g_probes; p->label != 0; p++)
		{
			if(p->first_ts == 0.0)
				continue;	// probe never reached (e.g. aborted early)
			fprintf(g_timing_out, "%.6f\tfxcorr-xpp\t%d\t%d\t%s:%d\t%s\t%.6f\n",
			        p->first_ts, (int)getpid(), 0, p->file, p->line, p->label,
			        *p->total);
		}
	}
	fflush(g_timing_out);
	if(g_timing_out != stderr)
	{
		fclose(g_timing_out);
		g_timing_out = stderr;	// harmless default if anything runs on
	}
}

// STAGE_LAP(timer, label): the one lap form -- returns the seconds since the
// timer's construction (or previous lap); with tracing on it also writes one
// event line, and label + __FILE__:__LINE__ name the step in both outputs
#define STAGE_LAP(timer, label) (timer).lap(label, __FILE__, __LINE__)

struct StageTimer
{
	double t0;
	StageTimer() : t0(g_timing ? nowsec() : 0.0) {}
	double lap(const char *label, const char *file, int line)
	{
		if(!g_timing)
			return 0.0;
		double t = nowsec();
		double d = t - t0;
		t0 = t;
		if(g_timing_trace || g_timing_brief)
			timing_event(label, file, line, d, epochsec());
		return d;
	}
};

// experiment name for DifxMessage identification, same as mpifxcorr's
// generateIdentifier (mpifxcorr.cpp:217-239): .input basename without the
// ".input" suffix
static string jobIdentifier(const string &inputfile)
{
	size_t slash = inputfile.find_last_of('/');
	string base = (slash == string::npos) ? inputfile : inputfile.substr(slash+1);
	size_t dot = base.find(".input");
	return (dot == string::npos) ? base : base.substr(0, dot);
}

// unified error exit: report to stderr, then send Alert + Aborting status
// (algo-plan.md P1, upstream alert.cpp:54 / fxmanager.cpp ABORTING)
static int fail(DifxMonitor &monitor, const string &msg)
{
	cerr << msg << endl;
	timing_finish();	// brief rows / trace buffer survive an aborted task
	monitor.alert(msg, DIFX_ALERT_LEVEL_ERROR);
	monitor.status(DIFX_STATE_ABORTING, msg, 0.0, 0, 0, 0.0, 0.0);
	return EXIT_FAILURE;
}

// BINARY_STA records, one per recorded band, per autocorrelation batch
// (algo-plan.md P1; core.cpp averageAndSendAutocorrs 1195-1253, V1
// single-thread form).  nsoffsetns/nswidthns describe the current ac batch
// (centre offset and width in ns).
// P9: datastreamsaveraged means the caller already ran averageFrequency()
// for this batch (core.cpp:1181-1187); the renormalisation and the channel
// count then refer to the averaged spectrum (core.cpp:1249-1254).
// fxcorr-xpp: lifted verbatim from fxcorr-f's main.cpp.  The station stage
// calls it from the ds-parallel region, so the call sites hold an
// omp critical -- fxcorr-f only ever called it from thread 0.
static void sendSTA(DifxMonitor *monitor, Configuration &config, int configindex, int dsindex,
                    Mode *mode, int scan, long long scanstartsec, int subintsec, int subintns,
                    double nsoffsetns, double nswidthns, bool datastreamsaveraged, const string &jobname)
{
	// core.cpp:1191-1192: no STA at all when one record does not fit the MTU
	int recordsize = sizeof(DifxMessageSTARecord) + sizeof(f32)*config.getSTADumpChannels();
	if(recordsize > config.getMTU())
		return;

	int nrecordedbands = config.getDNumRecordedBands(configindex, dsindex);
	for(int band=0;band<nrecordedbands;band++)
	{
		int freqindex = config.getDRecordedFreqIndex(configindex, dsindex, band);
		int freqchannels = config.getFNumChannels(freqindex);
		f32 weight = mode->getWeight(false, band);

		// minimum weight gate, core.cpp:1218-1220 (dodgy packet protection)
		double stasamples = 0.001*nswidthns*2*config.getFreqTableBandwidth(freqindex);
		if(weight < 0.333*stasamples/(2*freqchannels))
			continue;

		// core.cpp:1224: energy -> power normalisation
		f32 renormvalue = 1.0f/(2*freqchannels*weight);
		// core.cpp:1227-1232: on an averaged spectrum channels are summed
		// below, not averaged, so correct both factors
		if(datastreamsaveraged)
		{
			renormvalue /= config.getFChannelsToAverage(freqindex);
			freqchannels /= config.getFChannelsToAverage(freqindex);
		}

		// core.cpp:1234-1237: fold to the requested STA dump resolution
		int nchan = config.getSTADumpChannels();
		if(freqchannels < nchan)
			nchan = freqchannels;
		int chans_to_avg = freqchannels/nchan;

		cf32 *acdata = mode->getAutocorrelation(false, band);
		DifxMessageSTARecord *record = (DifxMessageSTARecord *)malloc(recordsize);
		memset(record, 0, recordsize);

		record->messageType = STA_AUTOCORRELATION;
		record->dsindex = dsindex;
		record->coreindex = 0;
		record->threadindex = 0;
		snprintf(record->identifier, DIFX_MESSAGE_PARAM_LENGTH, "%s",
		         jobname.substr(0, DIFX_MESSAGE_PARAM_LENGTH-1).c_str());
		record->nChan = nchan;
		record->scan = scan;
		// core.cpp:1208-1214: sec is day-of-observation seconds (the code,
		// not the "since scan start" comment, is authoritative)
		record->sec = (int)scanstartsec + subintsec;
		record->ns = subintns + (int)nsoffsetns;
		if(record->ns >= 1000000000)
		{
			record->ns -= 1000000000;
			record->sec++;
		}
		record->nswidth = (int)nswidthns;
		record->bandindex = band;
		// sum (not average) of chans_to_avg adjacent real parts, core.cpp:1244-1247.
		// Upstream indexes acdata as f32* with stride 2 (= cf32 stride 1); the
		// P1 port kept the factor 2 with a cf32* pointer and folded every
		// second chans_to_avg block instead of adjacent channels (fixed P9,
		// caught by the averaged-branch STA bitwise comparison).
		for(int k=0;k<nchan;k++)
		{
			record->data[k] = acdata[k*chans_to_avg].re;
			for(int l=1;l<chans_to_avg;l++)
				record->data[k] += acdata[k*chans_to_avg+l].re;
		}
		// vectorMulC_f32_I(renormvalue, ...): energy -> power
		for(int k=0;k<nchan;k++)
			record->data[k] *= renormvalue;

		monitor->staSend(record, sizeof(DifxMessageSTARecord) + sizeof(f32)*nchan);
		free(record);
	}
}

// BINARY_STA records with messageType STA_KURTOSIS, one per recorded band,
// per subint (algo-plan.md P9; core.cpp averageAndSendKurtosis 1378-1434,
// V1 single-thread form).  The mode has accumulated s1/s2 in process(); the
// excess-kurtosis statistic is folded and copied out by
// calculateAndAverageKurtosis.  nsoffsetns/nswidthns describe the whole
// subint (centre offset and width); unlike the autocorrelation STA there is
// no weight gate and no renormalisation (core.cpp:1420-1431).
static void sendKurtosis(DifxMonitor *monitor, Configuration &config, int configindex, int dsindex,
                         Mode *mode, int scan, long long scanstartsec, int subintsec, int subintns,
                         double nsoffsetns, double nswidthns, int numblocks, const string &jobname)
{
	bool valid = mode->calculateAndAverageKurtosis(numblocks, config.getSTADumpChannels());
	if(!valid)
		return;

	int nrecordedbands = config.getDNumRecordedBands(configindex, dsindex);
	for(int band=0;band<nrecordedbands;band++)
	{
		int freqindex = config.getDRecordedFreqIndex(configindex, dsindex, band);
		int freqchannels = config.getFNumChannels(freqindex);
		int nchan = config.getSTADumpChannels();
		if(freqchannels < nchan)
			nchan = freqchannels;

		int recordsize = sizeof(DifxMessageSTARecord) + sizeof(f32)*nchan;
		DifxMessageSTARecord *record = (DifxMessageSTARecord *)malloc(recordsize);
		memset(record, 0, recordsize);

		record->messageType = STA_KURTOSIS;
		record->dsindex = dsindex;
		record->coreindex = 0;
		record->threadindex = 0;
		snprintf(record->identifier, DIFX_MESSAGE_PARAM_LENGTH, "%s",
		         jobname.substr(0, DIFX_MESSAGE_PARAM_LENGTH-1).c_str());
		record->nChan = nchan;
		record->scan = scan;
		record->sec = (int)scanstartsec + subintsec;
		record->ns = subintns + (int)nsoffsetns;
		if(record->ns >= 1000000000)
		{
			record->ns -= 1000000000;
			record->sec++;
		}
		record->nswidth = (int)nswidthns;
		record->bandindex = band;
		f32 *sk = mode->getKurtosis(band);
		for(int k=0;k<nchan;k++)
			record->data[k] = sk[k];

		monitor->staSend(record, recordsize);
		free(record);
	}
}

// Valid FFT blocks of one subint's flags: the stream record carries one
// validity bit per block, filled by fxcorr-fp from the bytes actually read
// out of the raw file (DataReader::fillValidFlags, "valid flags, one bit per
// FFT block" -- the same flags fxcorr-f wrote into the .sp).  fxcorr-x used
// to ignore the .sp flags entirely and let the weight gate in
// Visibility::writedata decide - which makes "the raw data was never there"
// indistinguishable from "it was there but every weight came out zero", and
// both end as an SWIN with no records and no error (v6-plan.md "进行中的
// 发现" 2).  The batch-level check after the subint loop needs the real
// thing.  Bit scan kept identical to fxcorr-x's countValidBlocks.
static long long countValidBits(const s32 *flags, u32 bps, u32 words)
{
	long long n = 0;

	for(u32 i=0;i<words;i++)
	{
		const u32 base = i*32;
		if(base >= bps)
			break;
		u32 w = (u32)flags[i];
		if(bps - base < 32)	// trailing bits of the last word are not blocks
			w &= (1u << (bps - base)) - 1u;
		while(w)		// Kernighan: one iteration per set bit
		{
			w &= w - 1u;
			n++;
		}
	}
	return n;
}

// Minimal batch.json field extraction (fixed V1 format, data-spec 5.3 D9).
static bool extractJsonDouble(const string &json, const string &key, double *value)
{
	size_t pos = json.find("\"" + key + "\"");
	if(pos == string::npos)
		return false;
	pos = json.find(':', pos);
	if(pos == string::npos)
		return false;
	char *end;
	*value = strtod(json.c_str() + pos + 1, &end);
	return (end != json.c_str() + pos + 1);
}

static bool extractJsonInt(const string &json, const string &key, int *value)
{
	double d;
	if(!extractJsonDouble(json, key, &d))
		return false;
	*value = (int)(d + 0.5);
	return true;
}

static bool extractJsonString(const string &json, const string &key, string *value)
{
	size_t pos = json.find("\"" + key + "\"");
	if(pos == string::npos)
		return false;
	pos = json.find(':', pos);
	if(pos == string::npos)
		return false;
	size_t start = json.find('"', pos);
	size_t end = (start == string::npos) ? string::npos : json.find('"', start + 1);
	if(start == string::npos || end == string::npos)
		return false;
	*value = json.substr(start + 1, end - start - 1);
	return true;
}

// ---------------------------------------------------------------------------
// 分片模式（v5-plan.md P6 / data-spec 5.9）

// ds 组推导（deriveDsGroups / groupOfDs）已移至 fxcorrcommon/configuration.{h,cpp}——
// fxcorr-f（fengine 按组落盘）与 fxcorr-x（分片）共用同一实现（data-spec 第 8 节）。

// 一条记录在**整块缓冲区**中的位置与排序键。记录本身不再逐条分配：一个
// .part 一次读进 PartBuf::data，PartRecord 只记偏移与长度（2026-11-06——
// 实验级 merge 每批约 3400 条记录、共数百万条，逐条 string 分配是主要开销）。
// 归并 key 用**整数纳秒**（由 mjd 与秒偏移推算），不用浮点秒——浮点相等比较
// 不可靠（data-spec 5.9）。
struct PartRecord
{
	size_t off, len;	// 在 PartBuf::data 中的字节位置
	long long key;
	int baseline, freqindex, pulsarbin;
	int autocorr;		// 自相关记录（写盘时排在同周期的基线记录之后）
};

// 一个（或首尾拼接的若干个）.part 的整块内容与逐条索引。data 是文件原样
// 字节，recs 按读入顺序排列。
struct PartBuf
{
	vector<char> data;
	vector<PartRecord> recs;

	void clear()
	{
		data.clear();		// 只清长度、保留容量：实验级 merge 逐批复用
		recs.clear();
	}
};

// 排序键刻意**不含极化对**，并用 stable_sort：写盘顺序是
// baseline -> freq -> 相位中心 -> bin -> polproduct k，而记录头里只有极化对、
// 没有 k（RR/LL/RL/LR 的字典序不等于 k 的顺序）。stable_sort 保持同一
// (周期, baseline, freq, bin) 内的原有相对顺序，于是极化顺序天然正确；不同
// baseline 的记录不会撞这个键（它们来自不同的分片），跨分片归并仍然有序。
//
// autocorr 排在基线之后：writeSWIN 先写全部基线、再写自相关段，而自相关的
// baselinenumber 是 257*(天线序+1)，与基线的编号不在同一个序列上。用
// "非零且能被 257 整除"识别它是个启发式——万一某条基线的编号恰好撞上，
// 只会改变这一周期内的记录顺序（集合与时间单调性不受影响，difx2fits 也不
// 关心周期内顺序）。
static bool partRecordLess(const PartRecord &a, const PartRecord &b)
{
	if(a.key != b.key) return a.key < b.key;
	if(a.autocorr != b.autocorr) return a.autocorr < b.autocorr;
	if(a.baseline != b.baseline) return a.baseline < b.baseline;
	if(a.freqindex != b.freqindex) return a.freqindex < b.freqindex;
	return a.pulsarbin < b.pulsarbin;
}

// 把一个 .part **整块**读入 PartBuf（追加到已有内容之后），扫描校验 sync word
// 与记录长度，并把每条记录的位置与排序键记进 recs。任何不一致都报错——分片
// 文件写坏时宁可停下来，也不要写出一份看着正常、实则残缺的 SWIN。
//
// 2026-11-06：原实现每条记录做两次 string 分配（assign + resize），实验级
// merge 数百万条记录就是千万次堆分配；整块读 + 偏移索引把热路径压成一次
// read 加一遍扫描。
static bool loadPart(const string &path, Configuration &config, PartBuf *buf,
                     std::ostringstream *err)
{
	ifstream in(path.c_str(), ios::binary);
	if(!in.is_open())
	{
		*err << "cannot open " << path;
		return false;
	}
	in.seekg(0, ios::end);
	std::streamoff n = in.tellg();
	if(n < 0)
	{
		*err << "cannot determine the size of " << path;
		return false;
	}
	in.seekg(0);
	const size_t off0 = buf->data.size();
	buf->data.resize(off0 + (size_t)n);
	if(n > 0)
	{
		in.read(&buf->data[off0], (std::streamsize)n);
		if(in.gcount() != (std::streamsize)n)
		{
			*err << path << ": read failed (" << (std::streamoff)in.gcount() << " of "
			     << n << " byte(s))";
			return false;
		}
	}
	const size_t end = buf->data.size();
	size_t off = off0;
	long long nrec = 0;
	while(off < end)
	{
		if(end - off < 74)
		{
			*err << path << ": truncated record header after " << nrec << " record(s)";
			return false;
		}
		const char *head = &buf->data[off];
		unsigned int sync;
		memcpy(&sync, head, 4);
		if(sync != Visibility::SYNC_WORD)
		{
			*err << path << ": bad sync word 0x" << hex << sync << dec << " at record " << nrec;
			return false;
		}
		PartRecord rec;
		int mjd, freqindex, baselinenum;
		double seconds;
		memcpy(&baselinenum, head + 8, 4);
		memcpy(&mjd, head + 12, 4);
		memcpy(&seconds, head + 16, 8);
		memcpy(&freqindex, head + 32, 4);
		memcpy(&rec.pulsarbin, head + 38, 4);
		rec.baseline = baselinenum;
		rec.freqindex = freqindex;
		rec.autocorr = (baselinenum != 0 && baselinenum % 257 == 0) ? 1 : 0;
		rec.key = (long long)mjd * 86400LL * 1000000000LL +
		          (long long)floor(seconds * 1.0e9 + 0.5);
		// 每条记录的数据长度由它的 freqindex 决定：SWIN 头里没有 length 字段
		// （记录是定长头 + nchan 个 cf32），所以要读 .input 才知道边界
		int nchan = config.getFNumChannels(freqindex);
		int chansavg = config.getFChannelsToAverage(freqindex);
		if(nchan <= 0 || chansavg <= 0 || nchan % chansavg != 0)
		{
			*err << path << ": freqindex " << freqindex << " has a bad channel count ("
			     << nchan << "/" << chansavg << ")";
			return false;
		}
		size_t nbytes = (size_t)(nchan / chansavg) * sizeof(cf32);
		if(end - off - 74 < nbytes)
		{
			*err << path << ": truncated data in record " << nrec << " (wanted " << nbytes
			     << " bytes, got " << (end - off - 74) << ")";
			return false;
		}
		rec.off = off;
		rec.len = 74 + nbytes;
		buf->recs.push_back(rec);
		off += rec.len;
		nrec++;
	}
	return true;
}

// recs 是否已按 partRecordLess 有序。batch 级 merge 用同一比较器排好序才写出
// merged.part，实验级可以直接按原字节搬运；这里 O(n) 验证一次，万一输入被
// 外部改动就退回排序，输出与旧版逐字节一致。
static bool recsAreOrdered(const vector<PartRecord> &recs)
{
	for(size_t i = 1; i < recs.size(); i++)
		if(partRecordLess(recs[i], recs[i - 1]))
			return false;
	return true;
}

// 本 batch 的时间范围在目标 SWIN 里是否已有记录。分片与全量两种模式对同一
// batch **互斥**（data-spec 5.9）：分片任务不写 SWIN 而 merge 会追加，混跑会
// 写出重复记录并破坏 SWIN 的时间单调——difx2fits 顺序读记录、时间回退的被静默
// 丢弃（fitsUV.c:1227 的 RecordIsOld、fitsUV.c:1868 只打一行计数）。这里把那条
// 文档约束落成程序内检查，与 mpifxcorr 拒绝覆盖已有 SWIN 的行为一致。
//
// 只读 74 字节记录头，数据用 seekg 跳过：对一个 GB 级的 SWIN 也是秒级。
// 记录长度由 freqindex 查 .input 得到（头里没有 length 字段），与 loadPart 同。
static bool swinHasBatchRange(const string &path, Configuration &config,
                              double batchstartmjd, double durationsec,
                              long long *nrecords)
{
	*nrecords = 0;
	ifstream in(path.c_str(), ios::binary);
	if(!in.is_open())
		return false;			// 还没有这个文件 = 没写过
	const double lo = batchstartmjd * 86400.0;
	const double hi = lo + durationsec;
	while(true)
	{
		char head[74];
		in.read(head, sizeof(head));
		if(in.gcount() != (std::streamsize)sizeof(head))
			break;
		unsigned int sync;
		int mjd, freqindex;
		double sec;
		memcpy(&sync, head, 4);
		if(sync != Visibility::SYNC_WORD)
			break;			// 不是记录流，不猜
		memcpy(&mjd, head + 12, 4);
		memcpy(&sec, head + 16, 8);
		memcpy(&freqindex, head + 32, 4);
		double t = (double)mjd * 86400.0 + sec;
		if(t >= lo - 1e-6 && t < hi + 1e-6)
			(*nrecords)++;
		int nchan = config.getFNumChannels(freqindex);
		int chansavg = config.getFChannelsToAverage(freqindex);
		if(nchan <= 0 || chansavg <= 0)
			break;
		in.seekg((std::streamoff)((size_t)(nchan / chansavg) * sizeof(cf32)), ios::cur);
	}
	return *nrecords > 0;
}

// ---------------------------------------------------------------------------
// 两级 merge（v6-plan.md S4.1 / data-spec 5.9 末条）
//
// 形态 A：SWIN 的写入收敛到**实验级一次**。batch 级 merge 只写
// vis-parts/<batch_id>/merged.part（batch 内归并，不碰 SWIN），实验级
// `merge --experiment` 才写 SWIN、且是它唯一的写入者。
//
// 理由是 difx2fits 的读取语义：顺序读记录、按天线检查时间单调（fitsUV.c:1227 的
// RecordIsOld），时间回退的记录**静默丢弃**（fitsUV.c:1868 只打一行计数）。单节点
// 串行 batch 时"每 batch 完成即 merge"天然按时间序；数百节点并行处理不同 batch 时
// 不然——B 先完成先写、A 后写就是时间回退，全程无报错。

// 实验级 merge 认的 batch 清单来自 batches/<batch_id>.json——它是**切批规划**的产物
// （data-spec 第 6 节：由单点分配、单调递增），也就是"这个实验规划了哪些 batch"的
// 权威清单。只取用得到的字段。
struct BatchEntry
{
	string id;
	string configfile;	// .input 路径（相对 workdir）——认定"本实验"的依据
	string status;		// running/done/failed——**不参与判据**，只进报错信息
	double startmjd;
	int nsubints;
	bool hastime;		// 两个时间字段齐备时才参与 SWIN 互斥检查
};

static string mergedPartPath(const string &workdir, const string &batchid)
{
	return workdir + "/vis-parts/" + batchid + "/merged.part";
}

// 扫 batches/*.json 读成 BatchEntry 列表（按文件名排 = batch_id 升序）。目录扫描用
// dirent——三个 fxcorr 程序都跑在 POSIX 上（system("mkdir -p") 已在用）。
static bool scanBatches(const string &workdir, vector<BatchEntry> *out, ostringstream *err)
{
	const string dir = workdir + "/batches";
	DIR *d = opendir(dir.c_str());
	if(d == 0)
	{
		*err << "cannot open " << dir;
		return false;
	}
	vector<string> names;
	struct dirent *ent;
	while((ent = readdir(d)) != 0)
	{
		const string n = ent->d_name;
		if(n.size() > 5 && n.compare(n.size() - 5, 5, ".json") == 0)
			names.push_back(n);
	}
	closedir(d);
	sort(names.begin(), names.end());
	for(size_t i = 0; i < names.size(); i++)
	{
		const string path = dir + "/" + names[i];
		ifstream f(path.c_str());
		if(!f.is_open())
		{
			*err << "cannot open " << path;
			return false;
		}
		stringstream ss;
		ss << f.rdbuf();
		const string json = ss.str();
		BatchEntry be;
		be.id = names[i].substr(0, names[i].size() - 5);
		if(!extractJsonString(json, "config_file", &be.configfile))
		{
			*err << path << ": missing config_file";
			return false;
		}
		if(!extractJsonString(json, "status", &be.status))
			be.status = "?";	// make_testdata.sh 不写 status，run_batch.sh 才置
		be.hastime = extractJsonDouble(json, "start_mjd", &be.startmjd) &&
		             extractJsonInt(json, "n_subints", &be.nsubints);
		out->push_back(be);
	}
	return true;
}

// 认定"本实验"：候选 = 已有 merged.part 的 batch（它们证明这个实验确实在跑 batch 级
// merge）。候选之间 config_file 不一致即报错——那说明 workdir 混了多个实验，选任何
// 一个都会把 SWIN 写到错的实验目录去；候选为空同样是错（还没有东西可归并）。
static bool pickExperiment(const string &workdir, const vector<BatchEntry> &all,
                           vector<const BatchEntry *> *candidates, string *inputfile,
                           ostringstream *err)
{
	for(size_t i = 0; i < all.size(); i++)
	{
		ifstream probe(mergedPartPath(workdir, all[i].id).c_str());
		if(probe.is_open())
			candidates->push_back(&all[i]);
	}
	if(candidates->empty())
	{
		*err << "no batch under " << workdir << "/vis-parts/ has a merged.part yet - run the "
		        "batch-level merge first (fxcorr-x merge <batch_id> [workdir])";
		return false;
	}
	*inputfile = (*candidates)[0]->configfile;
	for(size_t i = 1; i < candidates->size(); i++)
	{
		if((*candidates)[i]->configfile != *inputfile)
		{
			*err << "candidate batches do not share one configuration: " << (*candidates)[0]->id
			     << " uses " << *inputfile << ", " << (*candidates)[i]->id << " uses "
			     << (*candidates)[i]->configfile << " - this workdir holds more than one "
			        "experiment, so the target SWIN would be ambiguous";
			return false;
		}
	}
	return true;
}

// 一个 merged.part 的**首记录时间**（整数纳秒，与 PartRecord::key 同口径）。实验级按它
// 给 batch 定序——**不是** batch_id 数值序：编号只保证由单点分配，未规定"编号 = 时间序"。
static bool firstRecordKey(const string &path, long long *key, ostringstream *err)
{
	ifstream in(path.c_str(), ios::binary);
	if(!in.is_open())
	{
		*err << "cannot open " << path;
		return false;
	}
	char head[74];
	in.read(head, sizeof(head));
	if(in.gcount() != (std::streamsize)sizeof(head))
	{
		*err << path << ": no complete record (empty or truncated)";
		return false;
	}
	unsigned int sync;
	int mjd;
	double sec;
	memcpy(&sync, head, 4);
	if(sync != Visibility::SYNC_WORD)
	{
		*err << path << ": bad sync word 0x" << hex << sync << dec;
		return false;
	}
	memcpy(&mjd, head + 12, 4);
	memcpy(&sec, head + 16, 8);
	*key = (long long)mjd * 86400LL * 1000000000LL + (long long)floor(sec * 1.0e9 + 0.5);
	return true;
}

struct ShardRef
{
	long long key;
	const BatchEntry *batch;
};

static bool shardRefLess(const ShardRef &a, const ShardRef &b)
{
	return a.key < b.key;
}

// batch 级 merge：读 vis-parts/<batch_id>/ 下的全部分片，按**整数纳秒**归并，写出
// vis-parts/<batch_id>/merged.part——**不碰 SWIN**（形态 A：实验级 merge 才是 SWIN 的
// 唯一写入者）。输出与 SWIN 逐字节同构（就是记录流），所以实验级只需按 batch 顺序搬运、
// 不需要理解语义；`.part` 的 glob 是 `ds*.part`，与 `merged.part` 不冲突——命名即隔离。
static int doMerge(Configuration &config, const string &workdir, const string &batchid,
                   DifxMonitor &monitor)
{
	int scan = 0;
	int configindex = config.getScanConfigIndex(scan);
	if(configindex < 0)
		return fail(monitor, "fxcorr-xpp merge: no configuration for scan 0");

	const string partdir = workdir + "/vis-parts/" + batchid;
	// ① 本 batch 应有几片：ds 组的个数（从 .input 的 baseline 表推导）
	vector<vector<int> > groups = deriveDsGroups(config, configindex);
	vector<char> haveshard(groups.size(), 0);
	PartBuf buf;
	std::ostringstream err;

	for(size_t g = 0; g < groups.size(); g++)
	{
		char name[64];
		snprintf(name, sizeof(name), "/ds%d.part", (int)g);
		string path = partdir + name;
		ifstream probe(path.c_str());
		if(!probe.is_open())
			continue;			// 缺片，下面统一报
		probe.close();
		haveshard[g] = 1;
		if(!loadPart(path, config, &buf, &err))
			return fail(monitor, "fxcorr-xpp merge: " + err.str());
	}

	// ② 缺片：默认报错退出、不写任何东西；FXCORR_X_MERGE_FORCE=1 时写出已到齐
	//    的部分并在 stderr 列明缺了哪些组（data-spec 5.9 的硬约束 + 逃生口）
	bool forced = false;
	if(const char *f = getenv("FXCORR_X_MERGE_FORCE"))
		forced = (strcmp(f, "1") == 0);
	string missing;
	for(size_t g = 0; g < groups.size(); g++)
	{
		if(!haveshard[g])
		{
			if(missing.empty())
				missing = "ds" + std::to_string((long long)g) + ".part";
			else
				missing += ", ds" + std::to_string((long long)g) + ".part";
		}
	}
	if(!missing.empty() && !forced)
	{
		return fail(monitor, "fxcorr-xpp merge: batch " + batchid + " is missing shard(s) "
		            + missing + " of " + std::to_string((long long)groups.size())
		            + "; nothing was written (set FXCORR_X_MERGE_FORCE=1 to write the "
		              "shards that are present, leaving those frequency bands empty)");
	}
	if(!missing.empty())
	{
		cerr << "fxcorr-xpp merge: WARNING: writing a partial merge, shard(s) " << missing
		     << " of " << groups.size() << " are missing - the frequency bands they cover "
		        "will have no records in this batch (" << partdir << ")" << endl;
	}
	if(buf.recs.empty())
	{
		cerr << "fxcorr-xpp merge: no records in " << partdir << ", nothing written" << endl;
		return EXIT_SUCCESS;
	}

	// ③ 归并：按积分周期排序（同一周期内的顺序不影响 difx2fits 的读取，
	//    它按记录逐条查 baseline 并对时间做单调检查）
	stable_sort(buf.recs.begin(), buf.recs.end(), partRecordLess);
	long long lastkey = -1;
	long long ntimeback = 0;
	for(size_t i = 0; i < buf.recs.size(); i++)
	{
		if(buf.recs[i].key < lastkey)
			ntimeback++;
		lastkey = buf.recs[i].key;
	}
	if(ntimeback > 0)
		cerr << "fxcorr-xpp merge: WARNING: " << ntimeback
		     << " record(s) out of time order after sorting (please report)" << endl;

	// ④ 写出 merged.part：本 batch 的归并结果，**不碰 SWIN**（形态 A——SWIN 由实验级
	//    merge 单点写出）。一次运行写一次、**第一次写截断**：重跑 batch 级 merge 得到
	//    新内容，不与上一次叠加（与分片 `.part` 的重跑语义一致）。
	string outpath = mergedPartPath(workdir, batchid);
	ofstream out(outpath.c_str(), ios::trunc|ios::binary);
	if(!out.is_open())
		return fail(monitor, string("fxcorr-xpp merge: cannot open ") + outpath + " for writing");
	for(size_t i = 0; i < buf.recs.size(); i++)
		out.write(buf.data.data() + buf.recs[i].off, buf.recs[i].len);
	out.close();
	if(!out)
		return fail(monitor, string("fxcorr-xpp merge: error writing ") + outpath);

	FXLOG(FXLOG_INFO) << "fxcorr-xpp merge: batch " << batchid << ", " << groups.size() << " shard(s), "
	                  << buf.recs.size() << " records -> " << outpath << endl;
	return EXIT_SUCCESS;
}

// 实验级 merge：把本实验全部 batch 的 merged.part 按**数据时间序**搬进 SWIN。它是 SWIN
// 的**唯一写入者**（形态 A）——difx2fits 依赖的时间单调性在这里得到保证，而不是靠"编排层
// 按时间序串行调 merge"这种外部约定。
//
// 四步都是容易写错的地方（细则见 data-spec 5.9 末条）：**应有集取自 batches/*.json 而
// 不是 meta/batches.index**（后者只在 done 时追加，会让 failed 与未调度的 batch 静默
// 消失）、**按首记录时间定序而不是 batch_id 序**、**逐个 batch 流式写出**（各 batch 时间
// 范围不重叠，把顺序定对即全局单调；全量读入不现实——24 h 观测的 SWIN 约 11.8 GB），
// 以及只对**候选**（有 merged.part 的）查根一致性。
static int doMergeExperiment(Configuration &config, const string &workdir,
                             const vector<BatchEntry> &all, const string &inputfile,
                             DifxMonitor &monitor)
{
	int configindex = config.getScanConfigIndex(0);
	if(configindex < 0)
		return fail(monitor, "fxcorr-xpp merge --experiment: no configuration for scan 0");

	// ① 应有集 = config_file 与本次选定者**相同**的全部 batch。**status 不参与判据**：
	//    done/failed/running 都仍是"规划过的 batch"，拿它过滤会让跑挂的 batch 静默消失
	vector<const BatchEntry *> expected;
	for(size_t i = 0; i < all.size(); i++)
	{
		if(all[i].configfile == inputfile)
			expected.push_back(&all[i]);
	}

	// ② 缺 merged.part 的即缺失：默认报错退出、不写任何东西（逃生口同 batch 级）
	vector<const BatchEntry *> ready;
	vector<string> missing;
	for(size_t i = 0; i < expected.size(); i++)
	{
		ifstream probe(mergedPartPath(workdir, expected[i]->id).c_str());
		if(probe.is_open())
			ready.push_back(expected[i]);
		else
			missing.push_back(expected[i]->id + " (status=" + expected[i]->status + ")");
	}
	bool forced = false;
	if(const char *f = getenv("FXCORR_X_MERGE_FORCE"))
		forced = (strcmp(f, "1") == 0);
	if(!missing.empty())
	{
		string list;
		for(size_t i = 0; i < missing.size(); i++)
			list += (i ? ", " : "") + missing[i];
		if(!forced)
		{
			return fail(monitor,"fxcorr-xpp merge --experiment: "
			            + std::to_string((long long)missing.size()) + " of "
			            + std::to_string((long long)expected.size()) + " batch(es) of this "
			              "experiment have no merged.part: " + list + "; nothing was written (set "
			              "FXCORR_X_MERGE_FORCE=1 to write the batches that are ready, leaving the "
			              "missing ones' time ranges empty)");
		}
		cerr << "fxcorr-xpp merge --experiment: WARNING: writing a partial merge, "
		     << missing.size() << " of " << expected.size()
		     << " batch(es) have no merged.part: " << list
		     << " - those time ranges will have no records in the SWIN" << endl;
	}
	if(ready.empty())
	{
		cerr << "fxcorr-xpp merge --experiment: no batch is ready, nothing written" << endl;
		return EXIT_SUCCESS;
	}

	// ③ 根一致性：对**每个候选** batch 逐个比对 VIS 根（实验级唯一用到的根）。范围取
	//    候选集而非应有集——缺 merged.part 的不参与写出，检查它没有意义（上面刚报过）。
	//    checkRoots 一致时静默、不一致时打印双方的值并返回 false，所以没有输出代价。
	static const FxcorrPath::Root usedroots[] = { FxcorrPath::ROOT_VIS };
	for(size_t i = 0; i < ready.size(); i++)
	{
		if(!FxcorrPath::checkRoots(ready[i]->id, usedroots, 1, "fxcorr-xpp"))
			return EXIT_FAILURE;
	}

	// ④ 定序：按各 merged.part 的**首记录时间**（不是 batch_id 数值序——编号只保证由
	//    单点分配，未规定"编号 = 时间序"）
	vector<ShardRef> ordered;
	for(size_t i = 0; i < ready.size(); i++)
	{
		ShardRef sr;
		ostringstream err;
		if(!firstRecordKey(mergedPartPath(workdir, ready[i]->id), &sr.key, &err))
			return fail(monitor, "fxcorr-xpp merge --experiment: " + err.str());
		sr.batch = ready[i];
		ordered.push_back(sr);
	}
	sort(ordered.begin(), ordered.end(), shardRefLess);
	// 两个序不一致时报错而非静默选一：那说明"编号由单点按时间分配"这个前提被破坏了，
	// 此时"哪个序才对"没有安全答案
	for(size_t i = 1; i < ordered.size(); i++)
	{
		if(ordered[i - 1].batch->id > ordered[i].batch->id)
		{
			return fail(monitor, "fxcorr-xpp merge --experiment: batch id order disagrees with data "
			            "time order: " + ordered[i - 1].batch->id + " holds earlier data than "
			            + ordered[i].batch->id + ".  Batch ids are meant to be allocated in time "
			              "order (data-spec section 6); refusing to guess which order to write in");
		}
	}

	// ⑤ SWIN 互斥：查**本次将写出的全部 batch 的总时间范围**（不是单个 batch 的）——
	//    实验级 merge 是 SWIN 的唯一写入者，只查一个 batch 的话重跑会追加一整份重复记录
	string outdir = config.getOutputFilename();
	char filename[4096];
	snprintf(filename, sizeof(filename), "%s/DIFX_%05d_%06d.s0000.b0000",
	         outdir.c_str(), config.getStartMJD(), config.getStartSeconds());
	{
		double lo = -1.0, hi = 0.0;
		for(size_t i = 0; i < ordered.size(); i++)
		{
			if(!ordered[i].batch->hastime)
			{
				return fail(monitor, "fxcorr-xpp merge --experiment: batches/"
				            + ordered[i].batch->id + ".json has no usable start_mjd / n_subints, "
				              "so its time range cannot be checked against the SWIN");
			}
			double s = ordered[i].batch->startmjd * 86400.0;
			double e = s + (double)ordered[i].batch->nsubints
			             * (double)config.getSubintNS(configindex) / 1.0e9;
			if(lo < 0.0 || s < lo) lo = s;
			if(e > hi) hi = e;
		}
		long long nconflict = 0;
		bool allow = false;
		if(const char *v = getenv("FXCORR_X_SWIN_CONFLICT"))
			allow = (strcmp(v, "allow") == 0);
		if(swinHasBatchRange(filename, config, lo / 86400.0, hi - lo, &nconflict) && !allow)
		{
			return fail(monitor, "fxcorr-xpp merge --experiment: " + string(filename) + " already holds "
			            + std::to_string(nconflict) + " record(s) inside the time range this run "
			              "would write (MJD span " + std::to_string(lo / 86400.0) + " + "
			            + std::to_string(hi - lo) + " s): this experiment has been merged already.  "
			              "Writing it again would append a duplicate copy and break the monotonic "
			              "time order difx2fits relies on (it silently drops out-of-time records).  "
			              "Remove or rename the SWIN file - or set FXCORR_X_SWIN_CONFLICT=allow - "
			              "if you really mean to redo it.");
		}
	}

	// ⑥ 逐个 batch 流式写出：读一个、批内按整数纳秒排序、追加、立刻释放。峰值内存因此
	//    是**单个 batch** 而不是整个实验
	if(system(("mkdir -p '" + outdir + "'").c_str()) != 0)
		return fail(monitor, "fxcorr-xpp merge --experiment: cannot create " + outdir);
	ofstream out(filename, ios::app|ios::binary);
	if(!out.is_open())
		return fail(monitor, string("fxcorr-xpp merge --experiment: cannot open ") + filename
		            + " for append");

	long long total = 0;
	long long lastkey = -1;
	long long ntimeback = 0;
	PartBuf buf;			// 跨批复用容量，避免每批重新分配
	for(size_t i = 0; i < ordered.size(); i++)
	{
		buf.clear();
		ostringstream err;
		if(!loadPart(mergedPartPath(workdir, ordered[i].batch->id), config, &buf, &err))
			return fail(monitor, "fxcorr-xpp merge --experiment: " + err.str());
		// merged.part 由 batch 级 merge 按同一比较器排好序写出，正常路径无需再排；
		// 只做 O(n) 验证，发现乱序（外部改动 / 旧文件）才退回排序，输出与旧版一致。
		const bool sorted_input = recsAreOrdered(buf.recs);
		if(!sorted_input)
			stable_sort(buf.recs.begin(), buf.recs.end(), partRecordLess);
		for(size_t k = 0; k < buf.recs.size(); k++)
		{
			if(buf.recs[k].key < lastkey)
				ntimeback++;
			lastkey = buf.recs[k].key;
		}
		// 有序时记录在 data 中连续且顺序未变——整块一次写出（2026-11-06）
		if(sorted_input)
			out.write(buf.data.data(), buf.data.size());
		else
			for(size_t k = 0; k < buf.recs.size(); k++)
				out.write(buf.data.data() + buf.recs[k].off, buf.recs[k].len);
		total += (long long)buf.recs.size();
		FXLOG(FXLOG_VERBOSE) << "fxcorr-xpp merge --experiment: batch " << ordered[i].batch->id
		                     << ", " << buf.recs.size() << " record(s)" << endl;
	}
	out.close();
	if(!out)
		return fail(monitor, string("fxcorr-xpp merge --experiment: error writing ") + filename);
	if(ntimeback > 0)
	{
		cerr << "fxcorr-xpp merge --experiment: WARNING: " << ntimeback
		     << " record(s) out of time order (please report)" << endl;
	}

	FXLOG(FXLOG_INFO) << "fxcorr-xpp merge --experiment: " << ordered.size() << " batch(es), "
	                  << total << " record(s) -> " << filename << endl;
	return EXIT_SUCCESS;
}

// ---- per-datastream segment pool (ds x block parallelism) ----------------
// One persistent pool per datastream.  Tasks are contiguous block segments of
// one fftloop; the calling thread (an OpenMP datastream worker) joins in as
// worker 0.  The pool is std::thread based, so the ds OpenMP loop and the
// segment parallelism compose without nesting OpenMP regions (libgomp
// deadlocks when a parallel loop keeps forking nested teams).  run() returns
// when every segment is done, which is also the fftloop reduction barrier.
struct SegmentPool
{
	int nworkers;
	std::vector<std::thread> workers;
	std::mutex m;
	std::condition_variable cv_start, cv_done;
	unsigned long generation;
	int ntasks, donecount;
	std::atomic<int> next;
	std::function<void(int)> fn;
	bool stop;

	explicit SegmentPool(int nthreads)
		: nworkers(nthreads > 1 ? nthreads - 1 : 0), generation(0), ntasks(0),
		  donecount(0), next(0), stop(false)
	{
		for(int w=0;w<nworkers;w++)
			workers.push_back(std::thread(&SegmentPool::workerLoop, this, w+1));
	}
	~SegmentPool()
	{
		{
			std::lock_guard<std::mutex> lk(m);
			stop = true;
		}
		cv_start.notify_all();
		for(size_t w=0;w<workers.size();w++)
			workers[w].join();
	}
	void workerLoop(int)
	{
		unsigned long seen = 0;
		for(;;)
		{
			std::function<void(int)> f;
			int n;
			{
				std::unique_lock<std::mutex> lk(m);
				cv_start.wait(lk, [&]{ return stop || generation != seen; });
				if(stop)
					return;
				seen = generation;
				f = fn;
				n = ntasks;
			}
			for(;;)
			{
				int i = next.fetch_add(1);
				if(i >= n)
					break;
				f(i);
			}
			{
				std::lock_guard<std::mutex> lk(m);
				donecount++;
				if(donecount >= nworkers)
					cv_done.notify_one();
			}
		}
	}
	void run(int n, const std::function<void(int)> &f)
	{
		{
			std::lock_guard<std::mutex> lk(m);
			fn = f;
			ntasks = n;
			next = 0;
			donecount = 0;
			generation++;
		}
		cv_start.notify_all();
		for(;;)
		{
			int i = next.fetch_add(1);
			if(i >= n)
				break;
			f(i);
		}
		std::unique_lock<std::mutex> lk(m);
		cv_done.wait(lk, [&]{ return donecount >= nworkers; });
	}
};

// compact sense-reversing barrier for one datastream's segment team.  A
// condition variable costs ~60 ms per fftloop round on this host (measured
// 2026-11-09), while spin+yield is a few microseconds; the team never exceeds
// the core count, so the spinning is cheap.
struct SpinBarrier
{
	std::atomic<int> count;
	std::atomic<unsigned long> gen;
	int n;
	explicit SpinBarrier(int nthreads) : count(0), gen(0), n(nthreads) {}
	void wait()
	{
		unsigned long g = gen.load(std::memory_order_acquire);
		if(count.fetch_add(1, std::memory_order_acq_rel) + 1 == n)
		{
			count.store(0, std::memory_order_relaxed);
			gen.fetch_add(1, std::memory_order_release);
		}
		else
		{
			// pure spin with PAUSE; sched_yield() is a syscall and turned the
			// barrier into a sys-time sink (measured 2026-11-09)
			while(gen.load(std::memory_order_acquire) == g)
			{
#if defined(__x86_64__) || defined(__i386__)
				for(int i=0;i<64;i++)
					__builtin_ia32_pause();
#endif
			}
		}
	}
};

int main(int argc, char **argv)
{
	if(argc < 2)
	{
		cerr << "usage:\n"
		     << "  fxcorr-xpp <batch_id> [workdir]\n"
		     << "      correlate every datastream of the batch, write SWIN\n"
		     << "  fxcorr-xpp <batch_id> [workdir] <ds_group>\n"
		     << "      shard mode: correlate only that ds group, write\n"
		     << "      vis-parts/<batch_id>/ds<G>.part instead of SWIN\n"
		     << "  fxcorr-xpp merge <batch_id> [workdir]\n"
		     << "      merge the batch's shards into vis-parts/<batch_id>/merged.part\n"
		     << "  fxcorr-xpp merge --experiment [workdir]\n"
		     << "      merge every batch's merged.part into SWIN in data time order;\n"
		     << "      this is the only writer of SWIN - run it once the experiment's\n"
		     << "      batches are all done\n"
		     << "  input: the v9 chain's per-datastream stream.bin (written by\n"
		     << "      fxcorr-fp into the batch's fengine directories), not .sp\n"
		     << "  env: FXCORR_WORKDIR (default .), overridden by the workdir argument;\n"
		     << "      FXCORR_X_MERGE_FORCE=1 writes a partial merge instead of failing\n"
		     << "      when a shard (batch level) or a batch (experiment level) is missing"
		     << endl;
		return EXIT_FAILURE;
	}
	// P3 (algo-plan.md): thread count from OMP_NUM_THREADS; unset = serial
	// (V2 behaviour unchanged).  Must run before any OpenMP parallel region.
	{
		const char *env = getenv("OMP_NUM_THREADS");
		if(env == 0 || env[0] == '\0')
			omp_set_num_threads(1);
	}
	// stage timing, three tiers (aggregate / brief / trace; same instrument
	// as fxcorr-f and fxcorr-x):
	//   1 aggregate: FXCORR_TIMING=1 -- one summary line, unchanged (output
	//     file via FXCORR_TIMING_FILE)
	//   2 brief: FXCORR_TIMING_BRIEF=1|<path> -- one TSV row per probe at
	//     exit, first timestamp + accumulated total (<=20 rows; 1 = stderr,
	//     a path = that file)
	//   3 trace: FXCORR_TIMING_TRACE=<path>|1 -- every lap buffered in memory
	//     and written once at exit; 1 = <workdir>/fxcorr-xpp-<batch>.timing.tsv
	// Trace wins over brief wins over aggregate when several are set.
	{
		const char *timingenv = getenv("FXCORR_TIMING");
		const char *fileenv = getenv("FXCORR_TIMING_FILE");
		const char *traceenv = getenv("FXCORR_TIMING_TRACE");
		const char *briefenv = getenv("FXCORR_TIMING_BRIEF");
		bool trace_path = (traceenv != 0 && strchr(traceenv, '/') != 0);
		bool brief_path = (briefenv != 0 && strchr(briefenv, '/') != 0);
		g_timing_trace = (traceenv != 0
		                  && (strcmp(traceenv, "1") == 0 || trace_path));
		g_timing_brief = (briefenv != 0
		                  && (strcmp(briefenv, "1") == 0 || brief_path))
		              && !g_timing_trace;
		g_timing = (timingenv != 0 && strcmp(timingenv, "1") == 0)
		        || (fileenv != 0 && fileenv[0] != '\0')
		        || g_timing_trace || g_timing_brief;
		g_timing_out = stderr;
		const char *outpath = 0;
		if(g_timing_trace && trace_path)
			outpath = traceenv;
		else if(g_timing_brief && brief_path)
			outpath = briefenv;
		else if(fileenv != 0 && fileenv[0] != '\0')
			outpath = fileenv;
		if(g_timing && outpath != 0)
		{
			FILE *f = fopen(outpath, "a");
			if(f == 0)
				cerr << "fxcorr-xpp: cannot open timing file " << outpath << ", using stderr" << endl;
			else
				g_timing_out = f;
		}
		if(g_timing)
			g_tt_start = nowsec();
	}
	// Four call forms (usage above).  ds_group sits after workdir, same
	// position as fxcorr-f's ds_index, so giving it requires giving workdir.
	bool merge = false;
	bool experiment = false;	// merge --experiment: batch_id is not known up front
	int dsgroup = -1;
	string batchid;
	string workdir = ".";
	if(const char *wd = getenv("FXCORR_WORKDIR"))
		workdir = wd;
	if(strcmp(argv[1], "merge") == 0)
	{
		merge = true;
		if(argc >= 3 && strcmp(argv[2], "--experiment") == 0)
		{
			// experiment level: no batch id, so [workdir] comes right after the flag
			// and the experiment itself is identified from batches/*.json later
			experiment = true;
			if(argc > 3)
				workdir = argv[3];	// argument takes precedence over the environment
			if(argc > 4)
			{
				cerr << "fxcorr-xpp merge --experiment: unexpected extra argument '"
				     << argv[4] << "'" << endl;
				return EXIT_FAILURE;
			}
		}
		else
		{
			if(argc < 3)
			{
				cerr << "fxcorr-xpp merge: missing batch_id (or --experiment)" << endl;
				return EXIT_FAILURE;
			}
			batchid = argv[2];
			if(argc > 3)
				workdir = argv[3];	// argument takes precedence over the environment
		}
	}
	else
	{
		batchid = argv[1];
		if(argc > 2)
			workdir = argv[2];	// argument takes precedence over the environment
		if(argc > 3)
		{
			char *end;
			long v = strtol(argv[3], &end, 10);
			if(end == argv[3] || *end != '\0' || v < 0)
			{
				cerr << "fxcorr-xpp: bad ds_group '" << argv[3]
				     << "' (a non-negative integer expected)" << endl;
				return EXIT_FAILURE;
			}
			dsgroup = (int)v;
		}
	}
	bool sharded = (dsgroup >= 0);
	// FXCORR_X_GROUPS_ONLY（见下面它的用法）是**纯诊断出口**：只读 .input 与
	// batch.json，不碰任何根。所以它必须跳过下面那道根一致性检查——否则在换过根
	// 布局的 workdir 上，一个只想问"这个 batch 分几组"的问题会被"这个 batch 当初
	// 是用别的根跑的"挡住（对照测试实测遇到：meta/roots/00000002.json 里留着上一
	// 次容器实验的 /dev/shm，而当前解析出的是 workdir/fengine）。
	bool groupsonly = (sharded && getenv("FXCORR_X_GROUPS_ONLY") != NULL);
	// hand the resolved workdir to the shared root resolver (V5 P5): the
	// library opens .input/.calc/.im paths through it, so every relative path
	// in the config set lands on the same roots this tool uses below
	FxcorrPath::init(workdir);
	workdir = FxcorrPath::workdir();
	FxcorrPath::print("fxcorr-xpp");
	// the roots each form touches: full correlation reads fengine and writes
	// vis; a shard only reads fengine (its output is vis-parts/, which has no
	// root - it always lives under the workdir); merge only writes vis
	static const FxcorrPath::Root usedroots_all[] = {
		FxcorrPath::ROOT_FENGINE, FxcorrPath::ROOT_VIS };
	static const FxcorrPath::Root usedroots_shard[] = {
		FxcorrPath::ROOT_FENGINE };
	static const FxcorrPath::Root usedroots_merge[] = {
		FxcorrPath::ROOT_VIS };
	if(groupsonly)
	{
		// 纯诊断，不碰根，见 groupsonly 的定义处
	}
	else if(merge)
	{
		if(!FxcorrPath::checkRoots(batchid, usedroots_merge, 1, "fxcorr-xpp"))
			return EXIT_FAILURE;
	}
	else if(sharded)
	{
		if(!FxcorrPath::checkRoots(batchid, usedroots_shard, 1, "fxcorr-xpp"))
			return EXIT_FAILURE;
	}
	else if(!FxcorrPath::checkRoots(batchid, usedroots_all, 2, "fxcorr-xpp"))
	{
		return EXIT_FAILURE;
	}

	double startmjd = 0.0;
	int nsubints = 0;
	string inputfile;
	// experiment level has no batch_id, so it identifies the experiment (and thus the
	// .input) from batches/*.json instead.  That has to happen before the
	// Configuration is built: inputfile is what feeds it.
	vector<BatchEntry> batches;
	if(experiment)
	{
		ostringstream err;
		vector<const BatchEntry *> candidates;
		if(!scanBatches(workdir, &batches, &err) ||
		   !pickExperiment(workdir, batches, &candidates, &inputfile, &err))
		{
			cerr << "fxcorr-xpp merge --experiment: " << err.str() << endl;
			return EXIT_FAILURE;
		}
	}
	else
	{
		// batch.json is pre-written by run_batch.sh (batches/<batch_id>.json)
		string batchjsonpath = workdir + "/batches/" + batchid + ".json";
		ifstream jf(batchjsonpath.c_str());
		if(!jf.is_open())
		{
			cerr << "fxcorr-xpp: cannot open " << batchjsonpath << endl;
			return EXIT_FAILURE;
		}
		stringstream jss;
		jss << jf.rdbuf();
		string json = jss.str();
		jf.close();
		if(!extractJsonDouble(json, "start_mjd", &startmjd) ||
		   !extractJsonInt(json, "n_subints", &nsubints) ||
		   !extractJsonString(json, "config_file", &inputfile))
		{
			cerr << "fxcorr-xpp: batch.json missing required fields" << endl;
			return EXIT_FAILURE;
		}
	}

	// FXCORR_LOGLEVEL has to be in force before the configuration is loaded:
	// fxcorrcommon reports the whole parsing pass through the Alert streams
	// (see log.h), which is most of what the tool otherwise prints.
	fxApplyAlertLevel();

	// parse .input (non-MPI constructor)
	Configuration config((workdir + "/" + inputfile).c_str(), 0);
	Model *model = config.getModel();

	// P1: DifxMessage status emission (algo-plan.md).  x plays the manager
	// role: mpiId = 0, RUNNING per written integration (via Integrator).
	// container mode logs to meta/difxmsg/<exp>_<batch>.xml
	string expname = jobIdentifier(inputfile);
	string containerprefix;
	if(const char *runmode = getenv("FXCORR_RUN_MODE"))
	{
		if(strcmp(runmode, "container") == 0)
			containerprefix = workdir + "/meta/difxmsg/" + expname + "_" + batchid;
	}
	if(containerprefix.size() > 0)
	{
		if(system(("mkdir -p " + workdir + "/meta/difxmsg").c_str()) != 0)
			cerr << "fxcorr-xpp: cannot create " << workdir << "/meta/difxmsg" << endl;
	}
	DifxMonitor monitor(0, expname, inputfile, containerprefix);

	// V1 restrictions (v1-plan 1 / data-spec 12)
	if(model->getNumScans() != 1)
	{
		ostringstream oss;
		oss << "fxcorr-xpp: V1 supports single-scan experiments only (got " << model->getNumScans() << " scans)";
		return fail(monitor, oss.str());
	}
	int scan = 0;
	int configindex = config.getScanConfigIndex(scan);
	if(configindex < 0)
		return fail(monitor, "fxcorr-xpp: no configuration for scan 0");
	// P7: cross-polar autocorrelations (maxproducts > 2) are supported; the
	// cross-pol section of the ac records (AcBatchCollector) decides whether
	// the extra results are accumulated (Integrator::addAutocorrs).

	// AC_INIT version of fxcorr-x
	monitor.status(DIFX_STATE_STARTING, "Version 0.1.0", 0.0, 0, 0, 0.0, 0.0);

	int subintns = config.getSubintNS(configindex);
	int blockspersend = config.getBlocksPerSend(configindex);
	int numbufferedffts = config.getNumBufferedFFTs(configindex);

	// intTime must be an integer multiple of subintns, so that Visibility's
	// dump grid coincides with the subint grid (offsetnsperintegration == 0).
	// Phased array batches write no SWIN and need no intTime grid (P8).
	if(!config.phasedArrayOn(configindex))
	{
		long long inttimens = (long long)(config.getIntTime(configindex)*1.0e9);
		if(inttimens % (long long)subintns != 0)
		{
			ostringstream oss;
			oss << "fxcorr-xpp: intTime " << config.getIntTime(configindex) << " s is not an integer multiple of subintNS " << subintns << " ns (required in V1)";
			return fail(monitor, oss.str());
		}
	}

	// 只报分组即退出（V7 P3 前加，2026-09-29）：一个**无数据依赖、无副作用**的 C++
	// 真值出口。bash 侧 `fxcorr/fxinput.py` 有一份同规则的组划分实现，两处之间没有
	// 编译器兜底，漂移的后果是分片边界算错——每片少算或多算 ds，而产物看起来完全
	// 正常。判据 `fxcorr/test/input/run_consistency.sh` 拿这里当真值对拍，所以它必须
	// 能在没有 raw/fengine 的机器上跑。
	//
	// **位置在下面那道 SWIN 互斥检查之前**，并且跳过了上面的根一致性检查：它不读
	// 数据、不写盘，不该被"这个 batch 已经写过了"之类的 workdir 状态拦住——对照
	// 测试恰恰常跑在已经跑过的 workdir 上（两道检查实测各拦了一次）。诊断上也有
	// 用：想只问"这个 batch 分几组、组里是谁"，不必先造 fengine。
	if(groupsonly)
	{
		vector<vector<int> > groups = deriveDsGroups(config, configindex);
		for(size_t g = 0; g < groups.size(); g++)
		{
			ostringstream oss;
			oss << "fxcorr-xpp: shard mode, ds group " << g << " of " << groups.size() << " = {";
			for(size_t k = 0; k < groups[g].size(); k++)
				oss << (k ? "," : "") << groups[g][k];
			oss << "}";
			FXLOG(FXLOG_INFO) << oss.str() << endl;
		}
		return EXIT_SUCCESS;
	}

	// 互斥检查：只有**会写 SWIN 的路径**需要它，也就是全量模式（不传 ds_group、直写
	// SWIN）。本 batch 的时间范围若已经在 SWIN 里，说明它被另一次运行写过（全量、或
	// 过去的实验级 merge），再写一遍就是追加重复记录、破坏时间单调。
	//
	// **分片模式不走这里**——`data-spec` 5.9 末条："batch 级 merge 与分片任务都不写
	// SWIN，**两者都不查**"。原先这里的条件是 `!merge`，把分片也圈了进来：后果不是
	// "多一道保护"，而是**卡住正常重跑**——一个 batch 只要被 merge 写过 SWIN，它的
	// 任何一个 ds 组就再也重跑不了，而重跑分片恰恰是并行调度里最常做的事（调并行度、
	// 补失败组、换线程数做对照）。分片与已有 SWIN 混跑的重复风险由**实验级 merge**
	// 兜住：doMergeExperiment 按"本次将写出的全部 batch 的总时间范围"查，那里才有
	// batch 清单（本函数里只有当前这一个 batch 的信息，够不着）。
	if(!merge && !sharded)
	{
		char swinpath[4096];
		snprintf(swinpath, sizeof(swinpath), "%s/DIFX_%05d_%06d.s0000.b0000",
		         config.getOutputFilename().c_str(), config.getStartMJD(), config.getStartSeconds());
		double duration = (double)nsubints * (double)config.getSubintNS(configindex) / 1.0e9;
		long long nconflict = 0;
		bool allow = false;
		if(const char *v = getenv("FXCORR_X_SWIN_CONFLICT"))
			allow = (strcmp(v, "allow") == 0);
		if(swinHasBatchRange(swinpath, config, startmjd, duration, &nconflict) && !allow)
		{
			ostringstream oss;
			oss << "fxcorr-xpp: " << swinpath << " already holds " << nconflict
			    << " record(s) inside this batch's time range (start_mjd " << startmjd
			    << ", duration " << duration << " s): this batch has been written already, "
			       "by a full (non-sharded) run or by a merge.  Writing it again would "
			       "append duplicate records and break the monotonic time order difx2fits "
			       "relies on (it silently drops out-of-time records).  Remove or rename "
			       "the SWIN file - or set FXCORR_X_SWIN_CONFLICT=allow - if you really "
			       "mean to redo this batch.";
			return fail(monitor, oss.str());
		}
	}

	// merge 分支（两级，data-spec 5.9 末条）：batch 级读本 batch 的分片、写
	// vis-parts/<batch_id>/merged.part；实验级读全部 batch 的 merged.part、写实验的 SWIN
	if(merge)
	{
		monitor.status(DIFX_STATE_STARTING, "Version 0.1.0", 0.0, 0, 0, 0.0, 0.0);
		int rc = experiment ? doMergeExperiment(config, workdir, batches, inputfile, monitor)
		                    : doMerge(config, workdir, batchid, monitor);
		monitor.status(DIFX_STATE_ENDING, "", 0.0, 0, 0, 0.0, 0.0);
		if(rc == EXIT_SUCCESS)
			monitor.status(DIFX_STATE_DONE, "", 0.0, 0, 0, 0.0, 0.0);
		return rc;
	}

	// v9 restrictions (fxcorr/v9-plan.md D11): the station stage is new code in
	// this tool, so the configurations it does not reproduce are refused up
	// front rather than run wrong -- multi phase centre, pulsar binning and
	// phased arrays (the set the shard mode used to refuse, now unconditional
	// because every fxcorr-xpp correlation runs the station stack); zoom and
	// crosspol are carried in the code and validated in stage 2.
	{
		int nphasecentres = model->getNumPhaseCentres(scan);
		if(nphasecentres != 1)
			return fail(monitor, "fxcorr-xpp: v9 supports a single phase centre only (got "
			            + std::to_string((long long)nphasecentres) + "); multi phase centre is not implemented");
		if(config.pulsarBinOn(configindex))
			return fail(monitor, "fxcorr-xpp: v9 does not support pulsar binning");
		if(config.phasedArrayOn(configindex))
			return fail(monitor, "fxcorr-xpp: v9 does not support phased array configurations");
	}
	// P9: STA / kurtosis dump switches, fxcorr-f environment semantics
	bool dosta = false;
	if(const char *staenv = getenv("FXCORR_STA"))
		dosta = (strcmp(staenv, "1") == 0);
	bool dokurtosis = false;
	if(const char *kurtosisenv = getenv("FXCORR_KURTOSIS"))
		dokurtosis = (strcmp(kurtosisenv, "1") == 0);

	// ---- 分片模式：只处理一个 ds 组，输出 D16 而不是 SWIN --------------------
	// 一组 = 跨站、含全部极化的一批 datastream，它们覆盖同一个频段组；成员从
	// .input 的 BASELINE TABLE 推导（连通分量），不是按 ds 序号或 freq 条目。
	// 全量模式（不传 ds_group）下两个掩码都是空的，所有循环与改动前一致。
	vector<char> dsactive(config.getNumDataStreams(), 1);
	vector<char> blactive(config.getNumBaselines(), 1);
	string partpath;
	if(sharded)
	{
		vector<vector<int> > groups = deriveDsGroups(config, configindex);
		if(dsgroup >= (int)groups.size())
			return fail(monitor, "fxcorr-xpp: ds_group " + std::to_string((long long)dsgroup)
			            + " is out of range - this batch has "
			            + std::to_string((long long)groups.size()) + " ds group(s)");
		for(int i = 0; i < config.getNumDataStreams(); i++)
			dsactive[i] = 0;
		for(size_t k = 0; k < groups[dsgroup].size(); k++)
			dsactive[groups[dsgroup][k]] = 1;
		// 一条 baseline 只有在它声明的两个 ds 都在场时才算得出来
		for(int b = 0; b < config.getNumBaselines(); b++)
		{
			int d1 = config.getBOrderedDataStream1Index(configindex, b);
			int d2 = config.getBOrderedDataStream2Index(configindex, b);
			blactive[b] = (dsactive[d1] && dsactive[d2]) ? 1 : 0;
		}
		partpath = workdir + "/vis-parts/" + batchid + "/ds"
		           + std::to_string((long long)dsgroup) + ".part";
		string mkdircommand = "mkdir -p " + workdir + "/vis-parts/" + batchid;
		if(system(mkdircommand.c_str()) != 0)
			return fail(monitor, "fxcorr-xpp: cannot create " + workdir + "/vis-parts/" + batchid);
		ostringstream oss;
		oss << "fxcorr-xpp: shard mode, ds group " << dsgroup << " of " << groups.size() << " = {";
		for(size_t k = 0; k < groups[dsgroup].size(); k++)
			oss << (k ? "," : "") << groups[dsgroup][k];
		oss << "} -> " << partpath;
		FXLOG(FXLOG_INFO) << oss.str() << endl;
	}

	// v9 station stack (fxcorr/v9-plan.md D5-D9): one Mode per datastream plus
	// its in-memory band views.  The data comes from fxcorr-fp's stream.bin,
	// living in the same fengine/<batch>/<g>/<station>/ds_<N>/ directory the
	// v8 chain kept band_XX.sp in, so the path derivation below is unchanged
	// (per-group layout, data-spec 5.3).
	double blockns = (double)subintns/(double)blockspersend;

	// optional intra-Mode band parallelism (xpp only): 0 = off, in which case
	// xpp runs exactly the xp serial band loop.  Nested level 2 lets the band
	// loop run inside the existing per-datastream OpenMP region.
	int bandthreads = 0;
	if(const char *bt = getenv("FXCORR_XPP_BAND_THREADS"))
		bandthreads = atoi(bt);

	// optional intra-datastream block parallelism (xpp only): N>1 splits each
	// datastream's fftloop blocks into N contiguous segments, processed by N
	// replica Modes and reduced in block order (fxcorr-f's P3 scheme).  The
	// datastreams themselves then run serially; the segment is the task, so a
	// thread pool is never entered once per block.  Mutually exclusive with
	// the band switch: the band form was the first experiment and pays a
	// synchronisation cost per block, so block mode wins when both are set.
	int blockthreads = 0;
	if(const char *bt = getenv("FXCORR_XPP_BLOCK_THREADS"))
		blockthreads = atoi(bt);
#ifndef _OPENMP
	blockthreads = 1;	// no team, no segmentation
#endif
	if(blockthreads > 1 && bandthreads > 1)
	{
		cerr << "fxcorr-xpp: FXCORR_XPP_BLOCK_THREADS and FXCORR_XPP_BAND_THREADS are mutually exclusive; using block parallelism" << endl;
		bandthreads = 0;
	}
#ifdef _OPENMP
	if(bandthreads > 1)
		omp_set_max_active_levels(2);
#endif

	int numdatastreams = config.getNumDataStreams();
	vector<Mode *> modes(numdatastreams, 0);
	// block-parallel replica Modes per datastream (empty unless
	// FXCORR_XPP_BLOCK_THREADS > 1); the primary Mode stays worker 0
	vector<vector<Mode *> > modereplicas(numdatastreams);
	// one segment pool per datastream (created with its replicas)
	vector<SegmentPool *> segpools(numdatastreams, 0);
	vector<StreamReader *> streams(numdatastreams, 0);
	vector<vector<SpReader *> > readers(numdatastreams);	// MemReader views
	vector<AcBatchCollector *> accollectors(numdatastreams, 0);
	vector<PcalWriter *> pcalwriters(numdatastreams, 0);
	vector<PcalTextWriter *> pcaltexts(numdatastreams, 0);
	// ds 组（fengine 按组布局，data-spec 5.3，2026-10-09）：sdir 的组层用
	vector<vector<int> > dsgroups = deriveDsGroups(config, configindex);
	for(int ds=0;ds<numdatastreams;ds++)
	{
		if(!dsactive[ds])
			continue;	// shard mode: not ours - leave the per-ds slots empty
		string station = config.getDStationName(configindex, ds);
		// fengine layout (data-spec 5.3, 2026-10-09): <batch>/<ds-group>/
		// <station>/ds_<N>/ — the group layer is the lifecycle unit
		// (per-group purge removes one directory tree; fxcorr-fp writes
		// stream.bin into the same layout).  N = station-local index.
		int dswithinstation = 0;
		for(int d=0;d<ds;d++)
		{
			if(config.getDStationName(configindex, d) == station)
				dswithinstation++;
		}
		int dsgroupof = groupOfDs(dsgroups, ds);
		if(dsgroupof < 0)
			return fail(monitor, "fxcorr-xpp: ds " + to_string(ds) + " not in any ds group");
		string sdir = FxcorrPath::root(FxcorrPath::ROOT_FENGINE) + "/" + batchid + "/" + to_string(dsgroupof) + "/" + station + "/ds_" + to_string(dswithinstation);

		// the station stack itself: one Mode (the same constructor fxcorr-f
		// used) and the stream.bin reader, whose header is cross-checked
		// against the config the way the .sp headers used to be
		modes[ds] = config.getMode(configindex, ds);
		if(!modes[ds]->initialisedOK())
			return fail(monitor, "fxcorr-xpp: mode initialisation failed for station " + station);
		// P9: Mode::process accumulates the kurtosis s1/s2 per FFT block only
		// when this is set (mode.cpp:1236, upstream core.cpp:702)
		modes[ds]->setDumpKurtosis(dokurtosis);
		streams[ds] = new StreamReader(sdir + "/stream.bin", batchid, station,
		                               dswithinstation, subintns, blockspersend,
		                               numbufferedffts, nsubints);
		if(!streams[ds]->ok())
			return fail(monitor, "fxcorr-xpp: cannot read " + sdir
			            + "/stream.bin (missing or header mismatch - was this batch run with fxcorr-fp?)");

		// per-block accumulators, exactly fxcorr-f's (its nthreads = 1 path):
		// the per-fftloop reduction below replays the blocks in block order,
		// so the per-band weights (getDataWeight) and the accumulation
		// sequences are the values fxcorr-f produced.  slot = the global block
		// index (blockslotbase = 0), so the whole subint fits.  The primary
		// keeps that mapping even in block mode (the serial path must stay
		// safe if the pcal capability check below falls back).
		modes[ds]->enableBlockAccumulators(blockspersend);
		modes[ds]->setBlockSlotBase(0);
		if(blockthreads > 1)
		{
			// the parallel pcal reduction needs the extractors' raw
			// accumulators; without them the replicas' shares cannot be
			// folded back in block order (fxcorr-f falls back the same way)
			if(config.getDPhaseCalIntervalHz(configindex, ds) > 0 && !modes[ds]->blockPcalCapable())
			{
				cerr << "fxcorr-xpp: pcal extractor does not expose its raw accumulator; falling back to serial station processing" << endl;
				blockthreads = 1;
			}
			// whole-subint slots: the block form processes the subint without
			// recycling per-fftloop slots, so every Mode needs blockspersend
			// slots (zoom bands alias a parent and per-band weights reuse the
			// slot numbering -- both fall back to the serial path)
			else if(!modes[ds]->setSubloopCapacity(blockspersend))
			{
				cerr << "fxcorr-xpp: cannot hold a whole subint of spectrum slots for datastream " << ds << "; falling back to serial station processing" << endl;
				blockthreads = 1;
			}
		}
		if(blockthreads > 1)
		{
			for(int t=1;t<blockthreads;t++)
			{
				Mode *rm = config.getMode(configindex, ds);
				if(!rm->initialisedOK())
					return fail(monitor, "fxcorr-xpp: replica mode initialisation failed for station " + station);
				rm->setDumpKurtosis(dokurtosis);
				rm->enableBlockAccumulators(blockspersend);
				rm->setBlockSlotBase(0);
				if(!rm->setSubloopCapacity(blockspersend))
				{
					cerr << "fxcorr-xpp: replica cannot hold a whole subint of spectrum slots; falling back to serial station processing" << endl;
					delete rm;
					blockthreads = 1;
					break;
				}
				modereplicas[ds].push_back(rm);
			}
			// the pool lives as long as this datastream's Mode stack; the
			// calling OpenMP thread joins run() as one of the segments
			if(blockthreads > 1)
				segpools[ds] = new SegmentPool(blockthreads);
		}
		// optional band parallelism; setBandParallel refuses zoom configs
		// (bands may alias a parent band) and single-band datastreams
		else if(bandthreads > 1)
			modes[ds]->setBandParallel(bandthreads);

		// one spectrum view per (datastream, total band); zoom bands are the
		// parent band's slice inside Mode, so every view reads the Mode
		int ntotalbands = config.getDNumTotalBands(configindex, ds);
		readers[ds].resize(ntotalbands);
		for(int band=0;band<ntotalbands;band++)
			readers[ds][band] = new MemReader(modes[ds], &config, configindex, ds, band);

		accollectors[ds] = new AcBatchCollector(&config, configindex, ds);

		// the station products v9 moved here from fxcorr-f (D6/D7): pcal.bin
		// into the fengine directory, the PCAL text file next to the SWIN
		// output - same names, same layout, same idempotent semantics
		if(config.getDPhaseCalIntervalHz(configindex, ds) > 0)
		{
			if(system(("mkdir -p " + config.getOutputFilename()).c_str()) != 0)
				return fail(monitor, "fxcorr-xpp: cannot create " + config.getOutputFilename());
			pcalwriters[ds] = new PcalWriter(sdir, &config, configindex, ds, nsubints);
			pcaltexts[ds] = new PcalTextWriter(config.getOutputFilename(), &config, configindex, ds);
		}
	}

	XmacEngine xmac(&config, configindex, model, scan);

	// batch start expressed as job-relative seconds (same as fxcorr-f main)
	long long scanstartsec = (long long)model->getScanStartSec(scan, config.getStartMJD(), config.getStartSeconds());
	double jobstart = (double)config.getStartMJD() + (double)config.getStartSeconds()/86400.0;
	double batchstartjob = (startmjd - jobstart)*86400.0;

	// data-spec section 12: the batch start must lie on a subint boundary;
	// tolerance absorbs the f64 representation error of start_mjd (~1 us)
	{
		long long batchstartns = (long long)floor(batchstartjob*1.0e9 + 0.5);
		if(batchstartns % subintns > 1000 && (subintns - batchstartns % subintns) > 1000)
		{
			ostringstream oss;
			oss << "fxcorr-xpp: batch start " << startmjd << " is not on a subint boundary ("
			    << batchstartns % subintns << " ns into a " << subintns << " ns subint)";
			return fail(monitor, oss.str());
		}
	}

	// P8's phased-array beam branch is not carried over from fxcorr-x: the v9
	// stage-1 restrictions above refuse phased-array configurations, so there
	// is no path here (fxcorr/v9-plan.md D11; the beam engine stays in
	// fxcorr-x until a stage-2 item asks for it).

	// Visibility start time: batch start relative to the scan start
	double reld = batchstartjob - (double)scanstartsec;
	if(reld < 0.0)
		return fail(monitor, "fxcorr-xpp: batch starts before the scan start");
	int initsec = (int)floor(reld);
	int initns = (int)((reld - (double)initsec)*1.0e9 + 0.5);
	if(initns >= 1000000000)
	{
		initsec++;
		initns -= 1000000000;
	}

	// Visibility::writedata stops when currentstartseconds + scanstartsec >=
	// executeseconds, i.e. executeseconds is measured from the scan start
	// (mpifxcorr EXECUTE TIME semantics); a batch starting initsec into the
	// scan must shift the limit by that offset (+1 so the last integration
	// always clears it)
	int executeseconds = (int)((double)nsubints*(double)subintns/1.0e9 + 0.5) + initsec + 1;
	Integrator integrator(&config, configindex, executeseconds, scan, initsec, initns, &monitor);

	// 分片模式：把"哪些 baseline / datastream 在场"交给两个引擎，并把记录
	// 写到 D16 而不是 SWIN（全量模式下这三个 setter 都不调，行为与改动前一致）
	if(sharded)
	{
		xmac.setActiveBaselines(blactive);
		integrator.visibility()->setActiveBaselines(blactive);
		integrator.visibility()->setActiveDatastreams(dsactive);
		integrator.visibility()->setOutputPath(partpath);
	}

	int coreresultlength = config.getCoreResultLength(configindex);
	cf32 *subintresults = vectorAlloc_cf32(coreresultlength);

	// per-subint loop (Core::processdata + FxManager::addData merged)
	double maxNSBetweenXCAvg = model->getMaxNSBetweenXCAvg(scan);
	int maxxcblocks = (int)(maxNSBetweenXCAvg/blockns);
	maxxcblocks -= maxxcblocks%numbufferedffts;
	if(maxxcblocks == 0)
	{
		maxxcblocks = numbufferedffts;
		cerr << "fxcorr-xpp: requested cross-correlation shift/average time of " << maxNSBetweenXCAvg << " ns cannot be met with " << numbufferedffts << " FFTs being buffered; the time resolution which will be attained is " << maxxcblocks*blockns << " ns" << endl;
	}

	// autocorrelation averaging batch, the same formula as fxcorr-f main /
	// core.cpp:769-783; it sizes the ac records the station stage collects
	int maxacblocks = (int)(model->getMaxNSBetweenACAvg(scan)/blockns);
	maxacblocks -= maxacblocks%numbufferedffts;
	if(maxacblocks == 0)
	{
		maxacblocks = numbufferedffts;
		cerr << "fxcorr-xpp: requested autocorrelation shift/average time of " << model->getMaxNSBetweenACAvg(scan) << " ns cannot be met with " << numbufferedffts << " FFTs being buffered; the time resolution which will be attained is " << maxacblocks*blockns << " ns" << endl;
	}

	int fftloops = (blockspersend + numbufferedffts - 1)/numbufferedffts;
	int integrationswritten = 0;
	long long validblocks = 0;	// over every stream record and subint, see countValidBits
	// per-subint autocorrelation batch records, rebuilt by each station stage
	// run and consumed by Integrator::addAutocorrs after the XMAC stage
	vector<vector<AutocorrBatch> > acrecords(numdatastreams);
	// subints per intTime for the PCAL text file (fxcorr-f main: run_batch.sh
	// validates intTime is a subint multiple, so boundaries never split a batch)
	double inttime = config.getIntTime(configindex);
	long long subintsperint = (long long)(inttime*1.0e9/(double)subintns + 0.5);
	if(subintsperint < 1)
		return fail(monitor, "fxcorr-xpp: intTime " + to_string(inttime)
		            + " s is smaller than one subint (" + to_string(subintns) + " ns)");
	// batch start in day-seconds, the PCAL text file's flush time base
	// (fxcorr-f main: config start seconds + job-relative batch start)
	long long batchstartsec = (long long)config.getStartSeconds() + (long long)floor(batchstartjob);
	int batchstartns = (int)((batchstartjob - (double)floor(batchstartjob))*1.0e9 + 0.5);
	if(batchstartns >= 1000000000)
	{
		batchstartsec++;
		batchstartns -= 1000000000;
	}
	long long batchstartabsns = batchstartsec*1000000000LL + batchstartns;
	// FXCORR_TIMING_TRACE=1 without a path: name the trace dump after the
	// workdir and batch, which are only known here
	if(g_timing_trace && g_timing_out == stderr)
	{
		string tp = workdir + "/fxcorr-xpp-" + batchid + ".timing.tsv";
		FILE *f = fopen(tp.c_str(), "a");
		if(f != 0)
			g_timing_out = f;
		else
			cerr << "fxcorr-xpp: cannot open timing file " << tp << ", using stderr" << endl;
	}
	for(int s=0;s<nsubints;s++)
	{
		StageTimer st;
		// this subint's grid time stamps (same formula as fxcorr-f main and
		// fxcorr-x, so expectedsec/expectedns line up with the records)
		double subintstart = batchstartjob + (double)s*(double)subintns/1.0e9;
		double sreld = subintstart - (double)scanstartsec;
		int expectedsec = (int)floor(sreld);
		int expectedns = (int)((sreld - (double)expectedsec)*1.0e9 + 0.5);
		// scan-relative seconds of the subint start, same time base as the
		// upstream delay interpolator (offsets[1] + offsets[2]/1e9)
		double offsetsec = (double)expectedsec + (double)expectedns/1.0e9;

		// read each datastream's stream record: it carries the loop's
		// offsetsec/offsetns, which must match this run's grid (the same
		// cross-check fxcorr-x applied to the .sp headers), plus the
		// per-block validity of the empty-input guard below
		for(int ds=0;ds<numdatastreams;ds++)
		{
			if(!dsactive[ds])
				continue;
			if(!streams[ds]->readSubint(s))
			{
				ostringstream oss;
				oss << "fxcorr-xpp: failed to read subint " << s << " of station "
				    << config.getDStationName(configindex, ds);
				return fail(monitor, oss.str());
			}
			if(streams[ds]->offsetSec() != expectedsec || streams[ds]->offsetNs() != expectedns)
			{
				ostringstream oss;
				oss << "fxcorr-xpp: subint " << s << " of station "
				    << config.getDStationName(configindex, ds) << " has offsets "
				    << streams[ds]->offsetSec() << "/" << streams[ds]->offsetNs()
				    << ", expected " << expectedsec << "/" << expectedns;
				return fail(monitor, oss.str());
			}
			validblocks += countValidBits(streams[ds]->flags(), (u32)blockspersend,
			                              streams[ds]->flagWords());
		}

		g_tt_read += STAGE_LAP(st, "read");

		// ---- station stage (fxcorr-f's per-datastream stack, D5/D8) --------
		// One OpenMP iteration per datastream: each thread owns its Mode and
		// walks that datastream's blocks in block order, so every
		// accumulation sequence stays the serial one and no cross-thread
		// per-block reduction is needed (D8).  What fxcorr-f wrote to disk per
		// fftloop happens in memory here -- the band views are filled by
		// MemReader::collectBand, the ac batches are collected into
		// addAutocorrs's records, and pcal.bin / the PCAL text file are
		// written per subint, all in fxcorr-f's exact order.
		{
			StageTimer ts;
			for(int ds=0;ds<numdatastreams;ds++)
				acrecords[ds].clear();
			// static (not dynamic): gcc's libgomp deadlocks when a
			// dynamic-scheduled outer loop creates nested parallel regions
			// per iteration (the optional band parallelism inside process)
			// band parallelism cannot nest inside the ds OpenMP loop: gcc's
			// libgomp deadlocks when an outer parallel loop keeps creating
			// nested parallel regions.  In band mode the datastreams run
			// serially and the band loop is the top-level parallel region.
			auto stationDs = [&](int ds)
			{
				if(!dsactive[ds])
					return;
				Mode *m = modes[ds];
				StreamReader *sr = streams[ds];
				int nrecordedbands = config.getDNumRecordedBands(configindex, ds);

				// fxcorr-f's per-subint setup, in its main-loop order; every
				// replica sees the same read-only stream record
				auto setupMode = [&](Mode *sm)
				{
					sm->zeroAutocorrelations();
					sm->setValidFlags((s32 *)sr->flags());
					sm->setData((u8 *)sr->data(), (int)sr->validBytes(), sr->scan(),
					            sr->sec(), sr->ns());
					sm->setOffsets(sr->scan(), sr->offsetSec(), sr->offsetNs());
					if(pcaltexts[ds] != 0)
						sm->resetpcal();
					if(dokurtosis)
						sm->zeroKurtosis();	// core.cpp:703-704
				};
				setupMode(m);
				for(size_t rt=0;rt<modereplicas[ds].size();rt++)
					setupMode(modereplicas[ds][rt]);

				int acblockcount = 0;
				int acshiftcount = 0;
				if(blockthreads > 1 && maxacblocks >= blockspersend)
				{
					// whole-subint block parallelism: each segment processes
					// one contiguous stretch of this subint's blocks and the
					// reduction/collection runs once at the end.  The Modes'
					// slots are global block indices (setSubloopCapacity), so
					// process(i, i) needs no slot remapping.
					const int nseg = blockthreads;
					SegmentPool *pool = segpools[ds];
					pool->run(nseg, [&](int seg)
					{
						Mode *wm = (seg == 0) ? m : modereplicas[ds][seg-1];
						int bstart = (int)((long long)seg*blockspersend/nseg);
						int bend = (int)((long long)(seg+1)*blockspersend/nseg);
						for(int i=bstart;i<bend;i++)
							wm->process(i, i);
						// publish this segment's spectra/weights into the
						// primary's slots (MemReader and the ac collector
						// only ever look at the primary Mode)
						if(seg != 0)
						{
							for(int i=bstart;i<bend;i++)
							{
								for(int j=0;j<nrecordedbands;j++)
								{
									int freqindex = config.getDRecordedFreqIndex(configindex, ds, j);
									int nchan = config.getFNumChannels(freqindex);
									memcpy(m->getFreqsWrite(j, i), wm->getFreqs(j, i), sizeof(cf32)*nchan);
									m->setDataWeight(j, i, wm->getDataWeight(j, i));
								}
							}
						}
					});

					// block-ordered reduction: slot = global block index
					int crosspols = m->writeCrossAutoCorrs() ? 2 : 1;
					for(int cp=0;cp<crosspols;cp++)
					{
						for(int j=0;j<nrecordedbands;j++)
						{
							int freqindex = config.getDRecordedFreqIndex(configindex, ds, j);
							int nchan = config.getFNumChannels(freqindex);
							cf32 *dst = m->getAutocorrelation(cp, j);
							f32 w = m->getWeightReduced(cp, j);
							for(int seg=0;seg<nseg;seg++)
							{
								Mode *rm = (seg == 0) ? m : modereplicas[ds][seg-1];
								int bstart = (int)((long long)seg*blockspersend/nseg);
								int bend = (int)((long long)(seg+1)*blockspersend/nseg);
								for(int i=bstart;i<bend;i++)
								{
									const cf32 *src = rm->getBlockAc(cp, i, j);
									for(int k=0;k<nchan;k++)
									{
										dst[k].re += src[k].re;
										dst[k].im += src[k].im;
									}
									w += rm->getBlockWeight(cp, i, j);
								}
							}
							m->setWeightReduced(cp, j, w);
						}
					}
					if(pcaltexts[ds] != 0)
					{
						for(int j=0;j<nrecordedbands;j++)
						{
							for(int seg=0;seg<nseg;seg++)
							{
								Mode *rm = (seg == 0) ? m : modereplicas[ds][seg-1];
								int bstart = (int)((long long)seg*blockspersend/nseg);
								int bend = (int)((long long)(seg+1)*blockspersend/nseg);
								for(int i=bstart;i<bend;i++)
								{
									uint64_t samples = 0;
									const f32 *accum = rm->getBlockPcal(j, i, &samples);
									m->addPcalBlock(j, accum, samples);
								}
							}
						}
					}
					if(dokurtosis)
					{
						for(size_t rt=0;rt<modereplicas[ds].size();rt++)
						{
							Mode *rm = modereplicas[ds][rt];
							for(int j=0;j<nrecordedbands;j++)
							{
								int freqindex = config.getDRecordedFreqIndex(configindex, ds, j);
								int nchan = config.getFNumChannels(freqindex);
								m->addKurtosisProducts(j, rm->getKurtosisProducts1(j),
								                       rm->getKurtosisProducts2(j), nchan);
							}
						}
					}

					// spectrum views: the slots are global block indices
					for(int fftloop=0;fftloop<fftloops;fftloop++)
						for(size_t band=0;band<readers[ds].size();band++)
							static_cast<MemReader *>(readers[ds][band])->collectBand(fftloop, true);

					// autocorrelation averaging batch (one per subint; the
					// block form requires maxacblocks >= blockspersend)
					acblockcount += blockspersend;
					if(acblockcount == maxacblocks)
					{
						if(pcaltexts[ds] != 0)
							pcaltexts[ds]->accumulateWeight(m);
						bool datastreamsaveraged = false;
						if(dosta && config.getMinPostAvFreqChannels(configindex) >= config.getSTADumpChannels())
						{
							m->averageFrequency();
							datastreamsaveraged = true;
						}
						if(dosta)
						{
							#pragma omp critical(fxcorr_sta)
							{
								sendSTA(&monitor, config, configindex, ds, m, scan, scanstartsec,
								        expectedsec, expectedns,
								        (acshiftcount*maxacblocks + maxacblocks/2.0)*blockns,
								        maxacblocks*blockns, datastreamsaveraged, config.getJobName());
							}
						}
						acrecords[ds].push_back(accollectors[ds]->collect(m, datastreamsaveraged));
						m->zeroAutocorrelations();
						acblockcount = 0;
						acshiftcount++;
					}
					// the replica's accumulation window is one subint
					for(size_t rt=0;rt<modereplicas[ds].size();rt++)
					{
						Mode *rm = modereplicas[ds][rt];
						rm->zeroAutocorrelations();
						if(dokurtosis)
							rm->zeroKurtosis();
						if(pcaltexts[ds] != 0)
							rm->resetpcal();
					}
				}
				else
				for(int fftloop=0;fftloop<fftloops;fftloop++)
				{
					int numffts = blockspersend - fftloop*numbufferedffts;
					if(numffts > numbufferedffts)
						numffts = numbufferedffts;

					// block-ordered processing; slot = the global block index
					// (blockslotbase = 0, set where the Mode was built)
					for(int b=0;b<numffts;b++)
						m->process(fftloop*numbufferedffts + b, b);

					// fxcorr-f's per-fftloop reduction at nthreads = 1: fold
					// the per-block accumulators back in block order, so the
					// autocorrelations and weights equal the serial sums
					int crosspols = m->writeCrossAutoCorrs() ? 2 : 1;
					for(int cp=0;cp<crosspols;cp++)
					{
						for(int j=0;j<nrecordedbands;j++)
						{
							int freqindex = config.getDRecordedFreqIndex(configindex, ds, j);
							int nchan = config.getFNumChannels(freqindex);
							cf32 *dst = m->getAutocorrelation(cp, j);
							f32 w = m->getWeightReduced(cp, j);
							for(int b=0;b<numffts;b++)
							{
								int slot = fftloop*numbufferedffts + b;
								const cf32 *src = m->getBlockAc(cp, slot, j);
								for(int k=0;k<nchan;k++)
								{
									dst[k].re += src[k].re;
									dst[k].im += src[k].im;
								}
								w += m->getBlockWeight(cp, slot, j);
							}
							m->setWeightReduced(cp, j, w);
						}
					}
					// pcal: same walk, one band's blocks in block order.
					// getBlockPcal returns null when the extractor cannot
					// park accumulators and addPcalBlock is then a no-op --
					// process() accumulated the extractor directly instead.
					if(pcaltexts[ds] != 0)
					{
						for(int j=0;j<nrecordedbands;j++)
						{
							for(int b=0;b<numffts;b++)
							{
								int slot = fftloop*numbufferedffts + b;
								uint64_t samples = 0;
								const f32 *accum = m->getBlockPcal(j, slot, &samples);
								m->addPcalBlock(j, accum, samples);
							}
						}
					}

					// the spectrum views, taken before the slots are reused
					// (the old writeSpectraBand position in the fftloop)
					for(size_t band=0;band<readers[ds].size();band++)
						static_cast<MemReader *>(readers[ds][band])->collectBand(fftloop);

					// autocorrelation averaging batch boundary (core.cpp:993-1003)
					acblockcount += numffts;
					if(acblockcount == maxacblocks)
					{
						if(pcaltexts[ds] != 0)
							pcaltexts[ds]->accumulateWeight(m);	// before zeroAutocorrelations clears weights
						// P9: an STA dump may demand the post-average spectrum
						// (core.cpp:1181-1187): average before the dump and
						// let the record collector skip its own average
						bool datastreamsaveraged = false;
						if(dosta && config.getMinPostAvFreqChannels(configindex) >= config.getSTADumpChannels())
						{
							m->averageFrequency();
							datastreamsaveraged = true;
						}
						if(dosta)
						{
							// the station stage runs one thread per datastream;
							// fxcorr-f only ever sent STA from thread 0
							#pragma omp critical(fxcorr_sta)
							{
								sendSTA(&monitor, config, configindex, ds, m, scan, scanstartsec,
								        expectedsec, expectedns,
								        (acshiftcount*maxacblocks + maxacblocks/2.0)*blockns,
								        maxacblocks*blockns, datastreamsaveraged, config.getJobName());
							}
						}
						acrecords[ds].push_back(accollectors[ds]->collect(m, datastreamsaveraged));
						m->zeroAutocorrelations();
						acblockcount = 0;
						acshiftcount++;
					}
				}
				// trailing partial batch (fxcorr-f main, same as core.cpp)
				if(acblockcount != 0)
				{
					if(pcaltexts[ds] != 0)
						pcaltexts[ds]->accumulateWeight(m);
					// P9: same pre-STA averaging as the full batch above
					bool datastreamsaveraged = false;
					if(dosta && config.getMinPostAvFreqChannels(configindex) >= config.getSTADumpChannels())
					{
						m->averageFrequency();
						datastreamsaveraged = true;
					}
					if(dosta)
					{
						#pragma omp critical(fxcorr_sta)
						{
							sendSTA(&monitor, config, configindex, ds, m, scan, scanstartsec,
							        expectedsec, expectedns,
							        (acshiftcount*maxacblocks + acblockcount/2.0)*blockns,
							        acblockcount*blockns, datastreamsaveraged, config.getJobName());
						}
					}
					acrecords[ds].push_back(accollectors[ds]->collect(m, datastreamsaveraged));
					m->zeroAutocorrelations();
				}

				// per-subint pcal products (fxcorr-f main order: pcal.bin,
				// then the text accumulator, then the intTime boundary flush)
				if(pcaltexts[ds] != 0)
				{
					pcalwriters[ds]->writeSubint(m);
					pcaltexts[ds]->accumulate(m);
					if(((long long)(s+1)) % subintsperint == 0)
					{
						// start of the intTime that just completed
						long long totalns = batchstartabsns + (long long)(s+1-subintsperint)*(long long)subintns;
						pcaltexts[ds]->flush(totalns/1000000000LL, (int)(totalns%1000000000LL));
					}
				}

				// P9: kurtosis covers the whole subint, not an ac batch
				// (core.cpp:1062-1063; single-thread numblocks = blockspersend)
				if(dokurtosis)
				{
					#pragma omp critical(fxcorr_sta)
					{
						sendKurtosis(&monitor, config, configindex, ds, m, scan, scanstartsec,
						             expectedsec, expectedns,
						             (blockspersend/2.0)*blockns, (double)blockspersend*blockns,
						             blockspersend, config.getJobName());
					}
				}
			};
			if(bandthreads > 1)
			{
				// band parallelism owns the whole core budget, so the
				// datastreams run serially (no nested teams)
				for(int ds=0;ds<numdatastreams;ds++)
					stationDs(ds);
			}
			else
			{
				#pragma omp parallel for schedule(dynamic)
				for(int ds=0;ds<numdatastreams;ds++)
					stationDs(ds);
			}
			g_tt_station += STAGE_LAP(ts, "station");
		}
		// zero the per-subint accumulation (threadcrosscorrs, baselineweights, results).
		// Own timer: the station stage sits between the read lap and this one, and
		// reusing st would charge the whole station stage to zero.
		StageTimer tz;
		xmac.zeroSubint();
		vectorZero_cf32(subintresults, coreresultlength);
		g_tt_zero += STAGE_LAP(tz, "zero");

		// XMAC + uvshift/average, batched exactly like core.cpp:786-1057
		int xcblockcount = 0, xcshiftcount = 0;
		for(int fftloop=0;fftloop<fftloops;fftloop++)
		{
			int numffts = blockspersend - fftloop*numbufferedffts;
			if(numffts > numbufferedffts)
				numffts = numbufferedffts;

			{
				StageTimer tx;
				xmac.xmacBatch(fftloop, readers, 0);
				g_tt_xmac += STAGE_LAP(tx, "xmac");
				xmac.accumulateWeights(fftloop, readers);
				g_tt_wacc += STAGE_LAP(tx, "wacc");
			}
			xcblockcount += numffts;
			if(xcblockcount == maxxcblocks)
			{
				double nsoffset = (xcshiftcount*maxxcblocks + ((double)maxxcblocks)/2.0)*blockns;
				StageTimer tu;
				xmac.uvshiftAndAverage(offsetsec, nsoffset, maxxcblocks*blockns, 0, subintresults);
				g_tt_uvshift += STAGE_LAP(tu, "uvshift");
				xcblockcount = 0;
				xcshiftcount++;
			}
		}
		if(xcblockcount != 0)
		{
			double nsoffset = (xcshiftcount*maxxcblocks + ((double)xcblockcount)/2.0)*blockns;
			StageTimer tu;
			xmac.uvshiftAndAverage(offsetsec, nsoffset, xcblockcount*blockns, 0, subintresults);
			g_tt_uvshift += STAGE_LAP(tu, "uvshift");
		}

		// baseline weights -> floatresults section (core.cpp:1065-1107, no locks)
		{
			StageTimer tc;
			xmac.copyBaselineWeights((f32*)subintresults);
			g_tt_cbw += STAGE_LAP(tc, "cbw");
		}

		// autocorrelations come from this subint's station-stage records
		// (v9: in memory, produced above; one list per datastream)
		{
			StageTimer ta;
			integrator.addAutocorrs(acrecords, subintresults);
			g_tt_auto += STAGE_LAP(ta, "auto");
		}

		{
			StageTimer ts;
			if(integrator.addSubint(subintresults))
				integrationswritten++;
			g_tt_addsub += STAGE_LAP(ts, "addsub");
		}
	}

	// Empty-input / empty-output guards (v6-plan.md "进行中的发现" 2).  Two
	// complementary checks, both aimed at the worst shape a silent corruption
	// can take here: every program exits 0, the log line looks normal, and the
	// product is an empty file.
	//
	//   F: not one valid FFT block in the whole batch.  fxcorr-fp writes the
	//      validity of every block into the stream record (the same flags
	//      fxcorr-f put into the .sp), so this says exactly "the raw data was
	//      never there" - typically because batch.json's time axis does not
	//      match what the raw files hold (a relink left over from another
	//      batch, or a VDIF regenerated under a different batch duration).
	//      fp then writes all-invalid records and xp happily integrates them,
	//      because the weight gate that drops the records is
	//      indistinguishable from "no data".
	//   C: data was fine but nothing reached the SWIN - e.g. every baseline
	//      weight came out zero.  Sharded runs write .part files that the
	//      batch-level merge collects, so they are not checked here.
	bool allowempty = false;
	if(const char *v = getenv("FXCORR_X_ALLOW_EMPTY"))
		allowempty = (strcmp(v, "1") == 0 || strcmp(v, "allow") == 0);

	if(validblocks == 0 && nsubints > 0 && !allowempty)
	{
		ostringstream oss;
		oss << "fxcorr-xpp: batch " << batchid << " read no valid data at all: every FFT block of every subint is flagged invalid in all "
		    << numdatastreams << " datastream(s) (start_mjd " << startmjd << ", " << nsubints << " subints of " << subintns << " ns).  "
		       "fxcorr-fp built the stream records from raw data that does not cover this batch's time range - check that the files "
		       "behind the DATA TABLE really hold this batch (run_batch.sh relinks them from "
		       "<raw>/<station>/<station>_<batch_id>[_ds<N>].vdif) and that the raw data was generated with the batch duration this "
		       ".input describes.  Without this check the run would finish with a normal-looking log line and an SWIN holding no "
		       "records.  Set FXCORR_X_ALLOW_EMPTY=1 if an empty batch is really what you want.";
		return fail(monitor, oss.str());
	}

	FXLOG(FXLOG_INFO) << "fxcorr-xpp: batch " << batchid << " complete, " << nsubints << " subints, " << integrationswritten << " integrations written" << endl;

	if(!sharded && integrationswritten > 0 && !allowempty)
	{
		char swinpath[4096];
		snprintf(swinpath, sizeof(swinpath), "%s/DIFX_%05d_%06d.s0000.b0000",
		         config.getOutputFilename().c_str(), config.getStartMJD(), config.getStartSeconds());
		double duration = (double)nsubints * (double)subintns / 1.0e9;
		long long nrecords = 0;
		swinHasBatchRange(swinpath, config, startmjd, duration, &nrecords);
		if(nrecords == 0)
		{
			ostringstream oss;
			oss << "fxcorr-xpp: batch " << batchid << " finished " << integrationswritten << " integration(s) but " << swinpath
			    << " holds no record inside this batch's time range (start_mjd " << startmjd << ", duration " << duration << " s).  "
			       "The stream input was not empty (valid FFT blocks were read), so this is not a raw/layout mismatch: either every "
			       "baseline weight came out zero, or the output went somewhere other than this file.  Set FXCORR_X_ALLOW_EMPTY=1 "
			       "to accept it.";
			return fail(monitor, oss.str());
		}
	}

	for(int ds=0;ds<numdatastreams;ds++)
	{
		for(size_t band=0;band<readers[ds].size();band++)
			delete readers[ds][band];
		delete accollectors[ds];
		delete streams[ds];
		if(pcalwriters[ds] != 0)
		{
			// the last pcal products must land before the task calls itself
			// done -- same discipline as fxcorr-f's FEngineWriter::finalise
			// (a full filesystem has to fail here, v7-plan 17.2)
			if(!pcalwriters[ds]->finalise())
				return fail(monitor, "fxcorr-xpp: station products for batch " + batchid + " are incomplete (see above)");
			delete pcalwriters[ds];
		}
		if(pcaltexts[ds] != 0)
		{
			if(pcaltexts[ds]->hasUnflushed())
				return fail(monitor, "fxcorr-xpp: batch " + batchid + " ends with unflushed pcal tones (batch length is not an intTime multiple)");
			delete pcaltexts[ds];
		}
		delete segpools[ds];
		for(size_t rt=0;rt<modereplicas[ds].size();rt++)
			delete modereplicas[ds][rt];
		delete modes[ds];
	}
	vectorFree(subintresults);

	// upstream ending sequence (fxmanager.cpp terminate/DONE): Ending then Done
	monitor.status(DIFX_STATE_ENDING, "", 0.0, 0, 0, 0.0, 0.0);
	monitor.status(DIFX_STATE_DONE, "", 0.0, 0, 0, 0.0, 0.0);

	if(g_timing && !merge)
	{
		if(!g_timing_trace && !g_timing_brief)
		{
			double total = nowsec() - g_tt_start;
			fprintf(g_timing_out,
			        "TIMING fxcorr-xpp batch=%s dsgroup=%s nthreads=%d nsubints=%d"
			        " total=%g read=%g station=%g zero=%g xmac=%g wacc=%g uvshift=%g"
			        " cbw=%g auto=%g addsub=%g\n",
			        batchid.c_str(), (sharded ? to_string(dsgroup) : string("all")).c_str(),
			        omp_get_max_threads(), nsubints, total,
			        g_tt_read, g_tt_station, g_tt_zero, g_tt_xmac, g_tt_wacc,
			        g_tt_uvshift, g_tt_cbw, g_tt_auto, g_tt_addsub);
			fflush(g_timing_out);
		}
		timing_finish();	// brief rows / trace buffer
	}

	return EXIT_SUCCESS;
}
