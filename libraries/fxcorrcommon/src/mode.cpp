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

#include <iomanip>
#include "mode.h"
#include "math.h"
#include "architecture.h"
#include "alert.h"
#include "pcal.h"

#ifdef _OPENMP
#include <omp.h>
#else
static inline int omp_get_thread_num(void) { return 0; }
#endif

//using namespace std;
const float Mode::TINY = 0.000000001;

#if (ARCH == GENERIC)
pthread_mutex_t FFTinitMutex = PTHREAD_MUTEX_INITIALIZER;
#endif

Mode::Mode(Configuration * conf, int confindex, int dsindex, int recordedbandchan, int chanstoavg, int bpersend, int gsamples, int nrecordedfreqs, double recordedbw, double * recordedfreqclkoffs, double * recordedfreqclkoffsdelta, double * recordedfreqphaseoffs, double * recordedfreqlooffs, int nrecordedbands, int nzoombands, int nbits, Configuration::datasampling sampling, Configuration::complextype tcomplex, int unpacksamp, bool fbank, bool linear2circular, int fringerotorder, int arraystridelen, bool cacorrs, double bclock)
  : config(conf), configindex(confindex), datastreamindex(dsindex), recordedbandchannels(recordedbandchan), channelstoaverage(chanstoavg), blockspersend(bpersend), guardsamples(gsamples), fftchannels(recordedbandchan*2), numrecordedfreqs(nrecordedfreqs), numrecordedbands(nrecordedbands), numzoombands(nzoombands), numbits(nbits), unpacksamples(unpacksamp), fringerotationorder(fringerotorder), arraystridelength(arraystridelen), recordedbandwidth(recordedbw), blockclock(bclock), filterbank(fbank), linear2circular(linear2circular), calccrosspolautocorrs(cacorrs), recordedfreqclockoffsets(recordedfreqclkoffs), recordedfreqclockoffsetsdelta(recordedfreqclkoffsdelta), recordedfreqphaseoffset(recordedfreqphaseoffs), recordedfreqlooffsets(recordedfreqlooffs), pcalresults(0), extractor(0), bandthreads(0), bandpool(0)
{
  // P3: per-block accumulators are opt-in (see mode.h).  Off by default so
  // that mpifxcorr and every serial consumer keep the original code path.
  blockac = 0;
  blockweight = 0;
  blockacblocks = 0;
  currentblock = 0;
  blockslotbase = 0;
  // P3 (pcal): per-block pcal slots (see mode.h); off by default as well
  blockpcal = 0;
  blockpcalsamples = 0;
  blockpcallen = 0;

  int status, localfreqindex, parentfreqindex;
  int decimationfactor = config->getDDecimationFactor(configindex, datastreamindex);
  estimatedbytes = 0;
  double looffsetcorrectioninterval, looffsetphasechange, worstlooffsetphasechange;

  if (sampling==Configuration::COMPLEX) {
    usecomplex=1;
    if (tcomplex==Configuration::DOUBLE) 
      usedouble=1;
    else
      usedouble=0;
  }  else {
    usecomplex=0;
    usedouble=0;
  }

  //Uses bitwise test to check if numchannels is power of 2
  if(!(fftchannels & (fftchannels - 1)))
  {
    isfft = true;
  }
  else
  {
    isfft = false;
  }


  fftslots = config->getNumBufferedFFTs(confindex);
  dataweight = vectorAlloc_f32(fftslots);
  for(int i=0;i<fftslots;++i)
  {
    dataweight[i] = 0.0;
  }
  perbandweights = 0;
  model = config->getModel();
  initok = true;
  intclockseconds = int(floor(config->getDClockCoeff(configindex, dsindex, 0)/1000000.0 + 0.5));
  if (usecomplex) fftchannels /=2;

  numfracstrides = numfrstrides = fftchannels/arraystridelength;
  if (usecomplex) numfracstrides *= 2;
  sampletime = 1.0/(2.0*recordedbandwidth); //microseconds
  if (usecomplex) sampletime *= 2.0;
  fftdurationmicrosec = fftchannels*sampletime;  // This is never used??
  flaglength = blockspersend/FLAGS_PER_INT;
  if(blockspersend%FLAGS_PER_INT > 0)
    flaglength++;
  validflags = vectorAlloc_s32(flaglength);
  estimatedbytes += sizeof(s32)*flaglength;
  fractionalLoFreq = false;
  for(int i=0;i<numrecordedfreqs;i++)
  {
    double loval = config->getDRecordedFreq(configindex, datastreamindex, i);
    if(usedouble)
    {
      if (config->getDRecordedLowerSideband(configindex, datastreamindex, i)) 
      {
        loval -= config->getDRecordedBandwidth(configindex, datastreamindex, i)/2.0;
      } 
      else 
      {
        loval += config->getDRecordedBandwidth(configindex, datastreamindex, i)/2.0;
      }
    }
    if(fabs(loval - int(loval + 0.5)) > TINY) {
      fractionalLoFreq = true;
    }
  }

  //check whether LO offset correction will decorrelate too badly, if used
  looffsetcorrectioninterval = sampletime/1e6;
  if(fringerotorder == 0)
    looffsetcorrectioninterval *= fftchannels;
  worstlooffsetphasechange = 0.0;
  for(int i=0;i<numrecordedfreqs;i++)
  {
    if(fabs(recordedfreqlooffsets[i]) > TINY) {
      looffsetphasechange = fabs(recordedfreqlooffsets[i])*looffsetcorrectioninterval*TWO_PI;
      if(looffsetphasechange > worstlooffsetphasechange)
        worstlooffsetphasechange = looffsetphasechange;
  }
  if(worstlooffsetphasechange > 0.1)
    csevere << startl << "LO offset will lead to significant decorrelation - phase change of " << worstlooffsetphasechange*360/TWO_PI << " degrees between updates!" << endl;
  else if (worstlooffsetphasechange > 0.01)
    cwarn << startl << "LO offset will lead to some decorrelation - phase change of " << worstlooffsetphasechange*360/TWO_PI << " degrees between updates!" << endl;
  }

  //now do the rest of the initialising
  samplesperblock = int(recordedbandwidth*2/blockclock);
  if(samplesperblock == 0)
  {
    cfatal << startl << "Error!!! Samplesperblock is less than 1, current implementation cannot handle this situation.  Aborting!" << endl;
    initok = false;
  }
  else
  {
    int numsamplebits = numbits;
    if (usecomplex) numsamplebits *=2;
    bytesperblocknumerator = (numrecordedbands*samplesperblock*numsamplebits*decimationfactor)/8;
    if(bytesperblocknumerator == 0)
    {
      bytesperblocknumerator = 1;
      bytesperblockdenominator = 8/(numrecordedbands*samplesperblock*numsamplebits*decimationfactor);
      unpacksamples += bytesperblockdenominator*sizeof(u16)*samplesperblock;
    }
    else
    {
      bytesperblockdenominator = 1;
    }
    samplesperlookup = (numrecordedbands*sizeof(u16)*samplesperblock*bytesperblockdenominator)/bytesperblocknumerator;
    numlookups = (unpacksamples*bytesperblocknumerator)/(bytesperblockdenominator*sizeof(u16)*samplesperblock);
    if(samplesperblock > 1)
      numlookups++;

    unpackedarrays = new f32*[numrecordedbands];
    if (usecomplex) unpackedcomplexarrays = new cf32*[numrecordedbands];
    for(int i=0;i<numrecordedbands;i++) {
      // P10: Mk5Mode::unpack writes samplestounpack = unpacksamples +
      // mark5stream->samplegranularity samples (mk5mode.cpp:53-55); the
      // upstream allocation of exactly unpacksamples overruns by the
      // granularity on every FFT and only survives by heap-layout luck.
      // +8 covers the largest granularity mark5access uses.
      unpackedarrays[i] = vectorAlloc_f32(unpacksamples + 8);
      estimatedbytes += sizeof(f32)*(unpacksamples + 8);
      if (usecomplex) unpackedcomplexarrays[i] = (cf32*) unpackedarrays[i];
    }

    interpolator = new f64[3];

    fftoutputs = new cf32**[numrecordedbands + numzoombands];
    conjfftoutputs = new cf32**[numrecordedbands + numzoombands];
    estimatedbytes += 4*(numrecordedbands + numzoombands);
    for(int j=0;j<numrecordedbands+numzoombands;j++)
    {
      fftoutputs[j] = new cf32*[fftslots];
      conjfftoutputs[j] = new cf32*[fftslots];
      for(int k=0;k<fftslots;k++)
      {
        if(j<numrecordedbands)
        {
	  if(fringerotationorder == 0) // post-F
	  {
	    fftoutputs[j][k] = vectorAlloc_cf32(recordedbandchannels+1);
            conjfftoutputs[j][k] = vectorAlloc_cf32(recordedbandchannels+1);
	  }
	  else
	  {
            fftoutputs[j][k] = vectorAlloc_cf32(recordedbandchannels);
            conjfftoutputs[j][k] = vectorAlloc_cf32(recordedbandchannels);
	  }
          estimatedbytes += 2*sizeof(cf32)*recordedbandchannels;
        }
        else
        {
          localfreqindex = config->getDLocalZoomFreqIndex(confindex, dsindex, j-numrecordedbands);
          parentfreqindex = config->getDZoomFreqParentFreqIndex(confindex, dsindex, localfreqindex);
          fftoutputs[j][k] = 0;
          conjfftoutputs[j][k] = 0;
          for(int l=0;l<numrecordedbands;l++) {
            if(config->getDLocalRecordedFreqIndex(confindex, dsindex, l) == parentfreqindex && config->getDRecordedBandPol(confindex, dsindex, l) == config->getDZoomBandPol(confindex, dsindex, j-numrecordedbands)) {
              fftoutputs[j][k] = &(fftoutputs[l][k][config->getDZoomFreqChannelOffset(confindex, dsindex, localfreqindex)]);
	      conjfftoutputs[j][k] = &(conjfftoutputs[l][k][config->getDZoomFreqChannelOffset(confindex, dsindex, localfreqindex)]);
            }
          }
          if(fftoutputs[j][k] == 0)
            csevere << startl << "Couldn't find the parent band for zoom band " << j-numrecordedbands << endl;
        }
      }
    }

    lookup = vectorAlloc_s16((MAX_U16+1)*samplesperlookup);
    linearunpacked = vectorAlloc_s16(numlookups*samplesperlookup);
    estimatedbytes += 2*(numlookups*samplesperlookup + (MAX_U16+1)*samplesperlookup);

    //initialise the fft info
    order = 0;
    while((fftchannels) >> order != 1)
      order++;
    flag = vecFFT_NoReNorm;
    hint = vecAlgHintFast;

    switch(fringerotationorder) {
      case 2: // Quadratic
        piecewiserotator = vectorAlloc_cf32(arraystridelength);
        quadpiecerotator = vectorAlloc_cf32(arraystridelength);
        estimatedbytes += 2*8*arraystridelength;

        subquadxval  = vectorAlloc_f64(arraystridelength);
        subquadphase = vectorAlloc_f64(arraystridelength);
        subquadarg   = vectorAlloc_f32(arraystridelength);
        subquadsin   = vectorAlloc_f32(arraystridelength);
        subquadcos   = vectorAlloc_f32(arraystridelength);
        estimatedbytes += (8+8+4+4+4)*arraystridelength;

        stepxoffsquared = vectorAlloc_f64(numfrstrides);
        tempstepxval = vectorAlloc_f64(numfrstrides);
        estimatedbytes += 16*numfrstrides;
      case 1:
        subtoff  = vectorAlloc_f64(arraystridelength);
        subtval  = vectorAlloc_f64(arraystridelength);
        subxoff  = vectorAlloc_f64(arraystridelength);
        subxval  = vectorAlloc_f64(arraystridelength);
        subphase = vectorAlloc_f64(arraystridelength);
        subarg   = vectorAlloc_f32(arraystridelength);
        subsin   = vectorAlloc_f32(arraystridelength);
        subcos   = vectorAlloc_f32(arraystridelength);
        estimatedbytes += (3*8+3*4)*arraystridelength;

        steptoff  = vectorAlloc_f64(numfrstrides);
        steptval  = vectorAlloc_f64(numfrstrides);
        stepxoff  = vectorAlloc_f64(numfrstrides);
        stepxval  = vectorAlloc_f64(numfrstrides);
        stepphase = vectorAlloc_f64(numfrstrides);
        steparg   = vectorAlloc_f32(numfrstrides);
        stepsin   = vectorAlloc_f32(numfrstrides);
        stepcos   = vectorAlloc_f32(numfrstrides);
        stepcplx  = vectorAlloc_cf32(numfrstrides);
        estimatedbytes += (3*8+3*4+8)*numfrstrides;

        complexunpacked = vectorAlloc_cf32(fftchannels);
        complexrotator = vectorAlloc_cf32(fftchannels);
        fftd = vectorAlloc_cf32(fftchannels);
        estimatedbytes += 3*sizeof(cf32)*fftchannels;

        for(int i=0;i<arraystridelength;i++) {
          subxoff[i] = (double(i)/double(fftchannels));
          subtoff[i] = i*sampletime/1e6;
        }
        for(int i=0;i<numfrstrides;i++) {
          stepxoff[i] = double(i*arraystridelength)/double(fftchannels);
          steptoff[i] = i*arraystridelength*sampletime/1e6;
        }
        if(fringerotationorder == 2) { // Quadratic
          for(int i=0;i<numfrstrides;i++)
            stepxoffsquared[i] = stepxoff[i]*stepxoff[i];
        }

        if (isfft) {
          status = vectorInitFFTC_cf32(&pFFTSpecC, order, flag, hint, &fftbuffersize, &fftbuffer);
          if (status != vecNoErr)
            csevere << startl << "Error in FFT initialisation!!!" << status << endl;
        }
        else {
          status = vectorInitDFTC_cf32(&pDFTSpecC, fftchannels, flag, hint, &fftbuffersize, &fftbuffer);
          if(status != vecNoErr)
            csevere << startl << "Error in DFT initialisation!!!" << status << endl;
        }
        break;
      case 0: //zeroth order interpolation, can do "post-F"
        if (isfft) {
          status = vectorInitFFTR_f32(&pFFTSpecR, order, flag, hint, &fftbuffersize, &fftbuffer);
          if (status != vecNoErr)
            csevere << startl << "Error in FFT initialisation!!!" << status << endl;
        }
        else {
          status = vectorInitDFTR_f32(&pDFTSpecR, fftchannels, flag, hint, &fftbuffersize, &fftbuffer);
          if (status != vecNoErr)
            csevere << startl << "Error in DFT initialisation!!!" << status << endl;
        }
        break;
    }
    estimatedbytes += fftbuffersize;

    subfracsamparg = vectorAlloc_f32(arraystridelength);
    subfracsampsin = vectorAlloc_f32(arraystridelength);
    subfracsampcos = vectorAlloc_f32(arraystridelength);
    subchannelfreqs = vectorAlloc_f32(arraystridelength);
    ldsbsubchannelfreqs = vectorAlloc_f32(arraystridelength);
    estimatedbytes += 6*4*arraystridelength;
    /*cout << "subfracsamparg is " << subfracsamparg << endl;
    cout << "subfracsampsin is " << subfracsampsin << endl;
    cout << "subfracsampcos is " << subfracsampcos << endl;
    cout << "subchannelfreqs is " << subchannelfreqs << endl; */
    for(int i=0;i<arraystridelength;i++) {
      subchannelfreqs[i] = (float)((TWO_PI*(i)*recordedbandwidth)/recordedbandchannels);
      ldsbsubchannelfreqs[i] = (float)((-TWO_PI*(i)*recordedbandwidth)/recordedbandchannels);
    }

    stepfracsamparg = vectorAlloc_f32(numfracstrides/2);
    stepfracsampsin = vectorAlloc_f32(numfracstrides/2);
    stepfracsampcos = vectorAlloc_f32(numfracstrides/2);
    stepfracsampcplx = vectorAlloc_cf32(numfracstrides/2);
    stepchannelfreqs = vectorAlloc_f32(numfracstrides/2);
    lsbstepchannelfreqs = vectorAlloc_f32(numfracstrides/2);
    dsbstepchannelfreqs = vectorAlloc_f32(numfracstrides/2);
    ldsbstepchannelfreqs = vectorAlloc_f32(numfracstrides/2);
    estimatedbytes += (7*2+4)*numfracstrides;

    for(int i=0;i<numfracstrides/2;i++) {
      stepchannelfreqs[i]     = (float)((TWO_PI*i*arraystridelength*recordedbandwidth)/recordedbandchannels);
      dsbstepchannelfreqs[i]  = (float)((TWO_PI*i*arraystridelength*recordedbandwidth)/recordedbandchannels - TWO_PI*recordedbandwidth/2.0);
      lsbstepchannelfreqs[i]  = (float)((-TWO_PI*((numfracstrides/2-i)*arraystridelength)*recordedbandwidth)/recordedbandchannels);
      //ldsbstepchannelfreqs[i] = (float)((-TWO_PI*((numfracstrides/2-i)*arraystridelength)*recordedbandwidth)/recordedbandchannels + TWO_PI*recordedbandwidth/2.0);
      ldsbstepchannelfreqs[i] = -dsbstepchannelfreqs[i];
    }

    deltapoloffsets = false;
    phasepoloffset = false;
    for (int i=0; i<numrecordedfreqs; i++) {
      if (recordedfreqclockoffsetsdelta[i]!=0.0) {
	deltapoloffsets = true;
      }
      if (recordedfreqphaseoffset[i]!=0.0) {
	phasepoloffset = true;
      }
    }

    fracsamprotatorA = vectorAlloc_cf32(recordedbandchannels);
    if (deltapoloffsets) {
      fracsamprotatorB = vectorAlloc_cf32(recordedbandchannels);
    } else {
      fracsamprotatorB = fracsamprotatorA;
    }

    estimatedbytes += 8*recordedbandchannels;
    /*cout << "Numstrides is " << numstrides << ", recordedbandchannels is " << recordedbandchannels << ", arraystridelength is " << arraystridelength << endl;
    cout << "fracsamprotator is " << fracsamprotator << endl;
    cout << "stepchannelfreqs[5] is " << stepchannelfreqs[5] << endl;
    cout << "subchannelfreqs[5] is " << subchannelfreqs[5] << endl;
    cout << "stepchannelfreqs(last) is " << stepchannelfreqs[numstrides/2-1] << endl;
    fracmult = vectorAlloc_f32(recordedbandchannels + 1);
    fracmultcos = vectorAlloc_f32(recordedbandchannels + 1);
    fracmultsin = vectorAlloc_f32(recordedbandchannels + 1);
    complexfracmult = vectorAlloc_cf32(recordedbandchannels + 1);

    channelfreqs = vectorAlloc_f32(recordedbandchannels + 1);
    for(int i=0;i<recordedbandchannels + 1;i++)
      channelfreqs[i] = (float)((TWO_PI*i*recordedbandwidth)/recordedbandchannels);
    lsbchannelfreqs = vectorAlloc_f32(recordedbandchannels + 1);
    for(int i=0;i<recordedbandchannels + 1;i++)
      lsbchannelfreqs[i] = (float)((-TWO_PI*(recordedbandchannels-i)*recordedbandwidth)/recordedbandchannels);
    */

    //space for the autocorrelations
    if(calccrosspolautocorrs)
      autocorrwidth = 2;
    else
      autocorrwidth = 1;
    autocorrelations = new cf32**[autocorrwidth];
    weights = new f32*[autocorrwidth];
    for(int i=0;i<autocorrwidth;i++)
    {
      weights[i] = new f32[numrecordedbands];
      estimatedbytes += sizeof(f32)*numrecordedbands;
      autocorrelations[i] = new cf32*[numrecordedbands+numzoombands];
      for(int j=0;j<numrecordedbands;j++) {
        autocorrelations[i][j] = vectorAlloc_cf32(recordedbandchannels);
        estimatedbytes += 8*recordedbandchannels;
      }
      for(int j=0;j<numzoombands;j++)
      {
        localfreqindex = config->getDLocalZoomFreqIndex(confindex, dsindex, j);
        parentfreqindex = config->getDZoomFreqParentFreqIndex(confindex, dsindex, localfreqindex);
        autocorrelations[i][j+numrecordedbands] = 0;
        for(int l=0;l<numrecordedbands;l++) {
          if(config->getDLocalRecordedFreqIndex(confindex, dsindex, l) == parentfreqindex && config->getDRecordedBandPol(confindex, dsindex, l) == config->getDZoomBandPol(confindex, dsindex, j)) {
            autocorrelations[i][j+numrecordedbands] = &(autocorrelations[i][l][config->getDZoomFreqChannelOffset(confindex, dsindex, localfreqindex)/channelstoaverage]);
          }
        }
        if(autocorrelations[i][j+numrecordedbands] == 0)
          csevere << startl << "Couldn't find the parent band for autocorr of zoom band " << j << endl;
      }
    }

    //kurtosis-specific stuff
    dumpkurtosis = false; //off by default
    s1 = 0;
    s2 = 0;
    sk = 0;
    kscratch = 0;
  }
  // Phase cal stuff
  // extractor/pcalresults are initialised to 0 in the member init list: a
  // configuration without phase cal never allocates them, and
  // Mode::blockPcalCapable() reads extractor == 0 as "no pcal extractor".
  // Before P3 nothing read the pointer in that state, so the missing
  // initialisation went unnoticed (no-pcal configurations crashed in
  // enableBlockAccumulators once P3 called blockPcalCapable unconditionally).
  PCal::setMinFrequencyResolution(1e6);
  if(config->getDPhaseCalIntervalHz(configindex, datastreamindex) > 0)
  {
    pcalresults = new cf32*[numrecordedbands]();
    extractor = new PCal*[numrecordedbands]();
    for(int i=0;i<numrecordedbands;i++)
    {
      localfreqindex = conf->getDLocalRecordedFreqIndex(confindex, dsindex, i);
      const long denom = config->getDPhaseCalDenominator(configindex, datastreamindex);
      const fraction tonespacing(config->getDPhaseCalIntervalHz(configindex, datastreamindex), denom);
      const fraction toneoffset(config->getDRecordedFreqPCalOffsetsHz(configindex, dsindex, localfreqindex), denom);
      pcalresults[i] = new cf32[conf->getDRecordedFreqNumPCalTones(configindex, dsindex, localfreqindex)]();
      extractor[i] = PCal::getNew(1e6*recordedbandwidth, tonespacing, toneoffset, 0, sampling, tcomplex);
      if (extractor[i]->getLength() != conf->getDRecordedFreqNumPCalTones(configindex, dsindex, localfreqindex))
        csevere << startl << "Developer Error: configuration.cpp and pcal.cpp do not agree on the number of tones: " << extractor[i]->getLength() << " != " << conf->getDRecordedFreqNumPCalTones(configindex, dsindex, localfreqindex) << " ." << endl;
      estimatedbytes += extractor[i]->getEstimatedBytes();
    }
  }

  if (linear2circular || phasepoloffset ) {
    
    tmpvec = new cf32[recordedbandchannels];

    phasecorrA = new cf32[numrecordedfreqs];
    phasecorrconjA = new cf32[numrecordedfreqs];
    phasecorrB = new cf32[numrecordedfreqs];
    phasecorrconjB = new cf32[numrecordedfreqs];
    double degphase;
    for (int i=0; i<numrecordedfreqs; i++) {
      degphase = -recordedfreqphaseoffset[i]/2;
      phasecorrA[i].re = cos(degphase*M_PI/180.0);
      phasecorrA[i].im = -sin(degphase*M_PI/180.0);
      phasecorrconjA[i].re = phasecorrA[i].re;
      phasecorrconjA[i].im = -phasecorrA[i].im;

      if (linear2circular) {
	degphase = 90+recordedfreqphaseoffset[i]/2;
      } else {
	degphase = recordedfreqphaseoffset[i]/2;
      }
      phasecorrB[i].re = cos(degphase*M_PI/180.0);
      phasecorrB[i].im = -sin(degphase*M_PI/180.0);
      phasecorrconjB[i].re = phasecorrA[i].re;
      phasecorrconjB[i].im = -phasecorrA[i].im;
    }
  }
}

