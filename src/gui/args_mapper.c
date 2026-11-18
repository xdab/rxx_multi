/**
 * @file args_mapper.c
 * @brief Pure mapping from #gui_launch_options_t to a backend command line.
 *
 * No Win32 here: the whole file is plain C so the mapping can be unit-tested
 * on any platform (args_mapper_test.c) and later reused by any front end.
 */
#include "args_mapper.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/** @brief Longest joined -O list: 128 channels x (host 256 + port + commas). */
#define OUTPUT_LIST_MAX (GUI_MAX_CHANNELS * 280)
/** @brief Longest joined -f list. */
#define FREQ_LIST_MAX (GUI_MAX_CHANNELS * 34)
/** @brief Longest joined -M list. */
#define MODE_LIST_MAX (GUI_MAX_CHANNELS * 8)
/** @brief Longest single joined -I argument. */
#define FILE_INPUT_MAX (260 + 32 + 32 + 4)

/** @brief Record a validation failure message and fail the build. */
static int fail(char *err, size_t err_size, const char *msg)
{
    snprintf(err, err_size, "%s", msg);
    return -1;
}

/** @brief Formatted variant of fail(). */
static int failf(char *err, size_t err_size, const char *what, int index, const char *why)
{
    snprintf(err, err_size, "Channel %d %s: %s", index + 1, what, why);
    return -1;
}

/** @brief Append raw text to cmd; -1 once the buffer is exhausted. */
static int append(char *cmd, size_t size, const char *text)
{
    size_t len = strlen(cmd);
    size_t add = strlen(text);
    if (len + add + 1 > size)
        return -1;
    memcpy(cmd + len, text, add + 1);
    return 0;
}

/** @brief True when the argument must be double-quoted for the child's argv. */
static int needs_quotes(const char *s) { return s[0] == '\0' || strpbrk(s, " \t\"") != NULL; }

/** @brief Append a value (no leading space), quoting when needed. */
static int append_value(char *cmd, size_t size, const char *s)
{
    if (!needs_quotes(s))
        return append(cmd, size, s);
    if (append(cmd, size, "\""))
        return -1;
    for (const char *p = s; *p; p++)
    {
        char one[2] = {*p, '\0'};
        if (*p == '"' && append(cmd, size, "\""))
            return -1;
        if (append(cmd, size, one))
            return -1;
    }
    return append(cmd, size, "\"");
}

/** @brief Append " value" with quoting when needed. */
static int append_arg(char *cmd, size_t size, const char *s)
{
    return append(cmd, size, " ") || append_value(cmd, size, s);
}

/** @brief Append " -X value" (or bare " -X" when value is NULL). */
static int append_flag(char *cmd, size_t size, const char *flag, const char *value)
{
    if (append(cmd, size, " ") || append(cmd, size, flag))
        return -1;
    return value ? append_arg(cmd, size, value) : 0;
}

/** @brief Reject values that would corrupt argv (embedded double quotes). */
static int clean_str(const char *s) { return strchr(s, '"') == NULL; }

/** @brief Non-empty, quote-free string. */
static int req_str(const char *s) { return s[0] != '\0' && clean_str(s); }

/** @brief Strict decimal port 1-65535. */
static int valid_port(const char *s)
{
    if (!s[0])
        return 0;
    for (const char *p = s; *p; p++)
        if (*p < '0' || *p > '9')
            return 0;
    long v = atol(s);
    return v >= 1 && v <= 65535;
}

/** @brief Render one channel's -O value into buf. Returns 0 or -1 (err set). */
static int channel_output_arg(
    const gui_channel_t *ch, int index, char *buf, size_t size, char *err, size_t err_size)
{
    switch (ch->output)
    {
    case GUI_OUT_TCP:
        if (!valid_port(ch->tcp_port))
            return failf(err, err_size, "TCP port", index, "must be a number 1-65535");
        snprintf(buf, size, "tcp:%s", ch->tcp_port);
        return 0;
    case GUI_OUT_UDP:
        if (!req_str(ch->udp_host))
            return failf(err, err_size, "UDP host", index, "must not be empty");
        if (!valid_port(ch->udp_port))
            return failf(err, err_size, "UDP port", index, "must be a number 1-65535");
        snprintf(buf, size, "udp:%s:%s", ch->udp_host, ch->udp_port);
        return 0;
    case GUI_OUT_FILE:
        /* -O is split on commas before parsing, so filenames can never
         * carry them, even in single-channel mode */
        if (!req_str(ch->file_name))
            return failf(err, err_size, "output file", index, "must not be empty");
        if (strchr(ch->file_name, ','))
            return failf(err, err_size, "output file", index, "may not contain commas");
        snprintf(buf, size, "%s", ch->file_name);
        return 0;
    }
    return failf(err, err_size, "output", index, "unknown output type");
}

