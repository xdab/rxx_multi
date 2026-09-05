#include "output.h"
#include "pipeline.h"
#include "thread.h"
#include "types.h"
#include <math.h>

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

    while (!do_exit)
    {
        pthread_mutex_lock(&d->ready_m);
        while (!d->data_ready && !do_exit)
            pthread_cond_wait(&d->ready, &d->ready_m);
        if (do_exit && !d->data_ready)
        {
            pthread_mutex_unlock(&d->ready_m);
            break;
        }
        d->data_ready = 0;
        pthread_mutex_unlock(&d->ready_m);

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
                continue; /* no clients, skip pipeline processing */
        }

        pthread_rwlock_wrlock(&d->rw);
        int status = pipeline_process(&d->pipeline, &d->input, &d->output);
        pthread_rwlock_unlock(&d->rw);

        if (status != 0)
        {
            do_exit = 1;
            continue;
        }

        if (pipeline_is_squelched(&d->pipeline))
        {
            d->pipeline.squelch_hits = d->pipeline.squelch_delay + 1;
            continue;
        }

        pthread_rwlock_wrlock(&o->rw);
        pack_output_samples(o, d->output.samples, d->output.len);
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