Mode::~Mode()
{
  if(perbandweights)
  {
    for(int i=0;i<fftslots;++i)
    {
      delete [] perbandweights[i];
    }
    delete [] perbandweights;
  }
  vectorFree(dataweight);
  vectorFree(validflags);
  for(int j=0;j<numrecordedbands+numzoombands;j++)
  {
    for(int k=0;k<fftslots;k++)
    {
      if(j<numrecordedbands) {
        vectorFree(fftoutputs[j][k]);
        vectorFree(conjfftoutputs[j][k]);
      }
    }
    delete [] fftoutputs[j];
    delete [] conjfftoutputs[j];
  }
  delete [] fftoutputs;
  delete [] conjfftoutputs;
  delete [] interpolator;

  for(int i=0;i<numrecordedbands;i++)
    vectorFree(unpackedarrays[i]);
  delete [] unpackedarrays;

  switch(fringerotationorder) {
    case 2: // Quadratic
      vectorFree(piecewiserotator);
      vectorFree(quadpiecerotator);

      vectorFree(subquadxval);
      vectorFree(subquadphase);
      vectorFree(subquadarg);
      vectorFree(subquadsin);
      vectorFree(subquadcos);

      vectorFree(stepxoffsquared);
      vectorFree(tempstepxval);
    case 1:
      vectorFree(subtoff);
      vectorFree(subtval);
      vectorFree(subxoff);
      vectorFree(subxval);
      vectorFree(subphase);
      vectorFree(subarg);
      vectorFree(subsin);
      vectorFree(subcos);

      vectorFree(steptoff);
      vectorFree(steptval);
      vectorFree(stepxoff);
      vectorFree(stepxval);
      vectorFree(stepphase);
      vectorFree(steparg);
      vectorFree(stepsin);
      vectorFree(stepcos);
      vectorFree(stepcplx);

      vectorFree(complexunpacked);
      vectorFree(complexrotator);
      vectorFree(fftd);
      if(isfft) {
	vectorFreeFFTC_cf32(pFFTSpecC);
      }
      else{
	vectorFreeDFTC_cf32(pDFTSpecC);
      }
      break;
    case 0: //zeroth order interpolation, "post-F"
      if(isfft) {
	vectorFreeFFTR_f32(pFFTSpecR);
      }
      else{
	vectorFreeDFTR_f32(pDFTSpecR);
      }
      break;
  }

  vectorFree(lookup);
  vectorFree(linearunpacked);
  vectorFree(fftbuffer);

  delete bandpool;
  bandpool = 0;
  for(size_t t=0;t<bandscratch.size();t++)
    freeBandScratch(bandscratch[t]);
  bandscratch.clear();

  vectorFree(subfracsamparg);
  vectorFree(subfracsampsin);
  vectorFree(subfracsampcos);
  vectorFree(subchannelfreqs);
  vectorFree(ldsbsubchannelfreqs);

  vectorFree(stepfracsamparg);
  vectorFree(stepfracsampsin);
  vectorFree(stepfracsampcos);
  vectorFree(stepfracsampcplx);
  vectorFree(stepchannelfreqs);
  vectorFree(lsbstepchannelfreqs);
  vectorFree(dsbstepchannelfreqs);
  vectorFree(ldsbstepchannelfreqs);

  vectorFree(fracsamprotatorA);
  if (deltapoloffsets) vectorFree(fracsamprotatorB);
  //vectorFree(fracmult);
  //vectorFree(fracmultcos);
  //vectorFree(fracmultsin);
  //vectorFree(complexfracmult);
  //vectorFree(channelfreqs);

  for(int i=0;i<autocorrwidth;i++)
  {
    for(int j=0;j<numrecordedbands;j++)
      vectorFree(autocorrelations[i][j]);
    delete [] autocorrelations[i];
    delete [] weights[i];
    if(blockacblocks > 0)
    {
      for(int b=0;b<blockacblocks;b++)
      {
        for(int j=0;j<numrecordedbands;j++)
          vectorFree(blockac[i][b][j]);
        delete [] blockac[i][b];
      }
      delete [] blockac[i];
      delete [] blockweight[i];
    }
  }
  delete [] weights;
  delete [] autocorrelations;
  if(blockacblocks > 0)
  {
    delete [] blockac;
    delete [] blockweight;
    if(blockpcal != 0)	// P3 (pcal): allocated with the block accumulators
    {
      for(int b=0;b<blockacblocks;b++)
      {
        for(int j=0;j<numrecordedbands;j++)
          vectorFree(blockpcal[b][j]);
        delete [] blockpcal[b];
        delete [] blockpcalsamples[b];
      }
      delete [] blockpcal;
      delete [] blockpcalsamples;
      delete [] blockpcallen;
    }
  }

  if(config->getDPhaseCalIntervalHz(configindex, datastreamindex) > 0)
  {
    for(int i=0;i<numrecordedbands;i++) {
       delete extractor[i];
       delete[] pcalresults[i];
    }
    delete[] pcalresults;
    delete[] extractor;
  }

  if(sk != 0) //also need to delete kurtosis stuff
  {
    for(int i=0;i<numrecordedbands;i++)
    {
      vectorFree(s1[i]);
      vectorFree(s2[i]);
      vectorFree(sk[i]);
    }
    delete [] s1;
    delete [] s2;
    delete [] sk;
    vectorFree(kscratch);
  }

  if (linear2circular) {
    delete [] tmpvec;
  }
}

