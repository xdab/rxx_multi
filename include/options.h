#ifndef OPTIONS_H
#define OPTIONS_H

#include "types.h"
#include <stdint.h>

enum demod_mode
{
    DEMOD_FM,
    DEMOD_RAW,
    DEMOD_AM,
    DEMOD_USB,
    DEMOD_LSB
};

/* Channel-specific options */
typedef struct
{
    uint32_t freq;
    enum demod_mode mode;
    output_mode_t output_mode;
    char filename[STATIC_STRING_SIZE];
    int tcp_port;
    char udp_host[STATIC_STRING_SIZE];
    int udp_port;
} channel_t;

/* Root options - global settings + channel array */
typedef struct
{
    int dev_given;
    int dev_index;

    /* Global settings (apply to all channels) */
    float rate_in;
    float rate_out;
    float rate_audio;

    int dc_block;
    int deemph;

    /* Raw IQ file input test mode (-I FILE:RATE:CENTER) */
    char input_file[STATIC_STRING_SIZE];
    float input_rate;   /* capture rate of the recording [Hz] */
    float input_center; /* center frequency of the recording [Hz] */

    /* Raw IQ recording (-R FILE, 1:1 with -I input) */
    char record_file[STATIC_STRING_SIZE];

    /* Graceful shutdown after this many seconds of running (-x); 0 = off */
    float run_timeout;

    /* Channels (used even for single-channel mode) */
    int channel_count;
    channel_t channels[FREQUENCIES_LIMIT];
} options_t;

int options_parse(int argc, char **argv, options_t *opts);
void options_usage(void);
void options_print_channels(const options_t *opts);

/* Backend-specific option handling; implemented once per backend.
 * device_parse_option() consumes device-specific flags (gain, bias-T,
 * ppm, ...). Returns 0 if handled, -1 on error, 1 if not recognized. */
int device_parse_option(int opt, const char *optarg, options_t *opts);
void device_print_options(void);

double atofs(char *s);
double atoft(char *s);
double atofp(char *s);

#endif /* OPTIONS_H */
