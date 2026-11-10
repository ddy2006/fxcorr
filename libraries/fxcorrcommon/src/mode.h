/***************************************************************************
 *   Copyright (C) 2006-2017 by Adam Deller                                *
 *                                                                         *
 *   This program is free software: you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation, either version 3 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 *   This program is distributed in the hope that it will be useful,       *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU General Public License for more details.                          *
 *                                                                         *
 *   You should have received a copy of the GNU General Public License     *
 *   along with this program.  If not, see <http://www.gnu.org/licenses/>. *
 ***************************************************************************/
#ifndef MODE_H
#define MODE_H

#include "architecture.h"
#include "configuration.h"
#include "pcal.h"
#include <iostream>
#include <fstream>
#include <cstdlib>
#include <vector>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <functional>
#include <atomic>

using namespace std;

/**
@class Mode 
@brief Abstract superclass for all modes.  Provides station-based processing functionality

Possesses all methods necessary for mode functionality but should not be instantiated as it does not build a
lookup table or handle unpacking - this is particular to each subclass of mode.  Station-based processing
(FFT, fringe rotation, fraction sample correction etc) is handled via the process method, based on
the provided data and control (delay) arrays

@author Adam Deller
*/
class Mode{
public:
 /**
  * Constructor: allocates memory, extracts stream information and calculates number of lookups etc
  * @param conf The configuration object, containing all information about the duration and setup of this correlation
  * @param confindex The index of the configuration this Mode is for
  * @param dsindex The index of the datastream this Mode is for
  * @param recordedbandchan The number of channels for each recorded subband
  * @param chanstoavg The number of channels to average for each subband
  * @param bpersend The number of FFT blocks to be processed in a message
  * @param gsamples The number of additional guard samples at the end of a message
  * @param nrecordedfreqs The number of recorded frequencies for this Mode
  * @param recordedbw The bandwidth of each of these IFs
  * @param recordedfreqclkoffs The time offsets in microseconds to be applied post-F for each of the frequencies
  * @param recordedfreqclkoffsdelta The delay offsets in microseconds between Rcp and Lcp
  * @param recordedfreqphaseoffs The phase offsets in degrees between Rcp and Lcp
  * @param recordedfreqlooffs The LO offsets in Hz for each recorded frequency
  * @param nrecordedbands The total number of subbands recorded
  * @param nzoombands The number of subbands to be taken from within the recorded bands - can be zero
  * @param nbits The number of bits per sample
  * @param sampling The bit sampling type (real/complex)
  * @param tcomplex Type of complex sampling (single or double sideband)
  * @param unpacksamp The number of samples to unpack in one hit
  * @param fbank Whether to use a polyphase filterbank to channelise (instead of FFT)
  * @param linear2circular Whether to do a linear to circular conversion after the FFT
  * @param fringerotorder The interpolation order across an FFT (Oth, 1st or 2nd order; 0th = post-F)
  * @param arraystridelen The number of samples to stride when doing complex multiplies to implement sin/cos operations efficiently
  * @param cacorrs Whether cross-polarisation autocorrelations are to be calculated
  * @param bclock The recorder clock-out frequency in MHz ("block clock")
  */

Mode(Configuration * conf, int confindex, int dsindex, int recordedbandchan, int chanstoavg, int bpersend, int gsamples, int nrecordedfreqs, double recordedbw, double * recordedfreqclkoffs, double * recordedfreqclkoffsdelta, double * recordedfreqphaseoffs, double * recordedfreqlooffs, int nrecordedbands, int nzoombands, int nbits, Configuration::datasampling sampling, Configuration::complextype tcomplex, int unpacksamp, bool fbank, bool linear2circular, int fringerotorder, int arraystridelen, bool cacorrs, double bclock);

 /**
  * Stores the FFT valid flags for this block of data
  * @param v The array of valid flags for each FFT
  */
  void setValidFlags(s32 * v);

 /**
  * Stores the raw data for the current block series
  * @param d The data array
  * @param dbytes The number of bytes in the data array
  * @param dscan The scan from which the data comes
  * @param dsec The seconds offset from the start of the scan
  * @param dns The offset in nanoseconds from the integer second
  */
  void setData(u8 * d, int dbytes, int dscan, int dsec, int dns);

