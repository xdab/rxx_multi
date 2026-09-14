/* Option 2 validator: SIMD decimator kernel variants vs production scalar.
 *
 * Replicates the profiled hot path of dsp_decimate_channel
 * (src/core/dsp.c): per 8192-sample complex chunk, ~65 outputs, each a
 * 1001-tap dot product of real taps h against a complex window.
 *
 * Variants:
 *   S: production decim_dot verbatim (scalar, 4 accumulators, restrict)
 *   A: deinterleave chunk once -> two unit-stride real dot loops
 *      (same 4-accumulator structure; gcc auto-vectorization test)
 *   B: explicit AVX2/FMA on interleaved data (pair-broadcast taps
 *      precomputed once per filter; 4 vector accumulators)
 *   C: explicit AVX2/FMA on deinterleaved data (plain vector FMAs)
 *   D: B with tap reuse across two output windows (3 loads per 2 dots)
 *
 * Measures: wall-clock per chunk over thousands of chunks (best of 3),
 * and numerical parity of every variant against S. Timed loops chain
 * each chunk's result back into the input so gcc cannot hoist work
 * out of the loop — without this the numbers are meaningless (ask the
 * author how they know).
 *
 * Verdict recorded in docs/improvement-options.md Option 2 (2026-09-14,
 * Ryzen 3700X / Zen 2): A 0.20x (auto-vec actively regresses), B 2.9x,
 * C 2.2x, D 3.8-3.9x — variant D is the recommended production kernel.
 *
 * Build: gcc -O3 -march=native -Wall -Wextra -o simd_decim_bench \
 *            simd_decim_bench.c -lliquid -lm
 *        (add -fopt-info-vec to see which loops gcc vectorizes)
 */
#include <complex.h>
#include <immintrin.h>
#include <liquid/liquid.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#define H_LEN 1001 /* 2*m*M+1, m=4, M=125 — production shape */
#define CHUNK 8192
#define OUTS 65 /* ~8192/125 outputs per chunk, incl. boundary splits */
#define BUF (2 * CHUNK)

static float h[H_LEN];
static __m256 h_pairs[(H_LEN + 3) / 4]; /* B: (h0,h0,h1,h1,h2,h2,h3,h3) */
static float complex x[BUF];
static float re[BUF], im[BUF];

