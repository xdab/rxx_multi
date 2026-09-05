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

/* Deliver a converted chunk to the single demod target (direct) or
 * fan-out to all demods[] (multi-channel) */
static void deliver_buf(struct device_state *s, int complex_len)
{
    struct demod_state *d = s->demod_target;
    int i;

    if (s->record_file)
        record_chunk(s, complex_len);

    if (d)
    {
        pthread_rwlock_wrlock(&d->rw);
        memcpy(d->input.samples, s->buf, sizeof(s->buf[0]) * (size_t)complex_len);
        d->input.len = complex_len;
        pthread_rwlock_unlock(&d->rw);

        pthread_mutex_lock(&d->ready_m);
        d->data_ready = 1;
        pthread_cond_signal(&d->ready);
        pthread_mutex_unlock(&d->ready_m);
    }
    else
    {
        /* Multi-channel mode: fan-out to all demods[] */
        for (i = 0; i < freq_len; i++)
        {
            struct demod_state *cd = &demods[i];

            pthread_rwlock_wrlock(&cd->rw);
            memcpy(cd->input.samples, s->buf, sizeof(s->buf[0]) * (size_t)complex_len);
            cd->input.len = complex_len;
            pthread_rwlock_unlock(&cd->rw);

            pthread_mutex_lock(&cd->ready_m);
            cd->data_ready = 1;
            pthread_cond_signal(&cd->ready);
            pthread_mutex_unlock(&cd->ready_m);
        }
    }
}

/* RTL-SDR callback - converts raw USB samples */
static void rtlsdr_callback(unsigned char *buf, uint32_t len, void *ctx)
{
    int i;
    struct device_state *s = ctx;

    if (do_exit)
        return;

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
    s->demod_target = NULL; /* set by main after channel allocation */
}