 /**
  * reset all pcal objects
  */
  void resetpcal();
  void finalisepcal();

 /**
  * Calculates fringe rotation and fractional sample correction arrays and FFTs, and autocorrelates
  * @param index The index of the FFT chunk to process
  * @param subloopindex The "subloop" index to put the output in
  */
  void process(int index, int subloopindex);

 /**
  * Optional intra-Mode band parallelism (fxcorr-xpp).  With nthreads > 1,
  * process() spreads the recorded-frequency loop over nthreads threads, each
  * using a private copy of the per-band scratch buffers.  Each band's
  * arithmetic is unchanged, so results are bit-identical to the serial path.
  * Default 0 (off): every other tool keeps the serial code and memory use.
  * Disabled for zoom configurations (bands can share a parent band).
  */
  void setBandParallel(int nthreads);
  bool bandParallelEnabled() const { return bandthreads > 1; }
  int bandParallelThreads() const { return bandthreads; }

 /**
  * Sets the autocorrelation arrays to contain 0's
  */
  void zeroAutocorrelations();

 /**
  * Sets the kurtosis arrays to contain 0's
  */
  void zeroKurtosis();

 /**
  * Stores the times for the first FFT chunk to be processed
  * @param scan The current scan
  * @param seconds The offset in seconds from the start of the scan
  * @param ns The offset in nanoseconds from the integer second
  */
  void setOffsets(int scan, int seconds, int ns);

 /**
  * Averages the autocorrelations down in frequency
  */
  void averageFrequency();

 /**
  * Calculates the kurtosis from intermediate products and averages down in frequency if requires
  * @param numblocks The number of FFTs that went into this average
  * @param maxchannels The number of channels to average down to
  * @return True if there are valid kurtosis data
  */
  bool calculateAndAverageKurtosis(int numblocks, int maxchannels);

 /**
  * Grabs the pointer to an autocorrelation array
  * @param crosspol Whether to return the crosspolarisation autocorrelation for this band
  * @param outputband The band index
  */
  inline cf32* getAutocorrelation(bool crosspol, int outputband) const { return autocorrelations[(crosspol)?1:0][outputband]; }

 /**
  * Grabs the pointer to a kurtosis array
  * @param outputband The band index
  */
  inline f32* getKurtosis(int outputband) const { return sk[outputband]; }

 /**
  * Grabs the weight for a given band
  * @param crosspol Whether to return the crosspolarisation autocorrelation for this band
  * @param outputband The band index
  */
  inline f32 getWeight(bool crosspol, int outputband) const { return weights[(crosspol)?1:0][outputband]; }

 /**
  * Grabs the data weight for a given band
  * @param outputband The band index
  * @param subloopindex The index into the number of buffered FFTs that were processed in one batch
  */
  inline f32 getDataWeight(int outputband, int subloopindex) const { return perbandweights ? perbandweights[subloopindex][outputband] : dataweight[subloopindex]; }

 /**
  * Gets the expected decorrelation ("van Vleck correction" ) for a given number of bits.
  * All cases other than 1 and 2 are approximate only!!!
  * @param nbits The number of bits
  * @return The square root of the decorrelation correction
  */
  static inline float getDecorrelationPercentage(int nbits) { return decorrelationpercentage[nbits-1]; }

 /**
  * @return Whether this Mode is writing cross-polarisation auto-corelations
  */
  inline bool writeCrossAutoCorrs() const { return calccrosspolautocorrs; }

 /**
  * @return Whether this Mode was initialied ok
  */
  inline bool initialisedOK() const { return initok; }

 /**
  * @param dk Whether to calculate the kurtosis or not
  */
  inline void setDumpKurtosis(bool dk) { dumpkurtosis = dk; }

 /**
  * Returns a pointer to the FFT'd data of the specified product
  * @param outputband The band to get
  * @param subloopindex The "subloop" index to get the visibilities from
  * @return Pointer to the FFT'd data (complex 32 bit float)
  */
  inline const cf32* getFreqs(int outputband, int subloopindex) const { return fftoutputs[outputband][subloopindex]; };

