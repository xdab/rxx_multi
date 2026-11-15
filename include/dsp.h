#ifndef DSP_H
#define DSP_H

/**
 * @file dsp.h
 * @brief Building blocks of the per-channel demod chain (one function per stage).
 *
 * Stages run in pipeline_process() order: shift -> decimate -> demodulate
 * (demod.h) -> de-emphasis -> DC block -> resample.
 */

#include "types.h"

/**
 * @brief Pre-create a channel's filter objects at setup time.
 *
 * Call after the pipeline's rates, decimation factor and mode flags are set.
 *
 * @param[out] pipeline Channel pipeline to equip.
 *
 * @retval 0 Success.
 * @retval -1 Filter setup failure.
 */
int dsp_init_filters(struct channel_pipeline *pipeline);

/**
 * @brief Shift the channel to baseband; no-op when frequency shifting is disabled.
 *
 * @param[in,out] pipeline Channel pipeline; carries the DDS accumulator, output
 *                        lands in pipeline->work.
 * @param[in] input Shared input chunk, not modified.
 *
 * @retval 0 Success.
 * @retval -1 Failure.
 */
int dsp_shift_frequency(struct channel_pipeline *pipeline, const struct iq_buffer *input);

/**
 * @brief Anti-aliasing streaming FIR decimator.
 *
 * History and phase carry across chunks, so outputs land at global
 * multiples of the decimation factor.
 *
 * @param[in,out] pipeline Channel pipeline; carries the taps and the history tail.
 * @param[in] input Input chunk, not modified.
 * @param[out] output Decimated chunk; may alias input (in-place safe).
 *
 * @retval 0 Success.
 * @retval -1 Failure.
 */
int dsp_decimate_channel(
    struct channel_pipeline *pipeline, const struct iq_buffer *input, struct iq_buffer *output);

/**
 * @brief Resample the demod-rate chunk to the audio output rate.
 *
 * @param[in,out] pipeline Channel pipeline; carries the resampler state.
 * @param[in,out] buffer Demod samples in; rewritten in place, len becomes the
 *                       produced output count.
 *
 * @retval 0 Success.
 * @retval -1 Failure.
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
 * @param[in] current Current complex sample.
 * @param[in] previous Previous complex sample.
 *
 * @return Phase delta, scaled to +/- (1 << 14) at pi radians.
 */
float dsp_polar_discriminant(float complex current, float complex previous);

/**
 * @brief Set up the shared pre-channelization resampler.
 *
 * Collapses the capture stream to shared_rate = K * rate_channel (K the
 * residual per-channel decimation factor) via one rational resampler
 * (P = K, Q = capture downsample) run once before the per-channel
 * fan-out. Engaged only when the capture rate is hardware-inflated AND
 * the predicted per-channel saving (decimator + NCO retimed to the
 * shared rate) exceeds the shared stage's cost at the current channel
 * count; otherwise the device is left in bypass (shared_rate = 0) and
 * the per-channel chain sees the capture rate unchanged.
 *
 * @param[out] dev Device state; receives shared_resamp, shared_rate,
 *                 chunk_samples and the staging buffers.
 * @param[in] rate_channel True per-channel rate (capture rate / downsample).
 * @param[in] width_shared Occupied span plus guards [Hz]; the resampler
 *                         passband covers half of it.
 *
 * @retval 0 Success.
 * @retval -1 Resampler setup failure.
 */
int dsp_shared_init(struct device_state *dev, uint32_t rate_channel, uint64_t width_shared);

/**
 * @brief Push full-rate capture samples through the shared resampler.
 *
 * Samples are staged into Q-blocks, resampled, and appended to the
 * shared-rate accumulator; the producer drains it into publish-sized
 * chunks (device.chunk_samples) afterwards. No-op in bypass mode.
 *
 * @param[in,out] dev Device state carrying the shared stage.
 * @param[in] in Full-rate samples (producer staging buffer), not modified.
 * @param[in] n Number of samples in @p in.
 */
void dsp_shared_push(struct device_state *dev, const float complex *in, unsigned n);

/**
 * @brief Free the shared resampler object; safe to call in bypass mode.
 */
void dsp_shared_cleanup(struct device_state *dev);

#endif /* DSP_H */
