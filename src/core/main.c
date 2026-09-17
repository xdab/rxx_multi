/*
 * rxx_multi, a multi-channel SDR receiver
 * Copyright (C) 2026 by Przemyslaw Wcislo <przemyslawxdab@gmail.com>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif

#include <math.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "demod.h"
#include "device.h"
#include "dsp.h"
#include "options.h"
#include "output.h"
#include "thread.h"
#include "types.h"

struct device_state device;
struct demod_state demods[FREQUENCIES_LIMIT];
struct output_state outputs[FREQUENCIES_LIMIT];
int freq_len;
volatile int do_exit = 0;

static void sighandler(int signum)
{
    (void)signum;
    signal(SIGPIPE, SIG_IGN);
    fprintf(stderr, "Signal caught, exiting!\n");
    do_exit = 1;
    device_signal_exit();
}

static demodulate_fn mode_demod_for(enum demod_mode mode)
{
    switch (mode)
    {
    case DEMOD_RAW:
        return &demodulate_raw;
    case DEMOD_AM:
        return &demodulate_am;
    case DEMOD_USB:
        return &demodulate_usb;
    case DEMOD_LSB:
        return &demodulate_lsb;
    case DEMOD_FM:
    default:
        return &demodulate_fm;
    }
}

/* Compute device center frequency, requested capture width and the
 * resulting capture plan. Single-channel: tune directly to the one
 * frequency. Multi-channel: tune to the midpoint; capture rate must
 * cover full span. File input (-I): rate and center come from the
 * recording itself. */
static int plan_capture(const options_t *opts, int file_input, int *downsample, int *rate_channel)
{
    int rate_in = (int)opts->rate_in;
    uint64_t width;
    struct capture_plan plan;

    if (file_input)
    {
        device.rate = (uint32_t)opts->input_rate;
        device.freq = (uint32_t)opts->input_center;
        strncpy(device.input_path, opts->input_file, sizeof(device.input_path) - 1);
        device.input_path[sizeof(device.input_path) - 1] = '\0';

        /* Every channel must fit inside the recorded span */
        int64_t limit = (int64_t)device.rate / 2 - rate_in / 2;
        for (int i = 0; i < opts->channel_count; i++)
        {
            int64_t offset = (int64_t)opts->channels[i].freq - (int64_t)device.freq;
            if (offset < -limit || offset > limit)
            {
                fprintf(
                    stderr,
                    "ERROR: channel %.3f MHz falls outside the IQ file span "
                    "(%.3f MHz +/-%.0f kHz).\n",
                    opts->channels[i].freq / 1e6,
                    device.freq / 1e6,
                    (float)device.rate / 2000.0f
                );
                return -1;
            }
        }

        /* The file rate is exact - no hardware rate snapping applies. */
        width = (uint64_t)device.rate;
    }
    else if (opts->channel_count > 1)
    {
        uint32_t minf = opts->channels[0].freq;
        uint32_t maxf = opts->channels[0].freq;
        for (int i = 1; i < opts->channel_count; i++)
        {
            if (opts->channels[i].freq < minf)
                minf = opts->channels[i].freq;
            if (opts->channels[i].freq > maxf)
                maxf = opts->channels[i].freq;
        }
        uint64_t span = (uint64_t)maxf - (uint64_t)minf;
        width = span + 2ULL * rate_in;
        device.freq = (uint32_t)(minf + span / 2);
    }
    else
    {
        /* Same oversampling the reference implementation picks for a
         * single channel; the backend snaps the resulting rate */
        int ds0 = (1000000 / rate_in) + 1;
        width = (uint64_t)ds0 * (uint64_t)rate_in;
        device.freq = opts->channels[0].freq;
    }

    if (device_plan_capture(file_input, rate_in, width, &plan) < 0)
        return -1;
    if (!file_input)
        device.rate = plan.rate;

    /* True channel rate after integer decimation of the capture rate */
    *downsample = plan.downsample;
    *rate_channel = (int)(device.rate / (uint32_t)plan.downsample);
    return 0;
}