  /**
   * Number of buffered spectrum slots (fftoutputs / dataweight rows).
   * Normally the .input NUM BUFFERED FFTS; the block-parallel form raises it
   * to blockspersend so a whole subint can be processed without per-fftloop
   * recycling (see setSubloopCapacity).
   */
  inline int subloopCapacity() const { return fftslots; }

  /**
   * Raises the spectrum slot count to nslots (never lowers it).  Only
   * supported when the Mode owns every slot (no zoom bands) and the per-band
   * weights are not in use; returns false otherwise, and the caller must
   * fall back to the normal buffered path.  The extra slots hold global block
   * indices, so process(i, i) writes block i to slot i.
   */
  bool setSubloopCapacity(int nslots);

 /**
  * Returns a pointer to the FFT'd and conjugated data of the specified product
  * @param outputband The band to get
  * @param subloopindex The "subloop" index to get the visibilities from
  * @return Pointer to the conjugate of the FFT'd data (complex 32 bit float)
  */
  inline const cf32* getConjugatedFreqs(int outputband, int subloopindex) const { return conjfftoutputs[outputband][subloopindex]; }

 // ---- P3 reduction interfaces (fxcorr-f block-parallel replica reduction) ----
 // The primary Mode accumulates the replicas' per-block outputs in block
 // order; callers must not use these to reorder floating point operations.

 /** Writable FFT output slot (reduction copies a replica's slot here). */
  inline cf32* getFreqsWrite(int outputband, int subloopindex) { return fftoutputs[outputband][subloopindex]; }

 /** Sets the per-slot data weight (reduction copies a replica's slot value here). */
  inline void setDataWeight(int outputband, int subloopindex, f32 w)
  {
    if(perbandweights)
      perbandweights[subloopindex][outputband] = w;
    else
      dataweight[subloopindex] = w;
  }

 /** Accumulates a replica's band weight into this Mode's weight. */
  inline void addWeight(bool crosspol, int outputband, f32 w)
  {
    if(blockacblocks > 0)
      blockweight[(crosspol)?1:0][currentblock*numrecordedbands + outputband] += w;
    else
      weights[(crosspol)?1:0][outputband] += w;
  }

  /**
   * Adds to the running weight directly, bypassing the per-block slots.
   * Needed because the Mode doing the block-order reduction has the slots on
   * too (it is also a worker), so addWeight() would divert into them.
   */
  inline void addWeightReduced(bool crosspol, int outputband, f32 w) { weights[(crosspol)?1:0][outputband] += w; }

  /**
   * Read/write access to the running band weight, for a reduction that runs
   * on a thread other than this Mode's own (the parallel block-ordered
   * reduction splits the (crosspol, band) pairs over the team).  Each pair is
   * owned by exactly one thread, so the read-modify-write is race-free and
   * reproduces the serial add sequence: fold the blocks into the value read
   * here, then write it back.
   */
  inline f32 getWeightReduced(bool crosspol, int outputband) const { return weights[(crosspol)?1:0][outputband]; }
  inline void setWeightReduced(bool crosspol, int outputband, f32 w) { weights[(crosspol)?1:0][outputband] = w; }

  /**
   * Turns on the per-block accumulators (see blockac).  slotcount is the
   * number of block slots to allocate, i.e. how many blocks this Mode is
   * going to process per subint -- the fxcorr-f parallel form gives every
   * thread its own Mode and only that thread's stretch of blocks, so the
   * slots (and their clearing cost) scale with the per-thread work instead of
   * the whole subint.  Call setBlockSlotBase() before each parallel batch to
   * map a global block index onto a slot (slot = index - base).
   */
  void enableBlockAccumulators(int slotcount);

  /**
   * Sets the mapping from global block indices to this Mode's slots
   * (slot = index - base).  The parallel form calls this once per fftloop
   * with that thread's base.
   */
  inline void setBlockSlotBase(int base) { blockslotbase = base; }

