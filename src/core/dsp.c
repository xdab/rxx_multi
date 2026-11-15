#include "dsp.h"
#include "types.h"
#include <complex.h>
#include <liquid/liquid.h>
#include <math.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__AVX2__) && defined(__FMA__)
#include <immintrin.h>
#endif

/* LUT + 32-bit integer DDS replaces liquid's nco_crcf mixing (object
 * state, 1024-bin table, temp-buffer memcpy) with one integer add, a
 * table index and a complex multiply. No drift: entries are exact unit
 * phasors and the accumulator never loses magnitude; worst-case spur is
 * half the table grid, 2*pi/2^(BITS+1) ~ -86 dB, below liquid's own
 * 1024-entry table (-66 dB) and the FM noise floor. */
#define SHIFT_LUT_BITS 16
#define SHIFT_LUT_SIZE (1u << SHIFT_LUT_BITS)

static float complex shift_lut[SHIFT_LUT_SIZE];
static pthread_once_t shift_lut_once = PTHREAD_ONCE_INIT;

static void shift_lut_build(void)
{
    for (unsigned int k = 0; k < SHIFT_LUT_SIZE; k++)
        shift_lut[k] = cexpf((float)(2.0 * M_PI * (double)k / (double)SHIFT_LUT_SIZE) * I);
}

int dsp_shift_frequency(struct channel_pipeline *pipeline, const struct iq_buffer *input)
{
    if (pipeline == NULL || input == NULL)
        return 0;

    struct iq_buffer *out = &pipeline->work;

    /* Keep the work length mirroring the chunk in every early-exit path */
    out->len = input->len;
    if (input->len <= 0)
        return 0;

    if (!pipeline->frequency_shift_enabled)
        return 0;

    pthread_once(&shift_lut_once, shift_lut_build);

    uint32_t acc = pipeline->shift_acc;
    const uint32_t step = pipeline->shift_step;
    const float complex *x = input->samples;
    float complex *o = out->samples;
    const unsigned int n = (unsigned int)input->len;

    for (unsigned int i = 0; i < n; i++)
    {
        o[i] = x[i] * shift_lut[acc >> (32 - SHIFT_LUT_BITS)];
        acc += step;
    }
    pipeline->shift_acc = acc;
    out->len = input->len;
    return 0;
}

/* Inner kernel: complex x real-taps dot with 4 interleaved accumulators
 * - matches liquid's run4 rounding depth and lets -O3 -march=native
 * emit AVX FMA. */
static inline float complex
decim_dot(const float *restrict h, const float complex *restrict x, unsigned int n_taps)
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
/* AVX2/FMA bulk kernels, replacing the scalar dot on outputs whose
 * windows sit fully inside the chunk. Validated in
 * tools/simd_decim_bench.c (3.8x wall-clock on Zen 2; outputs differ
 * only by float summation order). decim_dot2_avx2 processes TWO output
 * windows per pass - the tap load feeds both, cutting the kernel to
 * 1.5 loads/dot. The scalar path remains for: non-AVX2 builds, the
 * <= 8 boundary outputs straddling decimator_tail, the h_len % 4 tail
 * taps, and the odd last output of a chunk. */

/* horizontal fold of 8 accumulators with (re,im) alternating lanes */
static inline float complex avx_hsum_pairs(__m256 a0, __m256 a1, __m256 a2, __m256 a3)
{
    __m256 acc = _mm256_add_ps(_mm256_add_ps(a0, a1), _mm256_add_ps(a2, a3));
    __m128 sum = _mm_add_ps(_mm256_castps256_ps128(acc), _mm256_extractf128_ps(acc, 1));
    __m128 sum2 = _mm_add_ps(sum, _mm_movehl_ps(sum, sum));
    float out[2]; /* {re_total, im_total} in lanes 0, 1 */
    _mm_storel_pi((__m64 *)out, sum2);
    return out[0] + out[1] * I;
}

// NOLINTBEGIN(bugprone-easily-swappable-parameters): DSP calling convention
static inline float complex decim_dot_avx2(
    const float *restrict taps,
    const float *restrict pairs,
    const float complex *restrict x,
    unsigned int n_taps)
