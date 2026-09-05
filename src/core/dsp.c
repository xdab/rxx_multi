#include "dsp.h"
#include <liquid/liquid.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

int dsp_shift_frequency(struct channel_pipeline *pipeline, struct iq_buffer *buffer)
{
    if (pipeline == NULL || buffer == NULL || buffer->len <= 0 || !pipeline->frequency_shift_enabled)
        return 0;

    if (pipeline->frequency_shifter == NULL)
    {
        pipeline->frequency_shifter = nco_crcf_create(LIQUID_NCO);
        if (pipeline->frequency_shifter == NULL)
        {
            fprintf(stderr, "Failed to create NCO for channel shift\n");
            buffer->len = 0;
            return -1;
        }
        nco_crcf_set_frequency(pipeline->frequency_shifter, pipeline->phase_inc);
    }

    static _Thread_local float complex shifted[MAXIMUM_IQ_LENGTH];
    nco_crcf_mix_block_down(pipeline->frequency_shifter,
                            buffer->samples,
                            shifted,
                            (unsigned int)buffer->len);
    memcpy(buffer->samples, shifted, sizeof(shifted[0]) * (size_t)buffer->len);
    return 0;
}

int dsp_decimate_channel(struct channel_pipeline *pipeline, struct iq_buffer *buffer)
{
    /*
     * Anti-aliasing complex decimation stage.
     * - Purpose: remove spectral content above the new Nyquist (fs_out/2)
     *   before downsampling so high-frequency energy does not alias into
     *   the passband.
     * - Behavior: for integer decimation factors we use Liquid-DSP's
     *   `firdecim_crcf` block API to process M-sample blocks efficiently.
     */
    if (pipeline == NULL || buffer == NULL || buffer->len <= 0)
        return 0;

    if (pipeline->downsample_factor <= 1)
        return 0;

    unsigned int M = (unsigned int)pipeline->downsample_factor;
    float As = 40.0f;
    unsigned int m = 6; /* prototype filter delay */
    static _Thread_local float complex temp_out[MAXIMUM_IQ_LENGTH];

    if (M > sizeof(pipeline->decimator_tail) / sizeof(pipeline->decimator_tail[0]))
    {
        fprintf(stderr, "Unsupported downsample factor: %u\n", M);
        buffer->len = 0;
        return -1;
    }

    if (pipeline->channel_decimator == NULL)
    {
        pipeline->channel_decimator = firdecim_crcf_create_kaiser(M, m, As);
        if (pipeline->channel_decimator == NULL)
        {
            fprintf(stderr, "Failed to create firdecim_crcf for M=%u\n", M);
            buffer->len = 0;
            return -1;
        }
    }

    unsigned int in_len = (unsigned int)buffer->len;
    if (in_len == 0)
    {
        buffer->len = 0;
        return 0;
    }

    unsigned int out_len = 0;
    unsigned int consumed = 0;

    if (pipeline->decimator_tail_len > 0)
    {
        unsigned int needed = M - pipeline->decimator_tail_len;

        if (in_len < needed)
        {
            memcpy(pipeline->decimator_tail + pipeline->decimator_tail_len,
                   buffer->samples,
                   sizeof(buffer->samples[0]) * (size_t)in_len);
            pipeline->decimator_tail_len += in_len;
            buffer->len = 0;
            return 0;
        }

        memcpy(pipeline->decimator_tail + pipeline->decimator_tail_len,
               buffer->samples,
               sizeof(buffer->samples[0]) * (size_t)needed);

        firdecim_crcf_execute(pipeline->channel_decimator,
                              pipeline->decimator_tail,
                              &temp_out[out_len]);
        out_len++;
        pipeline->decimator_tail_len = 0;
        consumed = needed;
    }

    unsigned int remaining = in_len - consumed;
    unsigned int full_blocks = remaining / M;
    if (full_blocks > 0)
    {
        firdecim_crcf_execute_block(pipeline->channel_decimator,
                                    buffer->samples + consumed,
                                    full_blocks,
                                    temp_out + out_len);
        out_len += full_blocks;
        consumed += full_blocks * M;
    }

    remaining = in_len - consumed;
    if (remaining > 0)
        memcpy(pipeline->decimator_tail,
               buffer->samples + consumed,
               sizeof(buffer->samples[0]) * (size_t)remaining);
    pipeline->decimator_tail_len = remaining;

    memcpy(buffer->samples, temp_out, sizeof(temp_out[0]) * (size_t)out_len);
    buffer->len = (int)out_len;
    return 0;
}