  /**
   * Zeroes the per-block slots of one block.  The parallel form calls this
   * immediately before process() for the blocks a thread owns, which is both
   * correct (each block is processed exactly once per subint) and much
   * cheaper than zeroing every slot up front -- a worker touches only 1/N of
   * them, and zeroing all of them for all N Modes costs ~50 GB of writes per
   * batch on a 1310-block configuration.
   */
  void clearBlockAccumulators(int block);

  /**
   * Where process() should accumulate the autocorrelation of the block it is
   * working on: the per-block slot when the accumulators are on, the shared
   * per-band array otherwise (the original behaviour).
   */
  inline cf32* acTarget(int crosspol, int outputband) const
  {
    return (blockacblocks > 0) ? blockac[crosspol][currentblock][outputband]
                               : autocorrelations[crosspol][outputband];
  }

  /** Per-block autocorrelation slot; only valid after enableBlockAccumulators(). */
  inline cf32* getBlockAc(bool crosspol, int block, int outputband) const { return blockac[(crosspol)?1:0][block][outputband]; }

  /** Per-block weight slot; only valid after enableBlockAccumulators(). */
  inline f32 getBlockWeight(bool crosspol, int block, int outputband) const { return blockweight[(crosspol)?1:0][block*numrecordedbands + outputband]; }

 // ---- P3 (pcal): per-block accumulation slots (parallel form) ----
 // Parked together with the autocorrelation accumulators when every band's
 // pcal extractor exposes its raw accumulator (PCal::getAccumSize() > 0); the
 // parallel reduction adds the parked states back in block order, which is
 // the serial addition sequence bit for bit (see pcal.h).  The finalized
 // tone outputs must NOT be reduced -- see the note in pcal.h.

 /** True when every band's pcal extractor supports the per-block interface. */
 bool blockPcalCapable() const;

 /** True when the per-block pcal slots are on (the parallel pcal path). */
 inline bool hasBlockPcal() const { return blockpcal != 0; }

 /** Per-block pcal slot of one band, or NULL when the slots are off;
  *  *samples receives the block's integrated sample count. */
 inline const f32* getBlockPcal(int band, int block, uint64_t *samples) const
 {
   if(blockpcalsamples != 0)
     *samples = blockpcalsamples[block][band];
   return (blockpcal != 0) ? blockpcal[block][band] : 0;
 }

 /** Adds one parked block's pcal accumulation into this Mode's extractor. */
 void addPcalBlock(int band, const f32 *accum, uint64_t samples);

 /** Raw kurtosis products (s1/s2 accumulation) of a replica, for reduction. */
  inline const f32* getKurtosisProducts1(int outputband) const { return s1[outputband]; }
  inline const f32* getKurtosisProducts2(int outputband) const { return s2[outputband]; }

 /** Accumulates a replica's kurtosis products into this Mode's. */
  inline void addKurtosisProducts(int outputband, const f32 *s1src, const f32 *s2src, int nchan)
  {
    for(int k=0;k<nchan;k++)
    {
      s1[outputband][k] += s1src[k];
      s2[outputband][k] += s2src[k];
    }
  }

 /**
  * Returns the estimated number of bytes used by the Mode
  * @return Estimated memory size of the Mode (bytes)
  */
  inline long long getEstimatedBytes() const { return estimatedbytes; }

  virtual ~Mode();

  /** Constant for comparing two floats for equality (for freqs and bandwidths etc) */
  static const float TINY;

  /**
   * Returns a single pcal result.
   * @param outputband The band to get
   * @param tone The number of the tone to get
   * @return pcal result
   */
  inline cf32 getPcal(int outputband, int tone) const
  { 
    return pcalresults[outputband][tone]; 
  }
  
  ///constant indicating no valid data in a subint
  static const int INVALID_SUBINT = -99999999;

protected:
 /** 
  * Unpacks quantised data to float arrays.  The floating point values filled should
  * be in the range 0.0->1.0, and set appropriately to the expected input levels such that
  * the mean autocorrelation level at nominal sampler statistics is 0.??
  * @param sampleoffset The offset in number of time samples into the data array
  * @param subloopindex The "subloop" index that is currently being unpacked for (need to know to save weights in the right place)
  * @return The number of good samples unpacked scaled by the number of samples asked to unpack
  *         ie a weight in the range 0.0 to 1.0
  */
  virtual float unpack(int sampleoffset, int subloopindex);
  
