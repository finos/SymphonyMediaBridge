/* Copyright (C) 2007-2008 Jean-Marc Valin
   Copyright (C) 2008      Thorvald Natvig

   File: resample.c
   Arbitrary resampling code

   Redistribution and use in source and binary forms, with or without
   modification, are permitted provided that the following conditions are
   met:

   1. Redistributions of source code must retain the above copyright notice,
   this list of conditions and the following disclaimer.

   2. Redistributions in binary form must reproduce the above copyright
   notice, this list of conditions and the following disclaimer in the
   documentation and/or other materials provided with the distribution.

   3. The name of the author may not be used to endorse or promote products
   derived from this software without specific prior written permission.

   THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
   IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
   OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
   DISCLAIMED. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT,
   INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
   (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
   SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
   HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
   STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
   ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
   POSSIBILITY OF SUCH DAMAGE.

   NOTE: The resampler was packed into a class and the integer mode removed
         as well as the function pointer system to call the correct resampling
         function. Original sourcefiles are arch.h, resample.c and speex_resampler.h
         from the speexdsp library.
         https://github.com/xiph/speexdsp
*/

#pragma once

#include <cmath>

namespace speexport
{
enum
{
    RESAMPLER_ERR_SUCCESS = 0,
    RESAMPLER_ERR_ALLOC_FAILED = 1,
    RESAMPLER_ERR_BAD_STATE = 2,
    RESAMPLER_ERR_INVALID_ARG = 3,
    RESAMPLER_ERR_PTR_OVERLAP = 4,
    RESAMPLER_ERR_OVERFLOW = 5,

    RESAMPLER_ERR_MAX_ERROR
};

enum RESAMPLER_FUN
{
    NONE,
    ZERO,
    DIRECT_SINGLE,
    DIRECT_DOUBLE,
    INTERP_DOUBLE,
    INTERP_SINGLE,
};

typedef float spx_word16_t;
typedef float spx_word32_t;
typedef short spx_int16_t;
typedef int spx_int32_t;
typedef unsigned short spx_uint16_t;
typedef unsigned int spx_uint32_t;

class SpeexResampler
{
    RESAMPLER_FUN active_resampler = NONE;
    spx_uint32_t in_rate;
    spx_uint32_t out_rate;
    spx_uint32_t num_rate;
    spx_uint32_t den_rate;

    int quality;
    spx_uint32_t nb_channels;
    spx_uint32_t filt_len;
    spx_uint32_t mem_alloc_size;
    spx_uint32_t buffer_size;
    int int_advance;
    int frac_advance;
    float cutoff;
    spx_uint32_t oversample;
    int initialised;
    int started;

    /* These are per-channel */
    spx_int32_t* last_sample = nullptr;
    spx_uint32_t* samp_frac_num = nullptr;
    spx_uint32_t* magic_samples = nullptr;

    spx_word16_t* mem = nullptr;
    spx_word16_t* sinc_table = nullptr;
    spx_uint32_t sinc_table_length;
    int in_stride;
    int out_stride;

    int direct_single(spx_uint32_t channel_index,
        const spx_word16_t* in,
        spx_uint32_t* in_len,
        spx_word16_t* out,
        spx_uint32_t* out_len);

    /* This is the same as the previous function, except with a double-precision accumulator */
    int direct_double(spx_uint32_t channel_index,
        const spx_word16_t* in,
        spx_uint32_t* in_len,
        spx_word16_t* out,
        spx_uint32_t* out_len);

    int interpolate_single(spx_uint32_t channel_index,
        const spx_word16_t* in,
        spx_uint32_t* in_len,
        spx_word16_t* out,
        spx_uint32_t* out_len);

    int interpolate_double(spx_uint32_t channel_index,
        const spx_word16_t* in,
        spx_uint32_t* in_len,
        spx_word16_t* out,
        spx_uint32_t* out_len);

    /* This resampler is used to produce zero output in situations where memory
       for the filter could not be allocated.  The expected numbers of input and
       output samples are still processed so that callers failing to check error
       codes are not surprised, possibly getting into infinite loops. */
    int basic_zero(spx_uint32_t channel_index,
        const spx_word16_t* in,
        spx_uint32_t* in_len,
        spx_word16_t* out,
        spx_uint32_t* out_len);

    int update_filter();

    int process_native(spx_uint32_t channel_index, spx_uint32_t* in_len, spx_word16_t* out, spx_uint32_t* out_len);
    int magic(spx_uint32_t channel_index, spx_word16_t** out, spx_uint32_t out_len);

public:
    SpeexResampler();
    ~SpeexResampler();

