/*
 * SynthesisFunctions.cpp
 *
 *  Created on: 13 Oct 2016
 *      Author: ljuvela
 */

#include <gslwrap/random_generator.h>
#include <gslwrap/random_number_distribution.h>

#include <gslwrap/vector_double.h>
#include "definitions.h"
#include "Utils.h"
#include "SpFunctions.h"
#include "InverseFiltering.h"
#include "Filters.h"
#include "SynthesisFunctions.h"

void PostFilter(const double &postfilter_coefficient, const int &fs, gsl::matrix *lsf) {

   size_t POWER_SPECTRUM_FRAME_LEN = 4096;
	size_t frame_index,i;
	gsl::vector lsf_vec(lsf->get_rows());
   gsl::vector poly_vec(lsf->get_rows()+1);
   gsl::vector r(lsf->get_rows()+1);
   ComplexVector poly_fft(POWER_SPECTRUM_FRAME_LEN/2+1);
   gsl::vector fft_mag;
   gsl::vector_int peak_indices;
   gsl::vector peak_values;
   int POWER_SPECTRUM_WIN = 20;//rint(20*16000/fs); // Originally hard-coded as 20 samples, should be fs adaptive?

   std::cout << "Using LPC postfiltering with a coefficient of " << postfilter_coefficient << std::endl;

	/* Loop for every index of the LSF matrix */
	for(frame_index=0;frame_index<lsf->get_cols();frame_index++) {

		/* Convert LSF to LPC */
		Lsf2Poly(lsf->get_col_vec(frame_index),&poly_vec);

		/* Compute power spectrum */
		FFTRadix2(poly_vec,POWER_SPECTRUM_FRAME_LEN,&poly_fft);
      fft_mag = poly_fft.getAbs();
      for(i=0;i<fft_mag.size();i++)
         fft_mag(i) = 1.0/pow(fft_mag(i),2);

		/* Modification of the power spectrum */
		FindPeaks(fft_mag, 0.1, &peak_indices, &peak_values);
		SharpenPowerSpectrumPeaks(peak_indices, postfilter_coefficient, POWER_SPECTRUM_WIN, &fft_mag);

		/* Construct autocorrelation r */
		poly_fft.setReal(fft_mag);
		poly_fft.setAllImag(0.0);
      IFFTRadix2(poly_fft,&r);

      Levinson(r, &poly_vec);

		/* Convert LPC back to LSF */
      Poly2Lsf(poly_vec,&lsf_vec);
      lsf->set_col_vec(frame_index,lsf_vec);
	}
}


void ParameterSmoothing(const Param &params, SynthesisData *data) {
   /*if(params.lsf_vt_smooth_len > 2) {


   }

   if(params.lsf_glot_smooth_len > 2)

   if(params.gain_smooth_len > 2)

   if(params.hnr_smooth_len > 2)*/

}

gsl::vector GetSinglePulse(const size_t &pulse_len, const double &energy, const gsl::vector &base_pulse) {

   /* Modify pulse */
   gsl::vector pulse(pulse_len);
   //InterpolateSpline(base_pulse, pulse_len, &pulse);
   Interpolate(base_pulse, &pulse);
   pulse *= energy/getEnergy(pulse);
   ApplyWindowingFunction(HANN, &pulse);

   /* Window length normalization */

   /*
   gsl::vector ones(pulse_len);
   ones.set_all(1.0);
   ApplyWindowingFunction(HANN, &ones);
   pulse *= pulse_len / pow(ones.norm2(),2);
   */

   return pulse;
}