// NOLINTEND(bugprone-easily-swappable-parameters)
{
    const float *restrict xf = (const float *)x;
    unsigned int n4 = n_taps / 4;
    __m256 a0 = _mm256_setzero_ps(), a1 = _mm256_setzero_ps();
    __m256 a2 = _mm256_setzero_ps(), a3 = _mm256_setzero_ps();

    unsigned int j = 0;
    for (; j + 4 <= n4; j += 4)
    {
        a0 = _mm256_fmadd_ps(
            _mm256_loadu_ps(xf + 8 * (j + 0)), _mm256_loadu_ps(pairs + 8 * (j + 0)), a0);
        a1 = _mm256_fmadd_ps(
            _mm256_loadu_ps(xf + 8 * (j + 1)), _mm256_loadu_ps(pairs + 8 * (j + 1)), a1);
        a2 = _mm256_fmadd_ps(
            _mm256_loadu_ps(xf + 8 * (j + 2)), _mm256_loadu_ps(pairs + 8 * (j + 2)), a2);
        a3 = _mm256_fmadd_ps(
            _mm256_loadu_ps(xf + 8 * (j + 3)), _mm256_loadu_ps(pairs + 8 * (j + 3)), a3);
    }
    for (; j < n4; j++)
        a0 = _mm256_fmadd_ps(_mm256_loadu_ps(xf + 8 * j), _mm256_loadu_ps(pairs + 8 * j), a0);

    float complex res = avx_hsum_pairs(a0, a1, a2, a3);
    for (unsigned int k = 4 * n4; k < n_taps; k++)
        res += taps[k] * x[k];
    return res;
}

// NOLINTBEGIN(bugprone-easily-swappable-parameters): DSP calling convention
static inline void decim_dot2_avx2(
    const float *restrict taps,
    const float *restrict pairs,
    const float complex *restrict xa,
    const float complex *restrict xb,
    unsigned int n_taps,
    float complex *restrict ra,
    float complex *restrict rb)
// NOLINTEND(bugprone-easily-swappable-parameters)
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

/* Kaiser prototype + decimator state for one channel. */
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
    liquid_firdes_kaiser(h_len, 1.0f / (float)M, As, 0.0f, pipeline->decim_taps);
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
                pipeline->decim_taps_pairs[8 * j + r] = pipeline->decim_taps[4 * j];
            for (int r = 0; r < 2; r++)
                pipeline->decim_taps_pairs[8 * j + 2 + r] = pipeline->decim_taps[4 * j + 1];
            for (int r = 0; r < 2; r++)
                pipeline->decim_taps_pairs[8 * j + 4 + r] = pipeline->decim_taps[4 * j + 2];
            for (int r = 0; r < 2; r++)
                pipeline->decim_taps_pairs[8 * j + 6 + r] = pipeline->decim_taps[4 * j + 3];
        }
    }
#else
    pipeline->decim_taps_pairs = NULL;
#endif
    return 0;
}

/* Core streaming decimator, shared by the per-channel stage and the
 * pre-channelization shared stage: outputs land at global multiples of
 * M (history tail + remainder carry across chunks). Returns the output
 * count; out must hold in_len/M + 1 samples. */
static unsigned int decim_run(
    unsigned int M,
    const float *taps,
    const float *taps_pairs, /* NULL = scalar fallback */
    float complex *tail,     /* history, tail_len = taps_len - 1 samples */
    unsigned int tail_len,
    unsigned int *rem, /* stream position mod M */
    const float complex *in,
    unsigned int in_len,
    float complex *out)
{
    static _Thread_local float complex temp_out[MAXIMUM_IQ_LENGTH];

    unsigned int h_len = tail_len + 1;
    unsigned int r = *rem;
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

        temp_out[n] = decim_dot(taps, tail + e, split) + decim_dot(taps + split, in, h_len - split);
        n++;
        e += M;
    }
#if defined(__AVX2__) && defined(__FMA__)
    if (taps_pairs != NULL)
    {
        for (; n + 1 < out_len; n += 2, e += 2 * M)
            decim_dot2_avx2(
                taps,
                taps_pairs,
                in + e - tail_len,
                in + e + M - tail_len,
                h_len,
                &temp_out[n],
                &temp_out[n + 1]);
        if (n < out_len)
            temp_out[n] = decim_dot_avx2(taps, taps_pairs, in + e - tail_len, h_len);
    }
    else
#endif
    {
        for (; n < out_len; n++, e += M)
            temp_out[n] = decim_dot(taps, in + e - tail_len, h_len);
    }

    /* Slide the history forward by the whole chunk (unconsumed
     * remainder included), then carry the stream position mod M. */
    if (in_len >= tail_len)
        memcpy(tail, in + in_len - tail_len, tail_len * sizeof(tail[0]));
    else
    {
        memmove(tail, tail + in_len, (tail_len - in_len) * sizeof(tail[0]));
        memcpy(tail + tail_len - in_len, in, in_len * sizeof(tail[0]));
    }
    *rem = (r + in_len) % M;

    memcpy(out, temp_out, sizeof(temp_out[0]) * (size_t)out_len);
    return out_len;
}

int dsp_decimate_channel(
    struct channel_pipeline *pipeline, const struct iq_buffer *input, struct iq_buffer *output)
{
    if (pipeline == NULL || input == NULL || output == NULL || input->len <= 0)
        return 0;