/* ---- S: production kernel, verbatim from src/core/dsp.c:56 ---- */
static inline float complex decim_dot_s(const float *restrict h,
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

/* ---- A: deinterleave once, unit-stride real dots ---- */
static void deinterleave(const float complex *restrict src, unsigned n,
                         float *restrict re, float *restrict im)
{
    const float *xf = (const float *)src;
    for (unsigned i = 0; i < n; i++)
    {
        re[i] = xf[2 * i];
        im[i] = xf[2 * i + 1];
    }
}

static inline float complex decim_dot_a(const float *restrict h,
                                        const float *restrict re,
                                        const float *restrict im,
                                        unsigned int n_taps)
{
    float ar0 = 0.0f, ai0 = 0.0f;
    float ar1 = 0.0f, ai1 = 0.0f;
    float ar2 = 0.0f, ai2 = 0.0f;
    float ar3 = 0.0f, ai3 = 0.0f;

    unsigned int k = 0;
    for (; k + 4 <= n_taps; k += 4)
    {
        ar0 += h[k] * re[k];
        ai0 += h[k] * im[k];
        ar1 += h[k + 1] * re[k + 1];
        ai1 += h[k + 1] * im[k + 1];
        ar2 += h[k + 2] * re[k + 2];
        ai2 += h[k + 2] * im[k + 2];
        ar3 += h[k + 3] * re[k + 3];
        ai3 += h[k + 3] * im[k + 3];
    }
    for (; k < n_taps; k++)
    {
        ar0 += h[k] * re[k];
        ai0 += h[k] * im[k];
    }
    ar0 = (ar0 + ar1) + (ar2 + ar3);
    ai0 = (ai0 + ai1) + (ai2 + ai3);
    return ar0 + ai0 * I;
}

/* horizontal for 8 accumulators whose lanes alternate (r0,i0,r1,i1,...):
 * even lanes = re sum, odd lanes = im sum */
static inline float complex hsum8_pairs(__m256 a0, __m256 a1,
                                        __m256 a2, __m256 a3)
{
    __m256 acc = _mm256_add_ps(_mm256_add_ps(a0, a1), _mm256_add_ps(a2, a3));
    __m128 sum = _mm_add_ps(_mm256_castps256_ps128(acc),
                            _mm256_extractf128_ps(acc, 1));
    __m128 sum2 = _mm_add_ps(sum, _mm_movehl_ps(sum, sum));
    float out[2];
    _mm_storel_pi((__m64 *)out, sum2);
    return out[0] + out[1] * I;
}

/* ---- B: AVX2/FMA on interleaved data, pair-broadcast taps ---- */
static void prep_h_pairs(const float *h, unsigned n_taps)
{
    for (unsigned j = 0; 4 * j + 3 < n_taps; j++)
    {
        __m128 lo = _mm_setr_ps(h[4 * j], h[4 * j], h[4 * j + 1], h[4 * j + 1]);
        __m128 hi = _mm_setr_ps(h[4 * j + 2], h[4 * j + 2], h[4 * j + 3], h[4 * j + 3]);
        h_pairs[j] = _mm256_insertf128_ps(_mm256_castps128_ps256(lo), hi, 1);
    }
}

static inline float complex decim_dot_b(const float complex *restrict xc,
                                        unsigned int n_taps)
{
    const float *xf = (const float *)xc;
    unsigned n4 = n_taps / 4;
    __m256 a0 = _mm256_setzero_ps(), a1 = _mm256_setzero_ps();
    __m256 a2 = _mm256_setzero_ps(), a3 = _mm256_setzero_ps();

    unsigned j = 0;
    for (; j + 4 <= n4; j += 4)
    {
        a0 = _mm256_fmadd_ps(_mm256_loadu_ps(xf + 8 * (j + 0)), h_pairs[j + 0], a0);
        a1 = _mm256_fmadd_ps(_mm256_loadu_ps(xf + 8 * (j + 1)), h_pairs[j + 1], a1);
        a2 = _mm256_fmadd_ps(_mm256_loadu_ps(xf + 8 * (j + 2)), h_pairs[j + 2], a2);
        a3 = _mm256_fmadd_ps(_mm256_loadu_ps(xf + 8 * (j + 3)), h_pairs[j + 3], a3);
    }
    for (; j < n4; j++)
        a0 = _mm256_fmadd_ps(_mm256_loadu_ps(xf + 8 * j), h_pairs[j], a0);

    __m256 acc = _mm256_add_ps(_mm256_add_ps(a0, a1), _mm256_add_ps(a2, a3));
    float complex res = hsum8_pairs(a0, a1, a2, a3);
    (void)acc;
    /* remainder taps (n_taps % 4) */
    for (unsigned k = 4 * n4; k < n_taps; k++)
        res += h[k] * xc[k];
    return res;
}

/* ---- C: AVX2/FMA on deinterleaved data ---- */
static inline float complex decim_dot_c(const float *restrict h,
                                        const float *restrict re,
                                        const float *restrict im,
                                        unsigned int n_taps)
{
    unsigned n8 = n_taps / 8;
    __m256 a0 = _mm256_setzero_ps(), a1 = _mm256_setzero_ps();
    __m256 a2 = _mm256_setzero_ps(), a3 = _mm256_setzero_ps();

    unsigned j = 0;
    for (; j + 2 <= n8; j += 2)
    {
        a0 = _mm256_fmadd_ps(_mm256_loadu_ps(h + 8 * (j + 0)),
                             _mm256_loadu_ps(re + 8 * (j + 0)), a0);
        a1 = _mm256_fmadd_ps(_mm256_loadu_ps(h + 8 * (j + 1)),
                             _mm256_loadu_ps(re + 8 * (j + 1)), a1);
        a2 = _mm256_fmadd_ps(_mm256_loadu_ps(h + 8 * (j + 0)),
                             _mm256_loadu_ps(im + 8 * (j + 0)), a2);
        a3 = _mm256_fmadd_ps(_mm256_loadu_ps(h + 8 * (j + 1)),
                             _mm256_loadu_ps(im + 8 * (j + 1)), a3);
    }
    for (; j < n8; j++)
    {
        a0 = _mm256_fmadd_ps(_mm256_loadu_ps(h + 8 * j),
                             _mm256_loadu_ps(re + 8 * j), a0);
        a2 = _mm256_fmadd_ps(_mm256_loadu_ps(h + 8 * j),
                             _mm256_loadu_ps(im + 8 * j), a2);
    }
    __m256 rr = _mm256_add_ps(a0, a1);
    __m256 ii = _mm256_add_ps(a2, a3);
    __m128 sr = _mm_add_ps(_mm256_castps256_ps128(rr),
                           _mm256_extractf128_ps(rr, 1));
    __m128 si = _mm_add_ps(_mm256_castps256_ps128(ii),
                           _mm256_extractf128_ps(ii, 1));
    /* 4 same-kind lanes per xmm: fold to (l0+l2, l1+l3), then hadd */
    sr = _mm_add_ps(sr, _mm_movehl_ps(sr, sr));
    si = _mm_add_ps(si, _mm_movehl_ps(si, si));
    sr = _mm_hadd_ps(sr, sr);
    si = _mm_hadd_ps(si, si);
    float out[2];
    _mm_storel_pi((__m64 *)out, _mm_unpacklo_ps(sr, si));
    float complex res = out[0] + out[1] * I;
    for (unsigned k = 8 * n8; k < n_taps; k++)
        res += h[k] * (re[k] + im[k] * I);
    return res;
}

/* ---- D: B with tap reuse across two output windows ----
 * Loads per 4-tap group: 1 h vector + 2 x vectors for 2 dots
 * (vs 2 loads per dot in B) — attacks the load-port bottleneck. */
static inline void decim_dot_d(const float complex *restrict xa,
                               const float complex *restrict xb,
                               unsigned int n_taps,
                               float complex *ra, float complex *rb)
{
    const float *fa = (const float *)xa;
    const float *fb = (const float *)xb;
    unsigned n4 = n_taps / 4;
    __m256 a0 = _mm256_setzero_ps(), a1 = _mm256_setzero_ps();
    __m256 a2 = _mm256_setzero_ps(), a3 = _mm256_setzero_ps();
    __m256 b0 = _mm256_setzero_ps(), b1 = _mm256_setzero_ps();
    __m256 b2 = _mm256_setzero_ps(), b3 = _mm256_setzero_ps();

    unsigned j = 0;
    for (; j + 4 <= n4; j += 4)
    {
        __m256 h0 = h_pairs[j + 0];
        __m256 h1 = h_pairs[j + 1];
        a0 = _mm256_fmadd_ps(_mm256_loadu_ps(fa + 8 * (j + 0)), h0, a0);
        a1 = _mm256_fmadd_ps(_mm256_loadu_ps(fa + 8 * (j + 1)), h1, a1);
        a2 = _mm256_fmadd_ps(_mm256_loadu_ps(fa + 8 * (j + 2)), h_pairs[j + 2], a2);
        a3 = _mm256_fmadd_ps(_mm256_loadu_ps(fa + 8 * (j + 3)), h_pairs[j + 3], a3);
        b0 = _mm256_fmadd_ps(_mm256_loadu_ps(fb + 8 * (j + 0)), h0, b0);
        b1 = _mm256_fmadd_ps(_mm256_loadu_ps(fb + 8 * (j + 1)), h1, b1);
        b2 = _mm256_fmadd_ps(_mm256_loadu_ps(fb + 8 * (j + 2)), h_pairs[j + 2], b2);
        b3 = _mm256_fmadd_ps(_mm256_loadu_ps(fb + 8 * (j + 3)), h_pairs[j + 3], b3);
    }
    for (; j < n4; j++)
    {
        __m256 hv = h_pairs[j];
        a0 = _mm256_fmadd_ps(_mm256_loadu_ps(fa + 8 * j), hv, a0);
        b0 = _mm256_fmadd_ps(_mm256_loadu_ps(fb + 8 * j), hv, b0);
    }
    *ra = hsum8_pairs(a0, a1, a2, a3);
    *rb = hsum8_pairs(b0, b1, b2, b3);
    for (unsigned k = 4 * n4; k < n_taps; k++)
    {
        *ra += h[k] * xa[k];
        *rb += h[k] * xb[k];
    }
}

/* ---- harness ---- */
static double now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
}

