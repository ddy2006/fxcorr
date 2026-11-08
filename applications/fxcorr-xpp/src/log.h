#ifndef FXCORR_LOG_H
#define FXCORR_LOG_H

#include <cstdlib>
#include <cstring>
#include <iostream>

#include <fxcorrcommon/alert.h>
#include <difxmessage.h>

// How much of a tool's stderr chatter to emit, selected by FXCORR_LOGLEVEL
// (error, warn, info, verbose, debug).
//
// The default, info, keeps errors, warnings and the one-line summaries that
// say how a run went; the per-event detail behind those summaries is
// interesting only when something looks wrong, so it sits at verbose.
//
// Used as
//	FXLOG(FXLOG_VERBOSE) << "detail " << x << endl;
// which checks the level before anything is formatted.
//
// The tool's own prints are only half the chatter: fxcorrcommon is built
// from upstream code that reports through the Alert streams (cinfo, cdebug,
// ...) rather than FXLOG, so the level has to cover those too -- call
// fxApplyAlertLevel() early in main, before anything that can speak.  See
// that function for why those streams print instead of multicasting.

enum FxLogLevel
{
	FXLOG_ERROR = 0,
	FXLOG_WARN = 1,
	FXLOG_INFO = 2,
	FXLOG_VERBOSE = 3,
	FXLOG_DEBUG = 4
};

// Read once -- the level does not change while a tool runs.  An unrecognised
// value falls back to the default and says so rather than silently ignoring
// what is likely a typo.
inline int fxLogLevel()
{
	static int level = -1;
	if(level < 0)
	{
		level = FXLOG_INFO;
		if(const char *env = getenv("FXCORR_LOGLEVEL"))
		{
			if(strcmp(env, "error") == 0)
				level = FXLOG_ERROR;
			else if(strcmp(env, "warn") == 0)
				level = FXLOG_WARN;
			else if(strcmp(env, "info") == 0)
				level = FXLOG_INFO;
			else if(strcmp(env, "verbose") == 0)
				level = FXLOG_VERBOSE;
			else if(strcmp(env, "debug") == 0)
				level = FXLOG_DEBUG;
			else
				std::cerr << "FXCORR_LOGLEVEL: unknown level \"" << env
				          << "\" (error/warn/info/verbose/debug); using info"
				          << std::endl;
		}
	}
	return level;
}

// The Alert streams are the other half of the chatter: fxcorrcommon is built
// from upstream DiFX code, which reports progress and problems through
// cinfo, cdebug and friends rather than through stderr.  Those sends go out
// via difxMessageSendDifxAlert, which falls back to printing on stdout or
// stderr whenever difxMessagePort is unset -- and no fxcorr tool calls
// difxMessageInit, so the port keeps its -1 initial value and the fallback
// is the only path ever taken.  Multicasting never happens here; the choice
// is between printing and staying quiet, which is what this decides.
//
// Aligned with the FXLOG levels, one stream per level: cerror, csevere and
// cfatal stay on at every level (an error is never worth suppressing), and
// each stream below that is dropped once the selected level is above it.
inline void fxApplyAlertLevel()
{
	int level = fxLogLevel();

	if(level < FXLOG_DEBUG)
		cdebug.setAlertLevel(DIFX_ALERT_LEVEL_DO_NOT_SEND);
	if(level < FXLOG_VERBOSE)
		cverbose.setAlertLevel(DIFX_ALERT_LEVEL_DO_NOT_SEND);
	if(level < FXLOG_INFO)
		cinfo.setAlertLevel(DIFX_ALERT_LEVEL_DO_NOT_SEND);
	if(level < FXLOG_WARN)
		cwarn.setAlertLevel(DIFX_ALERT_LEVEL_DO_NOT_SEND);
}

#define FXLOG(lvl) if(fxLogLevel() >= (lvl)) std::cerr

#endif
