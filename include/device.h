#ifndef DEVICE_H
#define DEVICE_H

/**
 * @file device.h
 * @brief Backend interface: the hardware contract implemented once per backend.
 *
 * Core never touches hardware APIs; everything device-specific lives
 * behind these functions (see AGENTS.md for the backend table).
 */

#include "options.h"
#include "types.h"

/**
 * @struct capture_plan
 * @brief Backend-selected capture plan.
 *
 * Hardware capture rate plus integer decimation factor towards the
 * requested channel rate.
 */
struct capture_plan
{
    uint32_t rate;  /**< Hardware capture rate after backend snapping [Hz]. */
    int downsample; /**< Integer decimation from rate to the channel rate. */
};

/* Lifecycle and configuration; implemented once per backend in
 * src/backend/<name>/device.c */

/**
 * @brief Set backend defaults (rates, gain controls, chunk sync primitives).
 *
 * @param[out] s Device state to initialize.
 */
void device_init_state(struct device_state *s);

/**
 * @brief Consume backend-specific flags (-g, -L, -p, -T, non-universal -E).
 *
 * @param[in] opt Option character.
 * @param[in] optarg Option argument string.
 * @param[in,out] opts Root options, for context.
 *
 * @retval 0 Option handled by this backend.
 * @retval -1 Option recognized but invalid; error already printed.
 * @retval 1 Option not recognized (caller treats it as unknown).
 */
int device_parse_option(int opt, const char *optarg, options_t *opts);

/**
 * @brief Late device-option decisions that need the full option set.
 *
 * E.g. gain vs IQ-file-input conflicts; prints warnings, never fails.
 *
 * @param[in] opts Parsed root options.
 * @param[in] file_input Non-zero when -I file playback replaces hardware.
 */
void device_apply_options(options_t *opts, int file_input);

/**
 * @brief Turn the requested capture width into rate + downsample.
 *
 * Backend-specific rate snapping and range checks. In -I file mode the
 * plan is computed from the recording and hardware checks are skipped.
 *
 * @param[in] file_input Non-zero in -I file mode.
 * @param[in] rate_in Requested channel rate [Hz].
 * @param[in] width Channel span + guards [Hz].
 * @param[out] plan Resulting capture plan on success.
 *
 * @retval 0 Success.
 * @retval -1 Request cannot be served; error already printed.
 */
int device_plan_capture(
    int file_input, uint32_t rate_in, uint64_t width, struct capture_plan *plan);

/**
 * @brief Device selection by index or serial substring.
 *
 * @param[in] s Index or serial substring; NULL/empty selects the first device.
 *
 * @return Selected device index, -1 if none found.
 */
int verbose_device_search(char *s);

/* Configuration helpers - call between device_open() and
 * device_apply_settings() */

/**
 * @brief Tune the device center frequency.
 *
 * @param[in] frequency Center frequency [Hz].
 *
 * @return 0 on success, -1 on failure.
 */
int verbose_set_frequency(uint32_t frequency);

/**
 * @brief Set the device capture rate.
 *
 * @param[in] samp_rate Capture rate [Hz].
 *
 * @return 0 on success, -1 on failure.
 */
int verbose_set_sample_rate(uint32_t samp_rate);

/**
 * @brief Flush/stale-clear the device buffer before streaming.
 *
 * @return 0 on success, -1 on failure.
 */
int verbose_reset_buffer(void);

/**
 * @brief Open the hardware and apply one-time device setup.
 *
 * @param[in] dev_index Device index from verbose_device_search().
 *
 * @return 0 on success, -1 on failure.
 */
int device_open(int dev_index);

/**
 * @brief Apply queued settings; hardware starts streaming on device_thread_fn.
 *
 * @return 0 on success, -1 on failure.
 */
int device_apply_settings(void);

/**
 * @brief Ask the device thread to stop promptly (signal handler context).
 *
 * @note Must only touch async-signal-safe state (do_exit / backend flags).
 */
void device_signal_exit(void);

/**
 * @brief Stop streaming gracefully (post-run teardown, before device_close).
 */
void device_stream_stop(void);

/**
 * @brief Release the device handle.
 */
void device_close(void);

#endif /* DEVICE_H */
