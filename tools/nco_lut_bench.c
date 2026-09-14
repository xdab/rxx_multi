/* Option 3 validator: NCO LUT size + interpolation vs production LUT.
 *
 * Production (src/core/dsp.c:18-49): 64K-entry (512 KB) unit-phasor
 * table driven by a 32-bit DDS accumulator, direct lookup, in-place
 * complex multiply. The table cannot fit 32 KB L1; the profile blames
 * dsp_shift_frequency for 76% of all D1 read misses and ~20% of
 * runtime (latency attribution partly speculative).
 *
 * Variants:
 *   V0: production — 16-bit table, direct lookup
 *   V1: 12-bit table (32 KB, L1-resident), linear interpolation from
 *       the next 12 phase bits (AoS: float complex table)
 *   V2: same as V1 but SoA (separate cos[]/sin[] float arrays)
 *   V3: 12-bit table, direct lookup (control: interp value isolated)
 *   V4: phasor recurrence p *= r (one complex mul per sample, zero
 *       table traffic in the loop), re-anchored from cexp() every K
 *       samples (K = 1024 and K = 8192 = once per chunk)
 *
 * Measures:
 *   - accuracy: error vs a double-precision ideal shift over 1M
 *     samples (pristine input kept separately), chunked like
 *     production (persistent accumulator), reported as dBc (bounds
 *     every spur line)
 *   - wall-clock: ns/sample with an L1/L2-thrashing pass between
 *     chunks (~160 KB, mimicking the decimator + fan-out footprint
 *     sharing L2 with the table), and without thrash for reference
 *
 * Deterministic cache evidence: run under
 *   valgrind --tool=cachegrind --cache-sim=yes
 * and compare D1mr per shift_* function.
 *
 * Verdict recorded in docs/improvement-options.md Option 3 (2026-09-14,
 * Ryzen 3700X / Zen 2): REJECTED. Interpolation is accurate (−130 dBc)
 * but 29-130% slower than the shipped 16-bit direct lookup; the D1
 * misses it removes are L2-hits whose latency out-of-order execution
 * already hides. Only 12-bit direct is faster (+11%) and it degrades
 * spurs to −61 dBc. Benchmark pitfalls encoded here: keep a pristine
 * input copy for the error metric (the shift is in-place), and the
 * recurrence variant must advance its phasor AFTER using it.
 *
 * Build: gcc -O3 -march=native -Wall -Wextra -o nco_lut_bench \
 *            nco_lut_bench.c -lm
 */
#include <complex.h>
#include <math.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <time.h>

#define FS 2000000.0
#define NCHUNK 8192
#define ACC_N (1u << 20)
#define PERF_CHUNKS 3000
#define THRASH_FLOATS 40960 /* 160 KB touched between chunks */

static float complex lut16[1u << 16];
static float complex lut12[1u << 12];
static float cos12[1u << 12], sin12[1u << 12];

static uint32_t step_for(double f_off)
{
    return (uint32_t)llround(f_off / FS * 4294967296.0);
}

static void build_luts(void)
{
    for (unsigned k = 0; k < (1u << 16); k++)
        lut16[k] = cexp((2.0 * M_PI * (double)k / (double)(1u << 16)) * I);
    for (unsigned k = 0; k < (1u << 12); k++)
    {
        double a = 2.0 * M_PI * (double)k / (double)(1u << 12);
        lut12[k] = cexp(a * I);
        cos12[k] = (float)cos(a);
        sin12[k] = (float)sin(a);
    }
}

/* ---- V0: production, verbatim ---- */
static void shift_v0(float complex *x, unsigned n, uint32_t *acc, uint32_t step)
{
    uint32_t a = *acc;
    for (unsigned i = 0; i < n; i++)
    {
        x[i] = x[i] * lut16[a >> 16];
        a += step;
    }
    *acc = a;
}