void CreateExcitation(const Param &params, const SynthesisData &data, gsl::vector *excitation_signal) {

   gsl::vector single_pulse_base;
   gsl::random_generator rand_gen;

   gsl::gaussian_random gauss_gen(rand_gen);

   switch (params.excitation_method) {
   case SINGLE_PULSE_EXCITATION:
      single_pulse_base = StdVector2GslVector(kDGLOTPULSE) ;
      break;
   case DNN_GENERATED_EXCITATION:
      // Load DNN
      break;
   case PULSES_AS_FEATURES_EXCITATION:
      // Load pulses as features
      break;
   }

   size_t sample_index = 0;
   size_t frame_index;
   gsl::vector pulse;
   gsl::vector noise(params.frame_shift*2);
   double T0, energy;
   size_t pulse_len;
   while (sample_index < (size_t)params.signal_length) {
      frame_index = rint(params.speed_scale * sample_index / (params.signal_length-1) * (params.number_of_frames-1));
      /** Voiced excitation **/
      if(data.fundf(frame_index) > 0) {

         T0 = params.fs/data.fundf(frame_index);
         pulse_len = rint(2*T0);
         energy = LogEnergy2FrameEnergy(data.frame_energy(frame_index),pulse_len);

         switch (params.excitation_method) {
         case SINGLE_PULSE_EXCITATION:
            pulse = GetSinglePulse(pulse_len, energy, single_pulse_base);
            break;
         case DNN_GENERATED_EXCITATION:
            //pulse = GetDnnPulse();
            std::cout << "DNN" << std::endl;
            break;
         case PULSES_AS_FEATURES_EXCITATION:
            //pulse = GetPafPulse();
            std::cout << "PAF" << std::endl;
            break;
         }


         OverlapAdd(pulse,sample_index,excitation_signal);

         sample_index += rint(T0);

      /** Unvoiced excitation **/
      } else {
         size_t i;
         for(i=0;i<noise.size();i++) {
            noise(i) = gauss_gen.get();
         }
         energy = LogEnergy2FrameEnergy(data.frame_energy(frame_index),noise.size());

         noise *= params.noise_gain_unvoiced*energy/getEnergy(noise);
         noise /= 0.5*(double)noise.size()/(double)params.frame_shift; // Compensate OLA gain
         ApplyWindowingFunction(HANN,&noise);
         OverlapAdd(noise,sample_index,excitation_signal);

         sample_index += params.frame_shift;
      }
   }
}