static int setup_pipelines(const options_t *opts, int rate_channel, int downsample)
{
    float output_scale = (float)(1 << 15) / (128.0f * (float)downsample);
    if (output_scale < 1.0f)
        output_scale = 1.0f;

    memset(demods, 0, sizeof(demods));
    memset(outputs, 0, sizeof(outputs));

    for (int i = 0; i < opts->channel_count; i++)
    {
        const channel_t *ch = &opts->channels[i];
        struct channel_pipeline *pipeline;

        demod_init(&demods[i]);
        output_init(&outputs[i]);
        pipeline = &demods[i].pipeline;

        pipeline->input_rate = rate_channel;
        pipeline->demod_rate = rate_channel;
        pipeline->output_rate = (int)opts->rate_audio;
        pipeline->downsample_factor = downsample;
        pipeline->dc_block_enabled = opts->dc_block;
        pipeline->deemph_enabled = opts->deemph;
        pipeline->output_scale = (ch->mode == DEMOD_FM) ? 1.0f : output_scale;
        pipeline->demodulate = mode_demod_for(ch->mode);
        demods[i].output_target = &outputs[i];

        /* Shift the channel to baseband whenever it is offset from the
         * capture center: multi-channel captures are tuned to the span
         * midpoint, file recordings may be centered anywhere, and a lone
         * hardware channel sits dead-on (offset 0, no shift) */
        pipeline->target_frequency = ch->freq;
        pipeline->frequency_offset = (double)((int64_t)ch->freq - (int64_t)device.freq);
        pipeline->frequency_shift_enabled = (pipeline->frequency_offset != 0.0);
        /* DDS phase step for the channel-shift oscillator: mixing down
         * multiplies by e^(-j*2*pi*f_off/f_capture) per sample, so the
         * step is the negated offset as a 2^32-turn fraction; negative
         * offsets wrap naturally through the unsigned accumulator */
        pipeline->shift_step = (device.rate > 0)
                                   ? (uint32_t)(int64_t)(-pipeline->frequency_offset /
                                                         (double)device.rate * 4294967296.0)
                                   : 0;

        outputs[i].mode = ch->output_mode;
        strncpy(outputs[i].filename, ch->filename, sizeof(outputs[i].filename) - 1);
        outputs[i].filename[sizeof(outputs[i].filename) - 1] = '\0';
        outputs[i].rate = (uint32_t)opts->rate_audio;

        if (outputs[i].mode == OUTPUT_TCP)
        {
            if (tcp_init(&outputs[i], ch->tcp_port) < 0)
                return -1;
        }
        else if (outputs[i].mode == OUTPUT_UDP)
        {
            if (udp_init(&outputs[i], ch->udp_host, ch->udp_port) < 0)
                return -1;
        }
        else /* OUTPUT_FILE */
        {
            /* stdout is only valid in single-channel mode (already validated above) */
            outputs[i].file = (strcmp(ch->filename, "-") == 0) ? stdout : fopen(ch->filename, "wb");
            if (!outputs[i].file)
            {
                fprintf(stderr, "Failed to open %s\n", ch->filename);
                return -1;
            }
        }
    }

    if (opts->deemph)
    {
        /* Compute standard single-pole de-emphasis feed-forward coefficient:
         * b0 = 1 - exp(-1/(Fs * tau)), where tau = 75us. Store in
         * demod_state->deemph_a for use when creating the IIR filter.
         */
        float tau = 75e-6f;
        float Fs = (float)rate_channel;
        float d = expf(-1.0f / (tau * Fs));
        float deemph_a = 1.0f - d;
        for (int i = 0; i < opts->channel_count; i++)
            demods[i].pipeline.deemph_alpha = deemph_a;
    }

    /* Build the per-channel DSP filter objects now, not lazily on the
     * first chunk: avoids a one-time stall per channel right when
     * processing starts (e.g. when a TCP client connects) */
    for (int i = 0; i < opts->channel_count; i++)
        if (dsp_init_filters(&demods[i].pipeline) < 0)
            return -1;

    return 0;
}

static void bringup_file_input(const options_t *opts)
{
    (void)opts;
    fprintf(
        stderr,
        "IQ file input: %s @ %u Hz, center %.3f MHz\n",
        device.input_path,
        device.rate,
        device.freq / 1e6
    );
}

static int bringup_device(const options_t *opts, int record, int downsample)
{
    int r;

    if (!opts->dev_given)
        device.dev_index = verbose_device_search("0");
    if (device.dev_index < 0)
        return -1;

    if (device_open(device.dev_index) != 0)
    {
        fprintf(stderr, "Failed to open device #%d.\n", device.dev_index);
        device_close();
        return -1;
    }

    verbose_reset_buffer();

    r = verbose_set_frequency(device.freq);
    if (r < 0)
    {
        device_close();
        return -1;
    }

    fprintf(stderr, "Oversampling input by: %ix.\n", downsample);

    r = verbose_set_sample_rate(device.rate);
    if (r < 0)
    {
        device_close();
        return -1;
    }

    /* Open the recording before streaming starts */
    if (record)
    {
        strncpy(device.record_path, opts->record_file, sizeof(device.record_path) - 1);
        device.record_path[sizeof(device.record_path) - 1] = '\0';
        device.record_file = fopen(device.record_path, "wb");
        if (!device.record_file)
        {
            fprintf(stderr, "Failed to open %s for IQ recording\n", device.record_path);
            device_close();
            return -1;
        }
        fprintf(
            stderr,
            "Recording baseband to %s @ %u Hz, center %.3f MHz\n",
            device.record_path,
            device.rate,
            device.freq / 1e6
        );
        fprintf(stderr, "Replay with: -I %s:%u:%u\n", device.record_path, device.rate, device.freq);
    }

    r = device_apply_settings();
    if (r < 0)
    {
        device_close();
        return -1;
    }

    return 0;
}

