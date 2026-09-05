/*
 * rtl_multi - RTL-SDR backend (librtlsdr)
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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <rtl-sdr.h>

#include "device.h"
#include "types.h"

#define MAX_TUNER_GAINS 128
#define STATIC_STRING_SIZE_LOCAL 256

/* RTL2832U supported capture ranges */
#define RTL_RATE_MIN_LOW 225001
#define RTL_RATE_MAX_LOW 300000
#define RTL_RATE_MIN_HIGH 900001
#define RTL_RATE_MAX_HIGH 3200000
#define RTL_MAX_CAPTURE 2400000ULL

static rtlsdr_dev_t *rtl_dev(void)
{
    return (rtlsdr_dev_t *)device.dev;
}

static int nearest_gain(int target_gain)
{
    int i, r, err1, err2, count, nearest;
    int gains[MAX_TUNER_GAINS];

    r = rtlsdr_set_tuner_gain_mode(rtl_dev(), 1);
    if (r < 0)
    {
        fprintf(stderr, "WARNING: Failed to enable manual gain.\n");
        return r;
    }

    count = rtlsdr_get_tuner_gains(rtl_dev(), NULL);
    if (count <= 0)
        return 0;

    if (count > MAX_TUNER_GAINS)
    {
        fprintf(stderr, "ERROR: tuner gains count %d exceeds MAX_TUNER_GAINS (%d)\n", count, MAX_TUNER_GAINS);
        exit(1);
    }

    count = rtlsdr_get_tuner_gains(rtl_dev(), gains);
    nearest = gains[0];

    for (i = 0; i < count; i++)
    {
        err1 = abs(target_gain - nearest);
        err2 = abs(target_gain - gains[i]);
        if (err2 < err1)
        {
            nearest = gains[i];
        }
    }

    return nearest;
}

int verbose_set_frequency(uint32_t frequency)
{
    int r;

    r = rtlsdr_set_center_freq(rtl_dev(), frequency);
    if (r < 0)
        fprintf(stderr, "ERROR: Failed to set center freq.\n");
    else
        fprintf(stderr, "Tuned to %u Hz.\n", frequency);

    return r;
}

int verbose_set_sample_rate(uint32_t samp_rate)
{
    int r;

    r = rtlsdr_set_sample_rate(rtl_dev(), samp_rate);
    if (r < 0)
        fprintf(stderr, "ERROR: Failed to set sample rate.\n");
    else
        fprintf(stderr, "Sampling at %u S/s.\n", samp_rate);

    return r;
}

int verbose_reset_buffer(void)
{
    int r;

    r = rtlsdr_reset_buffer(rtl_dev());
    if (r < 0)
        fprintf(stderr, "WARNING: Failed to reset buffers.\n");

    return r;
}

int verbose_device_search(char *s)
{
    int i, device_count, found, offset;
    char *s2;
    char vendor[STATIC_STRING_SIZE_LOCAL];
    char product[STATIC_STRING_SIZE_LOCAL];
    char serial[STATIC_STRING_SIZE_LOCAL];

    device_count = rtlsdr_get_device_count();
    if (!device_count)
    {
        fprintf(stderr, "No supported devices found.\n");
        return -1;
    }

    fprintf(stderr, "Found %d device(s):\n", device_count);
    for (i = 0; i < device_count; i++)
    {
        rtlsdr_get_device_usb_strings(i, vendor, product, serial);
        fprintf(stderr, "  %d:  %s, %s, SN: %s\n", i, vendor, product, serial);
    }
    fprintf(stderr, "\n");

    /* does string look like raw id number */
    found = (int)strtol(s, &s2, 0);
    if (s2[0] == '\0' && found >= 0 && found < device_count)
    {
        fprintf(stderr, "Using device %d: %s\n", found, rtlsdr_get_device_name((uint32_t)found));
        return found;
    }

    /* does string exact match a serial */
    for (i = 0; i < device_count; i++)
    {
        rtlsdr_get_device_usb_strings(i, vendor, product, serial);
        if (strcmp(s, serial) != 0)
        {
            continue;
        }
        fprintf(stderr, "Using device %d: %s\n", i, rtlsdr_get_device_name((uint32_t)i));
        return i;
    }

    /* does string prefix match a serial */
    for (i = 0; i < device_count; i++)
    {
        rtlsdr_get_device_usb_strings(i, vendor, product, serial);
        if (strncmp(s, serial, strlen(s)) != 0)
        {
            continue;
        }
        fprintf(stderr, "Using device %d: %s\n", i, rtlsdr_get_device_name((uint32_t)i));
        return i;
    }

    /* does string suffix match a serial */
    for (i = 0; i < device_count; i++)
    {
        rtlsdr_get_device_usb_strings(i, vendor, product, serial);
        offset = strlen(serial) - strlen(s);
        if (offset < 0)
        {
            continue;
        }
        if (strncmp(s, serial + offset, strlen(s)) != 0)
        {
            continue;
        }
        fprintf(stderr, "Using device %d: %s\n", i, rtlsdr_get_device_name((uint32_t)i));
        return i;
    }

    fprintf(stderr, "No matching devices found.\n");
    return -1;
}

