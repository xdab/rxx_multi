#include "dsp.h"
#include <liquid/liquid.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__AVX2__) && defined(__FMA__)
#include <immintrin.h>
#endif

/* Channel-shift oscillator: precomputed unit-phasor lookup table driven
 * by a 32-bit DDS phase accumulator, mixing in place. Replaces liquid's
 * per-sample nco_crcf_mix_block_down (object state + nearest-bin 1024
 * table) with one integer add, a table index and a complex multiply,
 * and drops the temp-buffer memcpy. No drift: table entries are exact
 * unit phasors and the integer accumulator never loses magnitude.
 * Worst-case phase error is half the table grid, 2*pi/2^(BITS+1), i.e.
 * ~-86 dB spurs for 16 bits - below liquid's own 1024-entry table
 * (-66 dB) and far below the FM noise floor. */
#define SHIFT_LUT_BITS 16
#define SHIFT_LUT_SIZE (1u << SHIFT_LUT_BITS)

static float complex shift_lut[SHIFT_LUT_SIZE];
static pthread_once_t shift_lut_once = PTHREAD_ONCE_INIT;

static void shift_lut_build(void)
{
    for (unsigned int k = 0; k < SHIFT_LUT_SIZE; k++)
        shift_lut[k] = cexpf((float)(2.0 * M_PI * (double)k / (double)SHIFT_LUT_SIZE) * I);
}

int dsp_shift_frequency(struct channel_pipeline *pipeline, struct iq_buffer *buffer)
{
    if (pipeline == NULL || buffer == NULL || buffer->len <= 0 || !pipeline->frequency_shift_enabled)
        return 0;

    pthread_once(&shift_lut_once, shift_lut_build);

    uint32_t acc = pipeline->shift_acc;
    const uint32_t step = pipeline->shift_step;
    float complex *x = buffer->samples;
    const unsigned int n = (unsigned int)buffer->len;

    for (unsigned int i = 0; i < n; i++)
    {
        x[i] = x[i] * shift_lut[acc >> (32 - SHIFT_LUT_BITS)];
        acc += step;
    }
    pipeline->shift_acc = acc;
    return 0;
}

/* FIR decimator inner kernel: complex input x real taps, plain C with
 * restrict and 4 interleaved accumulators (limits float rounding depth
 * to liquid's run4 grouping; also the classic auto-vectorization
 * pattern) so -O3 -march=native emits AVX FMA (liquid's dotprod_crcf_run4
 * is a 4-wide SSE-era kernel without FMA). */
static inline float complex decim_dot(const float *restrict h,
                                      const float complex *restrict x,
                                      unsigned int n_taps)
{
    const float *restrict xf = (const float *)x;
    float ar0 = 0.0f, ai0 = 0.0f;
    float ar1 = 0.0f, ai1 = 0.0f;
    float ar2 = 0.0f, ai2 = 0.0f;
    float ar3 = 0.0f, ai3 = 0.0f;

    unsigned int k = 0;
    for (; k + 4 <= n_taps; k += 4)
    {
        ar0 += h[k] * xf[2 * k];
        ai0 += h[k] * xf[2 * k + 1];
        ar1 += h[k + 1] * xf[2 * k + 2];
        ai1 += h[k + 1] * xf[2 * k + 3];
        ar2 += h[k + 2] * xf[2 * k + 4];
        ai2 += h[k + 2] * xf[2 * k + 5];
        ar3 += h[k + 3] * xf[2 * k + 6];
        ai3 += h[k + 3] * xf[2 * k + 7];
    }
    for (; k < n_taps; k++)
    {
        ar0 += h[k] * xf[2 * k];
        ai0 += h[k] * xf[2 * k + 1];
    }
    ar0 = (ar0 + ar1) + (ar2 + ar3);
    ai0 = (ai0 + ai1) + (ai2 + ai3);
    return ar0 + ai0 * I;
}

