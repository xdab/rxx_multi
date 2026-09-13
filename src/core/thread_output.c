#include "output.h"
#include "thread.h"
#include "types.h"
#include <stdio.h>

void *output_thread_fn(void *arg)
{
    struct output_state *s = arg;
    const int out_index = (int)(s - outputs);
    double write_total = 0.0;
    unsigned long write_count = 0;
    double profile_t0 = mono_ts();
    double write_total0 = 0.0;
    unsigned long write_count0 = 0;

    while (1)
    {
        double wait_t0 = mono_ts();
        pthread_mutex_lock(&s->ready_m);
        while (!s->data_ready &&
               !(do_exit && s->seq_written == s->seq_packed))
            pthread_cond_wait(&s->ready, &s->ready_m);
        if (do_exit && !s->data_ready &&
            s->seq_written == s->seq_packed)
        {
            pthread_mutex_unlock(&s->ready_m);
            break;
        }
        s->data_ready = 0;
        pthread_mutex_unlock(&s->ready_m);

        double starved_ms = (mono_ts() - wait_t0) * 1e3;
        if (starved_ms > STARVE_MS)
        {
            log_ts();
            fprintf(stderr, "[OUT] starved %.1f ms waiting for audio\n",
                    starved_ms);
        }

        double write_t0 = mono_ts();
        pthread_rwlock_rdlock(&s->rw);

        switch (s->mode)
        {
        case OUTPUT_FILE:
            if (s->file)
                fwrite(s->result, 2, s->result_len, s->file);
            break;
        case OUTPUT_TCP:
            tcp_broadcast(s, s->result, s->result_len);
            break;
        case OUTPUT_UDP:
            udp_write(s, s->result, s->result_len);
            break;
        }

        pthread_rwlock_unlock(&s->rw);
        s->seq_written++;

        write_total += (mono_ts() - write_t0);
        write_count++;

        if (mono_ts() - profile_t0 > 5.0)
        {
            unsigned long n = write_count - write_count0;
            if (n > 0)
            {
                log_ts();
                fprintf(stderr,
                        "[OUT%d] profile: %lu writes, %.3f ms/write avg (mode %d)\n",
                        out_index, n,
                        1000.0 * (write_total - write_total0) / (double)n,
                        s->mode);
            }
            profile_t0 = mono_ts();
            write_total0 = write_total;
            write_count0 = write_count;
        }

        double write_ms = (mono_ts() - write_t0) * 1e3;
        if (write_ms > STALL_MS)
        {
            log_ts();
            fprintf(stderr, "[OUT] write slow %.1f ms (mode %d, %d samples)\n",
                    write_ms, s->mode, s->result_len);
        }
    }

    return 0;
}

void output_init(struct output_state *s)
{
    s->rate = DEFAULT_SAMPLE_RATE;
    s->data_ready = 0;
    pthread_rwlock_init(&s->rw, NULL);
    pthread_cond_init(&s->ready, NULL);
    pthread_mutex_init(&s->ready_m, NULL);
}

void output_cleanup(struct output_state *s)
{
    /* Close network sockets if open */
    if (s->mode == OUTPUT_TCP)
        tcp_close(s);
    else if (s->mode == OUTPUT_UDP)
        udp_close(s);

    pthread_rwlock_destroy(&s->rw);
    pthread_cond_destroy(&s->ready);
    pthread_mutex_destroy(&s->ready_m);
}
