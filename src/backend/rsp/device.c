/*
 * rsp_multi - SDRplay RSP backend (API v3)
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

#include "device.h"
#include <sdrplay_api.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* RSP backend limits (RTL parity: MAX_CAPTURE 2.4 MHz, rate gap snap) */
#define MIN_CAPTURE_RATE 2000000
#define MAX_CAPTURE_RATE 8000000
#define RSP_MAX_SNAP_RATE 4000000

/* Recommended manual gain defaults (lowest overall levels) */
#define DEFAULT_GRDB 59
#define DEFAULT_LNA_STATE 2

static sdrplay_api_DeviceT devices[SDRPLAY_MAX_DEVICES];
static unsigned int num_devices = 0;
static sdrplay_api_DeviceParamsT *dev_params = NULL;
static int api_open;
static int api_locked;
static int device_selected;
static int settings_applied;

sdrplay_api_DeviceT *device_handle(void)
{
    return device_selected ? &devices[device.dev_index] : NULL;
}

/* Open the API client itself (harmless if already open) */
static int device_ensure_open(void)
{
    sdrplay_api_ErrT err;
    float api_ver = 0.0f;

    if (api_open)
        return 0;

    err = sdrplay_api_Open();
    if (err != sdrplay_api_Success)
    {
        fprintf(stderr, "sdrplay_api_Open failed: %s\n"
                        "(is the sdrplay_apiService daemon running?)\n",
                sdrplay_api_GetErrorString(err));
        return -1;
    }
    api_open = 1;

    sdrplay_api_ApiVersion(&api_ver);
    fprintf(stderr, "SDRplay API v%.2f\n", api_ver);
    return 0;
}

static int device_refresh_list(void)
{
    sdrplay_api_ErrT err;

    if (device_ensure_open() != 0)
        return -1;

    err = sdrplay_api_LockDeviceApi();
    if (err != sdrplay_api_Success)
    {
        fprintf(stderr, "sdrplay_api_LockDeviceApi failed: %s\n",
                sdrplay_api_GetErrorString(err));
        return -1;
    }

    err = sdrplay_api_GetDevices(devices, &num_devices, SDRPLAY_MAX_DEVICES);
    if (err != sdrplay_api_Success)
    {
        fprintf(stderr, "sdrplay_api_GetDevices failed: %s\n",
                sdrplay_api_GetErrorString(err));
        sdrplay_api_UnlockDeviceApi();
        return -1;
    }

    sdrplay_api_UnlockDeviceApi();
    return 0;
}

int verbose_device_search(char *s)
{
    int i;
    char *s2;
    int found;

    if (device_refresh_list() != 0)
        return -1;

    if (num_devices == 0)
    {
        fprintf(stderr, "No supported devices found.\n");
        return -1;
    }

    fprintf(stderr, "Found %d device(s):\n", num_devices);
    for (i = 0; i < (int)num_devices; i++)
        fprintf(stderr, "  %d:  SN: %s\n", i, devices[i].SerNo);
    fprintf(stderr, "\n");

    /* does string look like raw id number */
    found = (int)strtol(s, &s2, 0);
    if (s2[0] == '\0' && found >= 0 && found < (int)num_devices)
    {
        fprintf(stderr, "Using device %d: SN %s\n", found, devices[found].SerNo);
        return found;
    }

    /* does string exact match a serial */
    for (i = 0; i < (int)num_devices; i++)
    {
        if (strcmp(s, devices[i].SerNo) == 0)
        {
            fprintf(stderr, "Using device %d: SN %s\n", i, devices[i].SerNo);
            return i;
        }
    }

    /* does string prefix match a serial */
    for (i = 0; i < (int)num_devices; i++)
    {
        if (strncmp(s, devices[i].SerNo, strlen(s)) == 0)
        {
            fprintf(stderr, "Using device %d: SN %s\n", i, devices[i].SerNo);
            return i;
        }
    }

    /* does string suffix match a serial */
    for (i = 0; i < (int)num_devices; i++)
    {
        int offset = (int)strlen(devices[i].SerNo) - (int)strlen(s);
        if (offset < 0)
            continue;
        if (strncmp(s, devices[i].SerNo + offset, strlen(s)) == 0)
        {
            fprintf(stderr, "Using device %d: SN %s\n", i, devices[i].SerNo);
            return i;
        }
    }

    fprintf(stderr, "No matching devices found.\n");
    return -1;
}

int verbose_set_frequency(uint32_t frequency)
{
    device.freq = frequency;
    fprintf(stderr, "Tuned to %u Hz.\n", frequency);
    return 0;
}