static unsigned rng = 42;
static double ur(void)
{
    rng ^= rng << 13;
    rng ^= rng >> 17;
    rng ^= rng << 5;
    return (double)(int)(rng & 0xFFFF) / 32768.0 - 1.0;
}

static float complex ys[OUTS], ya[OUTS], yb[OUTS], yc[OUTS], yd[OUTS];

int main(void)
{
    liquid_firdes_kaiser(H_LEN, 1.0f / 125.0f, 35.0f, 0.0f, h);
    prep_h_pairs(h, H_LEN);
    for (unsigned i = 0; i < BUF; i++)
        x[i] = (float)(10.0 * ur()) + (float)(10.0 * ur()) * I;

    const int ITERS = 4000; /* chunks per timed run */
    double t_s = 1e9, t_a = 1e9, t_b = 1e9, t_c = 1e9, t_d = 1e9;
    volatile double sink = 0;

    /* correctness pass (also warm-up) */
    deinterleave(x, BUF, re, im);
    for (int n = 0; n < OUTS; n++)
    {
        unsigned off = (unsigned)n * 125;
        ys[n] = decim_dot_s(h, x + off, H_LEN);
        ya[n] = decim_dot_a(h, re + off, im + off, H_LEN);
        yb[n] = decim_dot_b(x + off, H_LEN);
        yc[n] = decim_dot_c(h, re + off, im + off, H_LEN);
    }
    for (int n = 0; n + 1 < OUTS; n += 2)
        decim_dot_d(x + (unsigned)n * 125, x + (unsigned)(n + 1) * 125,
                    H_LEN, &yd[n], &yd[n + 1]);
    if (OUTS & 1)
        yd[OUTS - 1] = decim_dot_b(x + (unsigned)(OUTS - 1) * 125, H_LEN);
    double mxa = 0, mxb = 0, mxc = 0, mxd = 0, ymax = 0;
    for (int n = 0; n < OUTS; n++)
    {
        double bs = cabs(ys[n]);
        if (bs > ymax)
            ymax = bs;
        double da = cabs(ya[n] - ys[n]);
        double db = cabs(yb[n] - ys[n]);
        double dc = cabs(yc[n] - ys[n]);
        double dd = cabs(yd[n] - ys[n]);
        if (da > mxa)
            mxa = da;
        if (db > mxb)
            mxb = db;
        if (dc > mxc)
            mxc = dc;
        if (dd > mxd)
            mxd = dd;
    }

    for (int rep = 0; rep < 3; rep++)
    {
        double t0, t1, acc;

        /* chain each chunk's result back into the input: no iteration
         * can be hoisted or reused (fresh data every chunk, like the
         * real device path) */
        t0 = now_ms();
        acc = 0;
        for (int it = 0; it < ITERS; it++)
        {
            const float complex *xb = x + (it & 15) * 8;
            float complex s = 0;
            for (int n = 0; n < OUTS; n++)
                s += decim_dot_s(h, xb + (unsigned)n * 125, H_LEN);
            x[it & 63] = s;
            acc += creal(s) + cimag(s);
        }
        sink += acc;
        t1 = now_ms();
        if (t1 - t0 < t_s)
            t_s = t1 - t0;

        t0 = now_ms();
        acc = 0;
        for (int it = 0; it < ITERS; it++)
        {
            const float complex *xb = x + (it & 15) * 8;
            deinterleave(xb, CHUNK, re, im);
            float complex s = 0;
            for (int n = 0; n < OUTS; n++)
                s += decim_dot_a(h, re + (unsigned)n * 125,
                                 im + (unsigned)n * 125, H_LEN);
            x[it & 63] = s;
            acc += creal(s) + cimag(s);
        }
        sink += acc;
        t1 = now_ms();
        if (t1 - t0 < t_a)
            t_a = t1 - t0;

        t0 = now_ms();
        acc = 0;
        for (int it = 0; it < ITERS; it++)
        {
            const float complex *xb = x + (it & 15) * 8;
            float complex s = 0;
            for (int n = 0; n < OUTS; n++)
                s += decim_dot_b(xb + (unsigned)n * 125, H_LEN);
            x[it & 63] = s;
            acc += creal(s) + cimag(s);
        }
        sink += acc;
        t1 = now_ms();
        if (t1 - t0 < t_b)
            t_b = t1 - t0;

        t0 = now_ms();
        acc = 0;
        for (int it = 0; it < ITERS; it++)
        {
            const float complex *xb = x + (it & 15) * 8;
            deinterleave(xb, CHUNK, re, im);
            float complex s = 0;
            for (int n = 0; n < OUTS; n++)
                s += decim_dot_c(h, re + (unsigned)n * 125,
                                 im + (unsigned)n * 125, H_LEN);
            x[it & 63] = s;
            acc += creal(s) + cimag(s);
        }
        sink += acc;
        t1 = now_ms();
        if (t1 - t0 < t_c)
            t_c = t1 - t0;

        t0 = now_ms();
        acc = 0;
        for (int it = 0; it < ITERS; it++)
        {
            const float complex *xb = x + (it & 15) * 8;
            float complex s = 0;
            for (int n = 0; n + 1 < OUTS; n += 2)
            {
                float complex r0, r1;
                decim_dot_d(xb + (unsigned)n * 125, xb + (unsigned)(n + 1) * 125,
                            H_LEN, &r0, &r1);
                s += r0 + r1;
            }
            if (OUTS & 1)
                s += decim_dot_b(xb + (unsigned)(OUTS - 1) * 125, H_LEN);
            x[it & 63] = s;
            acc += creal(s) + cimag(s);
        }
        sink += acc;
        t1 = now_ms();
        if (t1 - t0 < t_d)
            t_d = t1 - t0;
    }

    double cmac = (double)ITERS * OUTS * H_LEN / 1e6;
    printf("sink %g  ymax %.1f\n", sink, ymax);
    printf("parity vs S (max abs): A %.3e  B %.3e  C %.3e  D %.3e\n",
           mxa, mxb, mxc, mxd);
    printf("S: %7.1f ms  %6.0f ns/chunk  %5.0f M cMAC/s  1.00x\n",
           t_s, t_s * 1e6 / ITERS, cmac / t_s * 1e3);
    printf("A: %7.1f ms  %6.0f ns/chunk  %5.0f M cMAC/s  %.2fx\n",
           t_a, t_a * 1e6 / ITERS, cmac / t_a * 1e3, t_s / t_a);
    printf("B: %7.1f ms  %6.0f ns/chunk  %5.0f M cMAC/s  %.2fx\n",
           t_b, t_b * 1e6 / ITERS, cmac / t_b * 1e3, t_s / t_b);
    printf("C: %7.1f ms  %6.0f ns/chunk  %5.0f M cMAC/s  %.2fx\n",
           t_c, t_c * 1e6 / ITERS, cmac / t_c * 1e3, t_s / t_c);
    printf("D: %7.1f ms  %6.0f ns/chunk  %5.0f M cMAC/s  %.2fx\n",
           t_d, t_d * 1e6 / ITERS, cmac / t_d * 1e3, t_s / t_d);
    return 0;
}
