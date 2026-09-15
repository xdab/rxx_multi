#ifndef DSP_H
#define DSP_H

#include "types.h"

int dsp_init_filters(struct channel_pipeline *pipeline);
/* Shift the channel to baseband: reads the (shared) input chunk, writes
 * the NCO-multiplied result into pipeline->work. No-op (input passed
 * through untouched) when frequency shifting is disabled. */
int dsp_shift_frequency(struct channel_pipeline *pipeline,
                        const struct iq_buffer *input);
/* Streaming FIR decimator: reads input, writes the decimated chunk to
 * output (in-place safe: output == input is the historical behavior).
 * Never writes to input. */
int dsp_decimate_channel(struct channel_pipeline *pipeline,
                         const struct iq_buffer *input,
                         struct iq_buffer *output);
int dsp_resample_output(struct channel_pipeline *pipeline, struct real_buffer *buffer);
void dsp_apply_deemphasis(struct channel_pipeline *pipeline, struct real_buffer *buffer);
void dsp_apply_dc_block(struct channel_pipeline *pipeline, struct real_buffer *buffer);
float dsp_polar_discriminant(float complex current, float complex previous);

#endif /* DSP_H */
