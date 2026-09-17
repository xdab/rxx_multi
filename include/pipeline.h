#ifndef PIPELINE_H
#define PIPELINE_H

/**
 * @file pipeline.h
 * @brief Per-channel DSP chain: staged processing of shared input chunks.
 */

#include "types.h"

/**
 * @brief Reset a channel pipeline to its defaults (12k/48k rates, FM demod, no filters).
 *
 * Does not allocate; filter objects are created lazily by dsp_init_filters().
 *
 * @param[out] pipeline Channel pipeline to initialize.
 */
void pipeline_init(struct channel_pipeline *pipeline);

/**
 * @brief Release the pipeline's liquid objects and filter taps.
 *
 * Safe to call on a pipeline whose filters were never created; every
 * handle is NULL-checked.
 *
 * @param[in,out] pipeline Channel pipeline to clean up.
 */
void pipeline_cleanup(struct channel_pipeline *pipeline);

/**
 * @brief Run one input chunk through the full demod chain.
 *
 * NCO shift -> FIR decimation -> demodulate -> de-emphasis -> DC block
 * -> audio resample. Raw mode stops after the demodulator; the other
 * stages run only when enabled and rates differ.
 *
 * @param[in,out] pipeline Channel pipeline to run; carries all stage state.
 * @param[in] input Shared input chunk, not modified.
 * @param[out] output Demodulated audio; len becomes the produced sample count.
 *
 * @retval 0 Success.
 * @retval -1 Failure (bad arguments or a stage failed); output->len is 0.
 */
int pipeline_process(
    struct channel_pipeline *pipeline, const struct iq_buffer *input, struct real_buffer *output);

#endif /* PIPELINE_H */
