/* Option 1 validator: factorized multi-stage decimation vs production
 * single-stage decimator.
 *
 * Build:  gcc -O3 -march=native -o multistage_sim multistage_sim.c -lliquid -lm
 * Run:    ./multistage_sim FACTORS MS [As]     (default As = 35)
 *           e.g. ./multistage_sim 5,5,5 4,4,4
 *                 ./multistage_sim 125 4        (must give all-zero deltas)
 *
 * Verdict recorded in docs/improvement-options.md Option 1 (2026-09-14):
 * REJECTED — naive cascades cost 0.72-1.27x the MACs of the single
 * stage AND collapse far-zone alias rejection by ~60 dB.
 *
 * Faithful port of the dsp.c decimator semantics (decim_dot, tail slide,
 * decim_rem carry, e-index arithmetic) as a per-stage primitive, then:
 *   1. MAC count per input sample (the headline claim to validate)
 *   2. in-band gain curve via dense tone grid (a decimator is
 *      time-varying; a single impulse excites one polyphase branch
 *      only, so tone probing is the ground truth)
 *   3. alias leakage via out-of-band complex tones
 *   4. chunking-independence (random chunk sizes vs one-shot)
 *   5. semantics vs brute-force direct convolution (zero-padded history)
 *
 * Baseline (single stage, m=4, As=35, M=prod(FACTORS)) is always computed
 * internally; all quality metrics are reported for baseline AND config.
 * NOTE: taps are normalized to unity DC gain here — liquid's
 * liquid_firdes_kaiser does not normalize (sum(h) ~= M/2), which the
 * production FM path is blind to but AM/SSB/raw are not.
 */
#include <complex.h>
#include <liquid/liquid.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_STAGES 8
#define MAX_CHUNK (1u << 20)

static double g_macs = 0.0;
static unsigned long long g_dots = 0;

/* --- verbatim from src/core/dsp.c (plus counters) --- */
static inline float complex decim_dot(const float *restrict h,
                                      const float complex *restrict x,
                                      unsigned int n_taps)
{
    const float *restrict xf = (const float *)x;
    float ar0 = 0.0f, ai0 = 0.0f;
    float ar1 = 0.0f, ai1 = 0.0f;
    float ar2 = 0.0f, ai2 = 0.0f;
    float ar3 = 0.0f, ai3 = 0.0f;

    g_macs += n_taps;
    g_dots++;

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
    float complex *tmp; /* scratch sized for max_chunk / M_i */
} stage_t;

typedef struct
{
    stage_t *st[MAX_STAGES];
    int nst;
    unsigned total_M;
} cascade_t;

static stage_t *stage_create(unsigned M, unsigned m, float As, unsigned max_in)
{
    stage_t *st = calloc(1, sizeof(*st));
    st->M = M;
    st->h_len = 2 * m * M + 1;
    st->h = malloc(st->h_len * sizeof(float));
    liquid_firdes_kaiser(st->h_len, 1.0f / (float)M, As, 0.0f, st->h);
    /* liquid's kaiser taps are NOT gain-normalized (sum ~= M/2 for
     * fc=1/M). Production gets away with it because FM's polar
     * discriminator is amplitude-blind; normalize here so dB metrics
     * are meaningful. */
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
        memcpy(st->tail, buf + in_len - tail_len,
               tail_len * sizeof(float complex));
    else
    {
        memmove(st->tail, st->tail + in_len,
                (tail_len - in_len) * sizeof(float complex));
        memcpy(st->tail + tail_len - in_len, buf,
               in_len * sizeof(float complex));
    }
    st->rem = (r + in_len) % M;
    memcpy(buf, st->tmp, out_len * sizeof(float complex));
    *plen = out_len;
}

static cascade_t *cascade_create(const unsigned *factors, const unsigned *ms,
                                 int nst, float As)
{
    cascade_t *c = calloc(1, sizeof(*c));
    c->nst = nst;
    unsigned cum = 1;
    for (int i = 0; i < nst; i++)
    {
        c->st[i] = stage_create(factors[i], ms[i], As, MAX_CHUNK / cum + 16);
        cum *= factors[i];
    }
    c->total_M = cum;
    return c;
}

static void cascade_reset(cascade_t *c)
{
    for (int i = 0; i < c->nst; i++)
        stage_reset(c->st[i]);
}

static void cascade_run(cascade_t *c, float complex *buf, unsigned *plen)
{
    for (int i = 0; i < c->nst; i++)
        stage_run(c->st[i], buf, plen);
}