  Configuration * config;
  int configindex, datastreamindex, recordedbandchannels, channelstoaverage, blockspersend, guardsamples, fftchannels, numrecordedfreqs, numrecordedbands, numzoombands, numbits, bytesperblocknumerator, bytesperblockdenominator, currentscan, offsetseconds, offsetns, order, flag, fftbuffersize, unpacksamples, unpackstartsamples, datasamples, avgdelsamples;
  long long estimatedbytes;
  int fringerotationorder, arraystridelength, numfrstrides, numfracstrides;
  double recordedbandwidth, blockclock, sampletime; //MHz, microseconds
  double a0, b0, c0, a, b, c, quadadd1, quadadd2;
  double fftstartmicrosec, fftdurationmicrosec, intclockseconds;
  int fftslots;	// spectrum slots; >= NUM BUFFERED FFTS in block mode
  f32 * dataweight;
  f32 ** perbandweights;
  int samplesperblock, samplesperlookup, numlookups, flaglength, autocorrwidth;
  int datascan, datasec, datans, datalengthbytes, usecomplex, usedouble;
  bool filterbank, calccrosspolautocorrs, fractionalLoFreq, initok, isfft, linear2circular;
  double * recordedfreqclockoffsets;
  double * recordedfreqclockoffsetsdelta;
  double * recordedfreqphaseoffset;
  double * recordedfreqlooffsets;
  bool deltapoloffsets, phasepoloffset;
  u8  *   data;
  s16 *   lookup;
  s16 *   linearunpacked;
  f32 **  unpackedarrays;
  cf32 **  unpackedcomplexarrays;
  cf32*** fftoutputs;
  cf32*** conjfftoutputs;
  f32 **  weights;
  s32 *   validflags;
  cf32*** autocorrelations;

  // P3 (fxcorr-f parallel form): per-block accumulators.
  //
  // The autocorrelation and its weight are the only *reduction*-type products
  // of process(): every FFT block adds into them, so the result depends on the
  // order the additions happen in.  The other products (fftoutputs, pcal,
  // kurtosis) are either position-indexed or small enough to ignore.
  //
  // fxcorr-f's parallel form gives each thread a different stretch of blocks,
  // so summing the per-thread totals reorders those additions relative to the
  // serial run and changes the last bits -- measured on the V7 target cluster
  // as ~0.5% of the autocorrelation values, which is exactly what SWIN's
  // autocorrelation section then disagreed on.  With these enabled, process()
  // parks each block's contribution in its own slot instead, and the primary
  // thread adds the slots up in block order -- the serial sequence again.
  //
  // NULL/0 (the default, and what mpifxcorr along with the serial form use)
  // keeps the original accumulation path untouched.
  //
  // Layout note for the GPU question: [crosspol][block][outputband][chan] is
  // already the flat, branch-free form a baseline-by-baseline kernel wants --
  // the natural parallel axes are (block, band, chan) and every lane does the
  // same multiply-accumulate.  Kept block-major so that a future accelerator
  // path can consume it without a transpose.
  cf32**** blockac;	// [crosspol][block][outputband] -> cf32*
  f32 **  blockweight;	// [crosspol][block][outputband]
  int     blockacblocks;	// slot count allocated; 0 = disabled
  int     currentblock;	// slot process() is accumulating into
  int     blockslotbase;	// slot = block index - blockslotbase
  // P3 (pcal): per-block pcal accumulation slots (see the interfaces above)
  f32 ***  blockpcal;		// [block][band][accum len]
  uint64_t ** blockpcalsamples;	// [block][band]
  int *    blockpcallen;	// [band] accumulator length in f32
  vecFFTSpecR_f32 * pFFTSpecR;
  vecFFTSpecC_cf32 * pFFTSpecC;
  vecDFTSpecR_f32 * pDFTSpecR;
  vecDFTSpecC_cf32 * pDFTSpecC;
  u8 * fftbuffer;
  vecHintAlg hint;
  Model * model;
  f64 * interpolator;