void HarmonicModification(const Param &params, const SynthesisData &data, gsl::vector *excitation_signal) {
   std::cout << "HNR modification ...";


   /* Variables */
   int hnr_channels = params.hnr_order;
   gsl::vector frame(params.frame_length_long);
   ComplexVector frame_fft;
   size_t NFFT = 8192; // Long FFT
   double MIN_LOG_POWER = -60.0;
   gsl::vector fft_mag(NFFT/2+1);
   gsl::vector fft_lower_env(NFFT/2+1);
   gsl::vector fft_upper_env(NFFT/2+1);
   gsl::vector fft_lower_env_target(NFFT/2+1);
   gsl::vector fft_noise(NFFT/2+1);

   gsl::vector_int harmonic_index;
   gsl::vector lower_env_values;
   gsl::vector upper_env_values;



   gsl::vector_int x_interp = LinspaceInt(0, 1,fft_mag.size()-1);

   gsl::vector hnr_interp(fft_mag.size());

   gsl::random_generator rand_gen;
   gsl::gaussian_random random_gauss_gen(rand_gen);
   ComplexVector noise_vec_fft;
   gsl::vector noise_vec(frame.size());
   gsl::vector excitation_orig(*excitation_signal);
   excitation_signal->set_all(0.0);

   int frame_index,i, ind1, ind2;
   double val;
   for(frame_index=0;frame_index<params.number_of_frames;frame_index++) {
      /** HNR modification only in voiced frames **/
      if(data.fundf(frame_index) > 0) {
         GetFrame(excitation_orig, frame_index, rint(params.frame_shift/params.speed_scale), &frame, NULL);
         ApplyWindowingFunction(COSINE, &frame);
         FFTRadix2(frame, NFFT, &frame_fft);
         fft_mag = frame_fft.getAbs();

         for(i=0;i<(int)fft_mag.size();i++) {
            val = 20*log10(fft_mag(i));
            fft_mag(i) = GSL_MAX(val,MIN_LOG_POWER); // Min log-power = -60dB
         }
         harmonic_index = FindHarmonicPeaks(fft_mag, data.fundf(frame_index), params.fs);
         lower_env_values = gsl::vector(harmonic_index.size());
         upper_env_values = gsl::vector(harmonic_index.size());


         for(i=0;i<(int)harmonic_index.size();i++) {

            /* Lower spectral valley */
            if(i>0) {
               ind1 = (harmonic_index(i)+harmonic_index(i-1))/2;
            } else {
               ind1 = harmonic_index(i)/2;
            }
            /* Upper spectral valley */
            if(i==(int)harmonic_index.size()-1) {
               ind2 = (harmonic_index(i)+fft_mag.size()-1)/2;
            } else {
               ind2 = (harmonic_index(i)+harmonic_index(i+1))/2;
            }
            lower_env_values(i) = (fft_mag(ind1) + fft_mag(ind2))/2.0;
            upper_env_values(i) = fft_mag(harmonic_index(i));
         }

         /* Get FFT length HNR estimates */
         InterpolateLinear(harmonic_index, lower_env_values, x_interp, &fft_lower_env); // P0
         InterpolateLinear(harmonic_index, upper_env_values, x_interp, &fft_upper_env);
         Erb2Linear(data.hnr_glot.get_col_vec(frame_index), params.fs, &hnr_interp);
         for(i=0;i<fft_lower_env_target.size();i++)
            fft_lower_env_target(i) = fft_upper_env(i) + hnr_interp(i); // Ptar

         /* Convert to linear scale */
         for(i=0;i<fft_lower_env_target.size();i++) {
            fft_noise(i) = pow(10,fft_lower_env_target(i)/20.0) - pow(10,fft_lower_env(i)/20.0);
         }

         for(i=0;i<noise_vec.size();i++)
            noise_vec(i) = random_gauss_gen.get();

         ApplyWindowingFunction(COSINE,&noise_vec);

         FFTRadix2(noise_vec, NFFT, &noise_vec_fft);

         size_t noise_low_freq_limit_ind = rint(NFFT*params.noise_low_freq_limit_voiced/params.fs);


         for(i=0;i<fft_mag.size();i++) {
            if(i < noise_low_freq_limit_ind) {
               noise_vec_fft.setReal(i,0.0);
               noise_vec_fft.setImag(i,0.0);
            } else {
               if(fft_noise(i) > 0) {
                  noise_vec_fft.setReal(i,noise_vec_fft.getReal(i)/noise_vec_fft.getAbs(i)*sqrt(fft_noise(i))*params.noise_gain_voiced);
                  noise_vec_fft.setImag(i,noise_vec_fft.getImag(i)/noise_vec_fft.getAbs(i)*sqrt(fft_noise(i))*params.noise_gain_voiced);
               } else {
                  noise_vec_fft.setReal(i,0.0);
                  noise_vec_fft.setImag(i,0.0);
               }
            }
         }

         IFFTRadix2(noise_vec_fft,&noise_vec);

         for(i=0;i<frame.size();i++)
            frame(i) += noise_vec(i);


         ApplyWindowingFunction(COSINE, &frame);
      } else {
         GetFrame(excitation_orig, frame_index, rint(params.frame_shift/params.speed_scale), &frame, NULL);
         ApplyWindowingFunction(HANN, &frame);
      }
         frame /= 0.5*(double)frame.size()/(double)params.frame_shift;
         OverlapAdd(frame,frame_index*rint(params.frame_shift/params.speed_scale),excitation_signal);
   }
   std::cout << " done." << std::endl;
}