void device_print_options(void)
{
    fprintf(stderr,
            "\t[-T enable bias-T on GPIO PIN 0 (works for rtl-sdr.com v3 dongles)]\n"
            "\t[-g tuner_gain (default: automatic)]\n"
            "\t[-p ppm_error (default: 0)]\n"
            "\t[-E also: direct/direct2 direct sampling (I/Q)]\n");
}

int device_parse_option(int opt, const char *optarg, options_t *opts)
{
    (void)opts;

    switch (opt)
    {
    case 'g':
        device.gain = (int)(atof(optarg) * 10); /* tenths of a dB */
        return 0;
    case 'p':
        device.ppm_error = atoi(optarg);
        return 0;
    case 'T':
        device.biastee = 1;
        return 0;
    case 'E':
        if (strcmp(optarg, "direct") == 0)
        {
            device.direct_sampling = 1;
            return 0;
        }
        if (strcmp(optarg, "direct2") == 0)
        {
            device.direct_sampling = 2;
            return 0;
        }
        return 1;
    default:
        return 1;
    }
}

void device_apply_options(options_t *opts, int file_input)
{
    (void)opts;

    if (file_input && device.gain != AUTO_GAIN)
        fprintf(stderr, "WARNING: -g has no effect in IQ file input mode.\n");
}

int device_plan_capture(int file_input, uint32_t rate_in, uint64_t width,
                        struct capture_plan *plan)
{
    plan->downsample = (int)(width / rate_in);
    if (plan->downsample < 1)
        plan->downsample = 1;
    if (plan->downsample > 256)
    {
        fprintf(stderr, "ERROR: required downsample factor %d too large.\n", plan->downsample);
        return -1;
    }
    plan->rate = (uint32_t)(plan->downsample * rate_in);

    if (file_input)
        return 0;

    if (plan->rate > RTL_RATE_MAX_HIGH || width > RTL_MAX_CAPTURE)
    {
        fprintf(stderr,
                "ERROR: channel span + guard (%.1f kHz) exceeds max capture (%.0f kHz).\n",
                (float)width / 1000.0, (float)RTL_MAX_CAPTURE / 1000.0);
        return -1;
    }

    /* RTL2832U only supports 225001-300000 and 900001-3200000 Hz.
     * Snap the rate to the nearest valid range if it falls in the gap. */
    if (plan->rate > RTL_RATE_MAX_LOW && plan->rate < RTL_RATE_MIN_HIGH)
    {
        fprintf(stderr,
                "WARNING: Computed rate %u Hz is in unsupported range (300k-900k). ",
                plan->rate);
        plan->downsample = (int)((RTL_RATE_MIN_HIGH + rate_in - 1) / rate_in); /* ceiling, >=900001 */
        plan->rate = (uint32_t)(plan->downsample * rate_in);
        fprintf(stderr, "Snapped to %u Hz (downsample %ix).\n", plan->rate, plan->downsample);
    }

    return 0;
}

int device_open(int dev_index)
{
    int r = rtlsdr_open((rtlsdr_dev_t **)&device.dev, (uint32_t)dev_index);
    if (r < 0)
        return -1;
    device.dev_index = dev_index;
    return 0;
}

int device_apply_settings(void)
{
    /* One-time controls; frequency/rate go through verbose_set_* */
    if (device.gain == AUTO_GAIN)
    {
        if (rtlsdr_set_tuner_gain_mode(rtl_dev(), 0) == 0)
            fprintf(stderr, "Tuner gain set to automatic.\n");
        else
            fprintf(stderr, "WARNING: Failed to set tuner gain.\n");
    }
    else
    {
        device.gain = nearest_gain(device.gain);
        rtlsdr_set_tuner_gain(rtl_dev(), device.gain);
        fprintf(stderr, "Tuner gain set to %0.2f dB.\n", device.gain / 10.0);
    }

    rtlsdr_set_bias_tee(rtl_dev(), device.biastee);
    if (device.biastee)
        fprintf(stderr, "activated bias-T on GPIO PIN 0\n");

    if (device.ppm_error != 0)
    {
        if (rtlsdr_set_freq_correction(rtl_dev(), device.ppm_error) < 0)
            fprintf(stderr, "WARNING: Failed to set ppm error.\n");
        else
            fprintf(stderr, "Tuner error set to %i ppm.\n", device.ppm_error);
    }

    if (device.direct_sampling)
    {
        if (rtlsdr_set_direct_sampling(rtl_dev(), device.direct_sampling) != 0)
        {
            fprintf(stderr, "WARNING: Failed to set direct sampling mode.\n");
            return -1;
        }
        if (device.direct_sampling == 1)
            fprintf(stderr, "Enabled direct sampling mode, input 1/I.\n");
        else if (device.direct_sampling == 2)
            fprintf(stderr, "Enabled direct sampling mode, input 2/Q.\n");
    }

    return 0;
}

void device_signal_exit(void)
{
    if (device.dev)
        rtlsdr_cancel_async(rtl_dev());
}

void device_stream_stop(void)
{
    /* rtlsdr_read_async returns on cancel; nothing to do here */
}

void device_close(void)
{
    if (device.dev)
    {
        rtlsdr_close(rtl_dev());
        device.dev = NULL;
    }
}