  //new arrays for strided complex multiply for fringe rotation and fractional sample correction
  cf32 * complexrotator;
  cf32 * complexunpacked;
  cf32 * fracsamprotatorA, * fracsamprotatorB;  // Allow different delay correction for each pol
  cf32 * fftd;

  // variables for pcal
  cf32 ** pcalresults;
  PCal ** extractor;
  
  f64 * subtoff;
  f64 * subtval;
  f64 * subxoff;
  f64 * subxval;
  f64 * subphase;
  f32 * subarg;
  f32 * subsin;
  f32 * subcos;

  f64 * steptoff;
  f64 * steptval;
  f64 * stepxoff;
  f64 * stepxval;
  f64 * stepphase;
  f32 * steparg;
  f32 * stepsin;
  f32 * stepcos;
  cf32 * stepcplx;

  f32 * subchannelfreqs;
  f32 * ldsbsubchannelfreqs;
  f32 * subfracsamparg;
  f32 * subfracsampsin;
  f32 * subfracsampcos;

  f32 * stepchannelfreqs;
  f32 * lsbstepchannelfreqs;
  f32 * dsbstepchannelfreqs;
  f32 * ldsbstepchannelfreqs;
  f32 * stepfracsamparg;
  f32 * stepfracsampsin;
  f32 * stepfracsampcos;
  cf32 * stepfracsampcplx;

  //extras necessary for quadratic (order == 2)
  cf32 * piecewiserotator;
  cf32 * quadpiecerotator;

  f64 * subquadxval;
  f64 * subquadphase;
  f32 * subquadarg;
  f32 * subquadsin;
  f32 * subquadcos;

  f64 * stepxoffsquared;
  f64 * tempstepxval;

  //kurtosis-specific variables
  bool dumpkurtosis;
  f32 *  kscratch; //[recordedbandchannels]
  f32 ** s1; //[numrecordedbands][recordedbandchannels]
  f32 ** s2; //[numrecordedbands][recordedbandchannels]
  f32 ** sk; //[numrecordedbands][recordedbandchannels]

  // Linear to circular conversion

  cf32 *phasecorrA, *phasecorrconjA, *phasecorrB, *phasecorrconjB; // 90 degrees + phase correction
  cf32 * tmpvec; 

private:
  ///Per-thread copy of the band-loop scratch buffers (band parallel mode).
  struct BandScratch
  {
    f64 *subphase;
    f32 *subarg, *subsin, *subcos;
    f64 *stepphase;
    f32 *steparg, *stepsin, *stepcos;
    cf32 *stepcplx;
    cf32 *complexunpacked, *complexrotator, *fftd;
    cf32 *fracsamprotatorA, *fracsamprotatorB;
    f32 *subfracsamparg, *subfracsampsin, *subfracsampcos;
    f32 *stepfracsamparg, *stepfracsampsin, *stepfracsampcos;
    cf32 *stepfracsampcplx;
    cf32 *piecewiserotator, *quadpiecerotator;
    f64 *subquadphase;
    f32 *subquadarg, *subquadsin, *subquadcos;
    f32 *kscratch;
    cf32 *tmpvec;
    vecFFTSpecR_f32 *fftspecR;
    vecFFTSpecC_cf32 *fftspecC;
    vecDFTSpecR_f32 *dftspecR;
    vecDFTSpecC_cf32 *dftspecC;
    u8 *fftbuffer;
    BandScratch();
  };
  int bandthreads;
  std::vector<BandScratch> bandscratch;
  struct BandPool;	// persistent worker threads (mode.cpp)
  BandPool *bandpool;
  void allocBandScratch(BandScratch &bs);
  void freeBandScratch(BandScratch &bs);

  ///Array containing decorrelation percentages for a given number of bits
  static const float decorrelationpercentage[];
};

/** 
 @class LBAMode 
 @brief A mode for 'standard' LBA 2 bit data

 Assumes data has been compressed if running at 128 Mbps or lower ie no redundancy.  Assumes running on a LITTLE-ENDIAN MACHINE!!!
 @author Adam Deller
 */