    if (pipeline->downsample_factor <= 1)
    {
        /* Decimation disabled: stage the untouched chunk through, so
         * downstream stages always read the private work buffer */
        if (output != input)
        {
            memcpy(output->samples, input->samples, sizeof(input->samples[0]) * (size_t)input->len);
            output->len = input->len;
        }
        return 0;
    }

    if (pipeline->decim_taps == NULL && decimator_create(pipeline) < 0)
    {
        output->len = 0;
        return -1;
    }

    output->len = (int)decim_run(
        (unsigned int)pipeline->downsample_factor,
        pipeline->decim_taps,
        pipeline->decim_taps_pairs,
        pipeline->decimator_tail,
        pipeline->decimator_tail_len,
        &pipeline->decim_rem,
        input->samples,
        (unsigned int)input->len,
        output->samples);
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

    unsigned int max_out = (unsigned int)ceilf((float)in_len * fabsf(r)) + 16;
    static _Thread_local float temp_buf[MAXIMUM_BUF_LENGTH];
    if (max_out > MAXIMUM_BUF_LENGTH)
        max_out = MAXIMUM_BUF_LENGTH;

    if (pipeline->audio_resampler == NULL && resampler_create(pipeline) < 0)
        return -1;

    unsigned int out_len = 0;
    resamp_rrrf_execute_block(
        pipeline->audio_resampler, buffer->samples, in_len, temp_buf, &out_len);

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
    float Fs =
        (pipeline->demod_rate > 0) ? (float)pipeline->demod_rate : (float)pipeline->output_rate;
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
    return atan2f(cimagf(delta), crealf(delta)) / (float)M_PI * (float)(1 << 14);
}

int dsp_init_filters(struct channel_pipeline *pipeline)
{
    if (pipeline == NULL)
        return -1;
    if (pipeline->downsample_factor > 1 && pipeline->decim_taps == NULL &&
        decimator_create(pipeline) < 0)
        return -1;
    if (pipeline->demod_rate != pipeline->output_rate && pipeline->audio_resampler == NULL &&
        resampler_create(pipeline) < 0)
        return -1;
    if (pipeline->deemph_enabled && pipeline->deemph_filter == NULL && deemph_create(pipeline) < 0)
        return -1;
    if (pipeline->dc_block_enabled && pipeline->dc_block_filter == NULL &&
        dcblock_create(pipeline) < 0)
        return -1;
    return 0;
}

/* --- shared pre-channelization decimator --- */

#define SHARED_AS 35.0f
#define SHARED_M_MIN 2u
/* Kaiser design rule: filter length N achieves transition width
 * df_norm ~= (As - 7.95) / (14.36 * N) relative to the rate it runs at.
 * As = 35 dB, the per-channel prototype's class. */
#define KAISER_NUM 27.05f
#define KAISER_DEN 14.36f
/* Ir cost weights, callgrind-measured on this code base: the hand-rolled
 * AVX2 decimator runs ~1.4 Ir per tap, the NCO LUT shift ~18 Ir per
 * sample. The shared stage costs the same per tap as the per-channel
 * one (same kernels), so the model is apples-to-apples. */
#define IR_DECIM_TAP 1.4
#define IR_NCO_SAMPLE 18.0