#if defined(__AVX2__) && defined(__FMA__)
/* AVX2/FMA bulk decimation kernels, replacing the scalar decim_dot on
 * the outputs whose windows sit fully inside the chunk. Validated in
 * tools/simd_decim_bench.c against the scalar kernel on the production
 * hot-path shape (65 x 1001-tap dots per 8192-sample chunk): 3.8x
 * wall-clock (Zen 2), outputs differ only by float summation order.
 *
 * decim_taps_pairs is the tap vector broadcast to lane pairs, 8 floats
 * per 4 taps (h0,h0,h1,h1,h2,h2,h3,h3): one _mm256 FMMA then
 * accumulates one complex tap-multiply, lanes alternating (re, im).
 *
 * decim_dot2_avx2 processes TWO output windows per pass (windows
 * advance by M < h_len, so consecutive outputs re-read the taps): the
 * tap load feeds both an A-window and a B-window FMA, cutting the
 * kernel from 2 loads/dot to 1.5 loads/dot — that is what lifts
 * throughput on load-port-limited cores beyond the ~2.9x of the
 * single-window kernel.
 *
 * Both kernels keep the scalar path for: non-AVX2 builds, the <= 8
 * boundary outputs straddling decimator_tail, the h_len % 4 tail
 * taps, and the odd last output of a chunk. */

/* horizontal fold of 8 accumulators with (re,im) alternating lanes */
static inline float complex avx_hsum_pairs(__m256 a0, __m256 a1,
                                           __m256 a2, __m256 a3)
{
    __m256 acc = _mm256_add_ps(_mm256_add_ps(a0, a1), _mm256_add_ps(a2, a3));
    __m128 sum = _mm_add_ps(_mm256_castps256_ps128(acc),
                            _mm256_extractf128_ps(acc, 1));
    __m128 sum2 = _mm_add_ps(sum, _mm_movehl_ps(sum, sum));
    float out[2]; /* {re_total, im_total} in lanes 0, 1 */
    _mm_storel_pi((__m64 *)out, sum2);
    return out[0] + out[1] * I;
}

static inline float complex decim_dot_avx2(const float *restrict taps,
                                           const float *restrict pairs,
                                           const float complex *restrict x,
                                           unsigned int n_taps)
{
    const float *restrict xf = (const float *)x;
    unsigned int n4 = n_taps / 4;
    __m256 a0 = _mm256_setzero_ps(), a1 = _mm256_setzero_ps();
    __m256 a2 = _mm256_setzero_ps(), a3 = _mm256_setzero_ps();

    unsigned int j = 0;
    for (; j + 4 <= n4; j += 4)
    {
        a0 = _mm256_fmadd_ps(_mm256_loadu_ps(xf + 8 * (j + 0)),
                             _mm256_loadu_ps(pairs + 8 * (j + 0)), a0);
        a1 = _mm256_fmadd_ps(_mm256_loadu_ps(xf + 8 * (j + 1)),
                             _mm256_loadu_ps(pairs + 8 * (j + 1)), a1);
        a2 = _mm256_fmadd_ps(_mm256_loadu_ps(xf + 8 * (j + 2)),
                             _mm256_loadu_ps(pairs + 8 * (j + 2)), a2);
        a3 = _mm256_fmadd_ps(_mm256_loadu_ps(xf + 8 * (j + 3)),
                             _mm256_loadu_ps(pairs + 8 * (j + 3)), a3);
    }
    for (; j < n4; j++)
        a0 = _mm256_fmadd_ps(_mm256_loadu_ps(xf + 8 * j),
                             _mm256_loadu_ps(pairs + 8 * j), a0);

    float complex res = avx_hsum_pairs(a0, a1, a2, a3);
    for (unsigned int k = 4 * n4; k < n_taps; k++)
        res += taps[k] * x[k];
    return res;
}

