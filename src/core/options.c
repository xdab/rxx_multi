/*
 * rtl_multi - CLI argument parsing
 *
 * Architecture: All operation uses channels (even single-channel).
 * Single-channel mode = channel_count = 1 with channels[0].
 */

#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "device.h"
#include "options.h"
#include "types.h"

/* Default values for a new channel */
static void channel_init_defaults(channel_t *ch)
{
    ch->mode = DEMOD_FM;
    ch->output_mode = OUTPUT_FILE;
    strncpy(ch->filename, "-", sizeof(ch->filename) - 1);
    ch->filename[sizeof(ch->filename) - 1] = '\0';
    ch->tcp_port = 0;
    ch->udp_host[0] = '\0';
    ch->udp_port = 0;
}

/* Add a new channel with the given frequency */
static int add_channel(options_t *opts, uint32_t freq)
{
    if (opts->channel_count >= FREQUENCIES_LIMIT)
        return -1;

    channel_t *ch = &opts->channels[opts->channel_count];
    ch->freq = freq;
    channel_init_defaults(ch);
    opts->channel_count++;
    return 0;
}

/* Apply modulation to a channel, returns mode or -1 on error */
static int apply_modulation(channel_t *ch, const char *arg, options_t *opts)
{
    if (strcmp("fm", arg) == 0)
        ch->mode = DEMOD_FM;
    else if (strcmp("raw", arg) == 0)
        ch->mode = DEMOD_RAW;
    else if (strcmp("am", arg) == 0)
        ch->mode = DEMOD_AM;
    else if (strcmp("usb", arg) == 0)
        ch->mode = DEMOD_USB;
    else if (strcmp("lsb", arg) == 0)
        ch->mode = DEMOD_LSB;
    else if (strcmp("wbfm", arg) == 0)
    {
        /* wbfm sets global options + channel mode */
        opts->rate_in = WBFM_SAMPLE_RATE;
        opts->rate_out = WBFM_SAMPLE_RATE;
        opts->deemph = 1;
        ch->mode = DEMOD_FM;
    }
    else
    {
        fprintf(stderr, "Unknown modulation: %s\n", arg);
        return -1;
    }
    return 0;
}

/* Apply output mode to channel(s) */
static int parse_output_option(channel_t *ch, const char *arg)
{
    /* Parse output mode: tcp:PORT or udp:HOST:PORT */
    if (strncmp(arg, "tcp:", 4) == 0)
    {
        int port = atoi(arg + 4);
        if (port <= 0 || port > 65535)
        {
            fprintf(stderr, "Invalid TCP port: %s\n", arg + 4);
            return -1;
        }
        ch->output_mode = OUTPUT_TCP;
        ch->tcp_port = port;
    }
    else if (strncmp(arg, "udp:", 4) == 0)
    {
        /* Parse udp:HOST:PORT format */
        char *host_port = (char *)(arg + 4);
        char *colon = strrchr(host_port, ':');
        if (!colon)
        {
            fprintf(stderr, "Invalid UDP format, use -O udp:HOST:PORT\n");
            return -1;
        }
        *colon = '\0';
        int port = atoi(colon + 1);
        if (port <= 0 || port > 65535)
        {
            fprintf(stderr, "Invalid UDP port: %s\n", colon + 1);
            return -1;
        }
        ch->output_mode = OUTPUT_UDP;
        strncpy(ch->udp_host, host_port, sizeof(ch->udp_host) - 1);
        ch->udp_host[sizeof(ch->udp_host) - 1] = '\0';
        ch->udp_port = port;
        *colon = ':'; /* Restore for potential error messages */
    }
    else
    {
        /* Bare filename: raw demodulated audio to disk. Note: filenames
         * are comma-separated in multi-channel mode, so they cannot
         * contain commas. */
        if (!arg[0])
        {
            fprintf(stderr, "ERROR: -O filename is empty\n");
            return -1;
        }
        ch->output_mode = OUTPUT_FILE;
        strncpy(ch->filename, arg, sizeof(ch->filename) - 1);
        ch->filename[sizeof(ch->filename) - 1] = '\0';
    }
    return 0;
}

