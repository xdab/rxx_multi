#include "output.h"
#include "pipeline.h"
#include "thread.h"
#include "types.h"
#include <math.h>
#include <unistd.h>

static int16_t float_to_pcm16(float sample)
{
    if (sample != sample)
        return 0;

    if (sample > 32767.0f)
        sample = 32767.0f;
    else if (sample < -32768.0f)
        sample = -32768.0f;

    return (int16_t)lrintf(sample);
}

static void pack_output_samples(struct output_state *out, const float *samples, int count)
{
    int copy_len = count;
    if (copy_len > MAXIMUM_BUF_LENGTH)
        copy_len = MAXIMUM_BUF_LENGTH;

    for (int i = 0; i < copy_len; i++)
        out->result[i] = float_to_pcm16(samples[i]);

    out->result_len = copy_len;
}

void *demod_thread_fn(void *arg)
{
    struct demod_state *d = arg;
    struct output_state *o = d->output_target;
    const int demod_index = (int)(d - demods);
    double profile_t0 = mono_ts();
    unsigned long profile_chunks0 = 0;
    double t_shift0 = 0.0, t_decim0 = 0.0, t_demod0 = 0.0;
    double t_iir0 = 0.0, t_resamp0 = 0.0;
    double back_wait_total = 0.0;
    unsigned long back_wait_count = 0;

    while (1)
    {
        double wait_t0 = mono_ts();
        pthread_mutex_lock(&d->ready_m);
        while (!d->data_ready &&
               !(do_exit && d->seq_processed == d->seq_delivered))
            pthread_cond_wait(&d->ready, &d->ready_m);
        if (do_exit && !d->data_ready &&
            d->seq_processed == d->seq_delivered)
        {
            pthread_mutex_unlock(&d->ready_m);
            break;
        }
        d->data_ready = 0;
        pthread_mutex_unlock(&d->ready_m);

        double starved_ms = (mono_ts() - wait_t0) * 1e3;
        if (starved_ms > STARVE_MS)
        {
            log_ts();
            fprintf(stderr, "[DEMOD] starved %.1f ms waiting for input\n",
                    starved_ms);
        }

        /* Fast-path: if output is TCP and there are no connected clients,
         * accept any pending connections then skip expensive demod processing
         * to save CPU until a client connects. */
        if (o && o->mode == OUTPUT_TCP)
        {
            /* Accept pending connections so new clients can connect while
             * demodulation is skipped. tcp_accept_clients is non-blocking. */
            tcp_accept_clients(o);

            pthread_mutex_lock(&o->net.tcp.clients_m);
            int clients = o->net.tcp.client_count;
            pthread_mutex_unlock(&o->net.tcp.clients_m);

            if (clients == 0)
            {
                /* Chunk consumed and discarded - unblock the producer */
                d->seq_processed++;
                continue;
            }
        }

        double proc_t0 = mono_ts();
        pthread_rwlock_wrlock(&d->rw);
        int status = pipeline_process(&d->pipeline, &d->input, &d->output);
        pthread_rwlock_unlock(&d->rw);

        double proc_ms = (mono_ts() - proc_t0) * 1e3;
        if (proc_ms > STALL_MS)
        {
            log_ts();
            fprintf(stderr, "[DEMOD] pipeline slow %.1f ms\n", proc_ms);
        }

        /* Input fully consumed - the producer may reuse it now */
        d->seq_processed++;

        /* Periodic per-stage CPU profile for this channel (every 5 s):
         * average ms per chunk spent in each pipeline stage */
        double now = mono_ts();
        if (now - profile_t0 > 5.0)
        {
            unsigned long n = d->pipeline.chunks_processed;
            unsigned long done = n - profile_chunks0;
            if (done > 0)
            {
                log_ts();
                fprintf(stderr,
                        "[DEMOD%d] profile: %lu chunks (%.2f ms/chunk total) | "
                        "shift %.3f decim %.3f demod %.3f iir %.3f resamp %.3f"
                        " backpres %.3f"
                        " | in %d Hz M=%d demod %d Hz out %d Hz\n",
                        demod_index, done,
                        1000.0 * (now - profile_t0) / (double)done,
                        1000.0 * (d->pipeline.t_shift - t_shift0) / (double)done,
                        1000.0 * (d->pipeline.t_decim - t_decim0) / (double)done,
                        1000.0 * (d->pipeline.t_demod - t_demod0) / (double)done,
                        1000.0 * (d->pipeline.t_iir - t_iir0) / (double)done,
                        1000.0 * (d->pipeline.t_resamp - t_resamp0) / (double)done,
                        back_wait_count
                            ? 1000.0 * back_wait_total / (double)back_wait_count
                            : 0.0,
                        d->pipeline.input_rate, d->pipeline.downsample_factor,
                        d->pipeline.demod_rate, d->pipeline.output_rate);
            }
            profile_t0 = now;
            profile_chunks0 = n;
            t_shift0 = d->pipeline.t_shift;
            t_decim0 = d->pipeline.t_decim;
            t_demod0 = d->pipeline.t_demod;
            t_iir0 = d->pipeline.t_iir;
            t_resamp0 = d->pipeline.t_resamp;
        }

        if (status != 0)
        {
            do_exit = 1;
            continue;
        }

        /* Lossless handoff: wait until the output stage has written
         * everything packed so far before overwriting o->result */
        double back_t0 = mono_ts();
        while (!do_exit && o->seq_written != o->seq_packed)
            usleep(100);
        back_wait_total += mono_ts() - back_t0;
        back_wait_count++;
        double back_ms = (mono_ts() - back_t0) * 1e3;
        if (back_ms > STALL_MS)
        {
            log_ts();
            fprintf(stderr,
                    "[DEMOD] output backpressure %.1f ms (seq_written %lu, seq_packed %lu)\n",
                    back_ms, o->seq_written, o->seq_packed);
        }

        pthread_rwlock_wrlock(&o->rw);
        pack_output_samples(o, d->output.samples, d->output.len);
        o->seq_packed++;
        pthread_rwlock_unlock(&o->rw);

        pthread_mutex_lock(&o->ready_m);
        o->data_ready = 1;
        pthread_cond_signal(&o->ready);
        pthread_mutex_unlock(&o->ready_m);
    }

    return 0;
}

void demod_init(struct demod_state *s)
{
    s->input.len = 0;
    s->output.len = 0;
    s->data_ready = 0;
    pipeline_init(&s->pipeline);
    pthread_rwlock_init(&s->rw, NULL);
    pthread_cond_init(&s->ready, NULL);
    pthread_mutex_init(&s->ready_m, NULL);
    s->output_target = NULL;
}

void demod_cleanup(struct demod_state *s)
{
    pipeline_cleanup(&s->pipeline);
    pthread_rwlock_destroy(&s->rw);
    pthread_cond_destroy(&s->ready);
    pthread_mutex_destroy(&s->ready_m);
}
