#include "pipeline.h"

#include "demod.h"
#include "dsp.h"

#include <string.h>

void pipeline_init(struct channel_pipeline *pipeline)
{
    memset(pipeline, 0, sizeof(*pipeline));
    pipeline->input_rate = DEFAULT_SAMPLE_RATE;
    pipeline->demod_rate = DEFAULT_SAMPLE_RATE;
    pipeline->output_rate = DEFAULT_OUTPUT_RATE;
    pipeline->downsample_factor = 1;
    pipeline->output_scale = 1.0f;
    pipeline->demodulate = &demodulate_fm;
}

void pipeline_cleanup(struct channel_pipeline *pipeline)
{
    if (pipeline->audio_resampler != NULL)
        resamp_rrrf_destroy(pipeline->audio_resampler);
    free(pipeline->decim_taps);
    free(pipeline->decim_taps_pairs);
    if (pipeline->deemph_filter != NULL)
        iirfilt_rrrf_destroy(pipeline->deemph_filter);
    if (pipeline->dc_block_filter != NULL)
        iirfilt_rrrf_destroy(pipeline->dc_block_filter);
}

int pipeline_process(
    struct channel_pipeline *pipeline, const struct iq_buffer *input, struct real_buffer *output
)
{
    output->len = 0;
    if (pipeline == NULL || input == NULL || output == NULL || pipeline->demodulate == NULL)
        return -1;

    if (dsp_shift_frequency(pipeline, input) != 0)
        return -1;

    /* Shifted chunks are staged into the private work buffer; unshifted
     * ones are decimated straight from the shared slot */
    const struct iq_buffer *cur = input;
    if (pipeline->frequency_shift_enabled)
        cur = &pipeline->work;

    if (dsp_decimate_channel(pipeline, cur, &pipeline->work) != 0)
        return -1;

    pipeline->demodulate(pipeline, &pipeline->work, output);
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
