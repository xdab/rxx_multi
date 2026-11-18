/**
 * @file args_mapper.h
 * @brief UI-friendly launch options and their mapping to a backend command line.
 *
 * The GUI keeps all of its state in a #gui_launch_options_t; the pure
 * function gui_options_build_command_line() turns that into the exact
 * command line for bin/rtl_multi.exe or bin/rsp_multi.exe. This module has
 * no Win32 dependencies, so the mapping is unit-testable on any platform
 * (see args_mapper_test.c).
 */
#ifndef ARGS_MAPPER_H
#define ARGS_MAPPER_H

#include <stddef.h>

#ifdef __cplusplus
extern "C"
{
#endif

/** @brief Maximum number of channels (mirrors FREQUENCIES_LIMIT in types.h). */
#define GUI_MAX_CHANNELS 128

    /** @brief Selected backend binary. */
    typedef enum
    {
        GUI_BACKEND_RTL = 0, /**< librtlsdr backend (rtl_multi.exe) */
        GUI_BACKEND_RSP = 1  /**< SDRplay RSP backend (rsp_multi.exe) */
    } gui_backend_t;

    /** @brief Per-channel demodulation mode; passes straight through to -M. */
    typedef enum
    {
        GUI_MODE_FM = 0,
        GUI_MODE_WBFM = 1, /**< @note also sets global rates + de-emphasis in the CLI */
        GUI_MODE_AM = 2,
        GUI_MODE_USB = 3,
        GUI_MODE_LSB = 4,
        GUI_MODE_RAW = 5
    } gui_mode_t;

    /** @brief Per-channel output kind. */
    typedef enum
    {
        GUI_OUT_TCP = 0, /**< tcp:PORT */
        GUI_OUT_UDP = 1, /**< udp:HOST:PORT */
        GUI_OUT_FILE = 2 /**< bare filename (commas rejected) */
    } gui_output_type_t;

    /** @brief One channel: frequency + mode + output, positional in the CLI lists. */
    typedef struct
    {
        char freq[32];            /**< -f entry, k/M/G suffixes allowed ("" invalid) */
        gui_mode_t mode;          /**< -M entry */
        gui_output_type_t output; /**< -O kind */
        char tcp_port[8];         /**< GUI_OUT_TCP: 1-65535 */
        char udp_host[256];       /**< GUI_OUT_UDP: host/IP */
        char udp_port[8];         /**< GUI_OUT_UDP: 1-65535 */
        char file_name[260];      /**< GUI_OUT_FILE: path, may not contain commas */
    } gui_channel_t;

    /** @brief Everything the GUI needs to launch one backend invocation. */
    typedef struct
    {
        gui_backend_t backend; /**< which binary to run */
        char device[64];       /**< -d index or serial, "" = default device */
        char rate_in[32];      /**< -s capture rate, "" = backend default */
        char rate_out[32];     /**< -r audio rate, "" = backend default */
        int dc_block;          /**< emit -E dc */
        int deemph;            /**< emit -E deemp */

        /* RTL-only (ignored when backend != GUI_BACKEND_RTL) */
        char rtl_gain[32]; /**< -g gain in dB, "" = API AGC */
        char rtl_ppm[16];  /**< -p ppm error, "" = none */
        int rtl_biastee;   /**< emit -T */
        int rtl_direct;    /**< 0 = off, 1 = -E direct, 2 = -E direct2 */

        /* RSP-only (ignored when backend != GUI_BACKEND_RSP) */
        char rsp_grdb[16]; /**< -g gRdB 0-59, "" = API AGC */
        char rsp_lna[16];  /**< -L LNA state 0-2, "" = API default */

        /* Input source: live device, or -I file replay */
        int use_file_input;   /**< 1 = emit -I (device flags still passed) */
        char file_path[260];  /**< -I path (drive letters OK) */
        char file_rate[32];   /**< -I capture rate */
        char file_center[32]; /**< -I center frequency */

        char record_path[260]; /**< -R CF32 recording path, "" = off */
        char timeout[16];      /**< -x seconds, "" = run forever */

        gui_channel_t channels[GUI_MAX_CHANNELS]; /**< -f/-M/-O lists, positional */
        int channel_count;                        /**< 1..GUI_MAX_CHANNELS */
    } gui_launch_options_t;

    /**
 * @brief Build the full command line for the configured backend binary.
 *
 * Emits a canonical flag order (exe, -d, -s, -r, -E ..., backend gains,
 * -I, -R, -x, -f, -M, -O) with arguments quoted only when needed. Global
 * options and channels are validated first; on any problem nothing is
 * returned and @p err explains why.
 *
 * @param[in] opts UI options to map
 * @param[out] cmd buffer receiving the quoted executable + arguments
 * @param[in] cmd_size size of @p cmd in bytes
 * @param[out] err buffer receiving a human-readable message on failure
 * @param[in] err_size size of @p err in bytes
 * @retval 0 success, @p cmd holds the command line
 * @retval -1 validation failed, @p err holds the reason
 */
    int gui_options_build_command_line(
        const gui_launch_options_t *opts, char *cmd, size_t cmd_size, char *err, size_t err_size);

#ifdef __cplusplus
}
#endif

#endif /* ARGS_MAPPER_H */
