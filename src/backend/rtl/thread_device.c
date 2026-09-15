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

/* Record the chunk as CF32 at +-1.0 full scale: the slot holds the RTL
 * float convention (~+-128), so scale by 1/128. Replay via -I
 * multiplies by 128 again - the round trip is bit-exact. */
static void record_chunk(struct device_state *s, const struct iq_buffer *slot)
{
    static _Thread_local float complex rec[MAXIMUM_IQ_LENGTH];
    int i;

    for (i = 0; i < slot->len; i++)
        rec[i] = slot->samples[i] * (1.0f / 128.0f);
    if (fwrite(rec, sizeof(rec[0]), (size_t)slot->len,
               s->record_file) != (size_t)slot->len)
    {
        fprintf(stderr, "IQ recording write failed, stopping recording\n");
        fclose(s->record_file);
        s->record_file = NULL;
    }
}

/* Acquire the fill slot for chunk k = s->chunk_seq: slots[k & 1] last
 * held chunk k-2, so it may be refilled only once every demod has fully
 * consumed it (seq_processed >= k-1). The first two chunks find both
 * slots free. Abort on do_exit. */
static struct iq_buffer *acquire_fill_slot(struct device_state *s)
{
    unsigned long k = s->chunk_seq;

    if (k >= 2)
    {
        for (int i = 0; i < freq_len; i++)
        {
            struct demod_state *d = &demods[i];
            while (!do_exit && d->seq_processed < k - 1)
                usleep(50);
        }
    }

    if (do_exit)
        return NULL;

    return &s->slots[k & 1];
}

/* EOF drain: block until every demod has fully consumed all published
 * chunks (seq_processed == chunk_seq). Abort on do_exit. */
static void wait_demods_drained(struct device_state *s)
{
    for (int i = 0; i < freq_len; i++)
    {
        struct demod_state *d = &demods[i];
        while (!do_exit && d->seq_processed != s->chunk_seq)
            usleep(50);
    }
}

/* Publish a filled slot to every demod[] via fan-out (single-channel
 * mode is the N=1 case of the same loop). No copy: demods find the
 * chunk by index in slots[k & 1]. Bump chunk_seq BEFORE signalling:
 * the demod predicate is level-based on the counter, so a signal that
 * races or coalesces can never lose a chunk; the mutex handoff in the
 * signal makes the slot contents visible. */
static void deliver_buf(struct device_state *s, struct iq_buffer *slot)
{
    if (do_exit)
        return;

    if (s->record_file)
        record_chunk(s, slot);

    s->chunk_seq++;

    for (int i = 0; i < freq_len; i++)
        safe_cond_signal(&demods[i].ready, &demods[i].ready_m);
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

    struct iq_buffer *slot = acquire_fill_slot(s);
    if (!slot)
        return;

    for (i = 0; i < complex_len; i++)
    {
        float inphase = (float)((int)buf[2 * i] - 127);
        float quadrature = (float)((int)buf[2 * i + 1] - 127);
        slot->samples[i] = inphase + I * quadrature;
    }
    slot->len = complex_len;

    deliver_buf(s, slot);
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

        struct iq_buffer *slot = acquire_fill_slot(s);
        if (!slot)
            break;

        for (size_t i = 0; i < got; i++)
            slot->samples[i] = raw[i] * 128.0f;
        slot->len = (int)got;

        deliver_buf(s, slot);
    }

    fclose(f);

    /* EOF drain: every delivered chunk must be fully consumed (and its
     * audio flagged to the output) before shutdown begins */
    wait_demods_drained(s);

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