int verbose_set_sample_rate(uint32_t samp_rate)
{
    if (samp_rate < MIN_CAPTURE_RATE || samp_rate > MAX_CAPTURE_RATE)
    {
        fprintf(stderr,
                "ERROR: sample rate %u Hz outside RSP range %d-%d Hz.\n",
                samp_rate, MIN_CAPTURE_RATE, MAX_CAPTURE_RATE);
        return -1;
    }
    device.rate = samp_rate;
    fprintf(stderr, "Sampling at %u S/s.\n", samp_rate);
    return 0;
}

int verbose_reset_buffer(void)
{
    /* Nothing to reset on the SDRplay API */
    return 0;
}

int device_open(int dev_index)
{
    sdrplay_api_ErrT err;

    if (device_ensure_open() != 0)
        return -1;

    /* NOTE: do not memset device here - main sets gain/dev_index first */
    device.dev_index = dev_index;
    if (device.rate == 0)
        device.rate = MIN_CAPTURE_RATE;

    if (device_refresh_list() != 0)
        return -1;

    if (num_devices == 0)
    {
        fprintf(stderr, "no RSP devices found\n");
        return -1;
    }

    if (dev_index < 0 || (unsigned int)dev_index >= num_devices)
    {
        fprintf(stderr, "device index %d out of range\n", dev_index);
        return -1;
    }

    fprintf(stderr, "device: %s (hwVer %u)\n", devices[dev_index].SerNo,
            devices[dev_index].hwVer);

    err = sdrplay_api_SelectDevice(&devices[dev_index]);
    if (err != sdrplay_api_Success)
    {
        fprintf(stderr, "sdrplay_api_SelectDevice failed: %s\n",
                sdrplay_api_GetErrorString(err));
        return -1;
    }
    device_selected = 1;
    device.dev = &devices[dev_index];

    sdrplay_api_UnlockDeviceApi();
    api_locked = 0;

    err = sdrplay_api_GetDeviceParams(devices[dev_index].dev, &dev_params);
    if (err != sdrplay_api_Success || dev_params == NULL ||
        dev_params->devParams == NULL || dev_params->rxChannelA == NULL)
    {
        fprintf(stderr, "sdrplay_api_GetDeviceParams failed\n");
        return -1;
    }

    return 0;
}

int device_apply_settings(void)
{
    sdrplay_api_RxChannelParamsT *ch;
    unsigned int decim_factor = 1;

    if (!device_selected || dev_params == NULL)
        return -1;

    /* ADC always runs at 8 MHz; the API decimates down to device.rate.
     * This yields ~14-bit right-justified samples instead of the 8-bit
     * left-justified dribbles of direct low-rate ZIF. */
    if (device.rate <= 2000000)
        decim_factor = 4;
    else if (device.rate <= 4000000)
        decim_factor = 2;

    dev_params->devParams->fsFreq.fsHz = 8000000.0;

    ch = dev_params->rxChannelA;
    ch->tunerParams.rfFreq.rfHz = device.freq;
    /* Analog IF filter must cover the capture but not overflow it */
    if (device.rate <= 2000000)
        ch->tunerParams.bwType = sdrplay_api_BW_1_536;
    else if (device.rate <= 5000000)
        ch->tunerParams.bwType = sdrplay_api_BW_5_000;
    else if (device.rate <= 6000000)
        ch->tunerParams.bwType = sdrplay_api_BW_6_000;
    else if (device.rate <= 7000000)
        ch->tunerParams.bwType = sdrplay_api_BW_7_000;
    else
        ch->tunerParams.bwType = sdrplay_api_BW_8_000;
    ch->tunerParams.ifType = sdrplay_api_IF_Zero;
    ch->tunerParams.loMode = sdrplay_api_LO_Auto;

    if (device.agc)
    {
        ch->ctrlParams.agc.enable = sdrplay_api_AGC_CTRL_EN;
        ch->ctrlParams.agc.setPoint_dBfs = -30;
    }
    else
    {
        ch->ctrlParams.agc.enable = sdrplay_api_AGC_DISABLE;
    }
    ch->tunerParams.gain.gRdB = device.gain_rdb;
    ch->tunerParams.gain.LNAstate = (unsigned char)device.lna_state;

    if (decim_factor > 1)
    {
        ch->ctrlParams.decimation.enable = 1;
        ch->ctrlParams.decimation.decimationFactor = (unsigned char)decim_factor;
        ch->ctrlParams.decimation.wideBandSignal = 1;
    }

    settings_applied = 1;
    return 0;
}

int device_stream_start(sdrplay_api_CallbackFnsT *cb_fns)
{
    sdrplay_api_ErrT err;

    if (!settings_applied)
        return -1;

    err = sdrplay_api_Init(devices[device.dev_index].dev, cb_fns, NULL);
    if (err != sdrplay_api_Success)
    {
        fprintf(stderr, "sdrplay_api_Init failed: %s\n",
                sdrplay_api_GetErrorString(err));
        return -1;
    }

    fprintf(stderr, "tuned %.0f Hz @ %.2f MS/s\n", (double)device.freq,
            device.rate / 1e6);
    return 0;
}

