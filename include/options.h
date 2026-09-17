#ifndef OPTIONS_H
#define OPTIONS_H

/**
 * @file options.h
 * @brief CLI parsing types and helpers, plus the backend option hooks.
 */

#include "types.h"
#include <stdint.h>

/**
 * @enum demod_mode
 * @brief Modulation selector set by -M.
 */
enum demod_mode
{
    DEMOD_FM,  /**< FM (also covers wbfm via rate/de-emphasis defaults). */
    DEMOD_RAW, /**< Raw IQ passthrough. */
    DEMOD_AM,  /**< AM envelope detection. */
    DEMOD_USB, /**< Upper sideband. */
    DEMOD_LSB  /**< Lower sideband. */
};

/**
 * @struct channel_t
 * @brief Per-channel options parsed from one comma-separated -f entry + -O.
 */
typedef struct
{
    uint32_t freq;                     /**< Channel center frequency [Hz]. */
    enum demod_mode mode;              /**< Modulation (-M, global default). */
    output_mode_t output_mode;         /**< File, TCP or UDP sink. */
    char filename[STATIC_STRING_SIZE]; /**< Output file path (OUTPUT_FILE). */
    int tcp_port;                      /**< Listen port (OUTPUT_TCP). */
    char udp_host[STATIC_STRING_SIZE]; /**< Destination host (OUTPUT_UDP). */
    int udp_port;                      /**< Destination port (OUTPUT_UDP). */
} channel_t;

/**
 * @struct options_t
 * @brief Root options: global settings plus the per-channel array.
 */
typedef struct
{
    int dev_given; /**< Non-zero when -d selected a device. */
    int dev_index; /**< Device index or serial substring from -d. */

    /* Global settings (apply to all channels) */
    float rate_in;    /**< Channel rate (-s) [Hz]. */
    float rate_out;   /**< Mirrors rate_in (-s); CLI parity, see AGENTS.md [Hz]. */
    float rate_audio; /**< Audio output rate (-r) [Hz]. */

    int dc_block; /**< -E dc: DC blocker on. */
    int deemph;   /**< -E deemp: 75 us de-emphasis on. */

    /* Raw IQ file input test mode (-I FILE:RATE:CENTER) */
    char input_file[STATIC_STRING_SIZE]; /**< Recording path. */
    float input_rate;                    /* capture rate of the recording [Hz] */
    float input_center;                  /* center frequency of the recording [Hz] */

    /* Raw IQ recording (-R FILE, 1:1 with -I input) */
    char record_file[STATIC_STRING_SIZE]; /**< Baseband recording path. */

    /* Graceful shutdown after this many seconds of running (-x); 0 = off */
    float run_timeout;

    /* Channels; single-channel operation is the N=1 case */
    int channel_count;                     /**< Occupied entries in channels[]. */
    channel_t channels[FREQUENCIES_LIMIT]; /**< One per -f list entry. */
} options_t;

/**
 * @brief Parse argv into opts; on option errors prints ERROR and returns -1.
 *
 * Unknown options fall through to the backend hook, then trigger
 * usage + exit like getopt would.
 *
 * @param[in] argc Argument count.
 * @param[in] argv Argument vector.
 * @param[out] opts Parsed options.
 *
 * @retval 0 Success.
 * @retval -1 Parse error; message already printed.
 */
int options_parse(int argc, char **argv, options_t *opts);

/**
 * @brief Print the usage/help block (universal + backend flags) to stderr.
 *
 * @note Never returns; exits with status 1.
 */
void options_usage(void);

/**
 * @brief Print the per-channel summary shown at startup in multi-channel mode.
 *
 * @param[in] opts Parsed options.
 */
void options_print_channels(const options_t *opts);

/* Backend-specific option handling; implemented once per backend. */

/**
 * @brief Backend option hook; consumes device flags (-g, -L, -p, ...).
 *
 * @see device_parse_option in device.h for the full contract.
 */
int device_parse_option(int opt, const char *optarg, options_t *opts);

/**
 * @brief Backend option hook; appends the backend help block in options_usage().
 */
void device_print_options(void);

/**
 * @brief String to number with binary magnitude suffixes (k/M/G, case-insensitive).
 *
 * @param[in,out] s Number string, optionally suffixed; temporarily modified.
 *
 * @return Parsed value; plain strings pass through atof().
 */
double atofs(char *s);

/**
 * @brief String to seconds with time suffixes (s/m/h, case-insensitive).
 *
 * @param[in,out] s Duration string, optionally suffixed; temporarily modified.
 *
 * @return Duration in seconds.
 */
double atoft(char *s);

/**
 * @brief String to ratio with a percent suffix ('%' -> fraction of 100).
 *
 * @param[in,out] s Number string, optionally suffixed; temporarily modified.
 *
 * @return Parsed value (0.01 per percent point).
 */
double atofp(char *s);

#endif /* OPTIONS_H */
