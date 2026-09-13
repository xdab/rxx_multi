#include "device.h"
#include "output.h"
#include "thread.h"
#include "types.h"
#include <sdrplay_api.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* Backend-private: implemented in rsp/device.c */
int device_stream_start(sdrplay_api_CallbackFnsT *cb_fns);
sdrplay_api_DeviceT *device_handle(void);

/* Recommended gain defaults: max IF gain reduction, max LNA gain */
#define DEFAULT_GRDB 59
#define DEFAULT_LNA_STATE 0

/* SDRplay stream callback - converts ZIF samples and delivers to demods.
 * Sample format: 8-bit signed, left-justified in a 16-bit word.
 *
 * The API delivers small dribbles (~1344 samples, ~1500x/s) whereas the
 * demod pipeline is paced by condvar wakeups designed for RTL-SDR's
 * ~8192-pair USB chunks (~125x/s). Accumulate into device.buf and hand
 * off rtl_multi-sized chunks to restore the reference pacing. */
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

/* Total chunks handed to demods (heartbeat + gap accounting) */
static volatile unsigned long chunks_delivered;
/* Cumulative time spent blocked waiting for demods to drain (s) */
static double drain_wait_total;
static unsigned long drain_wait_count;

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
        drain_wait_total += mono_ts() - t0;
        drain_wait_count++;
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
static void deliver_buf(int complex_len)
{
    struct device_state *s = &device;

    if (do_exit)
        return;

    wait_demods_drained();

    if (do_exit)
        return;

    chunks_delivered++;

    if (s->record_file)
        record_chunk(s, complex_len);

    for (int i = 0; i < freq_len; i++)
    {
        struct demod_state *d = &demods[i];

        pthread_rwlock_wrlock(&d->rw);
        memcpy(d->input.samples, s->buf,
               sizeof(s->buf[0]) * (size_t)complex_len);
        d->input.len = complex_len;
        d->seq_delivered++;
        pthread_rwlock_unlock(&d->rw);

        pthread_mutex_lock(&d->ready_m);
        d->data_ready = 1;
        pthread_cond_signal(&d->ready);
        pthread_mutex_unlock(&d->ready_m);
    }
}

/* Drop the first ~300 ms after Init: DC-offset calibration transient */
#define STARTUP_DROP_SAMPLES 600000

static void sdrplay_stream_cb(short *xi, short *xq,
                              sdrplay_api_StreamCbParamsT *params,
                              unsigned int numSamples, unsigned int reset,
                              void *cbContext)
{
    struct device_state *s = &device;
    static unsigned int buf_pending;
    static unsigned long dropped;
    static double last_cb_ts;
    unsigned int n = numSamples;

    (void)params;
    (void)reset;
    (void)cbContext;

    if (do_exit)
        return;

    /* Diagnose API-side starvation: how long since the previous callback */
    if (last_cb_ts > 0.0)
    {
        double gap = (mono_ts() - last_cb_ts) * 1e3;
        if (gap > STALL_MS)
        {
            log_ts();
            fprintf(stderr,
                    "[STREAM] callback gap %.1f ms (n=%u, pending=%u)\n",
                    gap, numSamples, buf_pending);
        }
    }
    last_cb_ts = mono_ts();

    if (dropped < STARTUP_DROP_SAMPLES)
    {
        unsigned int skip = STARTUP_DROP_SAMPLES - dropped;
        if (skip > n)
            skip = n;
        xi += skip;
        xq += skip;
        n -= skip;
        dropped += skip;
        if (n == 0)
            return;
    }

    while (n > 0)
    {
        unsigned int space = MAXIMUM_IQ_LENGTH - buf_pending;
        unsigned int take = (n < space) ? n : space;
        unsigned int i;

        for (i = 0; i < take; i++)
        {
            /* 8 MHz ADC with API decimation delivers right-justified
             * 16-bit samples. Keep the RTL-SDR float convention
             * (~±128 full scale) so output_scale and the DC-offset ratio
             * in the polar discriminator behave identically to rtl_multi */
            float inphase = (float)xi[i] / 256.0f;
            float quadrature = (float)xq[i] / 256.0f;
            s->buf[buf_pending + i] = inphase + I * quadrature;
        }
        buf_pending += take;
        xi += take;
        xq += take;
        n -= take;

        /* Flush when the chunk is full or enough for one demod cycle */
        if (buf_pending >= CHUNK_SAMPLES)
        {
            deliver_buf((int)buf_pending);
            buf_pending = 0;
        }
    }
}

