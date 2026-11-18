/**
 * @file args_mapper_test.c
 * @brief Unit tests for gui_options_build_command_line() (run by `make gui-test`).
 *
 * Plain asserts, no framework: each case builds a #gui_launch_options_t and
 * compares the mapped command line against the expected string, or expects a
 * validation failure. Returns nonzero if anything failed.
 */
#include "args_mapper.h"

#include <stdio.h>
#include <string.h>

static int failures;

/** @brief Build the minimal valid options: one FM channel, TCP out. */
static gui_launch_options_t base_options(void)
{
    gui_launch_options_t o;
    memset(&o, 0, sizeof(o));
    o.backend = GUI_BACKEND_RTL;
    strcpy(o.channels[0].freq, "99.85M");
    o.channels[0].mode = GUI_MODE_FM;
    o.channels[0].output = GUI_OUT_TCP;
    strcpy(o.channels[0].tcp_port, "8100");
    o.channel_count = 1;
    return o;
}

/** @brief Expect a successful mapping equal to @p want. */
static void check_cmd(const gui_launch_options_t *o, const char *want, int line)
{
    char cmd[8192], err[256];
    if (gui_options_build_command_line(o, cmd, sizeof(cmd), err, sizeof(err)) != 0)
    {
        printf("FAIL line %d: unexpected error: %s\n", line, err);
        failures++;
        return;
    }
    if (strcmp(cmd, want) != 0)
    {
        printf("FAIL line %d:\n  got:  %s\n  want: %s\n", line, cmd, want);
        failures++;
    }
}

/** @brief Expect a validation failure with a non-empty message. */
static void check_err(const gui_launch_options_t *o, int line)
{
    char cmd[8192], err[256];
    err[0] = '\0';
    if (gui_options_build_command_line(o, cmd, sizeof(cmd), err, sizeof(err)) == 0)
    {
        printf("FAIL line %d: expected validation error, got: %s\n", line, cmd);
        failures++;
        return;
    }
    if (!err[0])
    {
        printf("FAIL line %d: error message is empty\n", line);
        failures++;
    }
}

