#include "demod.h"
#include "dsp.h"

void demodulate_fm(struct channel_pipeline *pipeline,
                   const struct iq_buffer *input,
                   struct real_buffer *output)
{
    if (input->len < 1)
    {
        output->len = 0;
        return;
    }

    const float complex *samples = input->samples;
    output->samples[0] = dsp_polar_discriminant(samples[0], pipeline->prev_sample);

    for (int i = 1; i < input->len; i++)
        output->samples[i] = dsp_polar_discriminant(samples[i], samples[i - 1]);

    pipeline->prev_sample = samples[input->len - 1];
    output->len = input->len;
}

void demodulate_am(struct channel_pipeline *pipeline,
                   const struct iq_buffer *input,
                   struct real_buffer *output)
{
    const float complex *samples = input->samples;
    float *out = output->samples;

    for (int i = 0; i < input->len; i++)
        out[i] = cabsf(samples[i]) * pipeline->output_scale;

    output->len = input->len;
}

void demodulate_usb(struct channel_pipeline *pipeline,
                    const struct iq_buffer *input,
                    struct real_buffer *output)
{
    const float complex *samples = input->samples;
    float *out = output->samples;

    for (int i = 0; i < input->len; i++)
        out[i] = (crealf(samples[i]) + cimagf(samples[i])) * pipeline->output_scale;

    output->len = input->len;
}

void demodulate_lsb(struct channel_pipeline *pipeline,
                    const struct iq_buffer *input,
                    struct real_buffer *output)
{
    const float complex *samples = input->samples;
    float *out = output->samples;

    for (int i = 0; i < input->len; i++)
        out[i] = (crealf(samples[i]) - cimagf(samples[i])) * pipeline->output_scale;

    output->len = input->len;
}

void demodulate_raw(struct channel_pipeline *pipeline,
                    const struct iq_buffer *input,
                    struct real_buffer *output)
{
    (void)pipeline;

    for (int i = 0; i < input->len; i++)
    {
        output->samples[2 * i] = crealf(input->samples[i]);
        output->samples[2 * i + 1] = cimagf(input->samples[i]);
    }

    output->len = input->len * 2;
}
