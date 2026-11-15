/* Shared pre-channelization decimation validator: [shared rational
 * resampler -> per-channel small-K decimator] vs the production single
 * [per-channel big-M decimator].
 *
 * Build:  gcc -O3 -march=native -o shared_decim_sim shared_decim_sim.c -lliquid -lm
 * Run:    ./shared_decim_sim
 *
 * The production chain decimates capture_rate -> rate_channel in one
 * stage (m=4, As=35, h_len = 2*m*M+1) once per channel, with the NCO
 * also at capture rate. The proposed chain runs ONE shared resampler
 * (capture_rate -> shared_rate = K*rate_channel) before fan-out,
 * leaving each channel a cheap K-decimator and an NCO at the shared
 * rate. Because capture_rate = ds*rate_channel exactly (both backends
 * snap the hardware rate to an integer multiple of the channel rate),
 * shared_rate/capture_rate = K/ds is RATIONAL with small P=K, Q=ds
 * (both <= 256), so liquid's rresamp_crcf polyphase bank is tiny and
 * the rate is exact - no fractional-tau drift.
 *
 * Variants (rresamp_crcf_create_kaiser(P=K, Q=ds, m, bw, As=60)):
 *   rr6a / rr12a : m = 6 / 12, bw = -1 (liquid auto = critical 0.5*P/Q,
 *                  i.e. passband up to shared_rate/2)
 *   rr12r        : m = 12, bw = roi/capture (passband tightened to the
 *                  ROI edge)
 *   hb2          : hand-staged resamp2 half-band chain (As=50, m=2) +
 *                  resamp_crcf (m=3, npfb=64) remainder - kept as the
 *                  all-integer-stages comparison
 *
 * msresamp_crcf was evaluated and set aside on cost alone: ~34 ms per
 * 1M inputs regardless of ratio (fixed m=7/npfb=256 arbitrary stage +
 * As=60 half-bands dominate). An earlier "not chunk-independent"
 * claim was a harness artifact (missing reset between one-shot and
 * chunked runs) and is retracted: msresamp is streaming-safe, as its
 * per-sample internal buffering promises.
 *
 * Per scenario (CAPTURE RATE_CH WIDTH, all Hz), for baseline and each
 * variant:
 *   1. analytic shared MACs/s + break-even channel count (per-channel
 *      saving includes the NCO moving from capture rate to shared rate)
 *   2. wall-clock of the whole chain vs baseline over the same stream
 *   3. passband flatness (v <= 0.45), near out-of-band leak
 *      (0.45 < v < 3), far leak (v >= 3) via tone probing on an
 *      IDENTICAL grid, each chain normalized by its own measured DC
 *      gain (a decimator is time-varying; tone probing is the ground
 *      truth, not the DTFT of an impulse). The production baseline
 *      passes everything below fc = 1/M (the full +-rate_channel), so
 *      leak around v = 1 is baseline reality, not a new-chain defect;
 *      the bar is delta >= 0.
 *   4. chunking independence (producer feeds arbitrary-size blocks;
 *      the sim re-blocks them to Q-aligned rresamp blocks exactly as
 *      the production producer will)
 *
 * Taps are normalized to unity DC gain here (liquid's
 * liquid_firdes_kaiser is not), so dB metrics are meaningful.
 */
#include <complex.h>
#include <liquid/liquid.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define MAX_CHUNK (1u << 20)
#define N_TAPS_MARGIN 6.0f /* NCO complex mult as MAC-equivalents */

/* --- verbatim port of src/core/dsp.c decimator --- */
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

typedef struct
{
    unsigned M;
    float *h;
    unsigned h_len;
    float complex *tail;
    unsigned tail_len;
    unsigned rem;
    float complex *tmp;
} stage_t;