float Mode::unpack(int sampleoffset, int subloopindex)
{
  int status, leftoversamples, stepin = 0;

  if(bytesperblockdenominator/bytesperblocknumerator == 0)
    leftoversamples = 0;
  else
    leftoversamples = sampleoffset%(bytesperblockdenominator/bytesperblocknumerator);
  unpackstartsamples = sampleoffset - leftoversamples;
  if(samplesperblock > 1)
    stepin = unpackstartsamples%(samplesperblock*bytesperblockdenominator);
  u16 * packed = (u16 *)(&(data[((unpackstartsamples/samplesperblock)*bytesperblocknumerator)/bytesperblockdenominator]));

  //copy from the lookup table to the linear unpacked array
  for(int i=0;i<numlookups;i++)
  {
    status = vectorCopy_s16(&lookup[packed[i]*samplesperlookup], &linearunpacked[i*samplesperlookup], samplesperlookup);
    if(status != vecNoErr) {
      csevere << startl << "Error in lookup for unpacking!!!" << status << endl;
      return 0;
    }
  }

  //split the linear unpacked array into the separate subbands
  status = vectorSplitScaled_s16f32(&(linearunpacked[stepin*numrecordedbands]), unpackedarrays, numrecordedbands, unpacksamples);
  if(status != vecNoErr) {
    csevere << startl << "Error in splitting linearunpacked!!!" << status << endl;
    return 0;
  }

  return 1.0;
}

// gcc 11's -O2 (the build default) does not auto-vectorise; the per-channel
// loops below are element-wise complex ops through the inline generic*
// helpers (architecture.h), so this one function gets -O3.  Deliberately no
// -march/-ffast-math: the vectorisation stays SSE2 (x86-64 baseline) with no
// FMA contraction, every element is rounded exactly as in the -O2 build, and
// the products stay bit-identical (checked against the OMP=1 reference in
// fxcorr-f's four-station runs).
struct Mode::BandPool
{
  int nworkers;
  std::vector<std::thread> workers;
  std::mutex m;
  std::condition_variable cv_start, cv_done;
  unsigned long generation;
  int ntasks, donecount;
  std::atomic<int> next;
  std::function<void(int,int)> fn;
  bool stop;

  explicit BandPool(int nthreads)
    : nworkers(nthreads > 1 ? nthreads - 1 : 0), generation(0), ntasks(0),
      donecount(0), next(0), stop(false)
  {
    for(int w=0;w<nworkers;w++)
      workers.push_back(std::thread(&Mode::BandPool::workerLoop, this, w+1));
  }
  ~BandPool()
  {
    {
      std::lock_guard<std::mutex> lk(m);
      stop = true;
    }
    cv_start.notify_all();
    for(size_t w=0;w<workers.size();w++)
      workers[w].join();
  }
  void workerLoop(int tid)
  {
    unsigned long seen = 0;
    for(;;)
    {
      std::function<void(int,int)> f;
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
        f(i, tid);
      }
      {
        std::lock_guard<std::mutex> lk(m);
        donecount++;
        if(donecount >= nworkers)
          cv_done.notify_one();
      }
    }
  }
  void run(int n, const std::function<void(int,int)> &f)
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
      f(i, 0);
    }
    std::unique_lock<std::mutex> lk(m);
    cv_done.wait(lk, [&]{ return donecount >= nworkers; });
  }
};

Mode::BandScratch::BandScratch()
  : subphase(0), subarg(0), subsin(0), subcos(0),
    stepphase(0), steparg(0), stepsin(0), stepcos(0), stepcplx(0),
    complexunpacked(0), complexrotator(0), fftd(0),
    fracsamprotatorA(0), fracsamprotatorB(0),
    subfracsamparg(0), subfracsampsin(0), subfracsampcos(0),
    stepfracsamparg(0), stepfracsampsin(0), stepfracsampcos(0),
    stepfracsampcplx(0),
    piecewiserotator(0), quadpiecerotator(0),
    subquadphase(0), subquadarg(0), subquadsin(0), subquadcos(0),
    kscratch(0), tmpvec(0),
    fftspecR(0), fftspecC(0), dftspecR(0), dftspecC(0), fftbuffer(0)
{
}

void Mode::allocBandScratch(BandScratch &bs)
{
  int numfrstrides = fftchannels/arraystridelength;
  int numfracstrides = numfrstrides;
  if(usecomplex)
    numfracstrides *= 2;

  bs.subphase  = vectorAlloc_f64(arraystridelength);
  bs.subarg    = vectorAlloc_f32(arraystridelength);
  bs.subsin    = vectorAlloc_f32(arraystridelength);
  bs.subcos    = vectorAlloc_f32(arraystridelength);
  bs.stepphase = vectorAlloc_f64(numfrstrides);
  bs.steparg   = vectorAlloc_f32(numfrstrides);
  bs.stepsin   = vectorAlloc_f32(numfrstrides);
  bs.stepcos   = vectorAlloc_f32(numfrstrides);
  bs.stepcplx  = vectorAlloc_cf32(numfrstrides);

  bs.complexunpacked = vectorAlloc_cf32(fftchannels);
  bs.complexrotator  = vectorAlloc_cf32(fftchannels);
  bs.fftd            = vectorAlloc_cf32(fftchannels);

  bs.fracsamprotatorA = vectorAlloc_cf32(recordedbandchannels);
  bs.fracsamprotatorB = vectorAlloc_cf32(recordedbandchannels);

  bs.subfracsamparg   = vectorAlloc_f32(arraystridelength);
  bs.subfracsampsin   = vectorAlloc_f32(arraystridelength);
  bs.subfracsampcos   = vectorAlloc_f32(arraystridelength);
  bs.stepfracsamparg  = vectorAlloc_f32(numfracstrides/2);
  bs.stepfracsampsin  = vectorAlloc_f32(numfracstrides/2);
  bs.stepfracsampcos  = vectorAlloc_f32(numfracstrides/2);
  bs.stepfracsampcplx = vectorAlloc_cf32(numfracstrides/2);

  bs.piecewiserotator = vectorAlloc_cf32(arraystridelength);
  bs.quadpiecerotator = vectorAlloc_cf32(arraystridelength);
  bs.subquadphase = vectorAlloc_f64(arraystridelength);
  bs.subquadarg   = vectorAlloc_f32(arraystridelength);
  bs.subquadsin   = vectorAlloc_f32(arraystridelength);
  bs.subquadcos   = vectorAlloc_f32(arraystridelength);

  bs.kscratch = vectorAlloc_f32(recordedbandchannels);
  bs.tmpvec   = vectorAlloc_cf32(recordedbandchannels);

  // one FFT plan per band thread: the non-IPP build executes a plan whose
  // input/output buffers are bound at creation, so a plan cannot be shared
  // between concurrent process() calls (the serial path gets away with it)
  // the spec kind follows fringerotationorder exactly as the constructor does
  // (case 0 uses the R->C plan, cases 1/2 the C->C plan); usecomplex only
  // changes fftchannels, not which plan is used
  int wbufsize = 0;
  switch(fringerotationorder)
  {
    case 1:
    case 2:
      if(isfft)
        vectorInitFFTC_cf32(&bs.fftspecC, order, flag, vecAlgHintFast, &wbufsize, &bs.fftbuffer);
      else
        vectorInitDFTC_cf32(&bs.dftspecC, fftchannels, flag, vecAlgHintFast, &wbufsize, &bs.fftbuffer);
      break;
    default:
      if(isfft)
        vectorInitFFTR_f32(&bs.fftspecR, order, flag, vecAlgHintFast, &wbufsize, &bs.fftbuffer);
      else
        vectorInitDFTR_f32(&bs.dftspecR, fftchannels, flag, vecAlgHintFast, &wbufsize, &bs.fftbuffer);
      break;
  }
}

void Mode::freeBandScratch(BandScratch &bs)
{
  if(bs.subphase) vectorFree(bs.subphase);
  if(bs.subarg) vectorFree(bs.subarg);
  if(bs.subsin) vectorFree(bs.subsin);
  if(bs.subcos) vectorFree(bs.subcos);
  if(bs.stepphase) vectorFree(bs.stepphase);
  if(bs.steparg) vectorFree(bs.steparg);
  if(bs.stepsin) vectorFree(bs.stepsin);
  if(bs.stepcos) vectorFree(bs.stepcos);
  if(bs.stepcplx) vectorFree(bs.stepcplx);
  if(bs.complexunpacked) vectorFree(bs.complexunpacked);
  if(bs.complexrotator) vectorFree(bs.complexrotator);
  if(bs.fftd) vectorFree(bs.fftd);
  if(bs.fracsamprotatorA) vectorFree(bs.fracsamprotatorA);
  if(bs.fracsamprotatorB) vectorFree(bs.fracsamprotatorB);
  if(bs.subfracsamparg) vectorFree(bs.subfracsamparg);
  if(bs.subfracsampsin) vectorFree(bs.subfracsampsin);
  if(bs.subfracsampcos) vectorFree(bs.subfracsampcos);
  if(bs.stepfracsamparg) vectorFree(bs.stepfracsamparg);
  if(bs.stepfracsampsin) vectorFree(bs.stepfracsampsin);
  if(bs.stepfracsampcos) vectorFree(bs.stepfracsampcos);
  if(bs.stepfracsampcplx) vectorFree(bs.stepfracsampcplx);
  if(bs.piecewiserotator) vectorFree(bs.piecewiserotator);
  if(bs.quadpiecerotator) vectorFree(bs.quadpiecerotator);
  if(bs.subquadphase) vectorFree(bs.subquadphase);
  if(bs.subquadarg) vectorFree(bs.subquadarg);
  if(bs.subquadsin) vectorFree(bs.subquadsin);
  if(bs.subquadcos) vectorFree(bs.subquadcos);
  if(bs.kscratch) vectorFree(bs.kscratch);
  if(bs.tmpvec) vectorFree(bs.tmpvec);
  if(bs.fftspecR) vectorFreeFFTR_f32(bs.fftspecR);
  if(bs.fftspecC) vectorFreeFFTC_cf32(bs.fftspecC);
  if(bs.dftspecR) vectorFreeDFTR_f32(bs.dftspecR);
  if(bs.dftspecC) vectorFreeDFTC_cf32(bs.dftspecC);
  if(bs.fftbuffer) vectorFree(bs.fftbuffer);
}

void Mode::setBandParallel(int nthreads)
{
  delete bandpool;
  bandpool = 0;
  for(size_t t=0;t<bandscratch.size();t++)
    freeBandScratch(bandscratch[t]);
  bandscratch.clear();
  bandthreads = 0;

  // zoom bands can alias a parent recorded band, which would make two loop
  // iterations write the same fftoutputs; keep those configurations serial.
  // linear2circular keeps the serial "stop after the pair" semantics too.
  if(nthreads <= 1 || numrecordedfreqs <= 1 || numzoombands > 0 || linear2circular)
    return;

  bandthreads = nthreads;
  bandscratch.resize(nthreads);
  for(int t=0;t<nthreads;t++)
    allocBandScratch(bandscratch[t]);
  bandpool = new BandPool(nthreads);
}