static void sdrplay_event_cb(sdrplay_api_EventT eventId,
                             sdrplay_api_TunerSelectT tuner,
                             sdrplay_api_EventParamsT *params, void *cbContext)
{
    (void)cbContext;

    switch (eventId)
    {
    case sdrplay_api_GainChange:
        log_ts();
        fprintf(stderr, "gain: %.2f dB (gRdB %u, LNA %u)\n",
                params->gainParams.currGain, params->gainParams.gRdB,
                params->gainParams.lnaGRdB);
        break;
    case sdrplay_api_PowerOverloadChange:
        log_ts();
        fprintf(stderr, "ADC overload %s\n",
                params->powerOverloadParams.powerOverloadChangeType ==
                        sdrplay_api_Overload_Detected
                    ? "detected"
                    : "corrected");
        /* Acknowledge so the API keeps reporting */
        sdrplay_api_Update(device_handle()->dev, tuner,
                           sdrplay_api_Update_Ctrl_OverloadMsgAck,
                           sdrplay_api_Update_Ext1_None);
        break;
    case sdrplay_api_DeviceRemoved:
        fprintf(stderr, "Device removed (unplugged), exiting...\n");
        do_exit = 1;
        break;
    case sdrplay_api_DeviceFailure:
        fprintf(stderr, "Device failure reported by API, exiting...\n");
        do_exit = 1;
        break;
    default:
        break;
    }
}

/* Raw IQ file playback (-I test mode). Input is interleaved float32 I/Q
 * (CF32, e.g. an SDR++ baseband recording), +-1.0 full scale; scaled by
 * 128 to keep the RTL float convention (~+-128). Chunks are delivered
 * through the same chunking as live RSP samples, as fast as the demod
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

        deliver_buf((int)got);
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
    sdrplay_api_CallbackFnsT cb_fns;

    cb_fns.StreamACbFn = sdrplay_stream_cb;
    cb_fns.StreamBCbFn = sdrplay_stream_cb;
    cb_fns.EventCbFn = sdrplay_event_cb;

    if (device_stream_start(&cb_fns) != 0)
    {
        do_exit = 1;
        return 0;
    }

    /* Streaming happens on API-internal callback threads; park until
     * exit, printing a 1 s heartbeat of delivered chunks. Steady state
     * is ~rate/CHUNK_SAMPLES chunks per tick; lumpy deltas mean the
     * API-internal thread is delivering in bursts. */
    unsigned long last_heartbeat_count = 0;
    while (!do_exit)
    {
        usleep(1000000);
        if (do_exit)
            break;
        unsigned long count = chunks_delivered;
        unsigned long waits = drain_wait_count;
        log_ts();
        fprintf(stderr,
                "[DEVICE] +%lu chunks (%.1f MS/s equivalent), drain wait %.2f ms/chunk avg\n",
                count - last_heartbeat_count,
                (double)(count - last_heartbeat_count) * CHUNK_SAMPLES / 1e6,
                waits ? 1000.0 * drain_wait_total / (double)waits : 0.0);
        last_heartbeat_count = count;
    }

    (void)s;
    return 0;
}

void device_init_state(struct device_state *s)
{
    memset(s, 0, sizeof(*s));
    /* -1 = gain controls unset (API AGC) */
    s->gain_rdb = -1;
    s->lna_state = -1;
    s->agc = 1;
}