static stage_t *stage_create(unsigned M, unsigned m, float As, unsigned max_in)
{
    stage_t *st = calloc(1, sizeof(*st));
    st->M = M;
    st->h_len = 2 * m * M + 1;
    st->h = malloc(st->h_len * sizeof(float));
    liquid_firdes_kaiser(st->h_len, 1.0f / (float)M, As, 0.0f, st->h);
    double hs = 0.0;
    for (unsigned i = 0; i < st->h_len; i++)
        hs += st->h[i];
    for (unsigned i = 0; i < st->h_len; i++)
        st->h[i] /= (float)hs;
    st->tail_len = st->h_len - 1;
    st->tail = calloc(st->tail_len, sizeof(float complex));
    st->tmp = calloc(max_in / M + 2, sizeof(float complex));
    return st;
}

static void stage_reset(stage_t *st)
{
    memset(st->tail, 0, st->tail_len * sizeof(float complex));
    st->rem = 0;
}

/* faithful port of dsp_decimate_channel body, one stage, in place */
static void stage_run(stage_t *st, float complex *buf, unsigned *plen)
{
    unsigned M = st->M;
    if (M <= 1)
        return;
    unsigned h_len = st->h_len;
    unsigned tail_len = st->tail_len;
    unsigned r = st->rem;
    unsigned in_len = *plen;
    unsigned out_len = (in_len + r) / M;
    unsigned e = ((M - r) % M == 0 ? M : (M - r) % M) - 1;

    unsigned n = 0;
    while (n < out_len && e < tail_len)
    {
        unsigned split = tail_len - e;
        st->tmp[n] = decim_dot(st->h, st->tail + e, split) +
                     decim_dot(st->h + split, buf, h_len - split);
        n++;
        e += M;
    }
    for (; n < out_len; n++, e += M)
        st->tmp[n] = decim_dot(st->h, buf + e - tail_len, h_len);

    if (in_len >= tail_len)
        memcpy(st->tail, buf + in_len - tail_len, tail_len * sizeof(float complex));
    else
    {
        memmove(st->tail, st->tail + in_len, (tail_len - in_len) * sizeof(float complex));
        memcpy(st->tail + tail_len - in_len, buf, in_len * sizeof(float complex));
    }
    st->rem = (r + in_len) % M;
    memcpy(buf, st->tmp, out_len * sizeof(float complex));
    *plen = out_len;
}

typedef struct
{
    const char *name;
    int hand;                /* 1 = hb2 half-band chain, 0 = rresamp */
    rresamp_crcf rr;         /* rational resampler (hand == 0) */
    unsigned Q, P;           /* its block sizes */
    double macs_per_input;   /* analytic shared MACs per input sample */
    double dc_gain;          /* measured whole-chain DC gain */
    /* hand-chain state */
    resamp2_crcf hb[8];
    unsigned n_hb;
    resamp_crcf arb;
    unsigned hb_carry_n;
    float complex hb_carry;
    /* Q-blocking accumulator (production feeds rresamp in Q-blocks) */
    float complex qbuf[512];
    unsigned qbuf_len;
    stage_t *ch; /* per-channel K decimator */
} chain_t;

static void chain_init_rr(chain_t *c, unsigned K, unsigned ds, unsigned m, double bw,
                          double roi, double capture, double shared)
{
    memset(c, 0, sizeof(*c));
    c->Q = ds;
    c->P = K;
    c->rr = rresamp_crcf_create_kaiser(K, ds, m, (float)bw, 60.0f);
    (void)roi;
    (void)capture;
    (void)shared;
    /* per output: one 2m-tap polyphase branch dot; outputs per input = P/Q */
    c->macs_per_input = (double)(2 * m) * (double)K / (double)ds;
    c->ch = stage_create(K, 4, 35.0f, MAX_CHUNK);
}

static void chain_init_hand(chain_t *c, double capture, double shared, double roi, unsigned K)
{
    memset(c, 0, sizeof(*c));
    c->hand = 1;
    double r = capture;
    c->macs_per_input = 0.0;
    while (r / 2.0 >= shared && r / 8.0 >= roi && c->n_hb < 8)
    {
        c->hb[c->n_hb++] = resamp2_crcf_create(2, 0.0f, 50.0f);
        c->macs_per_input += (9 / 2.0) * (r / capture);
        r /= 2.0;
    }
    double rr = shared / r;
    if (rr < 0.999)
    {
        double fc = roi / shared;
        if (fc > 0.45)
            fc = 0.45;
        c->arb = resamp_crcf_create((float)rr, 3, (float)fc, 60.0f, 64);
        c->macs_per_input += rr * 12.0 * (r / capture);
    }
    c->ch = stage_create(K, 4, 35.0f, MAX_CHUNK);
}