static inline void decim_dot2_avx2(const float *restrict taps,
                                   const float *restrict pairs,
                                   const float complex *restrict xa,
                                   const float complex *restrict xb,
                                   unsigned int n_taps,
                                   float complex *restrict ra,
                                   float complex *restrict rb)
{
    const float *restrict fa = (const float *)xa;
    const float *restrict fb = (const float *)xb;
    unsigned int n4 = n_taps / 4;
    __m256 a0 = _mm256_setzero_ps(), a1 = _mm256_setzero_ps();
    __m256 a2 = _mm256_setzero_ps(), a3 = _mm256_setzero_ps();
    __m256 b0 = _mm256_setzero_ps(), b1 = _mm256_setzero_ps();
    __m256 b2 = _mm256_setzero_ps(), b3 = _mm256_setzero_ps();

    unsigned int j = 0;
    for (; j + 4 <= n4; j += 4)
    {
        __m256 h0 = _mm256_loadu_ps(pairs + 8 * (j + 0));
        __m256 h1 = _mm256_loadu_ps(pairs + 8 * (j + 1));
        __m256 h2 = _mm256_loadu_ps(pairs + 8 * (j + 2));
        __m256 h3 = _mm256_loadu_ps(pairs + 8 * (j + 3));
        a0 = _mm256_fmadd_ps(_mm256_loadu_ps(fa + 8 * (j + 0)), h0, a0);
        a1 = _mm256_fmadd_ps(_mm256_loadu_ps(fa + 8 * (j + 1)), h1, a1);
        a2 = _mm256_fmadd_ps(_mm256_loadu_ps(fa + 8 * (j + 2)), h2, a2);
        a3 = _mm256_fmadd_ps(_mm256_loadu_ps(fa + 8 * (j + 3)), h3, a3);
        b0 = _mm256_fmadd_ps(_mm256_loadu_ps(fb + 8 * (j + 0)), h0, b0);
        b1 = _mm256_fmadd_ps(_mm256_loadu_ps(fb + 8 * (j + 1)), h1, b1);
        b2 = _mm256_fmadd_ps(_mm256_loadu_ps(fb + 8 * (j + 2)), h2, b2);
        b3 = _mm256_fmadd_ps(_mm256_loadu_ps(fb + 8 * (j + 3)), h3, b3);
    }
    for (; j < n4; j++)
    {
        __m256 hv = _mm256_loadu_ps(pairs + 8 * j);
        a0 = _mm256_fmadd_ps(_mm256_loadu_ps(fa + 8 * j), hv, a0);
        b0 = _mm256_fmadd_ps(_mm256_loadu_ps(fb + 8 * j), hv, b0);
    }

    *ra = avx_hsum_pairs(a0, a1, a2, a3);
    *rb = avx_hsum_pairs(b0, b1, b2, b3);
    for (unsigned int k = 4 * n4; k < n_taps; k++)
    {
        *ra += taps[k] * xa[k];
        *rb += taps[k] * xb[k];
    }
}
#endif /* __AVX2__ && __FMA__ */

/* Kaiser prototype + decimator state for one channel; shared by the
 * setup-time pre-creation (dsp_init_filters) and the lazy path. */
