#ifndef THREAD_H
#define THREAD_H

/**
 * @file thread.h
 * @brief Thread bodies for the three pipeline stages and init/cleanup helpers.
 */

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

/**
 * @brief Device streaming thread body; implemented once per backend.
 *
 * RTL blocks in the librtlsdr async reader; RSP starts the API stream
 * and parks while API-internal callback threads deliver samples. Either
 * way, hardware samples are converted to the shared +/-128 float
 * convention and fanned out to every channel through the chunk slots.
 *
 * @param[in,out] arg The device_state.
 *
 * @return NULL (pthread exit convention).
 */
void *device_thread_fn(void *arg);

/**
 * @brief Raw IQ file playback thread body (-I test mode, no hardware).
 *
 * Reads CF32 (+/-1.0) from device_state.input_path, scales by 128 and
 * delivers chunks through the same handoff as live samples, as fast as
 * the demod stage consumes them. Drains the demods at EOF, then sets
 * do_exit for a clean shutdown.
 *
 * @param[in,out] arg The device_state.
 *
 * @return NULL (pthread exit convention).
 */
void *file_input_thread_fn(void *arg);

/**
 * @brief Per-channel demod thread: chunk wait -> pipeline_process -> PCM16 pack.
 *
 * Waits for the next published chunk (skipping DSP while a TCP output
 * has no clients), runs the pipeline, then hands packed PCM16 to the
 * channel's output stage under the lossless handoff protocol. Sets
 * do_exit when the pipeline fails.
 *
 * @param[in,out] arg The demod_state.
 *
 * @return NULL (pthread exit convention).
 */
void *demod_thread_fn(void *arg);

/**
 * @brief Per-channel output thread: waits for packed audio and writes it out.
 *
 * Dispatches by output mode: file write, TCP broadcast or UDP send.
 * Broadcasts written after every chunk so the demod blocked on the
 * lossless handoff can drain.
 *
 * @param[in,out] arg The output_state.
 *
 * @return NULL (pthread exit convention).
 */
void *output_thread_fn(void *arg);

/* Initialization & cleanup functions */

/**
 * @brief Set backend device defaults (rates, gain controls, chunk sync).
 * @see device.h for the full lifecycle contract.
 */
void device_init_state(struct device_state *s);

/**
 * @brief Initialize a demod channel: default pipeline and sync primitives.
 */
void demod_init(struct demod_state *s);

/**
 * @brief Release the demod's pipeline objects and sync primitives.
 */
void demod_cleanup(struct demod_state *s);

/**
 * @brief Initialize an output channel: sync primitives only, mode set later.
 */
void output_init(struct output_state *s);

/**
 * @brief Close network sockets (TCP/UDP) and destroy sync primitives.
 */
void output_cleanup(struct output_state *s);

#endif /* THREAD_H */