__attribute__((optimize("O3")))
void Mode::process(int index, int subloopindex)  //frac sample error is in microseconds
{
  // slot for the per-block accumulators (no-op when they are off); index is
  // the FFT position within the subint, mapped to this Mode's slots by
  // blockslotbase (setBlockSlotBase, once per parallel batch)
  currentblock = index - blockslotbase;
  // each block is processed exactly once per subint, so clearing its slots
  // here is both correct and N times cheaper than zeroing every slot of every
  // Mode up front (see clearBlockAccumulators)
  clearBlockAccumulators(index);

  double phaserotation, averagedelay, nearestsampletime, starttime, lofreq, walltimesecs, fracwalltime, fftcentre, d0, d1, d2, fraclooffset;
  f32 phaserotationfloat, fracsampleerror;
  int status, count, nearestsample, integerdelay, RcpIndex, LcpIndex, intwalltime;
  cf32* fftptr;
  f32* currentstepchannelfreqs;
  f32* currentsubchannelfreqs;
  int indices[10];
  bool looff, isfraclooffset;
  //cout << "For Mode of datastream " << datastreamindex << ", index " << index << ", validflags is " << validflags[index/FLAGS_PER_INT] << ", after shift you get " << ((validflags[index/FLAGS_PER_INT] >> (index%FLAGS_PER_INT)) & 0x01) << endl;

  //since these data weights can be retreived after this processing ends, reset them to a default of zero in case they don't get updated
  dataweight[subloopindex] = 0.0;
  if(perbandweights)
  {
    for(int b = 0; b < numrecordedbands; ++b)
    {
      perbandweights[subloopindex][b] = 0.0;
    }
  }
  
  if((datalengthbytes <= 1) || (offsetseconds == INVALID_SUBINT) || (((validflags[index/FLAGS_PER_INT] >> (index%FLAGS_PER_INT)) & 0x01) == 0))
  {
    for(int i=0;i<numrecordedbands;i++)
    {
      status = vectorZero_cf32(fftoutputs[i][subloopindex], recordedbandchannels);
      if(status != vecNoErr)
        csevere << startl << "Error trying to zero fftoutputs when data is bad!" << endl;
      status = vectorZero_cf32(conjfftoutputs[i][subloopindex], recordedbandchannels);
      if(status != vecNoErr)
        csevere << startl << "Error trying to zero fftoutputs when data is bad!" << endl;
    }
    //cout << "Mode for DS " << datastreamindex << " is bailing out of index " << index << "/" << subloopindex << " which is scan " << currentscan << ", sec " << offsetseconds << ", ns " << offsetns << " because datalengthbytes is " << datalengthbytes << " and validflag was " << ((validflags[index/FLAGS_PER_INT] >> (index%FLAGS_PER_INT)) & 0x01) << endl;
    return; //don't process crap data
  }

  fftcentre = index+0.5;
  averagedelay = interpolator[0]*fftcentre*fftcentre + interpolator[1]*fftcentre + interpolator[2];
  fftstartmicrosec = index*fftchannels*sampletime; //CHRIS CHECK
  starttime = (offsetseconds-datasec)*1000000.0 + (static_cast<long long>(offsetns) - static_cast<long long>(datans))/1000.0 + fftstartmicrosec - averagedelay;
  nearestsample = int(starttime/sampletime + 0.5);
  walltimesecs = model->getScanStartSec(currentscan, config->getStartMJD(), config->getStartSeconds()) + offsetseconds + offsetns/1.0e9 + fftstartmicrosec/1.0e6;
  intwalltime = static_cast<int>(walltimesecs);
  fracwalltime = walltimesecs - intwalltime;
  //cinfo << startl << "ATD: fftstartmicrosec " << fftstartmicrosec << ", sampletime " << sampletime << ", fftchannels " << fftchannels << ", bytesperblocknumerator " << bytesperblocknumerator << ", nearestsample " << nearestsample << endl;

  //if we need to, unpack some more data - first check to make sure the pos is valid at all
  //cout << "Datalengthbytes for " << datastreamindex << " is " << datalengthbytes << endl;
  //cout << "Fftchannels for " << datastreamindex << " is " << fftchannels << endl;
  //cout << "samplesperblock for " << datastreamindex << " is " << samplesperblock << endl;
  //cout << "nearestsample for " << datastreamindex << " is " << nearestsample << endl;
  //cout << "bytesperblocknumerator for " << datastreamindex << " is " << bytesperblocknumerator << endl;
  //cout << "bytesperblockdenominator for " << datastreamindex << " is " << bytesperblockdenominator << endl;
  if(nearestsample < -1 || (((nearestsample + fftchannels)/samplesperblock)*bytesperblocknumerator)/bytesperblockdenominator > datalengthbytes)
  {
    cerror << startl << "MODE error for datastream " << datastreamindex << " - trying to process data outside range - aborting!!! nearest sample was " << nearestsample << ", the max bytes should be " << datalengthbytes << " and hence last sample should be " << (datalengthbytes*bytesperblockdenominator)/(bytesperblocknumerator*samplesperblock)  << " (fftchannels is " << fftchannels << "), offsetseconds was " << offsetseconds << ", offsetns was " << offsetns << ", index was " << index << ", average delay was " << averagedelay << ", datasec was " << datasec << ", datans was " << datans << ", fftstartmicrosec was " << fftstartmicrosec << endl;
    for(int i=0;i<numrecordedbands;i++)
    {
      status = vectorZero_cf32(fftoutputs[i][subloopindex], recordedbandchannels);
      if(status != vecNoErr)
        csevere << startl << "Error trying to zero fftoutputs when data is bad!" << endl;
      status = vectorZero_cf32(conjfftoutputs[i][subloopindex], recordedbandchannels);
      if(status != vecNoErr)
        csevere << startl << "Error trying to zero fftoutputs when data is bad!" << endl;
    }
    return;
  }
  if(nearestsample == -1)
  {
    nearestsample = 0;
    dataweight[subloopindex] = unpack(nearestsample, subloopindex);
  }
  else if(nearestsample < unpackstartsamples || nearestsample > unpackstartsamples + unpacksamples - fftchannels)
    //need to unpack more data
    dataweight[subloopindex] = unpack(nearestsample, subloopindex);

 /*
  * After DiFX-2.4, it is proposed to change the handling of lower sideband and dual sideband data, such
  * that the data are manipulated here (directly after unpacking) to ensure that it appears like single 
  * sideband USB data.  In order to do that, we will loop over all recorded bands, performing the 
  * following checks and if necessary manipulations:
  * 1) if band sense is LSB, cast the unpacked data as a complex f32 and conjugate it.  If it was a complex
  *    sampled band, this flips the sideband.  If it was a real sampled band, then every second sample
  *    will be multiplied by -1, which is exactly was is required to flip the sideband also.
  *    *** NOTE: For real data, will need to use fracwalltimesecs plus the sampling rate to determine
  *              whether it is necessary to offset the start of the vector by one sample.
  * 2) Now the frequencies definitely run from most negative to most positive, but we also want the lowest
  *    frequency channel to be "DC", and this is not the case for complex double sideband data.  So for
  *    complex double sideband data, rotate the unpacked data by e^{-i 2 pi BW t} to shift the most negative
  *    frequency component up to 0 Hz.  Need to use wallclocksecs for time here too.
  * Now nothing in mode.cpp or core.cpp needs to know about whether the data was originally lower sideband
  * or not.  That will mean taking out some of the current logic, pretty much all to do with fractional sample
  * correction.
  * 
  * Some other specific implementation notes:
  * - Need to do this straight after an unpack, for the whole unpacksamples, so the two calls to unpack()
  *   above will need to be combined.
  * - It may be profitable to move the LO offset correction up to here also, and possibly also to refactor 
  *   it to change the steptval array rather than doing a separate addition. (although a separate addition
  *   for fraclooffset if required would still be needed).  Be careful of zero-order fringe rotation.
  * - lsbfracsample arrays will need to be removed, as will the checks that select them.
  * - Elsewhere, it will probably be preferable to maintain information slightly differently (for each
  *   subband, maintain lower edge frequency, bandwidth, SSLO, sampling type [real/complex], matching band).
  *   This would be in configuration.cpp/.h, maybe also vex2difx?
  * - mark5access has option to unpack real data as complex - could consider using this to save time.  
  *   Would need to make a similar option for LBA data.
  */

  if(!(dataweight[subloopindex] > 0.0)) {
    for(int i=0;i<numrecordedbands;i++)
    {
      status = vectorZero_cf32(fftoutputs[i][subloopindex], recordedbandchannels);
      if(status != vecNoErr)
        csevere << startl << "Error trying to zero fftoutputs when data is bad!" << endl;
      status = vectorZero_cf32(conjfftoutputs[i][subloopindex], recordedbandchannels);
      if(status != vecNoErr)
        csevere << startl << "Error trying to zero fftoutputs when data is bad!" << endl;
    }
    return;
  }

  nearestsampletime = nearestsample*sampletime;
  fracsampleerror = float(starttime - nearestsampletime);

  if(!(config->getDPhaseCalIntervalHz(configindex, datastreamindex) == 0))
  {
      size_t samplenrsincestart = nearestsample + static_cast<size_t>(double(datasec*1e9+datans)/(sampletime*1e3) + 0.5);
      for(int i=0;i<numrecordedbands;i++)
      {
        extractor[i]->adjustSampleOffset(samplenrsincestart);
        if (!usecomplex)
	        status = extractor[i]->extractAndIntegrate (&(unpackedarrays[i][nearestsample
	                 - unpackstartsamples]), fftchannels);
        else
	        status = extractor[i]->extractAndIntegrate ((f32 *) (&(unpackedcomplexarrays[i][nearestsample
	                 - unpackstartsamples])), fftchannels);
        if(status != true)
          csevere << startl << "Error in phase cal extractAndIntegrate" << endl;
      }

      // P3 (pcal): park this block's integration state and restart, so the
      // parallel reduction can add the blocks back in block order (see pcal.h)
      if(blockpcal != 0)
      {
        for(int i=0;i<numrecordedbands;i++)
        {
          if(extractor[i]->getAccum(blockpcal[currentblock][i], &blockpcalsamples[currentblock][i]))
            extractor[i]->resetAccum();
        }
      }
  }

  integerdelay = 0;
  switch(fringerotationorder) {
    case 0: //post-F
      integerdelay = static_cast<int>(averagedelay);
      break;
    case 1: //linear
      d0 = interpolator[0]*index*index + interpolator[1]*index + interpolator[2];
      d1 = interpolator[0]*(index+0.5)*(index+0.5) + interpolator[1]*(index+0.5) + interpolator[2];
      d2 = interpolator[0]*(index+1)*(index+1) + interpolator[1]*(index+1) + interpolator[2];
      a = d2-d0;
      b = d0 + (d1 - (a*0.5 + d0))/3.0;
      integerdelay = static_cast<int>(b);
      b -= integerdelay;

      status = vectorMulC_f64(subxoff, a, subxval, arraystridelength);
      if(status != vecNoErr)
        csevere << startl << "Error in linearinterpolate, subval multiplication" << endl;
      status = vectorMulC_f64(stepxoff, a, stepxval, numfrstrides);
      if(status != vecNoErr)
        csevere << startl << "Error in linearinterpolate, stepval multiplication" << endl;
      status = vectorAddC_f64_I(b, subxval, arraystridelength);
      if(status != vecNoErr)
        csevere << startl << "Error in linearinterpolate, subval addition!!!" << endl;
      break;
    case 2: //quadratic
      a = interpolator[0];
      b = interpolator[1] + index*interpolator[0]*2.0;
      c = interpolator[2] + index*interpolator[1] + index*index*interpolator[0];
      integerdelay = int(c);
      c -= integerdelay;

      status = vectorMulC_f64(subxoff, b + a*stepxoff[1], subxval, arraystridelength);
      if(status != vecNoErr)
        csevere << startl << "Error in quadinterpolate, subval multiplication" << endl;
      status = vectorMulC_f64(subxoff, 2*a*stepxoff[1], subquadxval, arraystridelength);
      if(status != vecNoErr)
        csevere << startl << "Error in quadinterpolate, subquadval multiplication" << endl;
      status = vectorMulC_f64(stepxoff, b, stepxval, numfrstrides);
      if(status != vecNoErr)
        csevere << startl << "Error in quadinterpolate, stepval multiplication" << endl;
      status = vectorMulC_f64(stepxoffsquared, a, tempstepxval, numfrstrides);
      if(status != vecNoErr)
        csevere << startl << "Error in quadinterpolate, tempstepval multiplication" << endl;
      status = vectorAdd_f64_I(tempstepxval, stepxval, numfrstrides);
      if(status != vecNoErr)
        csevere << startl << "Error in quadinterpolate, stepval addition!!!" << endl;
      status = vectorAddC_f64_I(c, subxval, arraystridelength);
      if(status != vecNoErr)
        csevere << startl << "Error in quadinterpolate, subval addition!!!" << endl;
      break;
  }

  // Do the main work here
  // Loop over each frequency and to the fringe rotation and FFT of the data
  //
  // Optional band parallelism (fxcorr-xpp): with bandthreads > 1 this loop is
  // distributed over threads; each thread binds the scratch names below to its
  // own copy (BandScratch).  The serial path binds them to the original
  // members, so every band's arithmetic is identical either way.

  const bool bandpar = (bandthreads > 1);
  // band tasks are independent; the serial form used "break" after the
  // linear-to-circular pair, so the flag stops the remaining tasks
  bool l2cdone = false;
  auto bandBody = [&](int i, int tid)
  {
    if(l2cdone)
      return;
    BandScratch *bs = bandpar ? &bandscratch[tid] : 0;
    f64 *&subphase = bs ? bs->subphase : this->subphase;
    f32 *&subarg = bs ? bs->subarg : this->subarg;
    f32 *&subsin = bs ? bs->subsin : this->subsin;
    f32 *&subcos = bs ? bs->subcos : this->subcos;
    f64 *&stepphase = bs ? bs->stepphase : this->stepphase;
    f32 *&steparg = bs ? bs->steparg : this->steparg;
    f32 *&stepsin = bs ? bs->stepsin : this->stepsin;
    f32 *&stepcos = bs ? bs->stepcos : this->stepcos;
    cf32 *&stepcplx = bs ? bs->stepcplx : this->stepcplx;
    cf32 *&complexunpacked = bs ? bs->complexunpacked : this->complexunpacked;
    cf32 *&complexrotator = bs ? bs->complexrotator : this->complexrotator;
    cf32 *&fftd = bs ? bs->fftd : this->fftd;
    cf32 *&fracsamprotatorA = bs ? bs->fracsamprotatorA : this->fracsamprotatorA;
    cf32 *&fracsamprotatorB = bs ? bs->fracsamprotatorB : this->fracsamprotatorB;
    f32 *&subfracsamparg = bs ? bs->subfracsamparg : this->subfracsamparg;
    f32 *&subfracsampsin = bs ? bs->subfracsampsin : this->subfracsampsin;
    f32 *&subfracsampcos = bs ? bs->subfracsampcos : this->subfracsampcos;
    f32 *&stepfracsamparg = bs ? bs->stepfracsamparg : this->stepfracsamparg;
    f32 *&stepfracsampsin = bs ? bs->stepfracsampsin : this->stepfracsampsin;
    f32 *&stepfracsampcos = bs ? bs->stepfracsampcos : this->stepfracsampcos;
    cf32 *&stepfracsampcplx = bs ? bs->stepfracsampcplx : this->stepfracsampcplx;
    cf32 *&piecewiserotator = bs ? bs->piecewiserotator : this->piecewiserotator;
    cf32 *&quadpiecerotator = bs ? bs->quadpiecerotator : this->quadpiecerotator;
    f64 *&subquadphase = bs ? bs->subquadphase : this->subquadphase;
    f32 *&subquadarg = bs ? bs->subquadarg : this->subquadarg;
    f32 *&subquadsin = bs ? bs->subquadsin : this->subquadsin;
    f32 *&subquadcos = bs ? bs->subquadcos : this->subquadcos;
    f32 *&kscratch = bs ? bs->kscratch : this->kscratch;
    cf32 *&tmpvec = bs ? bs->tmpvec : this->tmpvec;
    u8 *&fftbuffer = bs ? bs->fftbuffer : this->fftbuffer;
    vecFFTSpecR_f32 *&pFFTSpecR = bs ? bs->fftspecR : this->pFFTSpecR;
    vecFFTSpecC_cf32 *&pFFTSpecC = bs ? bs->fftspecC : this->pFFTSpecC;
    vecDFTSpecR_f32 *&pDFTSpecR = bs ? bs->dftspecR : this->pDFTSpecR;
    vecDFTSpecC_cf32 *&pDFTSpecC = bs ? bs->dftspecC : this->pDFTSpecC;

    // band-local state (the function-scope copies would be shared)
    int status, count, RcpIndex, LcpIndex;
    int indices[10];
    bool looff, isfraclooffset;
    f32 *currentstepchannelfreqs, *currentsubchannelfreqs;
    cf32 *fftptr;
    double phaserotation, lofreq;
    f32 phaserotationfloat;
    count = 0;
    //updated so that Nyquist channel is not accumulated for either USB or LSB data
    //and is excised entirely, so both USB and LSB data start at the same place (no sidebandoffset)
    currentstepchannelfreqs = stepchannelfreqs;
    currentsubchannelfreqs = subchannelfreqs;
    if(usedouble)
    {
      currentstepchannelfreqs = dsbstepchannelfreqs;
      /*if(config->getDRecordedLowerSideband(configindex, datastreamindex, i))
      {
        currentstepchannelfreqs = ldsbstepchannelfreqs;
        currentsubchannelfreqs = ldsbsubchannelfreqs;
      }
      else
      {
        currentstepchannelfreqs = dsbstepchannelfreqs;
      }*/
    }
    else
    {
      if(config->getDRecordedLowerSideband(configindex, datastreamindex, i))
      {
        currentstepchannelfreqs = lsbstepchannelfreqs;
      }
    }

    looff = false;
    isfraclooffset = false;
    if(recordedfreqlooffsets[i] > 0.0 || recordedfreqlooffsets[i] < 0.0) {
      looff = true;
      fraclooffset = fabs(recordedfreqlooffsets[i]) - int(fabs(recordedfreqlooffsets[i]));
      if (fraclooffset > Mode::TINY)
        isfraclooffset = true;
      if (recordedfreqlooffsets[i] < 0)
        fraclooffset = -fraclooffset;
    }

    //get ready to apply fringe rotation, if it is pre-F.  
    //By default, the local oscillator frequency (which is used for fringe rotation) is the band edge, as specified inthe input file
    lofreq = config->getDRecordedFreq(configindex, datastreamindex, i);

    // For double-sideband data, the LO frequency is at the centre of the band, not the band edge
    if (usecomplex) {
      if (config->getDRecordedLowerSideband(configindex, datastreamindex, i))
        lofreq = -lofreq;  // LSB case
      if (usedouble)
        lofreq += config->getDRecordedBandwidth(configindex, datastreamindex, i)/2.0;  // Double sideband move LO to middle of band
    }

    switch(fringerotationorder) {
      case 1: // linear

/* The actual calculation that is going on for the linear case is as follows:

   Calculate complexrotator[j]  (for j = 0 to fftchanels-1) as:

   complexrotator[j] = exp( 2 pi i * (A*j + B) )

   where:

   A = a*lofreq/fftchannels - sampletime*1.0e-6*recordedfreqlooffsets[i]
   B = b*lofreq/fftchannels + fraclofreq*integerdelay - recordedfreqlooffsets[i]*fracwalltime - fraclooffset*intwalltime

   And a, b are computed outside the recordedfreq loop (variable i)
*/

        status = vectorMulC_f64(subxval, lofreq, subphase, arraystridelength);
        if(status != vecNoErr)
          csevere << startl << "Error in linearinterpolate lofreq sub multiplication!!!" << status << endl;
        status = vectorMulC_f64(stepxval, lofreq, stepphase, numfrstrides);
        if(status != vecNoErr)
          csevere << startl << "Error in linearinterpolate lofreq step multiplication!!!" << status << endl;
        if(fractionalLoFreq) {
          status = vectorAddC_f64_I((lofreq-int(lofreq))*double(integerdelay), subphase, arraystridelength);
          if(status != vecNoErr)
            csevere << startl << "Error in linearinterpolate lofreq non-integer freq addition!!!" << status << endl;
        }
        if(looff) {
          status = vectorMulC_f64(subtoff, -recordedfreqlooffsets[i], subtval, arraystridelength);
          if(status != vecNoErr)
            csevere << startl << "Error in LO offset calculation (time domain, sub vector)" << status << endl;
          status = vectorMulC_f64(steptoff, -recordedfreqlooffsets[i], steptval, numfrstrides);
          if(status != vecNoErr)
            csevere << startl << "Error in LO offset calculation (time domain, step vector)" << status << endl;
          status = vectorAdd_f64_I(subtval, subphase, arraystridelength);
          if(status != vecNoErr)
            csevere << startl << "Error in LO offset addition (time domain, sub vector)" << status << endl;
          status = vectorAdd_f64_I(steptval, stepphase, numfrstrides);
          if(status != vecNoErr)
            csevere << startl << "Error in LO offset addition (time domain, step vector)" << status << endl;
          status = vectorAddC_f64_I(-recordedfreqlooffsets[i]*fracwalltime, subphase, arraystridelength);
          if(status != vecNoErr)
            csevere << startl << "Error in LO offset addition (time domain, wallclock offset)" << status << endl;
          if(isfraclooffset) {
            status = vectorAddC_f64_I(-fraclooffset*intwalltime, subphase, arraystridelength);
            if(status != vecNoErr)
              csevere << startl << "Error in LO offset addition (time domain, frac LO offset wallclock offset)" << status << endl;
          }
        }
        for(int j=0;j<arraystridelength;j++) {
          subarg[j] = -TWO_PI*(subphase[j] - int(subphase[j]));
        }
        for(int j=0;j<numfrstrides;j++) {
          steparg[j] = -TWO_PI*(stepphase[j] - int(stepphase[j]));
        }
        status = vectorSinCos_f32(subarg, subsin, subcos, arraystridelength);
        if(status != vecNoErr)
          csevere << startl << "Error in sin/cos of sub rotate argument!!!" << endl;
        status = vectorSinCos_f32(steparg, stepsin, stepcos, numfrstrides);
        if(status != vecNoErr)
          csevere << startl << "Error in sin/cos of step rotate argument!!!" << endl;
        status = vectorRealToComplex_f32(subcos, subsin, complexrotator, arraystridelength);
        if(status != vecNoErr)
          csevere << startl << "Error assembling sub into complex!!!" << endl;
        status = vectorRealToComplex_f32(stepcos, stepsin, stepcplx, numfrstrides);
        if(status != vecNoErr)
          csevere << startl << "Error assembling step into complex!!!" << endl;
        for(int j=1;j<numfrstrides;j++) {
          status = vectorMulC_cf32(complexrotator, stepcplx[j], &complexrotator[j*arraystridelength], arraystridelength);
          if(status != vecNoErr)
            csevere << startl << "Error doing the time-saving complex multiplication!!!" << endl;
        }
        break;
      case 2: // Quadratic
        status = vectorMulC_f64(subxval, lofreq, subphase, arraystridelength);
        if(status != vecNoErr)
          csevere << startl << "Error in quadinterpolate lofreq sub multiplication!!!" << status << endl;
        if(fractionalLoFreq) {
          status = vectorAddC_f64_I((lofreq-int(lofreq))*double(integerdelay), subphase, arraystridelength);
          if(status != vecNoErr)
            csevere << startl << "Error in linearinterpolate lofreq non-integer freq addition!!!" << status << endl;
        }
        status = vectorMulC_f64(subquadxval, lofreq, subquadphase, arraystridelength);
        if(status != vecNoErr)
          csevere << startl << "Error in quadinterpolate lofreq subquad multiplication!!!" << status << endl;
        status = vectorMulC_f64(stepxval, lofreq, stepphase, numfrstrides);
        if(status != vecNoErr)
          csevere << startl << "Error in quadinterpolate lofreq step multiplication!!!" << status << endl;
        if(looff) {
          status = vectorMulC_f64(subtoff, -recordedfreqlooffsets[i], subtval, arraystridelength);
          if(status != vecNoErr)
            csevere << startl << "Error in LO offset calculation (time domain, sub vector)" << status << endl;
          status = vectorMulC_f64(steptoff, -recordedfreqlooffsets[i], steptval, numfrstrides);
          if(status != vecNoErr)
            csevere << startl << "Error in LO offset calculation (time domain, step vector)" << status << endl;
          status = vectorAdd_f64_I(subtval, subphase, arraystridelength);
          if(status != vecNoErr)
            csevere << startl << "Error in LO offset addition (time domain, sub vector)" << status << endl;
          status = vectorAdd_f64_I(steptval, stepphase, numfrstrides);
          if(status != vecNoErr)
            csevere << startl << "Error in LO offset addition (time domain, step vector)" << status << endl;
          status = vectorAddC_f64_I(-recordedfreqlooffsets[i]*fracwalltime, subphase, arraystridelength);
          if(status != vecNoErr)
            csevere << startl << "Error in LO offset addition (time domain, wallclock offset)" << status << endl;
          if(isfraclooffset) {
            status = vectorAddC_f64_I(-fraclooffset*intwalltime, subphase, arraystridelength);
            if(status != vecNoErr)
              csevere << startl << "Error in LO offset addition (time domain, frac LO offset wallclock offset)" << status << endl;
          }
        }
        for(int j=0;j<arraystridelength;j++) {
          subarg[j] = -TWO_PI*(subphase[j] - int(subphase[j]));
          subquadarg[j] = -TWO_PI*(subquadphase[j] - int(subquadphase[j]));
        }
        for(int j=0;j<numfrstrides;j++) {
          steparg[j] = -TWO_PI*(stepphase[j] - int(stepphase[j]));
        }
        status = vectorSinCos_f32(subarg, subsin, subcos, arraystridelength);
        if(status != vecNoErr)
          csevere << startl << "Error in sin/cos of sub rotate argument!!!" << endl;
        status = vectorSinCos_f32(subquadarg, subquadsin, subquadcos, arraystridelength);
        if(status != vecNoErr)
          csevere << startl << "Error in sin/cos of subquad rotate argument!!!" << endl;
        status = vectorSinCos_f32(steparg, stepsin, stepcos, numfrstrides);
        if(status != vecNoErr)
          csevere << startl << "Error in sin/cos of step rotate argument!!!" << endl;
        status = vectorRealToComplex_f32(subcos, subsin, piecewiserotator, arraystridelength);
        if(status != vecNoErr)
          csevere << startl << "Error assembling sub into complex" << endl;
        status = vectorRealToComplex_f32(subquadcos, subquadsin, quadpiecerotator, arraystridelength);
        if(status != vecNoErr)
          csevere << startl << "Error assembling sub into complex" << endl;
        status = vectorRealToComplex_f32(stepcos, stepsin, stepcplx, numfrstrides);
        if(status != vecNoErr)
          csevere << startl << "Error assembling step into complex" << endl;
        for(int j=0;j<numfrstrides;j++) {
          status = vectorMulC_cf32(piecewiserotator, stepcplx[j], &complexrotator[j*arraystridelength], arraystridelength);
          if(status != vecNoErr)
            csevere << startl << "Error doing the time-saving complex mult (striding)" << endl;
          status = vectorMul_cf32_I(quadpiecerotator, piecewiserotator, arraystridelength);
          if(status != vecNoErr)
            csevere << startl << "Error doing the time-saving complex mult (adjusting linear gradient)" << endl;
        }
        break;
    }

    // Note recordedfreqclockoffsetsdata will usually be zero, but avoiding if statement
    status = vectorMulC_f32(currentsubchannelfreqs, fracsampleerror - recordedfreqclockoffsets[i] + recordedfreqclockoffsetsdelta[i]/2, subfracsamparg, arraystridelength);
    if(status != vecNoErr) {
      csevere << startl << "Error in frac sample correction, arg generation (sub)!!!" << status << endl;
      exit(1);
    }
    status = vectorMulC_f32(currentstepchannelfreqs, fracsampleerror - recordedfreqclockoffsets[i] + recordedfreqclockoffsetsdelta[i]/2, stepfracsamparg, numfracstrides/2);
    if(status != vecNoErr)
      csevere << startl << "Error in frac sample correction, arg generation (step)!!!" << status << endl;

    // For zero-th order (post-F) fringe rotation, calculate the fringe rotation (+ LO offset if necessary)
    if(fringerotationorder == 0) { // do both LO offset and fringe rotation  (post-F)
      phaserotation = (averagedelay-integerdelay)*lofreq;
      if(fractionalLoFreq)
        phaserotation += integerdelay*(lofreq-int(lofreq));
      phaserotation -= walltimesecs*recordedfreqlooffsets[i];
      phaserotationfloat = (f32)(-TWO_PI*(phaserotation-int(phaserotation)));
      status = vectorAddC_f32_I(phaserotationfloat, subfracsamparg, arraystridelength);
      if(status != vecNoErr)
        csevere << startl << "Error in post-f phase rotation addition (+ maybe LO offset correction), sub!!!" << status << endl;
    }

    //create the fractional sample correction array
    status = vectorSinCos_f32(subfracsamparg, subfracsampsin, subfracsampcos, arraystridelength);
    if(status != vecNoErr)
      csevere << startl << "Error in frac sample correction, sin/cos (sub)!!!" << status << endl;
    status = vectorSinCos_f32(stepfracsamparg, stepfracsampsin, stepfracsampcos, numfracstrides/2);
    if(status != vecNoErr)
      csevere << startl << "Error in frac sample correction, sin/cos (sub)!!!" << status << endl;
    status = vectorRealToComplex_f32(subfracsampcos, subfracsampsin, fracsamprotatorA, arraystridelength);
    if(status != vecNoErr)
      csevere << startl << "Error in frac sample correction, real to complex (sub)!!!" << status << endl;
    status = vectorRealToComplex_f32(stepfracsampcos, stepfracsampsin, stepfracsampcplx, numfracstrides/2);
    if(status != vecNoErr)
      csevere << startl << "Error in frac sample correction, real to complex (step)!!!" << status << endl;
    for(int j=1;j<numfracstrides/2;j++) {
      status = vectorMulC_cf32(fracsamprotatorA, stepfracsampcplx[j], &(fracsamprotatorA[j*arraystridelength]), arraystridelength);
      if(status != vecNoErr)
        csevere << startl << "Error doing the time-saving complex multiplication in frac sample correction!!!" << endl;
    }

    // now do the first arraystridelength elements (which are different from fracsampptr1 for LSB case)
    status = vectorMulC_cf32_I(stepfracsampcplx[0], fracsamprotatorA, arraystridelength);
    if(status != vecNoErr)
    csevere << startl << "Error doing the first bit of the time-saving complex multiplication in frac sample correction!!!" << endl;

    // Repeat the post F correction steps if each pol is different
    if (deltapoloffsets) {
      status = vectorMulC_f32(currentsubchannelfreqs, fracsampleerror - recordedfreqclockoffsets[i] - recordedfreqclockoffsetsdelta[i]/2, subfracsamparg, arraystridelength); 
      if(status != vecNoErr) {
	csevere << startl << "Error in frac sample correction, arg generation (sub)!!!" << status << endl;
	exit(1);
      }
      status = vectorMulC_f32(currentstepchannelfreqs, fracsampleerror - recordedfreqclockoffsets[i] - recordedfreqclockoffsetsdelta[i]/2, stepfracsamparg, numfracstrides/2);  //L2C add delay
      if(status != vecNoErr)
	csevere << startl << "Error in frac sample correction, arg generation (step)!!!" << status << endl;

      // For zero-th order (post-F) fringe rotation, calculate the fringe rotation (+ LO offset if necessary)
      if(fringerotationorder == 0) { // do both LO offset and fringe rotation  (post-F)
	phaserotation = (averagedelay-integerdelay)*lofreq;
	if(fractionalLoFreq)
	  phaserotation += integerdelay*(lofreq-int(lofreq));
	phaserotation -= walltimesecs*recordedfreqlooffsets[i];
	phaserotationfloat = (f32)(-TWO_PI*(phaserotation-int(phaserotation)));
	status = vectorAddC_f32_I(phaserotationfloat, subfracsamparg, arraystridelength);
	if(status != vecNoErr)
	  csevere << startl << "Error in post-f phase rotation addition (+ maybe LO offset correction), sub!!!" << status << endl;
      }
      
      //create the fractional sample correction array
      status = vectorSinCos_f32(subfracsamparg, subfracsampsin, subfracsampcos, arraystridelength);
      if(status != vecNoErr)
	csevere << startl << "Error in frac sample correction, sin/cos (sub)!!!" << status << endl;
      status = vectorSinCos_f32(stepfracsamparg, stepfracsampsin, stepfracsampcos, numfracstrides/2);
      if(status != vecNoErr)
	csevere << startl << "Error in frac sample correction, sin/cos (sub)!!!" << status << endl;
      status = vectorRealToComplex_f32(subfracsampcos, subfracsampsin, fracsamprotatorB, arraystridelength); // L2C change pointers
      if(status != vecNoErr)
	csevere << startl << "Error in frac sample correction, real to complex (sub)!!!" << status << endl;
      status = vectorRealToComplex_f32(stepfracsampcos, stepfracsampsin, stepfracsampcplx, numfracstrides/2);
      if(status != vecNoErr)
	csevere << startl << "Error in frac sample correction, real to complex (step)!!!" << status << endl;
      for(int j=1;j<numfracstrides/2;j++) {
	status = vectorMulC_cf32(fracsamprotatorB, stepfracsampcplx[j], &(fracsamprotatorB[j*arraystridelength]), arraystridelength); // L2C change pointers
	if(status != vecNoErr)
	  csevere << startl << "Error doing the time-saving complex multiplication in frac sample correction!!!" << endl;
      }

      // now do the first arraystridelength elements (which are different from fracsampptr1 for LSB case)
      status = vectorMulC_cf32_I(stepfracsampcplx[0], fracsamprotatorB, arraystridelength); // L2C change pointers
      if(status != vecNoErr)
	csevere << startl << "Error doing the first bit of the time-saving complex multiplication in frac sample correction!!!" << endl;

    }

    for(int j=0;j<numrecordedbands;j++)  // Loop over all recorded bands looking for the matching frequency we should be dealing with
    {
      if(config->matchingRecordedBand(configindex, datastreamindex, i, j))
      {
        indices[count++] = j;
        switch(fringerotationorder) {
          case 0: //post-F
            if (usecomplex) {
              cfatal << startl << "Post-F (0th order) fringe rotation not currently supported for complex sampled data!" << endl;
              exit(1);
            }
              
            fftptr = (config->getDRecordedLowerSideband(configindex, datastreamindex, i))?conjfftoutputs[j][subloopindex]:fftoutputs[j][subloopindex];

            //do the fft
            // Chris add C2C fft for complex data
            if(isfft) {
              status = vectorFFT_RtoC_f32(&(unpackedarrays[j][nearestsample - unpackstartsamples]), (f32*) fftptr, pFFTSpecR, fftbuffer);
              if (status != vecNoErr)
                csevere << startl << "Error in FFT!!!" << status << endl;
            //fix the lower sideband if required
            }
            else{
              status = vectorDFT_RtoC_f32(&(unpackedarrays[j][nearestsample - unpackstartsamples]), (f32*) fftptr, pDFTSpecR, fftbuffer);
              if (status != vecNoErr)
                csevere << startl << "Error in DFT!!!" << status << endl;
            }
            if(config->getDRecordedLowerSideband(configindex, datastreamindex, i))
            {
              status = vectorConjFlip_cf32(&(fftptr[1]), fftoutputs[j][subloopindex], recordedbandchannels);
              if(status != vecNoErr)
                csevere << startl << "Error in conjugate!!!" << status << endl;
            }
            break;
          case 1: // Linear
          case 2: // Quadratic
            if (usecomplex) {
              status = vectorMul_cf32(complexrotator, &unpackedcomplexarrays[j][nearestsample - unpackstartsamples], complexunpacked, fftchannels);
              // The following can be uncommented (and the above commented) if wanting to 'turn off' fringe rotation for testing in the complex case
              //status = vectorCopy_cf32(&unpackedcomplexarrays[j][nearestsample - unpackstartsamples], complexunpacked, fftchannels);
              if (status != vecNoErr)
                csevere << startl << "Error in complex fringe rotation" << endl;
            } else {
              status = vectorRealToComplex_f32(&(unpackedarrays[j][nearestsample - unpackstartsamples]), NULL, complexunpacked, fftchannels);
              if (status != vecNoErr)
                csevere << startl << "Error in real->complex conversion" << endl;
              status = vectorMul_cf32_I(complexrotator, complexunpacked, fftchannels);
              if(status != vecNoErr)
              	csevere << startl << "Error in fringe rotation!!!" << status << endl;
            }
            if(isfft) {
              status = vectorFFT_CtoC_cf32(complexunpacked, fftd, pFFTSpecC, fftbuffer);
              if(status != vecNoErr)
                csevere << startl << "Error doing the FFT!!!" << endl;
            }
            else {
              status = vectorDFT_CtoC_cf32(complexunpacked, fftd, pDFTSpecC, fftbuffer);
              if(status != vecNoErr)
                csevere << startl << "Error doing the DFT!!!" << endl;
            }

            if(config->getDRecordedLowerSideband(configindex, datastreamindex, i)) {
              // All lower sideband bands need to be conjugated (achieved by taking the second half of the band for real-valued inputs)
              // Additionally for the complex-valued inputs, the order of the frequency channels is reversed so they need to be flipped
              // (for the double sideband case, in two halves, for the regular case, the whole thing)
              if (usecomplex) {
                if (usedouble) {
                  status = vectorConjFlip_cf32(fftd, fftoutputs[j][subloopindex], recordedbandchannels/2+1);
                  status = vectorConjFlip_cf32(&fftd[recordedbandchannels/2]+1, &fftoutputs[j][subloopindex][recordedbandchannels/2]+1, recordedbandchannels/2-1);
                } else {
                  //status = vectorConjFlip_cf32(fftd, fftoutputs[j][subloopindex], recordedbandchannels);
                  // note: using vectorConjFlip_cf32() -lofreq breaks Complex LSB (non-DSB!) fringes for VGOS *assuming* VGOS RDBE-G indeed LSB like memos claim
                  // fix?: LSB fringes are restored at least for a synthetic fully correlated data set of Complex USB and Complex LSB data.
                  //       The reversal has to be changed as below to retain DC in bin 0, producing not [ch1 ch2 ch3 ... DC] but instead [DC ch1 ch2 ch3 ...]
                  // todo: validate fix on real world definitely-known-LSB data (evidenced by pcal tone positions etc), then uncomment the next lines:
                  status = vectorConjFlip_cf32(fftd+1, fftoutputs[j][subloopindex]+1, recordedbandchannels-1);
                  fftoutputs[j][subloopindex][0] = fftd[0];
                }
              }
              else {
		// By grabbing the "upper half" of the FFT output, channels are naturally in frequency assending order, even for LSB data
                status = vectorCopy_cf32(&(fftd[recordedbandchannels]), fftoutputs[j][subloopindex], recordedbandchannels);
              }
            }
            else {
              // For upper sideband bands, normally just need to copy the fftd channels.
              // However for complex double upper sideband, the two halves of the frequency space are swapped, so they need to be swapped back
              if (usecomplex && usedouble) {
                status = vectorCopy_cf32(fftd, &fftoutputs[j][subloopindex][recordedbandchannels/2], recordedbandchannels/2);
                status = vectorCopy_cf32(&fftd[recordedbandchannels/2], fftoutputs[j][subloopindex], recordedbandchannels/2);
              } else {
                status = vectorCopy_cf32(fftd, fftoutputs[j][subloopindex], recordedbandchannels);
              }
            }
            if(status != vecNoErr)
              csevere << startl << "Error copying FFT results!!!" << endl;
            break;
        }

	// At this point in the code the array fftoutputs[j] contains complex-valued voltage spectra with the following properties:
	//
	// 1. The zero element corresponds to the lowest sky frequency.  That is:
	//    fftoutputs[j][0] = Local Oscillator Frequency              (for Upper Sideband)
	//    fftoutputs[j][0] = Local Oscillator Frequency - bandwidth  (for Lower Sideband)
	//    fftoutputs[j][0] = Local Oscillator Frequency - bandwidth  (for Complex Lower Sideband)
	//    fftoutputs[j][0] = Local Oscillator Frequency - bandwidth/2(for Complex Double Upper Sideband)
	//    fftoutputs[j][0] = Local Oscillator Frequency - bandwidth/2(for Complex Double Lower Sideband)
	// 
	// 2. The frequency increases monotonically with index
	// 
	// 3. The last element of the array corresponds to the highest sky frequency minus the spectral resolution.
	//    (i.e., the first element beyond the array bound corresponds to the highest sky frequency)

        if(dumpkurtosis) //do the necessary accumulation
        {
          status = vectorMagnitude_cf32(fftoutputs[j][subloopindex], kscratch, recordedbandchannels);
          if(status != vecNoErr)
            csevere << startl << "Error taking kurtosis magnitude!" << endl;
          status = vectorSquare_f32_I(kscratch, recordedbandchannels);
          if(status != vecNoErr)
            csevere << startl << "Error in first kurtosis square!" << endl;
          status = vectorAdd_f32_I(kscratch, s1[j], recordedbandchannels);
          if(status != vecNoErr)
            csevere << startl << "Error in kurtosis s1 accumulation!" << endl;
          status = vectorSquare_f32_I(kscratch, recordedbandchannels);
          if(status != vecNoErr)
            csevere << startl << "Error in second kurtosis square!" << endl;
          status = vectorAdd_f32_I(kscratch, s2[j], recordedbandchannels);
          if(status != vecNoErr)
            csevere << startl << "Error in kurtosis s2 accumulation!" << endl;
        }

        //do the frac sample correct (+ phase shifting if applicable, + fringe rotate if its post-f)
	if (deltapoloffsets==false || config->getDRecordedBandPol(configindex, datastreamindex, j)=='R') {
	  status = vectorMul_cf32_I(fracsamprotatorA, fftoutputs[j][subloopindex], recordedbandchannels);
	} else {
	  status = vectorMul_cf32_I(fracsamprotatorB, fftoutputs[j][subloopindex], recordedbandchannels);
	}
	if(status != vecNoErr)
	  csevere << startl << "Error in application of frac sample correction!!!" << status << endl;

        //do the conjugation
        status = vectorConj_cf32(fftoutputs[j][subloopindex], conjfftoutputs[j][subloopindex], recordedbandchannels);
        if(status != vecNoErr)
          csevere << startl << "Error in conjugate!!!" << status << endl;

	if (!linear2circular) {
	  //do the autocorrelation (skipping Nyquist channel)
	  status = vectorAddProduct_cf32(fftoutputs[j][subloopindex], conjfftoutputs[j][subloopindex], acTarget(0, j), recordedbandchannels);
	  if(status != vecNoErr)
	    csevere << startl << "Error in autocorrelation!!!" << status << endl;

	  //store the weight for the autocorrelations
          if(perbandweights)
          {
	    addWeight(false, j, perbandweights[subloopindex][j]);
          }
          else
          {
	    addWeight(false, j, dataweight[subloopindex]);
          }
	}
      }
    }


    if (count>1) {
      // Do linear to circular conversion if required
      if (linear2circular) {

	// FIXME: Apply gain correction

	if (config->getDRecordedBandPol(configindex, datastreamindex, indices[0])=='R') {
	    RcpIndex = indices[0];
	    LcpIndex = indices[1];
	  } else {
	    RcpIndex = indices[1];
	    LcpIndex = indices[0];
	  }
	  
	  // Rotate Lcp by 90deg
	  vectorMulC_cf32_I(phasecorrA[i], fftoutputs[LcpIndex][subloopindex], recordedbandchannels);
	  vectorMulC_cf32_I(phasecorrconjA[i], conjfftoutputs[LcpIndex][subloopindex], recordedbandchannels);

	  // Add and subtract
	  vectorSub_cf32(fftoutputs[LcpIndex][subloopindex], fftoutputs[RcpIndex][subloopindex], tmpvec, recordedbandchannels);
	  vectorAdd_cf32_I(fftoutputs[LcpIndex][subloopindex], fftoutputs[RcpIndex][subloopindex], recordedbandchannels);
	  vectorCopy_cf32(tmpvec, fftoutputs[LcpIndex][subloopindex], recordedbandchannels);

	  vectorSub_cf32(conjfftoutputs[LcpIndex][subloopindex], conjfftoutputs[RcpIndex][subloopindex], tmpvec, recordedbandchannels);
	  vectorAdd_cf32_I(conjfftoutputs[LcpIndex][subloopindex], conjfftoutputs[RcpIndex][subloopindex], recordedbandchannels);
	  vectorCopy_cf32(tmpvec, conjfftoutputs[LcpIndex][subloopindex], recordedbandchannels);

	  // serial form: break; the pool task returns instead
	  l2cdone = true;
	  return;
      } else if (phasepoloffset) {
	// Add phase offset to Lcp

	if (config->getDRecordedBandPol(configindex, datastreamindex, indices[0])=='R') {
	    LcpIndex = indices[1];
	  } else {
	    LcpIndex = indices[0];
	  }
	  
	  // Rotate Lcp by phase offset deg
	  vectorMulC_cf32_I(phasecorrA[i], fftoutputs[LcpIndex][subloopindex], recordedbandchannels);
	  vectorMulC_cf32_I(phasecorrconjA[i], conjfftoutputs[LcpIndex][subloopindex], recordedbandchannels);
      }

      //if we need to, do the cross-polar autocorrelations
      if(calccrosspolautocorrs) {
	status = vectorAddProduct_cf32(fftoutputs[indices[0]][subloopindex], conjfftoutputs[indices[1]][subloopindex], acTarget(1, indices[0]), recordedbandchannels);
	if(status != vecNoErr)
	  csevere << startl << "Error in cross-polar autocorrelation!!!" << status << endl;
	status = vectorAddProduct_cf32(fftoutputs[indices[1]][subloopindex], conjfftoutputs[indices[0]][subloopindex], acTarget(1, indices[1]), recordedbandchannels);
	if(status != vecNoErr)
	  csevere << startl << "Error in cross-polar autocorrelation!!!" << status << endl;
      
	//store the weights
        if(perbandweights)
        {
	  addWeight(true, indices[0], perbandweights[subloopindex][indices[0]]*perbandweights[subloopindex][indices[1]]);
	  addWeight(true, indices[1], perbandweights[subloopindex][indices[0]]*perbandweights[subloopindex][indices[1]]);
        }
        else
        {
	  addWeight(true, indices[0], dataweight[subloopindex]);
	  addWeight(true, indices[1], dataweight[subloopindex]);
        }
      }
    }
    
    if (linear2circular) {// Delay this as it is possible for linear2circular to be active, but just one pol present
      for (int k=0; k<count; k++) {
	//do the autocorrelation (skipping Nyquist channel)
	status = vectorAddProduct_cf32(fftoutputs[indices[k]][subloopindex], conjfftoutputs[indices[k]][subloopindex], acTarget(0, indices[k]), recordedbandchannels);
	if(status != vecNoErr)
	  csevere << startl << "Error in autocorrelation!!!" << status << endl;

	//store the weight
        if(perbandweights)
        {
	  addWeight(false, indices[k], perbandweights[subloopindex][indices[k]]);
        }
        else
        {
	  addWeight(false, indices[k], dataweight[subloopindex]);
        }
      }
    }
  };
  if(bandpar && bandpool != 0)
    bandpool->run(numrecordedfreqs, bandBody);
  else
    for(int i=0;i<numrecordedfreqs;i++)
      bandBody(i, 0);
}