static int decimator_create(struct channel_pipeline *pipeline)
{
    unsigned int M = (unsigned int)pipeline->downsample_factor;
    float As = 35.0f;
    unsigned int m = 4; /* prototype filter delay */
    unsigned int h_len = 2 * m * M + 1;

    if (M > sizeof(pipeline->decimator_tail) / sizeof(pipeline->decimator_tail[0]) / (2 * m))
    {
        fprintf(stderr, "Unsupported downsample factor: %u\n", M);
        return -1;
    }

    pipeline->decim_taps = malloc(h_len * sizeof(float));
    if (pipeline->decim_taps == NULL)
    {
        fprintf(stderr, "Failed to allocate %u decimator taps\n", h_len);
        return -1;
    }
    liquid_firdes_kaiser(h_len, 1.0f / (float)M, As, 0.0f,
                         pipeline->decim_taps);
    pipeline->decim_taps_len = h_len;
    /* History starts zeroed (window warm-up): pipeline_init's memset
     * already zeroed decimator_tail, so just mark it full. */
    pipeline->decimator_tail_len = h_len - 1;
    pipeline->decim_rem = 0;

#if defined(__AVX2__) && defined(__FMA__)
    /* Broadcast copies of the taps for the AVX2 kernels: plain stores
     * at setup time, the kernels only load from them. Optimization
     * only — NULL falls back to the scalar dot. */
    pipeline->decim_taps_pairs = malloc(2 * h_len * sizeof(float));
    if (pipeline->decim_taps_pairs != NULL)
    {
        for (unsigned int j = 0; 4 * j + 3 < h_len; j++)
        {
            for (int r = 0; r < 2; r++)
                pipeline->decim_taps_pairs[8 * j + r] =
                    pipeline->decim_taps[4 * j];
            for (int r = 0; r < 2; r++)
                pipeline->decim_taps_pairs[8 * j + 2 + r] =
                    pipeline->decim_taps[4 * j + 1];
            for (int r = 0; r < 2; r++)
                pipeline->decim_taps_pairs[8 * j + 4 + r] =
                    pipeline->decim_taps[4 * j + 2];
            for (int r = 0; r < 2; r++)
                pipeline->decim_taps_pairs[8 * j + 6 + r] =
                    pipeline->decim_taps[4 * j + 3];
        }
    }
#else
    pipeline->decim_taps_pairs = NULL;
#endif
    return 0;
}

int dsp_decimate_channel(struct channel_pipeline *pipeline, struct iq_buffer *buffer)
{
    /*
     * Anti-aliasing complex decimation stage.
     * - Purpose: remove spectral content above the new Nyquist (fs_out/2)
     *   before downsampling so high-frequency energy does not alias into
     *   the passband.
     * - Behavior: streaming FIR with the same kaiser prototype that
     *   liquid's firdecim_crcf_create_kaiser(M, m, As) designs, but on a
     *   linear buffer: no circular-window rewrite per block, one dot
     *   product per output sample, history kept in decimator_tail.
     */
    if (pipeline == NULL || buffer == NULL || buffer->len <= 0)
        return 0;

    if (pipeline->downsample_factor <= 1)
        return 0;

    unsigned int M = (unsigned int)pipeline->downsample_factor;
    static _Thread_local float complex temp_out[MAXIMUM_IQ_LENGTH];

    if (pipeline->decim_taps == NULL && decimator_create(pipeline) < 0)
    {
        buffer->len = 0;
        return -1;
    }

    unsigned int h_len = pipeline->decim_taps_len;

    unsigned int tail_len = pipeline->decimator_tail_len;
    unsigned int r = pipeline->decim_rem;
    unsigned int in_len = (unsigned int)buffer->len;
    unsigned int out_len = (in_len + r) / M;
    /* In-chunk offset just past the first output's window end; bumped
     * to M when the stream sits exactly on an output boundary (that
     * output belongs to the previous chunk). */
    unsigned int e = ((M - r) % M == 0 ? M : (M - r) % M) - 1;

    /* Output n is a causal FIR anchored at its newest input sample:
     * window [e + n*M - h_len + 1, e + n*M] in chunk coords. In virtual
     * coords (tail ++ samples) the window starts at e + n*M, because
     * tail_len = h_len-1. Boundary outputs reach into the tail, the
     * bulk sits inside the chunk. */
    unsigned int n = 0;
    while (n < out_len && e < tail_len)
    {
        unsigned int split = tail_len - e;

        temp_out[n] = decim_dot(pipeline->decim_taps,
                                pipeline->decimator_tail + e, split) +
                      decim_dot(pipeline->decim_taps + split,
                                buffer->samples, h_len - split);
        n++;
        e += M;
    }
#if defined(__AVX2__) && defined(__FMA__)
    if (pipeline->decim_taps_pairs != NULL)
    {
        for (; n + 1 < out_len; n += 2, e += 2 * M)
            decim_dot2_avx2(pipeline->decim_taps,
                            pipeline->decim_taps_pairs,
                            buffer->samples + e - tail_len,
                            buffer->samples + e + M - tail_len,
                            h_len, &temp_out[n], &temp_out[n + 1]);
        if (n < out_len)
            temp_out[n] = decim_dot_avx2(pipeline->decim_taps,
                                         pipeline->decim_taps_pairs,
                                         buffer->samples + e - tail_len,
                                         h_len);
    }
    else
#endif
    {
        for (; n < out_len; n++, e += M)
            temp_out[n] = decim_dot(pipeline->decim_taps,
                                    buffer->samples + e - tail_len, h_len);
    }

    /* Slide the history forward by the whole chunk (unconsumed
     * remainder included), then carry the stream position mod M. */
    if (in_len >= tail_len)
        memcpy(pipeline->decimator_tail, buffer->samples + in_len - tail_len,
               tail_len * sizeof(pipeline->decimator_tail[0]));
    else
    {
        memmove(pipeline->decimator_tail, pipeline->decimator_tail + in_len,
                (tail_len - in_len) * sizeof(pipeline->decimator_tail[0]));
        memcpy(pipeline->decimator_tail + tail_len - in_len, buffer->samples,
               in_len * sizeof(pipeline->decimator_tail[0]));
    }
    pipeline->decim_rem = (r + in_len) % M;

    memcpy(buffer->samples, temp_out, sizeof(temp_out[0]) * (size_t)out_len);
    buffer->len = (int)out_len;
    return 0;
}

