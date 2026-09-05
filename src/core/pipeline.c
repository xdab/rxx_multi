#include "pipeline.h"

#include "demod.h"
#include "dsp.h"

#include <string.h>

static void mute_input(struct iq_buffer *input)
{
    for (int i = 0; i < input->len; i++)
        input->samples[i] = 0.0f;
}

void pipeline_init(struct channel_pipeline *pipeline)
{
    memset(pipeline, 0, sizeof(*pipeline));
    pipeline->input_rate = DEFAULT_SAMPLE_RATE;
    pipeline->demod_rate = DEFAULT_SAMPLE_RATE;
    pipeline->output_rate = DEFAULT_OUTPUT_RATE;
    pipeline->downsample_factor = 1;
    pipeline->output_scale = 1.0f;
    pipeline->squelch_delay = 10;
    pipeline->demodulate = &demodulate_fm;
}

void pipeline_cleanup(struct channel_pipeline *pipeline)
{
    if (pipeline->audio_resampler != NULL)
        resamp_rrrf_destroy(pipeline->audio_resampler);
    if (pipeline->channel_decimator != NULL)
        firdecim_crcf_destroy(pipeline->channel_decimator);
    if (pipeline->deemph_filter != NULL)
        iirfilt_rrrf_destroy(pipeline->deemph_filter);
    if (pipeline->dc_block_filter != NULL)
        iirfilt_rrrf_destroy(pipeline->dc_block_filter);
    if (pipeline->frequency_shifter != NULL)
        nco_crcf_destroy(pipeline->frequency_shifter);
}

int pipeline_process(struct channel_pipeline *pipeline,
                     struct iq_buffer *input,
                     struct real_buffer *output)
{
    float signal_level = 0.0f;

    output->len = 0;
    if (pipeline == NULL || input == NULL || output == NULL || pipeline->demodulate == NULL)
        return -1;

    if (dsp_shift_frequency(pipeline, input) != 0)
        return -1;

    if (dsp_decimate_channel(pipeline, input) != 0)
        return -1;

    if (pipeline->squelch_level > 0)
    {
        signal_level = dsp_rms_complex(input->samples, input->len);
        if (signal_level < pipeline->squelch_level)
        {
            pipeline->squelch_hits++;
            mute_input(input);
        }
        else
        {
            pipeline->squelch_hits = 0;
        }
    }

    pipeline->demodulate(pipeline, input, output);
    if (pipeline->demodulate == &demodulate_raw)
        return 0;

    if (pipeline->deemph_enabled)
        dsp_apply_deemphasis(pipeline, output);

    if (pipeline->dc_block_enabled)
        dsp_apply_dc_block(pipeline, output);

    if (pipeline->demod_rate != pipeline->output_rate)
        return dsp_resample_output(pipeline, output);

    return 0;
}

int pipeline_is_squelched(const struct channel_pipeline *pipeline)
{
    if (pipeline == NULL || pipeline->squelch_level <= 0)
        return 0;

    return pipeline->squelch_hits > pipeline->squelch_delay;
}