void Mode::averageFrequency()
{
  cf32 tempsum;
  int status, outputchans;

  if(channelstoaverage == 1)
    return; //no need to do anything;

  outputchans = recordedbandchannels/channelstoaverage;
  for(int i=0;i<autocorrwidth;i++)
  {
    for(int j=0;j<numrecordedbands;j++)
    {
      status = vectorMean_cf32(autocorrelations[i][j], channelstoaverage, &tempsum, vecAlgHintFast);
      if(status != vecNoErr)
        cerror << startl << "Error trying to average in frequency!" << endl;
      autocorrelations[i][j][0] = tempsum;
      for(int k=1;k<outputchans;k++)
      {
        status = vectorMean_cf32(&(autocorrelations[i][j][k*channelstoaverage]), channelstoaverage, &(autocorrelations[i][j][k]), vecAlgHintFast);
        if(status != vecNoErr)
          cerror << startl << "Error trying to average in frequency!" << endl;
      }
    }
  }
}

bool Mode::calculateAndAverageKurtosis(int numblocks, int maxchannels)
{
  int status, kchanavg;
  bool nonzero = false;

  for(int i=0;i<numrecordedbands;i++)
  {
    nonzero = false;
    for(int j=0;j<recordedbandchannels;j++)
    {
      if(s1[i][j] > 0.0)
      {
        nonzero = true;
        break;
      }
    }
    if(!nonzero)
      continue;
    status = vectorSquare_f32_I(s1[i], recordedbandchannels);
    if(status != vecNoErr)
      cerror << startl << "Problem calculating kurtosis (squaring s1)" << endl;
    status = vectorMulC_f32_I((f32)numblocks, s2[i], recordedbandchannels);
    if(status != vecNoErr)
      cerror << startl << "Problem calculating kurtosis (scaling s2)" << endl;
    status = vectorDivide_f32(s1[i], s2[i], sk[i], recordedbandchannels);
    if(status != vecNoErr)
      cerror << startl << "Problem calculating kurtosis (s2/s1^2)" << endl;
    status = vectorAddC_f32_I(-1.0, sk[i], recordedbandchannels);
    if(status != vecNoErr)
      cerror << startl << "Problem calculating kurtosis (sk - 1)" << endl;
    status = vectorMulC_f32_I((f32)numblocks/(numblocks - 1.0), sk[i], recordedbandchannels);
    if(status != vecNoErr)
      cerror << startl << "Problem calculating kurtosis (sk * numblocks/(numblocks-1))" << endl;
    status = vectorAddC_f32_I(-1.0, sk[i], recordedbandchannels);
    if(status != vecNoErr)
      cerror << startl << "Problem calculating kurtosis (sk - 1)" << endl;
    if(maxchannels < recordedbandchannels)
    {
      kchanavg = recordedbandchannels/maxchannels;
      status = vectorMulC_f32_I(1.0/((f32)kchanavg), sk[i], recordedbandchannels);
      if(status != vecNoErr)
        cerror << startl << "Problem calculating kurtosis (sk average)" << endl;
      for(int j=0;j<maxchannels;j++)
      {
        sk[i][j] = sk[i][j*kchanavg];
        for(int k=1;k<kchanavg;k++)
          sk[i][j] += sk[i][j*kchanavg + k];
      }
    }
  }

  return nonzero;
}