/* ---- V1: 12-bit AoS, interpolated ---- */
static void shift_v1(float complex *x, unsigned n, uint32_t *acc, uint32_t step)
{
    uint32_t a = *acc;
    for (unsigned i = 0; i < n; i++)
    {
        unsigned idx = a >> 20;
        float f = (float)((a >> 8) & 0xFFFu) * (1.0f / 4096.0f);
        float complex p0 = lut12[idx];
        float complex p1 = lut12[(idx + 1u) & 0xFFFu];
        x[i] = x[i] * (p0 + f * (p1 - p0));
        a += step;
    }
    *acc = a;
}

/* ---- V2: 12-bit SoA, interpolated ---- */
static void shift_v2(float complex *x, unsigned n, uint32_t *acc, uint32_t step)
{
    uint32_t a = *acc;
    for (unsigned i = 0; i < n; i++)
    {
        unsigned idx = a >> 20;
        unsigned nxt = (idx + 1u) & 0xFFFu;
        float f = (float)((a >> 8) & 0xFFFu) * (1.0f / 4096.0f);
        float c = cos12[idx] + f * (cos12[nxt] - cos12[idx]);
        float s = sin12[idx] + f * (sin12[nxt] - sin12[idx]);
        float xr = crealf(x[i]), xi = cimagf(x[i]);
        x[i] = (xr * c - xi * s) + (xr * s + xi * c) * I;
        a += step;
    }
    *acc = a;
}

/* ---- V3: 12-bit, direct lookup (control) ---- */
static void shift_v3(float complex *x, unsigned n, uint32_t *acc, uint32_t step)
{
    uint32_t a = *acc;
    for (unsigned i = 0; i < n; i++)
    {
        x[i] = x[i] * lut12[a >> 20];
        a += step;
    }
    *acc = a;
}

/* ---- V4: phasor recurrence, re-anchored every anchor_k samples ---- */
static int anchor_k = 1024;