/* --- test 5: brute-force reference (whole-stream direct convolution) --- */
static unsigned brute_stage(const float *h, unsigned h_len, unsigned M,
                             const float complex *x, unsigned n_in,
                             float complex *y)
{
    unsigned n_out = n_in / M;
    for (unsigned n = 0; n < n_out; n++)
    {
        float complex acc = 0;
        /* output n anchored at newest input e + n*M, taps h[0]=oldest */
        unsigned newest = (M - 1) + n * M; /* r starts at 0 -> e = M-1 */
        for (unsigned k = 0; k < h_len; k++)
        {
            int idx = (int)newest - (int)(h_len - 1) + (int)k;
            if (idx >= 0 && idx < (int)n_in)
                acc += h[k] * x[idx];
        }
        y[n] = acc;
    }
    return n_out;
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

static double ur(unsigned *s) /* uniform [-1,1] */
{
    return (double)(int)(xorshift32(s) & 0xFFFF) / 32768.0 - 1.0;
}

static double rms_c(const float complex *y, unsigned from, unsigned to)
{
    double p = 0;
    for (unsigned i = from; i < to; i++)
        p += creal(y[i]) * creal(y[i]) + cimag(y[i]) * cimag(y[i]);
    return sqrt(p / (double)(to - from));
}

/* DTFT of the decimated impulse is NOT the system response (the decimator
 * is time-varying: one impulse excites a single polyphase branch), so all
 * response metrics come from tone probing instead. */

int main(int argc, char **argv)
{
    if (argc < 3)
    {
        fprintf(stderr, "usage: %s FACTORS MS [As]\n", argv[0]);
        return 1;
    }
    unsigned factors[MAX_STAGES], ms[MAX_STAGES];
    int nst = 0;
    float As = (argc > 3) ? atof(argv[3]) : 35.0f;
    for (char *p = strtok(argv[1], ","); p && nst < MAX_STAGES; p = strtok(NULL, ","))
        factors[nst++] = atoi(p);
    int nms = 0;
    for (char *p = strtok(argv[2], ","); p && nms < MAX_STAGES; p = strtok(NULL, ","))
        ms[nms++] = atoi(p);
    if (nms != nst)
    {
        fprintf(stderr, "factors/ms count mismatch\n");
        return 1;
    }

    unsigned total = 1;
    for (int i = 0; i < nst; i++)
        total *= factors[i];
    cascade_t *base = calloc(1, sizeof(*base)); /* production single stage */
    base->st[0] = stage_create(total, 4, 35.0f, MAX_CHUNK);
    base->nst = 1;
    base->total_M = total;
    cascade_t *cfg = cascade_create(factors, ms, nst, As);

    static float complex x[MAX_CHUNK], ybuf[MAX_CHUNK], yref[MAX_CHUNK];
    unsigned seed = 42;

    /* ---- 1. MACs per input sample ---- */
    unsigned N = 1u << 20;
    for (unsigned i = 0; i < N; i++)
        x[i] = (float)ur(&seed) + (float)ur(&seed) * I;
    g_macs = 0;
    cascade_reset(base);
    unsigned len = N;
    cascade_run(base, x, &len);
    double macs_base = g_macs / (double)N;
    for (unsigned i = 0; i < N; i++)
        x[i] = (float)ur(&seed) + (float)ur(&seed) * I;
    g_macs = 0;
    cascade_reset(cfg);
    len = N;
    cascade_run(cfg, x, &len);
    double macs_cfg = g_macs / (double)N;

    /* ---- 2. in-band gain curve via dense tone grid ----
     * (a decimator is time-varying: a single impulse excites one
     * polyphase branch only, so tone probing is the ground truth) */
    unsigned Nt = 1u << 16;
    cascade_t *cs[2] = {base, cfg};
    double gain_b[64], gain_c[64];
    int ng = 0;
    for (double v = 0.01; v <= 0.951; v += 0.02)
    {
        double g[2];
        for (int c = 0; c < 2; c++)
        {
            for (unsigned i = 0; i < Nt; i++)
                x[i] = cexpf(2.0f * (float)M_PI * I * (float)(v / total) * (float)i);
            cascade_reset(cs[c]);
            len = Nt;
            cascade_run(cs[c], x, &len);
            g[c] = rms_c(x, 64, len);
        }
        gain_b[ng] = g[0];
        gain_c[ng] = g[1];
        ng++;
    }
    double pb_dev[2] = {0, 0}, h_ratio_dev = 0;
    for (int k = 0; k < ng; k++)
    {
        double v = 0.01 + 0.02 * k;
        double db[2];
        for (int c = 0; c < 2; c++)
        {
            db[c] = 20.0 * log10((c ? gain_c[k] : gain_b[k]) + 1e-30);
            if (v <= 0.25)
            {
                double dev = fabs(db[c]);
                if (dev > pb_dev[c])
                    pb_dev[c] = dev;
            }
        }
        if (v <= 0.45)
        {
            double rdb = fabs(db[1] - db[0]);
            if (rdb > h_ratio_dev)
                h_ratio_dev = rdb;
        }
    }

    /* ---- 3. alias leakage via out-of-band tones ---- */
    double worst_near[2] = {-999, -999}, worst_far[2] = {-999, -999};
    double vs[128];
    int nv = 0;
    for (double v = 0.55; v <= 1.501; v += 0.05)
        vs[nv++] = v;
    for (double v = 1.55; v <= 10.0; v += 0.25)
        vs[nv++] = v;
    for (double v = 11.0; v < 0.5 * total; v += 1.0)
        vs[nv++] = v;
    for (int iv = 0; iv < nv; iv++)
    {
        double v = vs[iv];
        for (int c = 0; c < 2; c++)
        {
            for (unsigned i = 0; i < Nt; i++)
                x[i] = cexpf(2.0f * (float)M_PI * I * (float)(v / total) * (float)i);
            cascade_reset(cs[c]);
            len = Nt;
            cascade_run(cs[c], x, &len);
            double dB = 20.0 * log10(rms_c(x, 64, len) + 1e-30);
            if (v < 3.0)
            {
                if (dB > worst_near[c])
                    worst_near[c] = dB;
            }
            else if (dB > worst_far[c])
                worst_far[c] = dB;
        }
    }

    /* ---- 4. chunking independence (cfg) ---- */
    unsigned Ns = 1u << 20;
    for (unsigned i = 0; i < Ns; i++)
        x[i] = (float)ur(&seed) * 0.5f + (float)ur(&seed) * 0.5f * I;
    cascade_reset(cfg);
    unsigned pos = 0, oc = 0;
    unsigned cs2 = 12345;
    while (pos < Ns)
    {
        unsigned chunk = 1 + (xorshift32(&cs2) % 20000);
        if (pos + chunk > Ns)
            chunk = Ns - pos;
        memcpy(ybuf, x + pos, chunk * sizeof(float complex));
        unsigned cl = chunk;
        cascade_run(cfg, ybuf, &cl);
        memcpy(yref + oc, ybuf, cl * sizeof(float complex));
        oc += cl;
        pos += chunk;
    }
    cascade_reset(cfg);
    memcpy(ybuf, x, Ns * sizeof(float complex));
    len = Ns;
    cascade_run(cfg, ybuf, &len);
    double maxd = 0, rr = 0;
    unsigned mlen = (len < oc) ? len : oc;
    for (unsigned i = 0; i < mlen; i++)
    {
        double d = cabs(ybuf[i] - yref[i]);
        if (d > maxd)
            maxd = d;
        rr += cabs(ybuf[i]) * cabs(ybuf[i]);
    }
    double chunk_rel = maxd / sqrt(rr / mlen);

    /* ---- 5. semantics vs brute force (cfg) ---- */
    unsigned Nb = 1u << 16;
    for (unsigned i = 0; i < Nb; i++)
        x[i] = (float)ur(&seed) * 0.5f + (float)ur(&seed) * 0.5f * I;
    static float complex bx[MAX_CHUNK];
    memcpy(bx, x, Nb * sizeof(float complex));
    for (int i = 0; i < cfg->nst; i++)
    {
        unsigned no = brute_stage(cfg->st[i]->h, cfg->st[i]->h_len,
                                  cfg->st[i]->M, bx, Nb, yref);
        memcpy(bx, yref, no * sizeof(float complex));
        Nb = no;
    }
    cascade_reset(cfg);
    len = 1u << 16;
    cascade_run(cfg, x, &len);
    double maxd2 = 0, rr2 = 0;
    unsigned ml2 = (len < Nb) ? len : Nb;
    for (unsigned i = 0; i < ml2; i++)
    {
        double d = cabs(x[i] - yref[i]);
        if (d > maxd2)
            maxd2 = d;
        rr2 += cabs(x[i]) * cabs(x[i]);
    }
    double brute_rel = maxd2 / sqrt(rr2 / ml2);

    /* ---- report ---- */
    printf("M=%u  stages=%d (", total, nst);
    for (int i = 0; i < nst; i++)
        printf("%s%u(h=%u)", i ? "," : "", factors[i], 2 * ms[i] * factors[i] + 1);
    printf(")  base=h %u taps\n", 2 * 4 * total + 1);
    printf("  cMAC/input      : base %.2f   cfg %.2f   ratio cfg/base %.3f\n",
           macs_base, macs_cfg, macs_cfg / macs_base);
    printf("  passband dev dB (v<=0.25): base %.2f cfg %.2f\n", pb_dev[0], pb_dev[1]);
    printf("  alias leak dB   : near base %.1f cfg %.1f (delta %+.1f) | far base %.1f cfg %.1f (delta %+.1f)\n",
           worst_near[0], worst_near[1], worst_near[1] - worst_near[0],
           worst_far[0], worst_far[1], worst_far[1] - worst_far[0]);
    printf("  resp parity |Hcfg/Hbase| (v<=0.45): %.2f dB\n", h_ratio_dev);
    printf("  chunk-independence rel err: %.2e   brute-force rel err: %.2e\n",
           chunk_rel, brute_rel);
    return 0;
}