static void chain_reset(chain_t *c)
{
    stage_reset(c->ch);
    c->qbuf_len = 0;
    if (c->hand)
    {
        for (unsigned i = 0; i < c->n_hb; i++)
            resamp2_crcf_reset(c->hb[i]);
        if (c->arb)
            resamp_crcf_reset(c->arb);
        c->hb_carry_n = 0;
    }
    else
        rresamp_crcf_reset(c->rr);
}

/* push n samples through shared stage + K decimator into y (capacity
 * out_max), return count. rresamp path re-blocks to Q-aligned blocks
 * exactly as the production producer will. */
static unsigned chain_push(chain_t *c, const float complex *x, unsigned n,
                           float complex *y, unsigned out_max)
{
    static _Thread_local float complex t1[MAX_CHUNK + 512], t2[MAX_CHUNK + 512];
    unsigned n1 = 0;
    if (!c->hand)
    {
        for (unsigned i = 0; i < n; i++)
        {
            c->qbuf[c->qbuf_len++] = x[i];
            if (c->qbuf_len == c->Q)
            {
                rresamp_crcf_execute(c->rr, c->qbuf, t1 + n1);
                n1 += c->P;
                c->qbuf_len = 0;
            }
        }
    }
    else
    {
        if (c->hb_carry_n)
        {
            t1[0] = c->hb_carry;
            memcpy(t1 + 1, x, n * sizeof(float complex));
            n1 = n + 1;
        }
        else
        {
            memcpy(t1, x, n * sizeof(float complex));
            n1 = n;
        }
        c->hb_carry_n = n1 & 1;
        if (c->hb_carry_n)
        {
            c->hb_carry = t1[n1 - 1];
            n1--;
        }
        for (unsigned i = 0; i < c->n_hb; i++)
        {
            unsigned half = n1 / 2;
            for (unsigned j = 0; j < half; j++)
                resamp2_crcf_decim_execute(c->hb[i], &t1[2 * j], &t2[j]);
            memcpy(t1, t2, half * sizeof(float complex));
            n1 = half;
        }
        if (c->arb)
        {
            unsigned na = 0;
            resamp_crcf_execute_block(c->arb, t1, n1, t2, &na);
            memcpy(t1, t2, na * sizeof(float complex));
            n1 = na;
        }
    }
    unsigned no = n1 < out_max ? n1 : out_max;
    memcpy(y, t1, no * sizeof(float complex));
    stage_run(c->ch, y, &no);
    return no;
}

static double now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
}

static double rms_c(const float complex *y, unsigned from, unsigned to)
{
    double p = 0;
    for (unsigned i = from; i < to; i++)
        p += creal(y[i]) * creal(y[i]) + cimag(y[i]) * cimag(y[i]);
    return sqrt(p / (double)(to - from));
}

static unsigned xorshift32(unsigned *s)
{
    unsigned x = *s;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *s = x;
    return x;
}

static double ur(unsigned *s)
{
    return (double)(int)(xorshift32(s) & 0xFFFF) / 32768.0 - 1.0;
}

static double probe_base(stage_t *base, unsigned M, double v, unsigned n,
                         float complex *x)
{
    for (unsigned i = 0; i < n; i++)
        x[i] = cexpf(2.0f * (float)M_PI * I * (float)(v / M) * (float)i);
    stage_reset(base);
    unsigned len = n;
    stage_run(base, x, &len);
    return 20.0 * log10(rms_c(x, 64, len) + 1e-30);
}

