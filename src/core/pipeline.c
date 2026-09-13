#include "pipeline.h"

#include "demod.h"
#include "dsp.h"
#include "thread.h"

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
    double t0;
    int status;

    output->len = 0;
    if (pipeline == NULL || input == NULL || output == NULL || pipeline->demodulate == NULL)
        return -1;

    t0 = mono_ts();
    if (dsp_shift_frequency(pipeline, input) != 0)
        return -1;
    pipeline->t_shift += mono_ts() - t0;

    t0 = mono_ts();
    if (dsp_decimate_channel(pipeline, input) != 0)
        return -1;
    pipeline->t_decim += mono_ts() - t0;

    t0 = mono_ts();
    pipeline->demodulate(pipeline, input, output);
    pipeline->t_demod += mono_ts() - t0;
    if (pipeline->demodulate == &demodulate_raw)
    {
        pipeline->chunks_processed++;
        return 0;
    }

    if (pipeline->deemph_enabled || pipeline->dc_block_enabled)
    {
        t0 = mono_ts();
        if (pipeline->deemph_enabled)
            dsp_apply_deemphasis(pipeline, output);
        if (pipeline->dc_block_enabled)
            dsp_apply_dc_block(pipeline, output);
        pipeline->t_iir += mono_ts() - t0;
    }

    if (pipeline->demod_rate != pipeline->output_rate)
    {
        t0 = mono_ts();
        status = dsp_resample_output(pipeline, output);
        pipeline->t_resamp += mono_ts() - t0;
        if (status == 0)
            pipeline->chunks_processed++;
        return status;
    }

    pipeline->chunks_processed++;
    return 0;
}