/* Parse -I FILE:RATE:CENTER (raw IQ file input test mode) */
static int parse_input_option(options_t *opts, const char *arg)
{
    char buf[2 * STATIC_STRING_SIZE];
    char *rate_str, *center_str;

    if (opts->input_file[0])
    {
        fprintf(stderr, "ERROR: -I given more than once\n");
        return -1;
    }

    strncpy(buf, arg, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    rate_str = strchr(buf, ':');
    if (!rate_str)
    {
        fprintf(stderr, "Invalid IQ input format, use -I FILE:RATE:CENTER\n");
        return -1;
    }
    *rate_str++ = '\0';

    center_str = strchr(rate_str, ':');
    if (!center_str)
    {
        fprintf(stderr, "Invalid IQ input format, use -I FILE:RATE:CENTER\n");
        return -1;
    }
    *center_str++ = '\0';

    if (strchr(center_str, ':'))
    {
        fprintf(stderr, "Invalid IQ input format, use -I FILE:RATE:CENTER\n");
        return -1;
    }

    opts->input_rate = (float)atofs(rate_str);
    opts->input_center = (float)atofs(center_str);
    if (opts->input_rate <= 0 || opts->input_center <= 0)
    {
        fprintf(stderr, "Invalid IQ input rate or center: %s\n", arg);
        return -1;
    }

    strncpy(opts->input_file, buf, sizeof(opts->input_file) - 1);
    opts->input_file[sizeof(opts->input_file) - 1] = '\0';
    return 0;
}

/* Count comma-separated values in a string */
static int count_values(const char *str)
{
    int count = 1;
    for (int i = 0; str[i]; i++)
        if (str[i] == ',')
            count++;
    return count;
}

/* Get the nth comma-separated value from a string */
static const char *get_value_at(const char *str, int index)
{
    static char buf[STATIC_STRING_SIZE];
    int len = strlen(str);
    int val_idx = 0;
    int start = 0;
    size_t buf_size = sizeof(buf);

    for (int i = 0; i <= len; i++)
    {
        if (str[i] == ',' || str[i] == '\0')
        {
            if (val_idx == index)
            {
                int cpy_len = i - start;
                if ((size_t)cpy_len >= buf_size)
                    cpy_len = buf_size - 1;
                strncpy(buf, str + start, cpy_len);
                buf[cpy_len] = '\0';
                return buf;
            }
            val_idx++;
            start = i + 1;
        }
    }
    return NULL;
}

void options_usage(void)
{
    fprintf(stderr,
            APP_NAME ", a simple narrow band demodulator\n"
            "\n"
            "Use:\t" APP_NAME " -f freq [-options] [filename]\n"
            "\t-f frequency_to_tune_to [Hz]\n"
            "\t    supports suffixes: k,M,G\n"
            "\t    examples: -f 145500000 \n"
            "\t           or -f 145500k \n"
            "\t           or -f 145M,145.5,146.52M (multi-channel operation)\n"
            "\t[-M modulation (default: fm)]\n"
            "\t    fm, wbfm, raw, am, usb, lsb\n"
            "\t    wbfm == -M fm -s 170k -E deemp\n"
            "\t    raw mode outputs 2x16 bit IQ pairs\n"
            "\t[-s channel_sample_rate (default: 12k)]\n"
            "\t[-r output_sample_rate (default: 48k)]\n"
            "\t[-d device_index or serial (default: 0)]\n");
    device_print_options();
    fprintf(stderr,
            "\t[-E enable_option (default: none)]\n"
            "\t    use multiple -E to enable multiple options\n"
            "\t    dc:      enable dc blocking filter\n"
            "\t    deemp:   enable de-emphasis filter\n"
            "\t[-O output_mode]\n"
            "\t    tcp:PORT    - TCP server on PORT (e.g., -O tcp:8000)\n"
            "\t    udp:HOST:PORT - UDP to HOST:PORT (e.g., -O udp:127.0.0.1:5000)\n"
            "\t    FILENAME    - raw audio to disk (e.g., -O ch1.raw,\n"
            "\t                  may not contain commas)\n"
            "\t    (if not specified, output to stdout)\n"
            "\t[-I FILE:RATE:CENTER - raw IQ file input (test mode, no device)]\n"
            "\t    FILE    interleaved float32 I/Q (CF32, e.g. SDR++ baseband)\n"
            "\t    RATE    capture rate of the recording (e.g. 2M)\n"
            "\t    CENTER  center frequency of the recording (e.g. 102.5M)\n"
            "\t[-R FILE - record raw baseband IQ to FILE (CF32, replay with -I)]\n"
            "\t    one file for the whole capture span, all channels included;\n"
            "\t    rate/center are printed at startup as the exact -I invocation\n"
            "\n"
            "Experimental options:\n"
            "\t[-x seconds - exit gracefully after SECONDS of running]\n"
            "\n"
            "Produces signed 16 bit ints, use Sox or aplay to hear them.\n"
            "\t" APP_NAME " ... | play -t raw -r 24k -es -b 16 -c 1 -V1 -\n"
            "\t          ... | aplay -r 24k -f S16_LE -t raw -c 1\n"
            "\t" APP_NAME " -f 101.0M -O tcp:8000\n"
            "\t          ... | nc localhost 8000 | aplay -r 24k -f S16_LE -t raw -c 1\n"
            "\t" APP_NAME " -f 101.0M -O udp:127.0.0.1:5000\n"
            "\t          ... | nc -lu 127.0.0.1 5000 | aplay -r 24k -f S16_LE -t raw -c 1\n"
            "\n");
    exit(1);
}

void options_print_channels(const options_t *opts)
{
    fprintf(stderr, APP_NAME " multi-channel mode:\n\n");

    for (int i = 0; i < opts->channel_count; i++)
    {
        const channel_t *ch = &opts->channels[i];

        fprintf(stderr, "Channel %d:\n", i);
        fprintf(stderr, "  Frequency: %.3f MHz\n", ch->freq / 1e6);

        /* Modulation mode */
        const char *mode_str = "unknown";
        switch (ch->mode)
        {
        case DEMOD_FM:
            mode_str = "FM";
            break;
        case DEMOD_AM:
            mode_str = "AM";
            break;
        case DEMOD_USB:
            mode_str = "USB";
            break;
        case DEMOD_LSB:
            mode_str = "LSB";
            break;
        case DEMOD_RAW:
            mode_str = "RAW";
            break;
        }
        fprintf(stderr, "  Modulation: %s\n", mode_str);

        /* Output mode */
        const char *out_str = "unknown";
        switch (ch->output_mode)
        {
        case OUTPUT_FILE:
            out_str = "file";
            break;
        case OUTPUT_TCP:
            out_str = "TCP";
            break;
        case OUTPUT_UDP:
            out_str = "UDP";
            break;
        }
        fprintf(stderr, "  Output: %s", out_str);
        if (ch->output_mode == OUTPUT_TCP)
            fprintf(stderr, " port %d", ch->tcp_port);
        else if (ch->output_mode == OUTPUT_UDP)
            fprintf(stderr, " to %s:%d", ch->udp_host, ch->udp_port);
        else if (ch->filename[0])
            fprintf(stderr, " file: %s", ch->filename);

        fprintf(stderr, "\n\n");
    }
}

int options_parse(int argc, char **argv, options_t *opts)
{
    int opt;

    /* Initialize options with defaults */
    memset(opts, 0, sizeof(options_t));
    opts->rate_in = DEFAULT_SAMPLE_RATE;
    opts->rate_out = DEFAULT_SAMPLE_RATE;
    opts->rate_audio = DEFAULT_OUTPUT_RATE;

    while ((opt = getopt(argc, argv, "d:f:g:I:L:s:r:p:R:x:E:M:hTO:")) != -1)
    {
        switch (opt)
        {
        case 'd':
            opts->dev_index = verbose_device_search(optarg);
            opts->dev_given = 1;
            break;
        case 'f':
            /* Check for comma-separated frequencies (multi-channel) */
            if (strchr(optarg, ','))
            {
                int val_count = count_values(optarg);
                for (int i = 0; i < val_count; i++)
                {
                    const char *val = get_value_at(optarg, i);
                    /* atofs expects char*, make a local copy */
                    char val_copy[64];
                    strncpy(val_copy, val, sizeof(val_copy) - 1);
                    val_copy[sizeof(val_copy) - 1] = '\0';
                    uint32_t freq = (uint32_t)atofs(val_copy);
                    add_channel(opts, freq);
                }
            }
            else if (strchr(optarg, ':'))
            {
                /* Range syntax - for now, just add as single frequency */
                uint32_t freq = (uint32_t)atofs(optarg);
                add_channel(opts, freq);
            }
            else
            {
                /* Single frequency */
                uint32_t freq = (uint32_t)atofs(optarg);
                add_channel(opts, freq);
            }
            break;
        case 'I':
            if (parse_input_option(opts, optarg) < 0)
                return -1;
            break;
        case 'R':
            if (opts->record_file[0])
            {
                fprintf(stderr, "ERROR: -R given more than once\n");
                return -1;
            }
            if (!optarg[0])
            {
                fprintf(stderr, "ERROR: -R requires a file name\n");
                return -1;
            }
            strncpy(opts->record_file, optarg, sizeof(opts->record_file) - 1);
            opts->record_file[sizeof(opts->record_file) - 1] = '\0';
            break;
        case 'x':
            opts->run_timeout = (float)atof(optarg);
            if (opts->run_timeout <= 0)
            {
                fprintf(stderr, "ERROR: -x (timeout seconds) must be > 0\n");
                return -1;
            }
            break;
        case 's':
            opts->rate_in = atofs(optarg);
            opts->rate_out = atofs(optarg);
            break;
        case 'r':
            opts->rate_audio = atofs(optarg);
            break;
        case 'E':
            if (strcmp("dc", optarg) == 0)
                opts->dc_block = 1;
            else if (strcmp("deemp", optarg) == 0)
                opts->deemph = 1;
            else
            {
                /* Not a universal -E option - let the backend look at it */
                if (device_parse_option(opt, optarg, opts) != 0)
                    return -1;
            }
            break;
        case 'M':
            /* Check for comma-separated values (per-channel) */
            if (opts->channel_count > 0 && strchr(optarg, ','))
            {
                int val_count = count_values(optarg);
                if (val_count != opts->channel_count)
                {
                    fprintf(stderr, "ERROR: -M has %d values but %d frequencies specified\n",
                            val_count, opts->channel_count);
                    fprintf(stderr, "       Use 1 value (applies to all) or %d values\n",
                            opts->channel_count);
                    return -1;
                }
                /* Apply per-channel */
                for (int i = 0; i < val_count; i++)
                {
                    const char *val = get_value_at(optarg, i);
                    char val_copy[64];
                    strncpy(val_copy, val, sizeof(val_copy) - 1);
                    val_copy[sizeof(val_copy) - 1] = '\0';
                    if (apply_modulation(&opts->channels[i], val_copy, opts) < 0)
                        return -1;
                }
            }
            else
            {
                /* Single value - apply to all channels */
                for (int i = 0; i < opts->channel_count; i++)
                {
                    if (apply_modulation(&opts->channels[i], optarg, opts) < 0)
                        return -1;
                }
            }
            break;
        case 'O':
            /* For multi-channel mode, outputs must be unique per channel */
            if (opts->channel_count > 1)
            {
                /* Require comma-separated values (one per channel) */
                if (!strchr(optarg, ','))
                {
                    fprintf(stderr, "ERROR: -O requires one output per channel in multi-channel mode\n");
                    fprintf(stderr, "       Use -O tcp:8001,tcp:8002,... for %d channels\n",
                            opts->channel_count);
                    return -1;
                }
                int val_count = count_values(optarg);
                if (val_count != opts->channel_count)
                {
                    fprintf(stderr, "ERROR: -O has %d values but %d frequencies specified\n",
                            val_count, opts->channel_count);
                    fprintf(stderr, "       Must specify exactly %d outputs\n",
                            opts->channel_count);
                    return -1;
                }
                /* Apply per-channel */
                for (int i = 0; i < val_count; i++)
                {
                    const char *val = get_value_at(optarg, i);
                    char val_copy[64];
                    strncpy(val_copy, val, sizeof(val_copy) - 1);
                    val_copy[sizeof(val_copy) - 1] = '\0';
                    if (parse_output_option(&opts->channels[i], val_copy) < 0)
                        return -1;
                }
            }
            else if (opts->channel_count == 1)
            {
                /* Single channel mode: only one output allowed */
                if (strchr(optarg, ','))
                {
                    fprintf(stderr, "ERROR: -O only accepts one output for single channel mode\n");
                    fprintf(stderr, "       Use only -O tcp:6000, not -O tcp:6000,tcp:6001\n");
                    return -1;
                }
                if (parse_output_option(&opts->channels[0], optarg) < 0)
                    return -1;
            }
            /* channel_count == 0: defer to later, will show error */
            break;
        case 'h':
            options_usage();
            break;
        default:
        {
            /* Backend-specific flag (-g, -L, -p, -T, ...) */
            int rc = device_parse_option(opt, optarg, opts);
            if (rc == 0)
                break;
            if (rc < 0)
                return -1;
            /* Not claimed by the backend: same verdict as getopt on a
             * flag missing from this binary's option table */
            fprintf(stderr, "%s: invalid option -- '%c'\n", APP_NAME, opt);
            options_usage();
            break;
        }
        }
    }

    return 0;
}

double atofs(char *s)
/* standard suffixes */
{
    char last;
    int len;
    double suff = 1.0;
    len = strlen(s);
    last = s[len - 1];
    s[len - 1] = '\0';
    switch (last)
    {
    case 'g':
    case 'G':
        suff *= 1e3;
        /* fall-through */
    case 'm':
    case 'M':
        suff *= 1e3;
        /* fall-through */
    case 'k':
    case 'K':
        suff *= 1e3;
        suff *= atof(s);
        s[len - 1] = last;
        return suff;
    }
    s[len - 1] = last;
    return atof(s);
}

double atoft(char *s)
/* time suffixes, returns seconds */
{
    char last;
    int len;
    double suff = 1.0;

    len = strlen(s);
    last = s[len - 1];
    s[len - 1] = '\0';

    switch (last)
    {
    case 'h':
    case 'H':
        suff *= 60;
        /* fall-through */
    case 'm':
    case 'M':
        suff *= 60;
        /* fall-through */
    case 's':
    case 'S':
        suff *= atof(s);
        s[len - 1] = last;
        return suff;
    }

    s[len - 1] = last;
    return atof(s);
}

double atofp(char *s)
/* percent suffixes */
{
    char last;
    int len;
    double suff = 1.0;

    len = strlen(s);
    last = s[len - 1];
    s[len - 1] = '\0';

    if ('%' == last)
    {
        suff *= 0.01;
        suff *= atof(s);
        s[len - 1] = last;
        return suff;
    }

    s[len - 1] = last;
    return atof(s);
}
