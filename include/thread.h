#ifndef THREAD_H
#define THREAD_H

#include "types.h"
#include <pthread.h>
#include <stdio.h>
#include <time.h>

/* Pipeline stall diagnostics: monotonic timestamp, seconds since first
 * call. Print with log_ts() as a [ssss.mmm] prefix so log lines from the
 * different pipeline threads can be interleaved and correlated. */
static inline double mono_ts(void)
{
    static double epoch;
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    if (epoch == 0.0)
        epoch = (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9 - epoch;
}

static inline void log_ts(void)
{
    fprintf(stderr, "[%9.3f] ", mono_ts());
}

/* Stall warning thresholds (ms) used by the pipeline threads */
#define STALL_MS 50.0
#define STARVE_MS 200.0

/* Thread synchronization helpers */
#define safe_cond_signal(n, m)   \
    do                           \
    {                            \
        pthread_mutex_lock(m);   \
        pthread_cond_signal(n);  \
        pthread_mutex_unlock(m); \
    } while (0)

/* Thread functions */
void *device_thread_fn(void *arg);
void *file_input_thread_fn(void *arg);
void *demod_thread_fn(void *arg);
void *output_thread_fn(void *arg);

/* Initialization & cleanup functions */
void device_init_state(struct device_state *s);
void demod_init(struct demod_state *s);
void demod_cleanup(struct demod_state *s);
void output_init(struct output_state *s);
void output_cleanup(struct output_state *s);

#endif /* THREAD_H */
