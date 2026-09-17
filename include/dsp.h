#ifndef DSP_H
#define DSP_H

#include "types.h"

/**
 * @brief Pre-create a channel's filter objects at setup time.
 *
 * Call after the pipeline's rates, decimation factor and mode flags are set.
 *
 * @param pipeline Channel pipeline to equip.
 *
 * @return 0 on success, -1 on failure.
 */
int dsp_init_filters(struct channel_pipeline *pipeline);

/**
 * @brief Shift the channel to baseband; no-op when frequency shifting is disabled.
 *
 * @param pipeline Channel pipeline; carries the DDS accumulator, output lands in pipeline->work.
 * @param input Shared input chunk, not modified.
 *
 * @return 0 on success, -1 on failure.
 */
int dsp_shift_frequency(struct channel_pipeline *pipeline, const struct iq_buffer *input);

/**
 * @brief Anti-aliasing streaming FIR decimator.
 *
 * @param pipeline Channel pipeline; carries the taps and the history tail.
 * @param input Input chunk, not modified.
 * @param output Decimated chunk; may alias input (in-place safe).
 *
 * @return 0 on success, -1 on failure.
 */
int dsp_decimate_channel(
    struct channel_pipeline *pipeline, const struct iq_buffer *input, struct iq_buffer *output);

/**
 * @brief Resample the demod-rate chunk to the audio output rate.
 *
 * @param pipeline Channel pipeline.
 * @param buffer Demod samples in; rewritten in place, len becomes the produced output count.
 *
 * @return 0 on success, -1 on failure.
 */
int dsp_resample_output(struct channel_pipeline *pipeline, struct real_buffer *buffer);

/**
 * @brief 75 us single-pole IIR de-emphasis; no-op unless enabled (-E deemp).
 */
void dsp_apply_deemphasis(struct channel_pipeline *pipeline, struct real_buffer *buffer);

/**
 * @brief DC blocker; no-op unless enabled (-E dc).
 */
void dsp_apply_dc_block(struct channel_pipeline *pipeline, struct real_buffer *buffer);

/**
 * @brief FM polar discriminator.
 *
 * @param current Current complex sample.
 * @param previous Previous complex sample.
 *
 * @return Phase delta, scaled to +/- (1 << 14) at pi radians.
 */
float dsp_polar_discriminant(float complex current, float complex previous);

#endif /* DSP_H */
