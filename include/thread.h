#ifndef THREAD_H
#define THREAD_H

#include "types.h"
#include <pthread.h>

/* Thread synchronization helpers */
#define safe_cond_signal(n, m)                                                                     \
    do                                                                                             \
    {                                                                                              \
        pthread_mutex_lock(m);                                                                     \
        pthread_cond_signal(n);                                                                    \
        pthread_mutex_unlock(m);                                                                   \
    } while (0)

#define safe_cond_broadcast(n, m)                                                                  \
    do                                                                                             \
    {                                                                                              \
        pthread_mutex_lock(m);                                                                     \
        pthread_cond_broadcast(n);                                                                 \
        pthread_mutex_unlock(m);                                                                   \
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