static void shift_v4(float complex *x, unsigned n, uint32_t *acc, uint32_t step)
{
    uint32_t a = *acc;
    const double ang_step = 2.0 * M_PI * (double)step / 4294967296.0;
    const float rr = (float)cos(ang_step);
    const float ri = (float)sin(ang_step);
    float pr = 1.0f, pi = 0.0f; /* anchor at acc via cexp below */
    for (unsigned i = 0; i < n; i++)
    {
        if ((i & (unsigned)(anchor_k - 1)) == 0)
        {
            double aa = 2.0 * M_PI * (double)a / 4294967296.0;
            pr = (float)cos(aa);
            pi = (float)sin(aa);
        }
        /* x *= p (current phasor, THEN advance) */
        float xr = crealf(x[i]), xi = cimagf(x[i]);
        x[i] = (xr * pr - xi * pi) + (xr * pi + xi * pr) * I;
        /* p *= r */
        float nr = pr * rr - pi * ri;
        pi = pr * ri + pi * rr;
        pr = nr;
        a += step;
    }
    *acc = a;
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

typedef void (*shift_fn)(float complex *, unsigned, uint32_t *, uint32_t);

static float xin_re[ACC_N], xin_im[ACC_N]; /* pristine input */

static double accuracy(shift_fn fn, double f_off)
{
    static float complex x[ACC_N];
    for (unsigned i = 0; i < ACC_N; i++)
    {
        xin_re[i] = (float)ur();
        xin_im[i] = (float)ur();
        x[i] = xin_re[i] + xin_im[i] * I;
    }
    uint32_t acc = 0x12345678u; /* nonzero start catches offset bugs */
    uint32_t step = step_for(f_off);
    unsigned done = 0;
    while (done < ACC_N)
    {
        unsigned n = NCHUNK;
        if (done + n > ACC_N)
            n = ACC_N - done;
        fn(x + done, n, &acc, step);
        done += n;
    }
    double w = (double)step / 4294967296.0;
    double ph0 = 2.0 * M_PI * 0x12345678u / 4294967296.0;
    double err2 = 0, sig2 = 0;
    for (unsigned i = 0; i < ACC_N; i++)
    {
        double th = ph0 + 2.0 * M_PI * w * (double)i;
        double cr = cos(th), ci = sin(th);
        double ir = xin_re[i], ii = xin_im[i];
        double dr = crealf(x[i]) - (ir * cr - ii * ci);
        double di = cimagf(x[i]) - (ir * ci + ii * cr);
        err2 += dr * dr + di * di;
        sig2 += ir * ir + ii * ii;
    }
    return 10.0 * log10(err2 / sig2);
}

static float thrash[THRASH_FLOATS];

static double perf(shift_fn fn, double f_off, int do_thrash)
{
    static float complex x[NCHUNK];
    for (unsigned i = 0; i < NCHUNK; i++)
        x[i] = (float)ur() + (float)ur() * I;
    uint32_t acc = 0;
    uint32_t step = step_for(f_off);

    for (int k = 0; k < 50; k++) /* warm-up */
    {
        double s = 0;
        for (unsigned j = 0; j < THRASH_FLOATS; j += 16)
            s += thrash[j];
        fn(x, NCHUNK, &acc, step);
        if (s == 12345.0)
            printf(".");
    }

    double t0 = now_ms();
    double sink = 0;
    for (int k = 0; k < PERF_CHUNKS; k++)
    {
        if (do_thrash)
            for (unsigned j = 0; j < THRASH_FLOATS; j += 16)
                sink += thrash[j];
        fn(x, NCHUNK, &acc, step);
        x[0] = (float)sink; /* keep everything alive */
    }
    double t1 = now_ms();
    if (sink == 12345.0)
        printf("x");
    return (t1 - t0) * 1e6 / ((double)PERF_CHUNKS * NCHUNK); /* ns/sample */
}

int main(void)
{
    build_luts();
    const double offs[] = {225000.0, 1000.0};
    struct
    {
        const char *name;
        shift_fn fn;
    } vs[] = {
        {"V0 16-bit direct (prod)", shift_v0},
        {"V1 12-bit interp AoS", shift_v1},
        {"V2 12-bit interp SoA", shift_v2},
        {"V3 12-bit direct", shift_v3},
        {"V4 recurrence K=1024", shift_v4},
    };

    printf("== accuracy (total error vs double ideal, dBc — bounds all spurs) ==\n");
    for (unsigned v = 0; v < 5; v++)
    {
        printf("%-24s", vs[v].name);
        for (unsigned o = 0; o < 2; o++)
            printf("  off=%7.0f Hz: %7.1f dBc", offs[o], accuracy(vs[v].fn, offs[o]));
        printf("\n");
    }

    printf("== speed (ns/sample, 160 KB inter-chunk thrash), best of 3 ==\n");
    for (unsigned v = 0; v < 5; v++)
    {
        printf("%-24s", vs[v].name);
        for (unsigned o = 0; o < 2; o++)
        {
            double best = 1e9;
            for (int r = 0; r < 3; r++)
            {
                double t = perf(vs[v].fn, offs[o], 1);
                if (t < best)
                    best = t;
            }
            printf("  off=%7.0f Hz: %6.3f ns", offs[o], best);
        }
        printf("\n");
    }

    printf("== speed without thrash (hot-cache reference), off=225 kHz ==\n");
    for (unsigned v = 0; v < 5; v++)
    {
        double best = 1e9;
        for (int r = 0; r < 3; r++)
        {
            double t = perf(vs[v].fn, offs[0], 0);
            if (t < best)
                best = t;
        }
        printf("%-24s  %6.3f ns\n", vs[v].name, best);
    }

    /* V4 accuracy vs anchor interval */
    printf("== V4 anchor-interval sensitivity (off=225 kHz) ==\n");
    int ks[] = {256, 1024, 4096, 8192};
    for (unsigned k = 0; k < 4; k++)
    {
        anchor_k = ks[k];
        printf("  K=%5d: %7.1f dBc\n", ks[k], accuracy(shift_v4, offs[0]));
    }
    anchor_k = 1024;
    return 0;
}
