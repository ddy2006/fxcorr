AC_INIT([fxcorrcommon],[0.1],[kaichao])

AC_CONFIG_HEADERS([config.h])

#shared library versioning
LIBRARY_VERSION=0:0:0
#               | | |
#        +------+ | +---+
#        |        |     |
#     current:revision:age
#        |        |     |
#        |        |     +- increment if interfaces have been added
#        |        |        set to zero if interfaces have been removed
#        |        |           or changed
#        |        +- increment if source code has changed
#        |           set to zero if current is incremented
#        +- increment if interfaces have been added, removed or changed

PACKAGE_VERSION=AC_PACKAGE_VERSION

AC_SUBST([PACKAGE_VERSION])
AC_SUBST([LIBRARY_VERSION])

AC_CONFIG_MACRO_DIR([m4])

AM_INIT_AUTOMAKE([foreign])

LT_INIT

AC_PROG_CXX
AC_LANG(C++)

# AX_OPENMP probes the *current* language: switch to C++ first, otherwise it
# sets OPENMP_CFLAGS while OPENMP_CXXFLAGS stays empty (the library then builds
# without -fopenmp and every "pragma omp" in it is silently ignored).
AX_OPENMP

AC_ARG_ENABLE(quiet, [AS_HELP_STRING([--enable-quiet],[disable some verbiage])], [CFLAG_QUIET="-DQUIET"], [CFLAG_QUIET=" "])

dnl **********************************************************
dnl **** Set path for Intel Performance Primitive library ****
dnl **********************************************************

dnl Off by default (upstream had it on).  None of the fxcorr tools need IPP --
dnl fxcorr-sim does its FFTs with fftw3f, fxcorr-f/x likewise -- while an IPP
dnl build actively hurts: architecture.h then maps vectorAlloc_f64 to
dnl ippsMalloc_64f, and the Model arrays allocated that way come out subtly
dnl wrong.  The visible effect is fxcorr-sim generating simulated data whose
dnl delays are off by about a sample, i.e. a few 2-bit quantiser levels
dnl flipping, silently.  Build it back in with --enable-ipp if something
dnl genuinely needs IPP acceleration.
ipp_enabled=0
AC_ARG_ENABLE(ipp, [AS_HELP_STRING([--enable-ipp],[enables IPP acceleration (off by default, see configure.ac)])])
if test "x$enable_ipp" = xyes; then
	ipp_enabled=1
fi
dnl The option has to gate the probe itself, not just ipp_enabled: the
dnl AC_DEFINE sits in PKG_CHECK_MODULES' success branch, so an unguarded
dnl probe defined HAVE_IPP even under --disable-ipp.  That mattered enough to
dnl fix here: in an IPP build architecture.h maps vectorAlloc_f64 to
dnl ippsMalloc_64f, and the Model arrays allocated that way come out subtly
dnl wrong, which makes fxcorr-sim's simulated delays wrong by a sample or so
dnl (found in the V6 S2.5 investigation; the symptom is a handful of 2-bit
dnl quantiser levels flipping, with no error anywhere).
dnl pkg-config has to be located at top level, ahead of that 'if'.  This is the
dnl first PKG_CHECK_MODULES in the file, and that macro AC_REQUIREs
dnl PKG_PROG_PKG_CONFIG -- so the search for the pkg-config program itself lands
dnl inside whichever shell branch the first use sits in.  Guarded away by
dnl ipp_enabled=0 (the default) the block never runs, PKG_CONFIG stays empty,
dnl and every later PKG_CHECK_MODULES reports "not found" without a word about
dnl why: a fresh build dies at "checking for FFTW3... no".
PKG_PROG_PKG_CONFIG

if test ${ipp_enabled} = 1; then
	PKG_CHECK_MODULES(IPP, ipp, [AC_DEFINE([HAVE_IPP], 1, [1=has IPP])], ipp_enabled=0)