/* tone offset v (multiples of rate_ch); DC-gain-normalized output dB */
static double probe_new(chain_t *c, double capture, double rate_ch, double v,
                        unsigned n, float complex *x, float complex *y)
{
    for (unsigned i = 0; i < n; i++)
        x[i] = cexpf(2.0f * (float)M_PI * I * (float)(v * rate_ch / capture) * (float)i);
    chain_reset(c);
    unsigned no = chain_push(c, x, n, y, MAX_CHUNK);
    return 20.0 * log10(rms_c(y, 64, no) / c->dc_gain + 1e-30);
}

static double chain_dc_gain(chain_t *c, unsigned n, float complex *x, float complex *y)
{
    for (unsigned i = 0; i < n; i++)
        x[i] = 1.0f;
    chain_reset(c);
    unsigned no = chain_push(c, x, n, y, MAX_CHUNK);
    return rms_c(y, 64, no);
}

static void scenario(double capture, double rate_ch, double width, const char *name)
{
    printf("=== %s: capture %.0f Hz, channel %.0f Hz, ROI %.0f Hz\n", name, capture,
           rate_ch, width);

    unsigned M = (unsigned)(capture / rate_ch);
    unsigned K = (unsigned)ceil(width / rate_ch + 0.5);
    double shared = K * rate_ch;
    if (shared >= capture)
    {
        printf("  bypass: shared %.0f >= capture %.0f (per-channel M=%u unchanged)\n\n",
               shared, capture, M);
        return;
    }

    stage_t *base = stage_create(M, 4, 35.0f, MAX_CHUNK);
    enum { NV = 4 };
    chain_t cv[NV];
    const char *names[NV] = {"rr6a", "rr12a", "rr12r", "hb2"};
    chain_init_rr(&cv[0], K, M, 6, -1.0, width / 2.0, capture, shared);
    chain_init_rr(&cv[1], K, M, 12, -1.0, width / 2.0, capture, shared);
    chain_init_rr(&cv[2], K, M, 12, width / 2.0 / capture, width / 2.0, capture, shared);
    chain_init_hand(&cv[3], capture, shared, width / 2.0, K);
    for (int i = 0; i < NV; i++)
    {
        cv[i].name = names[i];
        static float complex xd[MAX_CHUNK], yd[MAX_CHUNK];
        cv[i].dc_gain = chain_dc_gain(&cv[i], 1u << 16, xd, yd);
    }

    static float complex x[MAX_CHUNK], y[MAX_CHUNK];

    /* ---- 1. analytic cost + break-even ---- */
    double save_ch = ((double)(2 * 4 * M + 1) - (double)(2 * 4 * K + 1)) * rate_ch +
                     N_TAPS_MARGIN * (capture - shared);
    printf("  K=%u shared=%.0f Hz (P/Q = %u/%u) baseline M=%u\n", K, shared, K, M, M);
    printf("  per-channel saving           : %.2fM MACs/s (decim + NCO retimed)\n",
           save_ch / 1e6);
    for (int i = 0; i < NV; i++)
        printf("  [%-5s] analytic shared: %6.2f MACs/in = %6.2fM MACs/s  N*=%.2f\n",
               cv[i].name, cv[i].macs_per_input, cv[i].macs_per_input * capture / 1e6,
               cv[i].macs_per_input * capture / save_ch);

    /* ---- 2. wall-clock, whole chain vs baseline ---- */
    unsigned N = 1u << 20;
    unsigned seed = 42;
    for (unsigned i = 0; i < N; i++)
        x[i] = (float)ur(&seed) + (float)ur(&seed) * I;
    double t0 = now_ms();
    stage_reset(base);
    unsigned len = N;
    stage_run(base, x, &len);
    double t_base = now_ms() - t0;
    printf("  wall-clock per 1M in-samples : base %.1f ms\n", t_base);
    for (int i = 0; i < NV; i++)
    {
        for (unsigned j = 0; j < N; j++)
            x[j] = (float)ur(&seed) + (float)ur(&seed) * I;
        t0 = now_ms();
        chain_reset(&cv[i]);
        chain_push(&cv[i], x, N, y, MAX_CHUNK);
        double t_new = now_ms() - t0;
        printf("  [%-5s] whole chain: %5.1f ms (base-side decim identical code)\n",
               cv[i].name, t_new);
    }

    /* ---- 3. tone probes on identical grids ---- */
    double pb_dev_b = 0, near_b = -999, far_b = -999;
    double pb_dev[NV], near[NV], far[NV];
    for (int i = 0; i < NV; i++)
    {
        pb_dev[i] = 0;
        near[i] = -999;
        far[i] = -999;
    }
    double vmax = capture / 2.0 / rate_ch;
    for (double v = 0.05; v <= vmax + 1e-9; v += 0.05)
    {
        double gb = probe_base(base, M, v, 1u << 16, x);
        if (v <= 0.45)
        {
            if (fabs(gb) > pb_dev_b)
                pb_dev_b = fabs(gb);
        }
        else if (v < 3.0)
        {
            if (gb > near_b)
                near_b = gb;
        }
        else if (gb > far_b)
            far_b = gb;
        for (int i = 0; i < NV; i++)
        {
            double gn = probe_new(&cv[i], capture, rate_ch, v, 1u << 16, x, y);
            if (v <= 0.45)
            {
                if (fabs(gn) > pb_dev[i])
                    pb_dev[i] = fabs(gn);
            }
            else if (v < 3.0)
            {
                if (gn > near[i])
                    near[i] = gn;
            }
            else if (gn > far[i])
                far[i] = gn;
        }
    }
    printf("  passband dev dB: base %.2f", pb_dev_b);
    for (int i = 0; i < NV; i++)
        printf(" | %s %.2f", cv[i].name, pb_dev[i]);
    printf("\n  near leak dB   : base %.1f", near_b);
    for (int i = 0; i < NV; i++)
        printf(" | %s %+.1f", cv[i].name, near[i] - near_b);
    printf("\n  far leak dB    : base %.1f", far_b);
    for (int i = 0; i < NV; i++)
        printf(" | %s %+.1f", cv[i].name, far[i] - far_b);
    printf("   (deltas vs base)\n");

    /* ---- 4. chunking independence ---- */
    unsigned Ns = 1u << 20;
    for (unsigned i = 0; i < Ns; i++)
        x[i] = (float)ur(&seed) * 0.5f + (float)ur(&seed) * 0.5f * I;
    for (int i = 0; i < NV; i++)
    {
        static float complex one[MAX_CHUNK], two[MAX_CHUNK];
        chain_reset(&cv[i]);
        unsigned n1 = chain_push(&cv[i], x, Ns, one, MAX_CHUNK);
        chain_reset(&cv[i]);
        unsigned pos = 0, ot = 0, cs = 12345;
        while (pos < Ns)
        {
            unsigned chunk = 1 + (xorshift32(&cs) % 9000);
            if (pos + chunk > Ns)
                chunk = Ns - pos;
            ot += chain_push(&cv[i], x + pos, chunk, two + ot, MAX_CHUNK - ot);
            pos += chunk;
        }
        double maxd = 0, rr = 0;
        unsigned ml = n1 < ot ? n1 : ot;
        for (unsigned j = 0; j < ml; j++)
        {
            double d = cabs(one[j] - two[j]);
            if (d > maxd)
                maxd = d;
            rr += cabs(one[j]) * cabs(one[j]);
        }
        printf("  [%-5s] chunk-independence rel err: %.2e\n", cv[i].name,
               maxd / sqrt(rr / ml));
    }
    printf("\n");
}

int main(void)
{
    scenario(228000, 12000, 36500, "RTL flagship: 2x NBFM 12.5k apart");
    scenario(1008000, 170000, 340000, "RTL wbfm single");
    scenario(2000000, 181818, 340000, "RSP wbfm single (2M snap)");
    scenario(2000000, 24000, 1848000, "regression scene: 13ch -s 24k");
    scenario(420000, 12000, 424000, "RTL multi non-floored (bypass)");
    return 0;
}