void device_print_options(void)
{
    fprintf(stderr,
            "\t[-g RSP IF gain reduction, 0-59 dB (default: auto)]\n"
            "\t[-L RSP LNA state 0-2 on RSP1, 2 = max attenuation (default: auto)]\n"
            "\t    giving either -g or -L selects manual gain; no flags = API AGC\n");
}

int device_parse_option(int opt, const char *optarg, options_t *opts)
{
    (void)opts;

    switch (opt)
    {
    case 'g':
    {
        int v = atoi(optarg);
        if (v < 0 || v > 59)
        {
            fprintf(stderr, "ERROR: -g (IF gain reduction) must be 0-59 dB\n");
            return -1;
        }
        device.gain_rdb = v;
        return 0;
    }
    case 'L':
    {
        int v = atoi(optarg);
        if (v < 0 || v > 2)
        {
            fprintf(stderr, "ERROR: -L (LNA state) must be 0-2 on RSP1\n");
            return -1;
        }
        device.lna_state = v;
        return 0;
    }
    case 'T':
        fprintf(stderr, "WARNING: -T has no RSP equivalent yet; ignored.\n");
        return 0;
    case 'p':
        fprintf(stderr,
                "WARNING: -p has no RSP equivalent (API handles correction); ignored.\n");
        return 0;
    case 'E':
        fprintf(stderr,
                "WARNING: -E %s has no RSP equivalent; ignored.\n", optarg);
        return 0;
    default:
        return 1;
    }
}

void device_apply_options(options_t *opts, int file_input)
{
    (void)opts;

    /* Manual gain if either RSP gain control was given; fill the unset
     * control with the recommended default (min IF gain, max LNA att.) */
    if (device.gain_rdb >= 0 || device.lna_state >= 0)
    {
        if (file_input)
        {
            fprintf(stderr, "WARNING: -g/-L have no effect in IQ file input mode.\n");
            device.agc = 1;
        }
        else
        {
            device.agc = 0;
            if (device.gain_rdb < 0)
                device.gain_rdb = DEFAULT_GRDB;
            if (device.lna_state < 0)
                device.lna_state = DEFAULT_LNA_STATE;
            fprintf(stderr, "Manual gain: gRdB %d dB, LNA state %d\n",
                    device.gain_rdb, device.lna_state);
        }
    }
    else
    {
        device.agc = 1;
    }
}

int device_plan_capture(int file_input, uint32_t rate_in, uint64_t width,
                        struct capture_plan *plan)
{
    if (file_input)
    {
        plan->downsample = (int)(width / rate_in);
        if (plan->downsample < 1)
            plan->downsample = 1;
        if (plan->downsample > 256)
        {
            fprintf(stderr, "ERROR: required downsample factor %d too large.\n",
                    plan->downsample);
            return -1;
        }
        return 0;
    }

    if (width > MAX_CAPTURE_RATE)
    {
        fprintf(stderr,
                "ERROR: channel span + guard (%.1f kHz) exceeds max capture (%.0f kHz).\n",
                (float)width / 1000.0, (float)MAX_CAPTURE_RATE / 1000.0);
        return -1;
    }

    /* The RSP ADC runs at 8 MHz and the API decimates by powers of two,
     * so the capture output rate is one of {2, 4, 8} MHz. Snap up. */
    if (width <= MIN_CAPTURE_RATE)
        plan->rate = MIN_CAPTURE_RATE;
    else if (width <= RSP_MAX_SNAP_RATE)
        plan->rate = RSP_MAX_SNAP_RATE;
    else
        plan->rate = MAX_CAPTURE_RATE;
    if (plan->rate < width)
        fprintf(stderr,
                "WARNING: capture %u Hz snapped up from %.1f kHz request.\n",
                plan->rate, (float)width / 1000.0);

    plan->downsample = (int)(plan->rate / rate_in);
    if (plan->downsample < 1)
        plan->downsample = 1;
    if (plan->downsample > 256)
    {
        fprintf(stderr, "ERROR: required downsample factor %d too large.\n",
                plan->downsample);
        return -1;
    }
    return 0;
}

void device_signal_exit(void)
{
    /* Streaming runs on API-internal callback threads; they observe
     * do_exit on their own */
}

void device_stream_stop(void)
{
    if (device_selected)
        sdrplay_api_Uninit(devices[device.dev_index].dev);
}

void device_close(void)
{
    if (device_selected)
    {
        sdrplay_api_ReleaseDevice(&devices[device.dev_index]);
        device_selected = 0;
    }
    if (api_locked)
    {
        sdrplay_api_UnlockDeviceApi();
        api_locked = 0;
    }
    if (api_open)
    {
        sdrplay_api_Close();
        api_open = 0;
    }
}