    /** Create a new resampler with integer input and output rates.
     * @param nb_channels Number of channels to be processed
     * @param rate_in Input sampling rate (integer number of Hz).
     * @param rate_out Output sampling rate (integer number of Hz).
     * @param quality Resampling quality between 0 and 10, where 0 has poor quality
     * and 10 has very high quality.
     * @return Newly created resampler state
     * @retval NULL Error: not enough memory
     */
    int init(spx_uint32_t nb_channels, spx_uint32_t rate_in, spx_uint32_t rate_out, int quality, int* err);
    /** Resample a float array. The input and output buffers must *not* overlap.
     * @param st Resampler state
     * @param channel_index Index of the channel to process for the multi-channel
     * base (0 otherwise)
     * @param in Input buffer
     * @param in_len Number of input samples in the input buffer. Returns the
     * number of samples processed
     * @param out Output buffer
     * @param out_len Size of the output buffer. Returns the number of samples written
     */
    int process(spx_uint32_t channel_index, const float* in, spx_uint32_t* in_len, float* out, spx_uint32_t* out_len);

    /** Set (change) the input/output sampling rates (integer value).
     * @param st Resampler state
     * @param in_rate Input sampling rate (integer number of Hz).
     * @param out_rate Output sampling rate (integer number of Hz).
     */
    int set_rate(spx_uint32_t in_rate, spx_uint32_t out_rate);

    /** Get the current input/output sampling rates (integer value).
     * @param st Resampler state
     * @param in_rate Input sampling rate (integer number of Hz) copied.
     * @param out_rate Output sampling rate (integer number of Hz) copied.
     */
    void get_rate(spx_uint32_t* in_rate, spx_uint32_t* out_rate);

    /** Set (change) the input/output sampling rates and resampling ratio
     * (fractional values in Hz supported).
     * @param st Resampler state
     * @param ratio_num Numerator of the sampling rate ratio
     * @param ratio_den Denominator of the sampling rate ratio
     * @param in_rate Input sampling rate rounded to the nearest integer (in Hz).
     * @param out_rate Output sampling rate rounded to the nearest integer (in Hz).
     */
    int set_rate_frac(spx_uint32_t ratio_num, spx_uint32_t ratio_den, spx_uint32_t in_rate, spx_uint32_t out_rate);

    /** Get the current resampling ratio. This will be reduced to the least
     * common denominator.
     * @param st Resampler state
     * @param ratio_num Numerator of the sampling rate ratio copied
     * @param ratio_den Denominator of the sampling rate ratio copied
     */
    void get_ratio(spx_uint32_t* ratio_num, spx_uint32_t* ratio_den);

    /** Set (change) the conversion quality.
     * @param st Resampler state
     * @param quality Resampling quality between 0 and 10, where 0 has poor
     * quality and 10 has very high quality.
     */
    int set_quality(int pQuality);

    /** Get the conversion quality.
     * @param st Resampler state
     * @param quality Resampling quality between 0 and 10, where 0 has poor
     * quality and 10 has very high quality.
     */
    void get_quality(int* pQuality) { *pQuality = quality; }

    /** Set (change) the input stride.
     * @param st Resampler state
     * @param stride Input stride
     */
    void set_input_stride(spx_uint32_t stride) { in_stride = stride; }

    /** Get the input stride.
     * @param st Resampler state
     * @param stride Input stride copied
     */
    void get_input_stride(spx_uint32_t* stride) { *stride = in_stride; }

    /** Set (change) the output stride.
     * @param st Resampler state
     * @param stride Output stride
     */
    void set_output_stride(spx_uint32_t stride) { out_stride = stride; }

    /** Get the output stride.
     * @param st Resampler state copied
     * @param stride Output stride
     */
    void get_output_stride(spx_uint32_t* stride) { *stride = out_stride; }

    /** Get the latency introduced by the resampler measured in input samples.
     * @param st Resampler state
     */
    int get_input_latency() { return filt_len / 2; }

    /** Get the latency introduced by the resampler measured in output samples.
     * @param st Resampler state
     */
    int get_output_latency() { return ((filt_len / 2) * den_rate + (num_rate >> 1)) / num_rate; }

    /** Make sure that the first samples to go out of the resamplers don't have
     * leading zeros. This is only useful before starting to use a newly created
     * resampler. It is recommended to use that when resampling an audio file, as
     * it will generate a file with the same length. For real-time processing,
     * it is probably easier not to use this call (so that the output duration
     * is the same for the first frame).
     * @param st Resampler state
     */
    int skip_zeros();

    /** Reset a resampler so a new (unrelated) stream can be processed.
     * @param st Resampler state
     */
    int reset_mem();
};
} // namespace speexport
