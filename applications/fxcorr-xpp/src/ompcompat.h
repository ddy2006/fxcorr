#ifndef OMPCOMPAT_H
#define OMPCOMPAT_H

/* OpenMP compatibility shim: with -fopenmp the compiler defines _OPENMP and
 * omp.h provides the runtime; without it (e.g. configure detected no OpenMP
 * support) the pragmas are ignored and these stubs keep the code serial, so
 * the build never depends on libgomp being present. */
#ifdef _OPENMP
#include <omp.h>
#else
static inline int omp_get_thread_num(void) { return 0; }
static inline int omp_get_max_threads(void) { return 1; }
static inline void omp_set_num_threads(int) {}
#endif

#endif
