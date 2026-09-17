#ifndef DEMOD_H
#define DEMOD_H

/**
 * @file demod.h
 * @brief Demodulator entry points selected via pipeline->demodulate.
 *
 * All share the demodulate_fn signature: one decimated baseband chunk
 * in, real samples out at the demod rate (raw mode doubles the count
 * by interleaving I/Q).
 */

#include "types.h"

/**
 * @brief FM polar discriminator; one audio sample per IQ sample.
 *
 * Phase continuity across chunk boundaries is carried in
 * pipeline->prev_sample.
 *
 * @param[in,out] pipeline Channel pipeline; reads/updates prev_sample.
 * @param[in] input Decimated baseband chunk, not modified.
 * @param[out] output Demodulated audio, same length as the input.
 */
void demodulate_fm(
    struct channel_pipeline *pipeline, const struct iq_buffer *input, struct real_buffer *output);

/**
 * @brief AM envelope detector (magnitude), scaled by pipeline->output_scale.
 *
 * @param[in] pipeline Channel pipeline; carries output_scale.
 * @param[in] input Decimated baseband chunk, not modified.
 * @param[out] output Demodulated audio, same length as the input.
 */
void demodulate_am(
    struct channel_pipeline *pipeline, const struct iq_buffer *input, struct real_buffer *output);

/**
 * @brief USB phasing detector (I + Q), scaled by pipeline->output_scale.
 *
 * Expects the channel already shifted so the signal sits at baseband.
 *
 * @param[in] pipeline Channel pipeline; carries output_scale.
 * @param[in] input Decimated baseband chunk, not modified.
 * @param[out] output Demodulated audio, same length as the input.
 */
void demodulate_usb(
    struct channel_pipeline *pipeline, const struct iq_buffer *input, struct real_buffer *output);

/**
 * @brief LSB phasing detector (I - Q), scaled by pipeline->output_scale.
 *
 * Expects the channel already shifted so the signal sits at baseband.
 *
 * @param[in] pipeline Channel pipeline; carries output_scale.
 * @param[in] input Decimated baseband chunk, not modified.
 * @param[out] output Demodulated audio, same length as the input.
 */
void demodulate_lsb(
    struct channel_pipeline *pipeline, const struct iq_buffer *input, struct real_buffer *output);

/**
 * @brief Raw IQ passthrough; interleaves I/Q into the real buffer.
 *
 * Produces 2 output samples per input sample. pipeline_process() skips
 * de-emphasis, DC block and resampling in this mode.
 *
 * @param[in] pipeline Unused.
 * @param[in] input Decimated baseband chunk, not modified.
 * @param[out] output Interleaved I/Q, twice the input length.
 */
void demodulate_raw(
    struct channel_pipeline *pipeline, const struct iq_buffer *input, struct real_buffer *output);

#endif /* DEMOD_H */
