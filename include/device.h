#ifndef DEVICE_H
#define DEVICE_H

#include "options.h"
#include "types.h"

/* Backend-selected capture plan: hardware capture rate + integer
 * decimation factor towards the requested channel rate */
struct capture_plan
{
    uint32_t rate;
    int downsample;
};

/* Lifecycle and configuration; implemented once per backend in
 * src/backend/<name>/device.c */

void device_init_state(struct device_state *s);

/* Backend-specific option handling: parse device flags (-g, -L, -p,
 * -T, non-universal -E values) into the device state. Returns 0 if
 * handled, -1 on error, 1 if not recognized. */
int device_parse_option(int opt, const char *optarg, options_t *opts);

/* Late device-option decisions that need the full option set
 * (e.g. gain vs IQ-file-input conflicts). Prints warnings. */
void device_apply_options(options_t *opts, int file_input);

/* Turn the requested capture width into rate + downsample for this
 * backend (rate snapping, range checks). Returns -1 on error. */
int device_plan_capture(int file_input, uint32_t rate_in, uint64_t width,
                        struct capture_plan *plan);

/* Device selection (index or serial substring), returns index or -1 */
int verbose_device_search(char *s);

/* Configuration helpers - call between device_open() and
 * device_apply_settings() */
int verbose_set_frequency(uint32_t frequency);
int verbose_set_sample_rate(uint32_t samp_rate);
int verbose_reset_buffer(void);

/* Open the hardware and apply one-time device setup */
int device_open(int dev_index);

/* Apply queued settings; hardware starts streaming on device_thread_fn */
int device_apply_settings(void);

/* Ask the device thread to stop promptly (signal handler context) */
void device_signal_exit(void);

void device_stream_stop(void);
void device_close(void);

#endif /* DEVICE_H */