int dsp_resample_output(struct channel_pipeline *pipeline, struct real_buffer *buffer)
{
    float r;
    unsigned int in_len;

    if (pipeline == NULL || buffer == NULL)
        return -1;

    r = (float)pipeline->output_rate / (float)pipeline->demod_rate;
    in_len = (unsigned int)buffer->len;
    if (in_len == 0)
    {
        buffer->len = 0;
        return 0;
    }

    unsigned int max_out = (unsigned int)ceilf(in_len * fabsf(r)) + 16;
    static _Thread_local float temp_buf[MAXIMUM_BUF_LENGTH];
    if (max_out > MAXIMUM_BUF_LENGTH)
        max_out = MAXIMUM_BUF_LENGTH;

    unsigned int m = 10;
    float bw = 0.45f;
    float As = 40.0f;
    unsigned int npfb = 32;
    if (pipeline->audio_resampler == NULL)
        pipeline->audio_resampler = resamp_rrrf_create(r, m, bw, As, npfb);

    if (pipeline->audio_resampler == NULL)
        return -1;

    unsigned int out_len = 0;
    resamp_rrrf_execute_block(pipeline->audio_resampler,
                              buffer->samples,
                              in_len,
                              temp_buf,
                              &out_len);

    unsigned int copy_len = out_len;
    if (copy_len > MAXIMUM_BUF_LENGTH)
        copy_len = MAXIMUM_BUF_LENGTH;

    for (unsigned int i = 0; i < copy_len; i++)
        buffer->samples[i] = temp_buf[i];

    buffer->len = (int)copy_len;
    return 0;
}

float dsp_rms_complex(const float complex *samples, int len)
{
    if (len <= 0)
        return 0.0f;

    float power = 0.0f;
    float sum_i = 0.0f;
    float sum_q = 0.0f;
    for (int i = 0; i < len; i++)
    {
        float re = crealf(samples[i]);
        float im = cimagf(samples[i]);
        sum_i += re;
        sum_q += im;
        power += re * re + im * im;
    }

    float avg_i = sum_i / (float)len;
    float avg_q = sum_q / (float)len;
    float dc_power = avg_i * avg_i + avg_q * avg_q;
    float variance = (power / (float)len) - dc_power;
    if (variance < 0.0f)
        variance = 0.0f;

    return sqrtf(variance * 0.5f);
}

void dsp_apply_deemphasis(struct channel_pipeline *pipeline, struct real_buffer *buffer)
{
    if (pipeline == NULL || buffer == NULL || !pipeline->deemph_enabled || buffer->len <= 0)
        return;

    /* Create a standard single-pole de-emphasis filter if needed */
    if (pipeline->deemph_filter == NULL)
    {
        float tau = 75e-6f;
        float Fs = (pipeline->demod_rate > 0) ? (float)pipeline->demod_rate : (float)pipeline->output_rate;
        float d;
        float b0;

        if (pipeline->deemph_alpha > 0.0f)
        {
            b0 = pipeline->deemph_alpha;
            d = 1.0f - b0;
        }
        else
        {
            d = expf(-1.0f / (tau * Fs));
            b0 = 1.0f - d;
        }

        float b[2] = {b0, 0.0f};
        float a[2] = {1.0f, -d};
        pipeline->deemph_filter = iirfilt_rrrf_create(b, 2, a, 2);
        if (pipeline->deemph_filter == NULL)
        {
            fprintf(stderr, "Failed to create de-emphasis filter\n");
            return;
        }
    }

    unsigned int n = (unsigned int)buffer->len;
    if (n > MAXIMUM_BUF_LENGTH)
        n = MAXIMUM_BUF_LENGTH;
    static _Thread_local float tmp_out[MAXIMUM_BUF_LENGTH];
    iirfilt_rrrf_execute_block(pipeline->deemph_filter, buffer->samples, n, tmp_out);
    for (unsigned int i = 0; i < n; i++)
        buffer->samples[i] = tmp_out[i];
}

void dsp_apply_dc_block(struct channel_pipeline *pipeline, struct real_buffer *buffer)
{
    if (pipeline == NULL || buffer == NULL || !pipeline->dc_block_enabled || buffer->len <= 0)
        return;

    if (pipeline->dc_block_filter == NULL)
    {
        float alpha = 0.02f;
        pipeline->dc_block_filter = iirfilt_rrrf_create_dc_blocker(alpha);
        if (pipeline->dc_block_filter == NULL)
        {
            fprintf(stderr, "Failed to create DC-block filter\n");
            return;
        }
    }

    unsigned int n = (unsigned int)buffer->len;
    if (n > MAXIMUM_BUF_LENGTH)
        n = MAXIMUM_BUF_LENGTH;
    static _Thread_local float tmp_out2[MAXIMUM_BUF_LENGTH];
    iirfilt_rrrf_execute_block(pipeline->dc_block_filter, buffer->samples, n, tmp_out2);
    for (unsigned int i = 0; i < n; i++)
        buffer->samples[i] = tmp_out2[i];
}

float dsp_polar_discriminant(float complex current, float complex previous)
{
    float complex delta = current * conjf(previous);
    return atan2f(cimagf(delta), crealf(delta)) / M_PI * (float)(1 << 14);
}