fi
AC_SUBST(ipp_enabled)
dnl IPP headers are not on the default include path and libfxcorrcommon.a
dnl contains IPP calls: both the header path and the link flags must travel
dnl with the .pc file for downstream packages
IPP_PC_CFLAGS=""
IPP_PC_LIBS=""
if test ${ipp_enabled} = 1; then
	IPP_PC_CFLAGS="${IPP_CFLAGS}"
	IPP_PC_LIBS="${IPP_LIBS}"
fi
AC_SUBST(IPP_PC_CFLAGS)
AC_SUBST(IPP_PC_LIBS)
dnl **********************************************************

AC_CHECK_LIB(pthread, pthread_create,,[AC_MSG_ERROR("need libpthread")])

if test ${ipp_enabled} = 0; then
	PKG_CHECK_MODULES(FFTW3, fftw3, [hasfftw=true], [hasfftw=false])
	if test "$hasfftw" == "false"; then
		AC_MSG_ERROR([Error: fftw needs to be installed if IPP is not])
	fi
	PKG_CHECK_MODULES(FFTW3F, fftw3f, [hasfftwf=true], [hasfftwf=false])
	if test "$hasfftwf" == "false"; then
		AC_MSG_ERROR([Error: fftwf needs to be installed if IPP is not])
	fi
	MATH_LIBS="${FFTW3_LIBS} ${FFTW3F_LIBS}"
	MATH_CFLAGS="${FFTW3_CFLAGS} ${FFTW3F_CFLAGS}"
else
	MATH_LIBS="${IPP_LIBS} ${LIBS}"
	MATH_CFLAGS="${IPP_CFLAGS}"
fi

AC_SUBST(mathlibs, [${MATH_LIBS}])
AC_SUBST(mathcflags, [${MATH_CFLAGS}])

dnl **********************************************************
dnl **** Find difxmessage library                         ****
dnl **********************************************************
PKG_CHECK_MODULES(DIFXMESSAGE, [difxmessage >= 2.9.0])
CPPFLAGS="${CPPFLAGS} ${DIFXMESSAGE_CFLAGS}"

dnl **********************************************************
dnl **** Find vdifio library                              ****
dnl **********************************************************
PKG_CHECK_MODULES(VDIFIO, vdifio >= 1.6)
CPPFLAGS="${CPPFLAGS} ${VDIFIO_CFLAGS}"

dnl **********************************************************
dnl **** Find codifio library (CODIF_HEADER_BYTES)         ****
dnl **********************************************************
PKG_CHECK_MODULES(CODIFIO, codifio >= 0.2)
CPPFLAGS="${CPPFLAGS} ${CODIFIO_CFLAGS}"

dnl **********************************************************
dnl **** Find mark5access library                         ****
dnl **********************************************************
PKG_CHECK_MODULES(M5ACCESS, mark5access >= 1.7)

dnl for Mutex lock in datastream.cpp (kept for mode.cpp FFTinitMutex)
AC_CHECK_LIB(rt, clock_gettime)

AC_C_BIGENDIAN

CXXFLAGS="${CXXFLAGS} ${M5ACCESS_CFLAGS} ${VDIFIO_CFLAGS} ${CODIFIO_CFLAGS} ${MATH_CFLAGS} ${FFTW3_CFLAGS} ${DIFXMESSAGE_CFLAGS} ${OPENMP_CXXFLAGS} ${CFLAG_QUIET}"

LIBS="${M5ACCESS_LIBS} ${VDIFIO_LIBS} ${CODIFIO_LIBS} ${DIFXMESSAGE_LIBS} $LIBS"

dnl packaged for the .la LIBADD so libtool can resolve all symbols on
dnl platforms that require -no-undefined (macOS)
AC_SUBST(difxlibs, [${M5ACCESS_LIBS} ${VDIFIO_LIBS} ${CODIFIO_LIBS} ${DIFXMESSAGE_LIBS}])
AC_SUBST(difxcflags, [${M5ACCESS_CFLAGS} ${VDIFIO_CFLAGS} ${CODIFIO_CFLAGS} ${DIFXMESSAGE_CFLAGS}])

AC_CONFIG_FILES([
	Makefile \
	fxcorrcommon.pc \
	src/Makefile \
	src/architecture.h \
])

AC_OUTPUT