static int resampler_create(struct channel_pipeline *pipeline)
{
    float r = (float)pipeline->output_rate / (float)pipeline->demod_rate;
    unsigned int m = 10;
    float bw = 0.45f;
    float As = 40.0f;
    unsigned int npfb = 32;

    pipeline->audio_resampler = resamp_rrrf_create(r, m, bw, As, npfb);
    return (pipeline->audio_resampler == NULL) ? -1 : 0;
}

int dsp_resample_output(struct channel_pipeline *pipeline, struct real_buffer *buffer)
{
    float r;
    unsigned int in_len;

    if (pipeline == NULL || buffer == NULL)
        return -1;

    r = (float)pipeline->output_rate / (float)pipeline->demod_rate;
    in_len = (unsigned int)buffer->len;
    if (in_len == 0)
    {
        buffer->len = 0;
        return 0;
    }

    unsigned int max_out = (unsigned int)ceilf(in_len * fabsf(r)) + 16;
    static _Thread_local float temp_buf[MAXIMUM_BUF_LENGTH];
    if (max_out > MAXIMUM_BUF_LENGTH)
        max_out = MAXIMUM_BUF_LENGTH;

    if (pipeline->audio_resampler == NULL && resampler_create(pipeline) < 0)
        return -1;

    unsigned int out_len = 0;
    resamp_rrrf_execute_block(pipeline->audio_resampler,
                              buffer->samples,
                              in_len,
                              temp_buf,
                              &out_len);

    unsigned int copy_len = out_len;
    if (copy_len > MAXIMUM_BUF_LENGTH)
        copy_len = MAXIMUM_BUF_LENGTH;

    for (unsigned int i = 0; i < copy_len; i++)
        buffer->samples[i] = temp_buf[i];

    buffer->len = (int)copy_len;
    return 0;
}

