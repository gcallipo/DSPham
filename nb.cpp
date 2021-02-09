// Taken from ConvolutionSDR, which took it from UHSDR.


#include <Audio.h>
#include <arm_math.h>
#include <arm_const_structs.h>

#include "nb.h"
#include "global.h"

#define NB_FFT_SIZE (AUDIO_BLOCK_SAMPLES * N_BLOCKS / DF)

//alt noise blanking is trying to localize some impulse noise within the samples and after that
//trying to replace corrupted samples by linear predicted samples.
//therefore, first we calculate the lpc coefficients which represent the actual status of the
//speech or sound generating "instrument" (in case of speech this is an estimation of the current
//filter-function of the voice generating tract behind our lips :-) )
//after finding this function we inverse filter the actual samples by this function
//so we are eliminating the speech, but not the noise. Then we do a matched filtering an thereby detecting impulses
//After that we threshold the remaining samples by some
//level and so detecting impulse noise's positions within the current frame - if one (or more) impulses are there.
//finally some area around the impulse position will be replaced by predicted samples from both sides (forward and
//backward prediction)
//hopefully we have enough processor power left....
//#define debug_alternate_NR
void alt_noise_blanking(float* insamp, int Nsam, float* E )
{
#define boundary_blank 14//14 // for first trials very large!!!!
#define impulse_length NB_impulse_samples // 7 // has to be odd!!!! 7 / 3 should be enough
#define PL             (impulse_length - 1) / 2 //6 // 3 has to be (impulse_length-1)/2 !!!!
  int order    =     NB_taps; //10 // lpc's order
  arm_fir_instance_f32 LPC;
  float32_t lpcs[order + 1]; // we reserve one more than "order" because of a leading "1"
  float32_t reverse_lpcs[order + 1]; //this takes the reversed order lpc coefficients
  float32_t firStateF32[NB_FFT_SIZE + order];
  float32_t tempsamp[NB_FFT_SIZE];
  float32_t sigma2; //taking the variance of the inpo
  float32_t lpc_power;
  float32_t impulse_threshold;
  int impulse_positions[20];  //we allow a maximum of 5 impulses per frame
  int search_pos = 0;
  int impulse_count = 0;
  //    static float32_t last_frame_end[order+PL]; //this takes the last samples from the previous frame to do the prediction within the boundaries
  static float32_t last_frame_end[80]; //this takes the last samples from the previous frame to do the prediction within the boundaries
#ifdef debug_alternate_NR
  static int frame_count = 0; //only used for the distortion insertion - can alter be deleted
  int dist_level = 0; //only used for the distortion insertion - can alter be deleted
#endif

  float32_t R[11];  // takes the autocorrelation results
  float32_t e, k, alfa;

  float32_t any[order + 1]; //some internal buffers for the levinson durben algorithm

  float32_t Rfw[impulse_length + order]; // takes the forward predicted audio restauration
  float32_t Rbw[impulse_length + order]; // takes the backward predicted audio restauration
  float32_t Wfw[impulse_length], Wbw[impulse_length]; // taking linear windows for the combination of fwd and bwd

  float32_t s;

  for (int i = 0; i < impulse_length; i++) // generating 2 Windows for the combination of the 2 predictors
  { // will be a constant window later!
    Wbw[i] = 1.0 * i / (impulse_length - 1);
    Wfw[impulse_length - i - 1] = Wbw[i];
  }

  // calculate the autocorrelation of insamp (moving by max. of #order# samples)
  for (int i = 0; i < (order + 1); i++)
  {
    arm_dot_prod_f32(&insamp[0], &insamp[i], Nsam - i, &R[i]); // R is carrying the crosscorrelations
  }
  // end of autocorrelation

  //alternative levinson durben algorithm to calculate the lpc coefficients from the crosscorrelation
  R[0] = R[0] * (1.0 + 1.0e-9);

  lpcs[0] = 1;   //set lpc 0 to 1

  for (int i = 1; i < order + 1; i++)
    lpcs[i] = 0;                    // fill rest of array with zeros - could be done by memfill

  alfa = R[0];

  for (int m = 1; m <= order; m++)
  {
    s = 0.0;
    for (int u = 1; u < m; u++)
      s = s + lpcs[u] * R[m - u];

    k = -(R[m] + s) / alfa;

    for (int v = 1; v < m; v++)
      any[v] = lpcs[v] + k * lpcs[m - v];

    for (int w = 1; w < m; w++)
      lpcs[w] = any[w];

    lpcs[m] = k;
    alfa = alfa * (1 - k * k);
  }

  // end of levinson durben algorithm

  for (int o = 0; o < order + 1; o++ )           //store the reverse order coefficients separately
    reverse_lpcs[order - o] = lpcs[o];    // for the matched impulse filter

  arm_fir_init_f32(&LPC, order + 1, &reverse_lpcs[0], &firStateF32[0], NB_FFT_SIZE);                                   // we are using the same function as used in freedv
  arm_fir_f32(&LPC, insamp, tempsamp, Nsam); //do the inverse filtering to eliminate voice and enhance the impulses
  arm_fir_init_f32(&LPC, order + 1, &lpcs[0], &firStateF32[0], NB_FFT_SIZE);                                   // we are using the same function as used in freedv
  arm_fir_f32(&LPC, tempsamp, tempsamp, Nsam); // do a matched filtering to detect an impulse in our now voiceless signal

  arm_var_f32(tempsamp, NB_FFT_SIZE, &sigma2); //calculate sigma2 of the original signal ? or tempsignal
  arm_power_f32(lpcs, order, &lpc_power); // calculate the sum of the squares (the "power") of the lpc's

  impulse_threshold = NB_thresh * sqrtf(sigma2 * lpc_power);  //set a detection level (3 is not really a final setting)

  search_pos = order + PL; // lower boundary problem has been solved! - so here we start from 1 or 0?
  impulse_count = 0;

  do {        //going through the filtered samples to find an impulse larger than the threshold

    if ((tempsamp[search_pos] > impulse_threshold) || (tempsamp[search_pos] < (-impulse_threshold)))
    {
      impulse_positions[impulse_count] = search_pos - order; // save the impulse positions and correct it by the filter delay
      impulse_count++;
      search_pos += PL; //  set search_pos a bit away, cause we are already repairing this area later
      //  and the next impulse should not be that close
    }

    search_pos++;

  } while ((search_pos < NB_FFT_SIZE - boundary_blank) && (impulse_count < 20)); // avoid upper boundary

  //boundary handling has to be fixed later
  //as a result we now will not find any impulse in these areas

  // from here: reconstruction of the impulse-distorted audio part:

  // first we form the forward and backward prediction transfer functions from the lpcs
  // that is easy, as they are just the negated coefficients  without the leading "1"
  // we can do this in place of the lpcs, as they are not used here anymore and being recalculated in the next frame!

  arm_negate_f32(&lpcs[1], &lpcs[1], order);
  arm_negate_f32(&reverse_lpcs[0], &reverse_lpcs[0], order);
  
  for (int j = 0; j < impulse_count; j++)
  {
    for (int k = 0; k < order; k++) // we have to copy some samples from the original signal as
    { // basis for the reconstructions - could be done by memcopy

      if ((impulse_positions[j] - PL - order + k) < 0) // this solves the prediction problem at the left boundary
      {
        Rfw[k] = last_frame_end[impulse_positions[j] + k]; //take the sample from the last frame
      }
      else
      {
        Rfw[k] = insamp[impulse_positions[j] - PL - order + k]; //take the sample from this frame as we are away from the boundary
      }

      Rbw[impulse_length + k] = insamp[impulse_positions[j] + PL + k + 1];



    }     //bis hier alles ok

    for (int i = 0; i < impulse_length; i++) //now we calculate the forward and backward predictions
    {
      arm_dot_prod_f32(&reverse_lpcs[0], &Rfw[i], order, &Rfw[i + order]);
      arm_dot_prod_f32(&lpcs[1], &Rbw[impulse_length - i], order, &Rbw[impulse_length - i - 1]);

    }

    arm_mult_f32(&Wfw[0], &Rfw[order], &Rfw[order], impulse_length); // do the windowing, or better: weighing
    arm_mult_f32(&Wbw[0], &Rbw[0], &Rbw[0], impulse_length);

    //finally add the two weighted predictions and insert them into the original signal - thereby eliminating the distortion
    arm_add_f32(&Rfw[order], &Rbw[0], &insamp[impulse_positions[j] - PL], impulse_length);
  }

  for (int p = 0; p < (order + PL); p++)
  {
    last_frame_end[p] = insamp[NB_FFT_SIZE - 1 - order - PL + p]; // store 13 samples from the current frame to use at the next frame
  }
  //end of test timing zone
}