int gui_options_build_command_line(
    const gui_launch_options_t *opts, char *cmd, size_t cmd_size, char *err, size_t err_size)
{
    cmd[0] = '\0';
    err[0] = '\0';

    if (opts->channel_count < 1)
        return fail(err, err_size, "At least one channel is required");
    if (opts->channel_count > GUI_MAX_CHANNELS)
        return fail(err, err_size, "Too many channels (max 128)");

    const char *exe = opts->backend == GUI_BACKEND_RSP ? "rsp_multi.exe" : "rtl_multi.exe";
    if (append_value(cmd, cmd_size, exe))
        goto overflow;

    /* Validation pass first: no partial command lines on error */
    static _Thread_local char obuf[OUTPUT_LIST_MAX];
    static _Thread_local char fbuf[FREQ_LIST_MAX];
    static _Thread_local char mbuf[MODE_LIST_MAX];
    obuf[0] = fbuf[0] = mbuf[0] = '\0';
    for (int i = 0; i < opts->channel_count; i++)
    {
        const gui_channel_t *ch = &opts->channels[i];
        if (!req_str(ch->freq))
            return failf(err, err_size, "frequency", i, "must not be empty");
        if (i && append(fbuf, sizeof(fbuf), ","))
            goto overflow;
        if (append(fbuf, sizeof(fbuf), ch->freq))
            goto overflow;
        if (i)
        {
            if (append(mbuf, sizeof(mbuf), ",") || append(obuf, sizeof(obuf), ","))
                goto overflow;
        }
        if (append(
                mbuf,
                sizeof(mbuf),
                ch->mode == GUI_MODE_FM     ? "fm"
                : ch->mode == GUI_MODE_WBFM ? "wbfm"
                : ch->mode == GUI_MODE_AM   ? "am"
                : ch->mode == GUI_MODE_USB  ? "usb"
                : ch->mode == GUI_MODE_LSB  ? "lsb"
                                            : "raw"))
            goto overflow;
        if (channel_output_arg(
                ch, i, obuf + strlen(obuf), sizeof(obuf) - strlen(obuf), err, err_size))
            return -1;
    }

    if (req_str(opts->device) && (append_flag(cmd, cmd_size, "-d", opts->device)))
        goto overflow;
    if (req_str(opts->rate_in) && append_flag(cmd, cmd_size, "-s", opts->rate_in))
        goto overflow;
    if (req_str(opts->rate_out) && append_flag(cmd, cmd_size, "-r", opts->rate_out))
        goto overflow;
    if (opts->dc_block && append_flag(cmd, cmd_size, "-E", "dc"))
        goto overflow;
    if (opts->deemph && append_flag(cmd, cmd_size, "-E", "deemp"))
        goto overflow;

    if (opts->backend == GUI_BACKEND_RTL)
    {
        if (req_str(opts->rtl_gain) && append_flag(cmd, cmd_size, "-g", opts->rtl_gain))
            goto overflow;
        if (req_str(opts->rtl_ppm) && append_flag(cmd, cmd_size, "-p", opts->rtl_ppm))
            goto overflow;
        if (opts->rtl_biastee && append_flag(cmd, cmd_size, "-T", NULL))
            goto overflow;
        if (opts->rtl_direct == 1 && append_flag(cmd, cmd_size, "-E", "direct"))
            goto overflow;
        if (opts->rtl_direct == 2 && append_flag(cmd, cmd_size, "-E", "direct2"))
            goto overflow;
    }
    else
    {
        if (req_str(opts->rsp_grdb) && append_flag(cmd, cmd_size, "-g", opts->rsp_grdb))
            goto overflow;
        if (req_str(opts->rsp_lna) && append_flag(cmd, cmd_size, "-L", opts->rsp_lna))
            goto overflow;
    }

    if (opts->use_file_input)
    {
        if (!req_str(opts->file_path) || !req_str(opts->file_rate) || !req_str(opts->file_center))
            return fail(
                err,
                err_size,
                "File input needs a path, capture rate and center "
                "frequency");
        if (strchr(opts->file_path, '"'))
            return fail(err, err_size, "File input path may not contain quotes");
        static _Thread_local char ibuf[FILE_INPUT_MAX];
        snprintf(
            ibuf, sizeof(ibuf), "%s:%s:%s", opts->file_path, opts->file_rate, opts->file_center);
        if (append_flag(cmd, cmd_size, "-I", ibuf))
            goto overflow;
    }
    if (req_str(opts->record_path) && append_flag(cmd, cmd_size, "-R", opts->record_path))
        goto overflow;
    if (req_str(opts->timeout) && append_flag(cmd, cmd_size, "-x", opts->timeout))
        goto overflow;

    if (append_flag(cmd, cmd_size, "-f", fbuf) || append_flag(cmd, cmd_size, "-M", mbuf) ||
        append_flag(cmd, cmd_size, "-O", obuf))
        goto overflow;

    return 0;

overflow:
    return fail(err, err_size, "Command line too long");
}