static int deemph_create(struct channel_pipeline *pipeline)
{
    float tau = 75e-6f;
    float Fs = (pipeline->demod_rate > 0) ? (float)pipeline->demod_rate : (float)pipeline->output_rate;
    float d;
    float b0;

    if (pipeline->deemph_alpha > 0.0f)
    {
        b0 = pipeline->deemph_alpha;
        d = 1.0f - b0;
    }
    else
    {
        d = expf(-1.0f / (tau * Fs));
        b0 = 1.0f - d;
    }

    float b[2] = {b0, 0.0f};
    float a[2] = {1.0f, -d};
    pipeline->deemph_filter = iirfilt_rrrf_create(b, 2, a, 2);
    if (pipeline->deemph_filter == NULL)
    {
        fprintf(stderr, "Failed to create de-emphasis filter\n");
        return -1;
    }
    return 0;
}

void dsp_apply_deemphasis(struct channel_pipeline *pipeline, struct real_buffer *buffer)
{
    if (pipeline == NULL || buffer == NULL || !pipeline->deemph_enabled || buffer->len <= 0)
        return;

    if (pipeline->deemph_filter == NULL && deemph_create(pipeline) < 0)
        return;

    unsigned int n = (unsigned int)buffer->len;
    if (n > MAXIMUM_BUF_LENGTH)
        n = MAXIMUM_BUF_LENGTH;
    static _Thread_local float tmp_out[MAXIMUM_BUF_LENGTH];
    iirfilt_rrrf_execute_block(pipeline->deemph_filter, buffer->samples, n, tmp_out);
    for (unsigned int i = 0; i < n; i++)
        buffer->samples[i] = tmp_out[i];
}

static int dcblock_create(struct channel_pipeline *pipeline)
{
    float alpha = 0.02f;
    pipeline->dc_block_filter = iirfilt_rrrf_create_dc_blocker(alpha);
    if (pipeline->dc_block_filter == NULL)
    {
        fprintf(stderr, "Failed to create DC-block filter\n");
        return -1;
    }
    return 0;
}

void dsp_apply_dc_block(struct channel_pipeline *pipeline, struct real_buffer *buffer)
{
    if (pipeline == NULL || buffer == NULL || !pipeline->dc_block_enabled || buffer->len <= 0)
        return;

    if (pipeline->dc_block_filter == NULL && dcblock_create(pipeline) < 0)
        return;

    unsigned int n = (unsigned int)buffer->len;
    if (n > MAXIMUM_BUF_LENGTH)
        n = MAXIMUM_BUF_LENGTH;
    static _Thread_local float tmp_out2[MAXIMUM_BUF_LENGTH];
    iirfilt_rrrf_execute_block(pipeline->dc_block_filter, buffer->samples, n, tmp_out2);
    for (unsigned int i = 0; i < n; i++)
        buffer->samples[i] = tmp_out2[i];
}

float dsp_polar_discriminant(float complex current, float complex previous)
{
    float complex delta = current * conjf(previous);
    return atan2f(cimagf(delta), crealf(delta)) / M_PI * (float)(1 << 14);
}

/* Pre-create a channel's filter objects at setup time so the first
 * processed chunk does not pay the lazy-init spike (~80 ms per channel,
 * visible as a stall right when a TCP client connects). Call after the
 * pipeline's rates, decimation factor and mode flags are set; the lazy
 * paths in the dsp_* functions remain as a safety net. */
int dsp_init_filters(struct channel_pipeline *pipeline)
{
    if (pipeline == NULL)
        return -1;
    if (pipeline->downsample_factor > 1 && pipeline->decim_taps == NULL &&
        decimator_create(pipeline) < 0)
        return -1;
    if (pipeline->demod_rate != pipeline->output_rate &&
        pipeline->audio_resampler == NULL && resampler_create(pipeline) < 0)
        return -1;
    if (pipeline->deemph_enabled && pipeline->deemph_filter == NULL &&
        deemph_create(pipeline) < 0)
        return -1;
    if (pipeline->dc_block_enabled && pipeline->dc_block_filter == NULL &&
        dcblock_create(pipeline) < 0)
        return -1;
    return 0;
}