/* NOLINTBEGIN(bugprone-easily-swappable-parameters): positional contract */
int dsp_shared_init(struct device_state *dev, uint32_t rate_channel, uint64_t width_shared)
/* NOLINTEND(bugprone-easily-swappable-parameters) */
{
    dev->shared_taps = NULL;
    dev->shared_taps_pairs = NULL;
    dev->shared_tail_len = 0;
    dev->shared_rem = 0;
    dev->shared_factor = 1;
    dev->shared_rate = 0;
    dev->shared_acc_len = 0;

    if (rate_channel == 0 || dev->rate == 0)
        return 0;

    unsigned int ds = dev->rate / rate_channel;

    /* Largest power-of-two factor of ds whose shared rate still keeps
     * the ROI (2*roi) strictly below its own Nyquist: the shared output
     * must carry every channel intact. */
    double roi2 = (double)width_shared;
    unsigned int j = 0;
    while (j < 8 && (ds & ((2u << j) - 1)) == 0 && (double)(dev->rate >> (j + 1)) > roi2)
        j++;
    if (j == 0)
        return 0; /* odd downsample, or channels span too much of the capture */

    unsigned int M_sh = 1u << j;
    unsigned int K = ds >> j; /* per-channel residual decimation */
    uint32_t shared = dev->rate >> j;
    double gap = (double)shared - roi2; /* room for the transition */
    if (gap <= 0.0)
        return 0;

    /* Shared kaiser, same family as the per-channel prototype but with
     * the passband at the ROI edge: everything the shared decimation
     * folds (k*shared +- roi zones and beyond) must be in its stopband,
     * so the transition must fit between roi and shared - roi. */
    unsigned int h_len_min =
        (unsigned int)ceil(KAISER_NUM * (double)dev->rate / (KAISER_DEN * gap));
    unsigned int m_sh = (h_len_min - 1) / (2 * M_sh) + 1;
    if (m_sh < SHARED_M_MIN)
        m_sh = SHARED_M_MIN;
    unsigned int h_len = 2 * m_sh * M_sh + 1;
    /* cutoff midway between the passband edge and the stopband edge */
    float fc = (float)(0.5 * (double)shared / (double)dev->rate);

    /* Engage only when the predicted per-channel saving (decimator taps
     * halved per stage + NCO retimed to the shared rate) beats the
     * shared stage's own cost at this channel count. */
    double save = IR_NCO_SAMPLE * (double)(dev->rate - shared) +
                  IR_DECIM_TAP * (double)(8 * (ds - K)) * rate_channel;
    double cost = IR_DECIM_TAP * (double)h_len / (double)M_sh * (double)dev->rate;
    if ((double)freq_len * save <= cost)
        return 0;

    dev->shared_taps = malloc(h_len * sizeof(float));
    if (dev->shared_taps == NULL)
    {
        fprintf(stderr, "Failed to allocate %u shared decimator taps\n", h_len);
        return -1;
    }
    liquid_firdes_kaiser(h_len, fc, SHARED_AS, 0.0f, dev->shared_taps);
    /* Normalize DC gain to 1: the per-channel prototype keeps its
     * unnormalized convention (compensated by output_scale), and with
     * the shared stage at unity gain the amplitude math is bit-identical
     * to the bypassed path. */
    double hs = 0.0;
    for (unsigned int i = 0; i < h_len; i++)
        hs += dev->shared_taps[i];
    for (unsigned int i = 0; i < h_len; i++)
        dev->shared_taps[i] /= (float)hs;
    dev->shared_taps_len = h_len;
    dev->shared_tail_len = h_len - 1;
    memset(dev->shared_tail, 0, sizeof(dev->shared_tail));
    dev->shared_rem = 0;
    dev->shared_factor = M_sh;

#if defined(__AVX2__) && defined(__FMA__)
    dev->shared_taps_pairs = malloc(2 * h_len * sizeof(float));
    if (dev->shared_taps_pairs != NULL)
    {
        for (unsigned int p = 0; 4 * p + 3 < h_len; p++)
        {
            for (int r = 0; r < 2; r++)
                dev->shared_taps_pairs[8 * p + r] = dev->shared_taps[4 * p];
            for (int r = 0; r < 2; r++)
                dev->shared_taps_pairs[8 * p + 2 + r] = dev->shared_taps[4 * p + 1];
            for (int r = 0; r < 2; r++)
                dev->shared_taps_pairs[8 * p + 4 + r] = dev->shared_taps[4 * p + 2];
            for (int r = 0; r < 2; r++)
                dev->shared_taps_pairs[8 * p + 6 + r] = dev->shared_taps[4 * p + 3];
        }
    }
#endif

    /* Constant chunk duration: the producer's full-rate chunk yields
     * chunk_samples at the shared rate, so condvar pacing stays at the
     * reference wakeup rate */
    dev->chunk_samples = (DEFAULT_BUF_LENGTH / 2) >> j;
    dev->shared_rate = shared;

    fprintf(
        stderr,
        "Shared decimator: %u -> %u Hz (1/%u, %u taps, chunk %u samples)\n",
        dev->rate,
        shared,
        M_sh,
        h_len,
        dev->chunk_samples);
    return 0;
}

void dsp_shared_push(struct device_state *dev, const float complex *in, unsigned n)
{
    static _Thread_local float complex tmp[MAXIMUM_IQ_LENGTH];

    if (dev->shared_taps == NULL || n == 0)
        return;

    unsigned int out_len = decim_run(
        dev->shared_factor,
        dev->shared_taps,
        dev->shared_taps_pairs,
        dev->shared_tail,
        dev->shared_tail_len,
        &dev->shared_rem,
        in,
        n,
        tmp);
    if (dev->shared_acc_len + out_len > DEFAULT_BUF_LENGTH)
    {
        /* Cannot happen with producer-sized pushes (acc is drained
         * below chunk_samples before every push); drop rather than
         * overflow if that invariant is ever broken */
        fprintf(stderr, "ERROR: shared accumulator overflow, dropping output\n");
        return;
    }
    memcpy(dev->shared_acc + dev->shared_acc_len, tmp, out_len * sizeof(tmp[0]));
    dev->shared_acc_len += out_len;
}

void dsp_shared_cleanup(struct device_state *dev)
{
    free(dev->shared_taps);
    dev->shared_taps = NULL;
    free(dev->shared_taps_pairs);
    dev->shared_taps_pairs = NULL;
}