// P3: allocate the per-block accumulators (see mode.h).  Only the fxcorr-f
// parallel form calls this; everything else leaves them off and behaves
// exactly as before.  slotcount slots per band: the parallel form passes the
// number of blocks this thread will process per subint (a fraction of
// blockspersend), so the allocation -- and above all its first-touch page
// faults, which used to cost ~0.5 s per replica at 32000 blocks -- scale with
// the per-thread work.
bool Mode::setSubloopCapacity(int nslots)
{
  if(nslots <= fftslots)
    return true;
  // every slot must be owned by this Mode: zoom slots alias a parent band,
  // and per-band weights are indexed by the same slot numbers
  if(numzoombands > 0 || perbandweights != 0)
    return false;

  for(int j=0;j<numrecordedbands;j++)
  {
    cf32 **no = new cf32*[nslots];
    cf32 **nc = new cf32*[nslots];
    for(int k=0;k<fftslots;k++)
    {
      no[k] = fftoutputs[j][k];
      nc[k] = conjfftoutputs[j][k];
    }
    for(int k=fftslots;k<nslots;k++)
    {
      if(fringerotationorder == 0)	// post-F: one extra bin
      {
        no[k] = vectorAlloc_cf32(recordedbandchannels+1);
        nc[k] = vectorAlloc_cf32(recordedbandchannels+1);
      }
      else
      {
        no[k] = vectorAlloc_cf32(recordedbandchannels);
        nc[k] = vectorAlloc_cf32(recordedbandchannels);
      }
      estimatedbytes += 2*sizeof(cf32)*(recordedbandchannels + (fringerotationorder == 0 ? 1 : 0));
    }
    delete [] fftoutputs[j];
    fftoutputs[j] = no;
    delete [] conjfftoutputs[j];
    conjfftoutputs[j] = nc;
  }

  f32 *ndw = vectorAlloc_f32(nslots);
  for(int k=0;k<fftslots;k++)
    ndw[k] = dataweight[k];
  for(int k=fftslots;k<nslots;k++)
    ndw[k] = 0.0;
  vectorFree(dataweight);
  dataweight = ndw;
  fftslots = nslots;
  return true;
}