class LBAMode : public Mode{
public:
 /**
  * Constructor: calls Mode constructor then creates lookup table
  * @param conf The configuration object, containing all information about the duration and setup of this correlation
  * @param confindex The index of the configuration this Mode is for
  * @param dsindex The index of the datastream this Mode is for
  * @param nchan The number of channels per subband
  * @param chanstoavg The number of channels to average for each subband
  * @param bpersend The number of FFT blocks to be processed in a message
  * @param gblocks The number of additional guard blocks at the end of a message
  * @param nfreqs The number of frequencies for this Mode
  * @param bw The bandwidth of each of these IFs
  * @param recordedfreqclkoffs The time offsets in microseconds to be applied post-F for each of the frequencies
  * @param recordedfreqclkoffsdelta The offsets in microseconds between Rcp and Lcp
  * @param recordedfreqphaseoffs The phase offsets in degrees between Rcp and Lcp
  * @param recordedfreqlooffs The LO offsets in Hz for each recorded frequency
  * @param ninputbands The total number of subbands recorded
  * @param noutputbands The total number of subbands after prefiltering - not currently used (must be = numinputbands)
  * @param nbits The number of bits per sample
  * @param fbank Whether to use a polyphase filterbank to channelise (instead of FFT)
  * @param linear2circular Whether to do a linear to circular conversion after the FFT
  * @param fringerotorder The interpolation order across an FFT (Oth, 1st or 2nd order; 0th = post-F)
  * @param arraystridelen The number of samples to stride when doing complex multiplies to implement sin/cos operations efficiently
  * @param cacorrs Whether cross-polarisation autocorrelations are to be calculated
  * @param unpackvalues 4 element array containing floating point unpack values for the four possible two bit values
  */
  LBAMode(Configuration * conf, int confindex, int dsindex, int nchan, int chanstoavg, int bpersend, int gblocks, int nfreqs, double bw, double * recordedfreqclkoffs, double * recordedfreqclkoffsdelta,double * recordedfreqphaseoffs, double * recordedfreqlooffs, int ninputbands, int noutputbands, int nbits, bool fbank, bool linear2circular, int fringerotorder, int arraystridelen, bool cacorrs, const s16* unpackvalues);

    ///unpack mapping for "standard" recording modes
    static const s16 stdunpackvalues[];
    ///unpack mapping for "vsop-style" recording modes
    static const s16 vsopunpackvalues[];
};

/**
 @class LBA8BitMode
 @brief A mode for 'Bruce' style LBA 8 bit data

 Assumes running on a LITTLE-ENDIAN MACHINE!!!
 Also assumes Nyquist sampled clock
 @author Adam Deller
 */
class LBA8BitMode : public Mode{
public:
  LBA8BitMode(Configuration * conf, int confindex, int dsindex, int nchan, int chanstoavg, int bpersend, int gblocks, int nfreqs, double bw, double * recordedfreqclkoffs, double * recordedfreqclkoffsdelta, double * recordedfreqphaseoffs, double * recordedfreqlooffs, int ninputbands, int noutputbands, int nbits, bool fbank, bool linear2circular, int fringerotorder, int arraystridelen, bool cacorrs);

  virtual float unpack(int sampleoffset);
};

/**
 @class LBA16BitMode
 @brief A mode for 'Bruce' style LBA 16 bit data

 Assumes running on a LITTLE-ENDIAN MACHINE and the byte order of the 16 bit samples is little endian!!!
 Also assumes Nyquist sampled clock
 @author Adam Deller
 */
class LBA16BitMode : public Mode{
public:
  LBA16BitMode(Configuration * conf, int confindex, int dsindex, int nchan, int chanstoavg, int bpersend, int gblocks, int nfreqs, double bw, double * recordedfreqclkoffs, double * recordedfreqclkoffsdelta, double * recordedfreqphaseoffs, double * recordedfreqlooffs, int ninputbands, int noutputbands, int nbits, bool fbank, bool linear2circular, int fringerotorder, int arraystridelen, bool cacorrs);

  virtual float unpack(int sampleoffset);
};

#endif
// vim: shiftwidth=2:softtabstop=2:expandtab