static void supervise(const options_t *opts, int file_input)
{
    struct timespec t_start;
    int timed_out = 0;

    usleep(100000);
    for (int i = 0; i < opts->channel_count; i++)
        pthread_create(&outputs[i].thread, NULL, output_thread_fn, &outputs[i]);
    for (int i = 0; i < opts->channel_count; i++)
        pthread_create(&demods[i].thread, NULL, demod_thread_fn, &demods[i]);
    pthread_create(
        &device.thread, NULL, file_input ? file_input_thread_fn : device_thread_fn, &device
    );

    clock_gettime(CLOCK_MONOTONIC, &t_start);

    while (!do_exit)
    {
        usleep(100000);
        if (opts->run_timeout > 0)
        {
            struct timespec now;
            clock_gettime(CLOCK_MONOTONIC, &now);
            double elapsed = (double)(now.tv_sec - t_start.tv_sec) +
                             (double)(now.tv_nsec - t_start.tv_nsec) / 1e9;
            if (elapsed >= opts->run_timeout)
            {
                fprintf(stderr, "\nTimeout reached, exiting...\n");
                timed_out = 1;
                do_exit = 1;
            }
        }
    }
    if (!timed_out)
        fprintf(stderr, "\nUser cancel, exiting...\n");
}

static void teardown(const options_t *opts, int file_input)
{
    device_signal_exit();
    pthread_join(device.thread, NULL);

    /* Stop the producer completely before draining consumers: RSP
     * streaming runs on API-internal callback threads that only end at
     * Uninit, and no recording/output file may be closed or demod joined
     * while a callback can still be mid-delivery. */
    if (!file_input)
        device_stream_stop();

    if (device.record_file)
    {
        fclose(device.record_file);
        device.record_file = NULL;
        fprintf(stderr, "IQ recording saved to %s\n", device.record_path);
    }

    for (int i = 0; i < opts->channel_count; i++)
        safe_cond_signal(&demods[i].ready, &demods[i].ready_m);
    for (int i = 0; i < opts->channel_count; i++)
        pthread_join(demods[i].thread, NULL);
    for (int i = 0; i < opts->channel_count; i++)
        safe_cond_signal(&outputs[i].ready, &outputs[i].ready_m);
    for (int i = 0; i < opts->channel_count; i++)
        pthread_join(outputs[i].thread, NULL);

    for (int i = 0; i < opts->channel_count; i++)
    {
        if (outputs[i].file && outputs[i].file != stdout)
            fclose(outputs[i].file);
        demod_cleanup(&demods[i]);
        output_cleanup(&outputs[i]);
    }

    if (!file_input)
        device_close();
}

int main(int argc, char **argv)
{
    struct sigaction sigact;
    options_t opts;
    int file_input, record;
    int downsample = 1, rate_channel = 0;

    device_init_state(&device);

    if (options_parse(argc, argv, &opts) != 0)
        return EXIT_FAILURE;

    if (opts.channel_count == 0)
    {
        fprintf(stderr, "No frequencies specified. Use -f frequency\n");
        options_usage();
    }

    if (opts.channel_count >= FREQUENCIES_LIMIT)
    {
        fprintf(stderr, "Too many channels, maximum %i.\n", FREQUENCIES_LIMIT);
        return EXIT_FAILURE;
    }

    file_input = (opts.input_file[0] != '\0');
    record = (opts.record_file[0] != '\0');

    if (record && file_input)
    {
        fprintf(stderr, "ERROR: -R (recording) is not available with -I file input.\n");
        return EXIT_FAILURE;
    }

    /* Multi-channel exclusive features */
    if (opts.channel_count > 1)
    {
        options_print_channels(&opts);

        for (int i = 0; i < opts.channel_count; i++)
        {
            if (opts.channels[i].output_mode == OUTPUT_FILE &&
                strcmp(opts.channels[i].filename, "-") == 0)
            {
                fprintf(stderr, "ERROR: stdout not allowed in multi-channel mode.\n");
                fprintf(stderr, "       Use -O tcp:PORT or -O udp:HOST:PORT per channel.\n");
                return EXIT_FAILURE;
            }
        }
    }

    /* Apply device settings to device */
    freq_len = opts.channel_count;

    device.dev_index = opts.dev_index;
    device_apply_options(&opts, file_input);

    if (plan_capture(&opts, file_input, &downsample, &rate_channel) < 0)
        return EXIT_FAILURE;

    if (setup_pipelines(&opts, rate_channel, downsample) < 0)
        return EXIT_FAILURE;

    sigact.sa_handler = sighandler;
    sigemptyset(&sigact.sa_mask);
    sigact.sa_flags = 0;
    sigaction(SIGINT, &sigact, NULL);
    sigaction(SIGTERM, &sigact, NULL);
    sigaction(SIGQUIT, &sigact, NULL);
    sigaction(SIGPIPE, &sigact, NULL);

    if (file_input)
        bringup_file_input(&opts);
    else if (bringup_device(&opts, record, downsample) < 0)
        return EXIT_FAILURE;

    fprintf(stderr, "Output at %u Hz.\n", (uint32_t)rate_channel);
    fprintf(stderr, "Audio output at %u Hz.\n", (uint32_t)opts.rate_audio);

    supervise(&opts, file_input);
    teardown(&opts, file_input);
    return EXIT_SUCCESS;
}