void Mode::enableBlockAccumulators(int slotcount)
{
  if(blockacblocks > 0)
    return;	// already enabled

  if(slotcount <= 0)
    slotcount = blockspersend;	// conservative: the whole subint
  blockacblocks = slotcount;
  blockac = new cf32***[autocorrwidth];
  blockweight = new f32*[autocorrwidth];
  for(int i=0;i<autocorrwidth;i++)
  {
    blockac[i] = new cf32**[blockacblocks];
    blockweight[i] = new f32[blockacblocks*numrecordedbands];
    for(int b=0;b<blockacblocks;b++)
    {
      blockac[i][b] = new cf32*[numrecordedbands];
      for(int j=0;j<numrecordedbands;j++)
      {
        // no up-front zeroing: process() clears each block's slots as it
        // reaches them (clearBlockAccumulators), so the first-touch page
        // faults land on the slots that are actually used
        blockac[i][b][j] = vectorAlloc_cf32(recordedbandchannels);
      }
    }
  }

  // P3 (pcal): park each block's pcal integration state too, when every band's
  // extractor exposes its raw accumulator (PCal::getAccumSize()).  When one
  // does not, the slots stay off and the caller has to run the pcal path
  // serially (Mode::blockPcalCapable() tells it).
  if(extractor != 0 && blockPcalCapable())
  {
    blockpcallen = new int[numrecordedbands];
    blockpcal = new f32**[blockacblocks];
    blockpcalsamples = new uint64_t*[blockacblocks];
    for(int b=0;b<blockacblocks;b++)
    {
      blockpcal[b] = new f32*[numrecordedbands];
      blockpcalsamples[b] = new uint64_t[numrecordedbands];
      for(int j=0;j<numrecordedbands;j++)
      {
        blockpcallen[j] = (int)extractor[j]->getAccumSize();
        blockpcal[b][j] = vectorAlloc_f32(blockpcallen[j]);
      }
    }
  }
  currentblock = 0;
}