void SpectralMatchExcitation(const Param &params,const SynthesisData &data, gsl::vector *excitation_signal) {
   /* Get analysis filters for synthetic excitation */
   size_t frame_index;
   gsl::vector frame(params.frame_length);
   gsl::vector a_gen(params.lpc_order_glot+1);
   gsl::vector a_tar(params.lpc_order_glot+1);
   gsl::vector lsf_gen(params.lpc_order_glot);
   gsl::vector lsf_tar_interpolated(params.lpc_order_glot);
   gsl::vector lsf_gen_interpolated(params.lpc_order_glot);
   gsl::vector w;
   gsl::matrix lsf_glot_syn(params.lpc_order_glot, params.number_of_frames);
   for(frame_index=0;frame_index<(size_t)params.number_of_frames;frame_index++) {
      GetFrame(*excitation_signal,frame_index,rint(params.frame_shift/params.speed_scale),&frame,NULL);
      ApplyWindowingFunction(params.default_windowing_function,&frame);
      //LPC(frame,params.lpc_order_glot,&A);
      ArAnalysis(params.lpc_order_glot, 0.0, NONE, w, frame, &a_gen);
      Poly2Lsf(a_gen,&lsf_gen);
      lsf_glot_syn.set_col_vec(frame_index,lsf_gen);
   }

   /* Spectral match excitation */
   gsl::vector excitation_orig(excitation_signal->size());
   excitation_orig.copy(*excitation_signal);

   int sample_index,i;
   double gain = 1.0, sum, frame_index_double;
   int UPDATE_INTERVAL = rint(params.fs*0.005); // Hard-coded 5ms update interval
   for(sample_index=0;sample_index<(int)excitation_signal->size();sample_index++) {

      if(sample_index % UPDATE_INTERVAL == 0) { //TODO: interpolation of parameters between frames according to update_interval
         frame_index_double = params.speed_scale * sample_index / (params.signal_length-1) * (params.number_of_frames-1);
         InterpolateLinear(lsf_glot_syn,frame_index_double,&lsf_gen_interpolated);
         InterpolateLinear(data.lsf_glot,frame_index_double,&lsf_tar_interpolated);
         Lsf2Poly(lsf_gen_interpolated,&a_gen);
         Lsf2Poly(lsf_tar_interpolated,&a_tar);
         gain = GetFilteringGain(a_gen, a_tar, excitation_orig, sample_index, params.frame_length, 0.0); // Should this be from ecitation_signal or excitation_orig?
         a_tar(0) = 0.0;
      }
      sum = 0.0;
      for(i=0;i<GSL_MIN(params.lpc_order_glot+1,sample_index);i++) {
         sum += excitation_orig(sample_index-i)*a_gen(i)*gain - (*excitation_signal)(sample_index-i)*a_tar(i);
      }
      (*excitation_signal)(sample_index) = sum;
   }

}


void FilterExcitation(const Param &params, const SynthesisData &data, gsl::vector *signal) {

   int sample_index,i;
   double gain = 1.0, sum, frame_index_double;
   gsl::vector lsf_interp(params.lpc_order_vt);
   gsl::vector a_interp(params.lpc_order_vt+1);
   gsl::vector B(1);B(0)=1.0;
   int UPDATE_INTERVAL = rint(params.fs*0.005); // Hard-coded 5ms update interval
   signal->copy(data.excitation_signal);

   for(sample_index=0;sample_index<(int)signal->size();sample_index++) {

      if(sample_index % UPDATE_INTERVAL == 0) {
         frame_index_double = params.speed_scale * sample_index / (params.signal_length-1) * (params.number_of_frames-1);
         InterpolateLinear(data.lsf_vocal_tract,frame_index_double,&lsf_interp);
         Lsf2Poly(lsf_interp,&a_interp);
         gain = GetFilteringGain(B, a_interp, data.excitation_signal, sample_index, params.frame_length, params.warping_lambda_vt); // Should this be from ecitation_signal or excitation_orig?
         //a_interp(0) = 0.0;
      }
      /** Normal filtering **/
      if(params.warping_lambda_vt == 0.0) {
         sum = data.excitation_signal(sample_index)*gain;
         for(i=1;i<GSL_MIN(params.lpc_order_vt+1,sample_index);i++) {
            sum -=  (*signal)(sample_index-i)*a_interp(i);
         }
         (*signal)(sample_index) = sum;
      /** Warped filtering **/
      } else {
      //TODO
      }
   }



}