int main(void)
{
    /* Minimal single channel */
    gui_launch_options_t o = base_options();
    check_cmd(&o, "rtl_multi.exe -f 99.85M -M fm -O tcp:8100", __LINE__);

    /* Multi-channel: positional -f/-M/-O lists */
    o = base_options();
    strcpy(o.channels[1].freq, "89.5M");
    o.channels[1].mode = GUI_MODE_AM;
    o.channels[1].output = GUI_OUT_FILE;
    strcpy(o.channels[1].file_name, "out1.raw");
    o.channel_count = 2;
    check_cmd(&o, "rtl_multi.exe -f 99.85M,89.5M -M fm,am -O tcp:8100,out1.raw", __LINE__);

    /* TCP/UDP/file mix; Hz frequency passes through */
    o = base_options();
    o.channels[0].output = GUI_OUT_UDP;
    strcpy(o.channels[0].udp_host, "127.0.0.1");
    strcpy(o.channels[0].udp_port, "1234");
    o.channel_count = 1;
    check_cmd(&o, "rtl_multi.exe -f 99.85M -M fm -O udp:127.0.0.1:1234", __LINE__);

    /* Global options + RTL backend flags, canonical order */
    o = base_options();
    strcpy(o.device, "0");
    strcpy(o.rate_in, "2.4M");
    strcpy(o.rate_out, "48000");
    o.dc_block = 1;
    o.deemph = 1;
    strcpy(o.rtl_gain, "29.7");
    strcpy(o.rtl_ppm, "3");
    o.rtl_biastee = 1;
    o.rtl_direct = 1;
    check_cmd(
        &o,
        "rtl_multi.exe -d 0 -s 2.4M -r 48000 -E dc -E deemp -g 29.7 -p 3 -T "
        "-E direct -f 99.85M -M fm -O tcp:8100",
        __LINE__);

    /* direct2 variant */
    o = base_options();
    o.rtl_direct = 2;
    check_cmd(&o, "rtl_multi.exe -E direct2 -f 99.85M -M fm -O tcp:8100", __LINE__);

    /* RSP backend: gRdB/LNA emitted, RTL fields ignored */
    o = base_options();
    o.backend = GUI_BACKEND_RSP;
    strcpy(o.rsp_grdb, "30");
    strcpy(o.rsp_lna, "1");
    strcpy(o.rtl_gain, "29.7");
    o.rtl_biastee = 1;
    check_cmd(&o, "rsp_multi.exe -g 30 -L 1 -f 99.85M -M fm -O tcp:8100", __LINE__);

    /* File input with spaces in the path: single quoted -I argument */
    o = base_options();
    o.use_file_input = 1;
    strcpy(o.file_path, "C:/rec folder/cap.cf32");
    strcpy(o.file_rate, "2M");
    strcpy(o.file_center, "100M");
    check_cmd(
        &o,
        "rtl_multi.exe -I \"C:/rec folder/cap.cf32:2M:100M\" -f 99.85M -M fm "
        "-O tcp:8100",
        __LINE__);

    /* Recording + timeout */
    o = base_options();
    strcpy(o.record_path, "my rec.cf32");
    strcpy(o.timeout, "30");
    check_cmd(&o, "rtl_multi.exe -R \"my rec.cf32\" -x 30 -f 99.85M -M fm -O tcp:8100", __LINE__);

    /* Output filename with a space: quoted inside the joined -O list */
    o = base_options();
    o.channels[0].output = GUI_OUT_FILE;
    strcpy(o.channels[0].file_name, "my file.wav");
    check_cmd(&o, "rtl_multi.exe -f 99.85M -M fm -O \"my file.wav\"", __LINE__);

    /* wbfm passes through untouched (CLI applies the global side effects) */
    o = base_options();
    o.channels[0].mode = GUI_MODE_WBFM;
    check_cmd(&o, "rtl_multi.exe -f 99.85M -M wbfm -O tcp:8100", __LINE__);

    /* Serial-number device selector */
    o = base_options();
    strcpy(o.device, "00000001");
    check_cmd(&o, "rtl_multi.exe -d 00000001 -f 99.85M -M fm -O tcp:8100", __LINE__);

    /* Validation failures */
    o = base_options();
    o.channel_count = 0;
    check_err(&o, __LINE__);

    o = base_options();
    o.channels[0].freq[0] = '\0';
    check_err(&o, __LINE__);

    o = base_options();
    strcpy(o.channels[0].tcp_port, "0");
    check_err(&o, __LINE__);

    o = base_options();
    strcpy(o.channels[0].tcp_port, "70000");
    check_err(&o, __LINE__);

    o = base_options();
    strcpy(o.channels[0].tcp_port, "abc");
    check_err(&o, __LINE__);

    o = base_options();
    o.channels[0].output = GUI_OUT_UDP;
    o.channels[0].udp_host[0] = '\0';
    strcpy(o.channels[0].udp_port, "1234");
    check_err(&o, __LINE__);

    o = base_options();
    o.channels[0].output = GUI_OUT_FILE;
    strcpy(o.channels[0].file_name, "a,b.wav");
    check_err(&o, __LINE__);

    o = base_options();
    o.channels[0].output = GUI_OUT_FILE;
    o.channels[0].file_name[0] = '\0';
    check_err(&o, __LINE__);

    o = base_options();
    o.use_file_input = 1;
    strcpy(o.file_path, "cap.cf32");
    strcpy(o.file_rate, "2M");
    check_err(&o, __LINE__); /* missing center */

    o = base_options();
    o.channels[0].freq[0] = '"';
    check_err(&o, __LINE__); /* quote would corrupt argv */

    if (failures)
    {
        printf("%d test(s) FAILED\n", failures);
        return 1;
    }
    printf("args_mapper: all tests passed\n");
    return 0;
}