void Mode::zeroAutocorrelations()
{
  int status;

  for(int i=0;i<autocorrwidth;i++)
  {
    for(int j=0;j<numrecordedbands;j++)
    {
      status = vectorZero_cf32(autocorrelations[i][j], recordedbandchannels);
      if(status != vecNoErr)
        cerror << startl << "Error trying to zero autocorrelations!" << endl;
      weights[i][j] = 0.0;
    }
  }
  // Note: the per-block slots are NOT zeroed here.  They belong to a single
  // block of one subint and the parallel form clears each one just before the
  // block that fills it (clearBlockAccumulators), so only the slots actually
  // used are touched.  Zeroing them here would write every slot of every Mode
  // every subint for nothing.
}

// P3: zero one block's slots (all bands, both polarisations).  Cheap: the
// block arrays are per-thread private, so no synchronisation is involved.
// index is the global block index; the slot is index - blockslotbase.
void Mode::clearBlockAccumulators(int index)
{
  int block = index - blockslotbase;
  if(blockacblocks <= 0 || block < 0 || block >= blockacblocks)
    return;

  for(int i=0;i<autocorrwidth;i++)
  {
    for(int j=0;j<numrecordedbands;j++)
      vectorZero_cf32(blockac[i][block][j], recordedbandchannels);
    for(int j=0;j<numrecordedbands;j++)
      blockweight[i][block*numrecordedbands + j] = 0.0f;
  }
  if(blockpcal != 0)
  {
    // A block that never gets fed (invalid data returns early in process())
    // must contribute zeros, exactly like the serial form where nothing was
    // added to the extractor for it.
    for(int j=0;j<numrecordedbands;j++)
    {
      vectorZero_f32(blockpcal[block][j], blockpcallen[j]);
      blockpcalsamples[block][j] = 0;
    }
  }
}

// P3 (pcal): true when every band's pcal extractor exposes its raw
// accumulator, i.e. the block-ordered parallel reduction is available.  The
// slots stay off when this is false and fxcorr-f then runs the pcal path
// serially (better a slow correct run than a fast wrong one).
bool Mode::blockPcalCapable() const
{
  if(extractor == 0)
    return false;
  for(int i=0;i<numrecordedbands;i++)
  {
    if(extractor[i]->getAccumSize() == 0)
      return false;
  }
  return true;
}

// P3 (pcal): add one parked block's accumulation (and its sample count) back
// into this Mode's extractor.  The caller walks the blocks in block order,
// which reproduces the serial addition sequence exactly.
void Mode::addPcalBlock(int band, const f32 *accum, uint64_t samples)
{
  if(extractor == 0 || accum == 0)
    return;
  extractor[band]->addAccum(accum, samples);
}

void Mode::zeroKurtosis()
{
  int status;

  if(sk == 0)
  {
    s1 = new f32*[numrecordedbands];
    s2 = new f32*[numrecordedbands];
    sk = new f32*[numrecordedbands];
    kscratch = vectorAlloc_f32(recordedbandchannels);
    for(int i=0;i<numrecordedbands;i++)
    {
      s1[i] = vectorAlloc_f32(recordedbandchannels);
      s2[i] = vectorAlloc_f32(recordedbandchannels);
      sk[i] = vectorAlloc_f32(recordedbandchannels);
    }
  }

  for(int i=0;i<numrecordedbands;i++)
  {
    status = vectorZero_f32(s1[i], recordedbandchannels);
    if(status != vecNoErr)
      cerror << startl << "Error trying to zero kurtosis!" << endl;
    status = vectorZero_f32(s2[i], recordedbandchannels);
    if(status != vecNoErr)
      cerror << startl << "Error trying to zero kurtosis!" << endl;
  }
}

void Mode::setOffsets(int scan, int seconds, int ns)
{
  bool foundok;
  int srcindex;
  currentscan = scan;
  offsetseconds = seconds;
  offsetns = ns;
  if(datasec <= INVALID_SUBINT)
    return; //there is no valid data - this whole subint will be ignored
  if(currentscan != datascan) {
    cerror << startl << "Received a request to process scan " << currentscan << " (" << seconds << "/" << ns << " but received data from scan " << datascan << " (" << datasec << "/" << datans << ") - I'm confused and will ignore this data!" << endl;
    datalengthbytes = 0; //torch the whole subint - can't work with different scans!
    return;
  }

  //fill in the quadratic interpolator values from model
  if(model->getNumPhaseCentres(currentscan) == 1)
    srcindex = 1;
  else
    srcindex = 0;

  f64 timespan = blockspersend*2*recordedbandchannels*sampletime/1e6;
  if (usecomplex) timespan/=2;
  foundok = model->calculateDelayInterpolator(currentscan, (double)offsetseconds + ((double)offsetns)/1000000000.0, timespan, blockspersend, config->getDModelFileIndex(configindex, datastreamindex), srcindex, 2, interpolator);
  interpolator[2] -= 1000000*intclockseconds;

  if(!foundok) {
    cerror << startl << "Could not find a Model interpolator for scan " << scan << " offsetseconds " << seconds << " offsetns " << ns << " - will torch this subint!" << endl;
    offsetseconds = INVALID_SUBINT;
  }
}

void Mode::setValidFlags(s32 * v)
{
  int status = vectorCopy_s32(v, validflags, flaglength);
  if(status != vecNoErr)
    csevere << startl << "Error trying to copy valid data flags!!!" << endl;
}

void Mode::setData(u8 * d, int dbytes, int dscan, int dsec, int dns)
{
  data = d;
  datalengthbytes = dbytes;
  datascan = dscan;
  datasec = dsec;
  datans = dns;
  unpackstartsamples = -999999999;
  datasamples = static_cast<int>(datans/(sampletime*1e3) + 0.5);
}

void Mode::resetpcal()
{
  for(int i=0;i<numrecordedbands;i++)
  {
    extractor[i]->clear();
  }
}

void Mode::finalisepcal()
{
  for(int i=0;i<numrecordedbands;i++)
  {
    uint64_t samples = extractor[i]->getFinalPCal(pcalresults[i]);
    if ((samples == 0) && (datasec != INVALID_SUBINT) && (datalengthbytes > 1)) {
        //cdebug << startl << "finalisepcal band " << i << " samples==0 over valid subint " << datasec << "s+" << datans << "ns" << endl;
    }
  }
}

const float Mode::decorrelationpercentage[] = {0.63662, 0.88, 0.94, 0.96, 0.98, 0.99, 0.996, 0.998}; //note these are just approximate!!!



LBAMode::LBAMode(Configuration * conf, int confindex, int dsindex, int recordedbandchan, int chanstoavg, int bpersend, int gsamples, int nrecordedfreqs, double recordedbw, double * recordedfreqclkoffs, double * recordedfreqclkoffsdelta, double * recordedfreqphaseoffs, double * recordedfreqlooffs, int nrecordedbands, int nzoombands, int nbits, bool fbank, bool linear2circular, int fringerotorder, int arraystridelen, bool cacorrs, const s16* unpackvalues)
  : Mode(conf,confindex,dsindex,recordedbandchan,chanstoavg,bpersend,gsamples,nrecordedfreqs,recordedbw,recordedfreqclkoffs,recordedfreqclkoffsdelta,recordedfreqphaseoffs,recordedfreqlooffs,nrecordedbands,nzoombands,nbits,Configuration::REAL,Configuration::DOUBLE, recordedbandchan*2,fbank,linear2circular,fringerotorder,arraystridelen,cacorrs,(recordedbw<16.0)?recordedbw*2.0:32.0)
{
  int shift, outputshift;
  int count = 0;
  int numtimeshifts = (sizeof(u16)*bytesperblockdenominator)/bytesperblocknumerator;

  // CJP 18-Feb
  // The following line might be needed in some corner cases. I found it in some
  // uncommited test code. I cannot remember why I added it. I have not activated it as
  // it obviously is not normally needed. Leaving it here to maybe help future debugging

  //if (numtimeshifts==0) numtimeshifts = 1;

  //build the lookup table - NOTE ASSUMPTION THAT THE BYTE ORDER IS **LITTLE-ENDIAN**!!!
  for(u16 i=0;i<MAX_U16;i++)
  {
    shift = 0;
    for(int j=0;j<numtimeshifts;j++)
    {
      for(int k=0;k<numrecordedbands;k++)
      {
        for(int l=0;l<samplesperblock;l++)
        {
          if(samplesperblock > 1 && numrecordedbands > 1) //32 MHz or 64 MHz dual pol
            if(samplesperblock == 4) //64 MHz
              outputshift = 3*(2-l) - 3*k;
            else
              outputshift = -k*samplesperblock + k + l;
          else if (samplesperblock == 4) //64 MHz single pol
	    outputshift = -2*l + 3;
	  else
            outputshift = 0;

          //if(samplesperblock > 1 && numinputbands > 1) //32 MHz or 64 MHz dual pol
          //  outputshift = (2 - (k + l))*(samplesperblock-1);
          //else
          //  outputshift = 0;

          //littleendian means that the shift, starting from 0, will go through the MS byte, then the LS byte, just as we'd like
          lookup[count + outputshift] = unpackvalues[(i >> shift) & 0x03];
          shift += 2;
          count++;
        }
      }
    }
  }

  // Get the last values, i = 1111111111111111
  for (int i=0;i<samplesperlookup;i++)
  {
    lookup[count + i] = unpackvalues[3]; //every sample is 11 = 3
  }
}

LBA8BitMode::LBA8BitMode(Configuration * conf, int confindex, int dsindex, int recordedbandchan, int chanstoavg, int bpersend, int gsamples, int nrecordedfreqs, double recordedbw, double * recordedfreqclkoffs, double * recordedfreqclkoffsdelta, double * recordedfreqphaseoffs, double * recordedfreqlooffs, int nrecordedbands, int nzoombands, int nbits, bool fbank, bool linear2circular, int fringerotorder, int arraystridelen, bool cacorrs)
: Mode(conf,confindex,dsindex,recordedbandchan,chanstoavg,bpersend,gsamples,nrecordedfreqs,recordedbw,recordedfreqclkoffs,recordedfreqclkoffsdelta,recordedfreqphaseoffs,recordedfreqlooffs,nrecordedbands,nzoombands,nbits,Configuration::REAL,Configuration::SINGLE,recordedbandchan*2,fbank,linear2circular,fringerotorder,arraystridelen,cacorrs,recordedbw*2.0)
{}

float LBA8BitMode::unpack(int sampleoffset)
{
  unsigned char * packed = (unsigned char *)(&(data[((unpackstartsamples/samplesperblock)*bytesperblocknumerator)/bytesperblockdenominator]));

  for(int i=0;i<unpacksamples;i++)
  {
    for(int j=0;j<numrecordedbands;j++)
    {
      unpackedarrays[j][i] = (float)(*packed) - 128.0;
      packed++;
    }
  }
  return 1.0;
}

LBA16BitMode::LBA16BitMode(Configuration * conf, int confindex, int dsindex, int recordedbandchan, int chanstoavg, int bpersend, int gsamples, int nrecordedfreqs, double recordedbw, double * recordedfreqclkoffs, double * recordedfreqclkoffsdelta, double * recordedfreqphaseoffs, double * recordedfreqlooffs, int nrecordedbands, int nzoombands, int nbits, bool fbank, bool linear2circular, int fringerotorder, int arraystridelen, bool cacorrs)
  : Mode(conf,confindex,dsindex,recordedbandchan,chanstoavg,bpersend,gsamples,nrecordedfreqs,recordedbw,recordedfreqclkoffs,recordedfreqclkoffsdelta,recordedfreqphaseoffs,recordedfreqlooffs,nrecordedbands,nzoombands,nbits,Configuration::REAL,Configuration::SINGLE,recordedbandchan*2,fbank,linear2circular,fringerotorder,arraystridelen,cacorrs,recordedbw*2.0)
{}

float LBA16BitMode::unpack(int sampleoffset)
{
  unsigned short * packed = (unsigned short *)(&(data[((unpackstartsamples/samplesperblock)*bytesperblocknumerator)/bytesperblockdenominator]));

  for(int i=0;i<unpacksamples;i++)
  {
    for(int j=0;j<numrecordedbands;j++)
    {
      unpackedarrays[j][i] = (float)(*packed) - 32768.0;
      packed++;
    }
  }
  return 1.0;
}

const s16 LBAMode::stdunpackvalues[] = {MAX_S16/4, -MAX_S16/4 - 1, 3*MAX_S16/4, -3*MAX_S16/4 - 1};
const s16 LBAMode::vsopunpackvalues[] = {-3*MAX_S16/4 - 1, MAX_S16/4, -MAX_S16/4 - 1, 3*MAX_S16/4};
// vim: shiftwidth=2:softtabstop=2:expandtab
