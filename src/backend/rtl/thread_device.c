#include "output.h"
#include "thread.h"
#include "types.h"
#include <rtl-sdr.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* RTL-SDR delivers ~8192-pair USB chunks (~125x/s at 2M), matching the
 * condvar pacing the demod pipeline expects. */
#define CHUNK_SAMPLES (DEFAULT_BUF_LENGTH / 2)

/* Record the chunk as CF32 at +-1.0 full scale: buf holds the RTL float
 * convention (~+-128), so scale by 1/128. Replay via -I multiplies by
 * 128 again - the round trip is bit-exact. */
static void record_chunk(struct device_state *s, int complex_len)
{
    static _Thread_local float complex rec[MAXIMUM_IQ_LENGTH];
    int i;

    for (i = 0; i < complex_len; i++)
        rec[i] = s->buf[i] * (1.0f / 128.0f);
    if (fwrite(rec, sizeof(rec[0]), (size_t)complex_len,
               s->record_file) != (size_t)complex_len)
    {
        fprintf(stderr, "IQ recording write failed, stopping recording\n");
        fclose(s->record_file);
        s->record_file = NULL;
    }
}

/* Lossless handoff: block until every demod has fully consumed the
 * previous chunk (seq_processed == seq_delivered) before this chunk
 * overwrites s->buf / d->input. Abort on do_exit. */
static void wait_demods_drained(void)
{
    for (int i = 0; i < freq_len; i++)
    {
        struct demod_state *d = &demods[i];
        if (d->seq_processed == d->seq_delivered)
            continue;
        double t0 = mono_ts();
        while (!do_exit && d->seq_processed != d->seq_delivered)
            usleep(50);
        double ms = (mono_ts() - t0) * 1e3;
        if (!do_exit && ms > STALL_MS)
        {
            log_ts();
            fprintf(stderr,
                    "[DEVICE] producer stalled %.1f ms waiting for demod[%d] drain\n",
                    ms, i);
        }
    }
}

/* Deliver a converted chunk to every demod[] via fan-out
 * (single-channel mode is the N=1 case of the same loop) */
static void deliver_buf(struct device_state *s, int complex_len)
{
    if (do_exit)
        return;

    wait_demods_drained();

    if (do_exit)
        return;

    if (s->record_file)
        record_chunk(s, complex_len);

    for (int i = 0; i < freq_len; i++)
    {
        struct demod_state *d = &demods[i];

        pthread_rwlock_wrlock(&d->rw);
        memcpy(d->input.samples, s->buf, sizeof(s->buf[0]) * (size_t)complex_len);
        d->input.len = complex_len;
        d->seq_delivered++;
        pthread_rwlock_unlock(&d->rw);

        pthread_mutex_lock(&d->ready_m);
        d->data_ready = 1;
        pthread_cond_signal(&d->ready);
        pthread_mutex_unlock(&d->ready_m);
    }
}

/* RTL-SDR callback - converts raw USB samples */
static void rtlsdr_callback(unsigned char *buf, uint32_t len, void *ctx)
{
    static double last_cb_ts;
    int i;
    struct device_state *s = ctx;

    if (do_exit)
        return;

    /* Diagnose USB-side starvation: how long since the previous callback */
    if (last_cb_ts > 0.0)
    {
        double gap = (mono_ts() - last_cb_ts) * 1e3;
        if (gap > STALL_MS)
        {
            log_ts();
            fprintf(stderr, "[STREAM] callback gap %.1f ms (len=%u)\n",
                    gap, (unsigned)len);
        }
    }
    last_cb_ts = mono_ts();

    if (!ctx)
        return;

    if (s->mute)
    {
        for (i = 0; i < s->mute; i++)
            buf[i] = 127;
        s->mute = 0;
    }

    int complex_len = (int)(len / 2);
    if (complex_len > MAXIMUM_IQ_LENGTH)
        complex_len = MAXIMUM_IQ_LENGTH;

    for (i = 0; i < complex_len; i++)
    {
        float inphase = (float)((int)buf[2 * i] - 127);
        float quadrature = (float)((int)buf[2 * i + 1] - 127);
        s->buf[i] = inphase + I * quadrature;
    }

    deliver_buf(s, complex_len);
}

/* Raw IQ file playback (-I test mode). Input is interleaved float32 I/Q
 * (CF32, e.g. an SDR++ baseband recording), +-1.0 full scale; scaled by
 * 128 to keep the RTL float convention (~+-128). Chunks are delivered
 * through the same chunking as live RTL samples, as fast as the demod
 * stage consumes them. EOF terminates the run cleanly. */
void *file_input_thread_fn(void *arg)
{
    struct device_state *s = arg;
    static _Thread_local float complex raw[CHUNK_SAMPLES];
    FILE *f;

    f = fopen(s->input_path, "rb");
    if (!f)
    {
        fprintf(stderr, "Failed to open IQ input file: %s\n", s->input_path);
        do_exit = 1;
        return 0;
    }

    while (!do_exit)
    {
        size_t got = fread(raw, sizeof(raw[0]), CHUNK_SAMPLES, f);
        if (got == 0)
        {
            if (ferror(f))
                fprintf(stderr, "IQ input read error, exiting...\n");
            break;
        }

        for (size_t i = 0; i < got; i++)
            s->buf[i] = raw[i] * 128.0f;

        deliver_buf(s, (int)got);
    }

    fclose(f);

    /* EOF drain: every delivered chunk must be fully consumed (and its
     * audio flagged to the output) before shutdown begins */
    wait_demods_drained();

    fprintf(stderr, "IQ input file exhausted, exiting...\n");
    do_exit = 1;
    return 0;
}

void *device_thread_fn(void *arg)
{
    struct device_state *s = arg;

    int r = rtlsdr_read_async(s->dev, rtlsdr_callback, s, 0, s->buf_len);
    if (r != 0 && !do_exit)
    {
        fprintf(stderr, "\nDevice error detected, async read returned: %d\n", r);
        do_exit = 1;
    }

    return 0;
}

void device_init_state(struct device_state *s)
{
    s->rate = DEFAULT_SAMPLE_RATE;
    s->buf_len = DEFAULT_BUF_LENGTH;
    s->gain = AUTO_GAIN; // tenths of a dB
    s->mute = 0;
    s->direct_sampling = 0;
    s->ppm_error = 0;
    s->biastee = 0;
}
