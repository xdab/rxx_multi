#ifndef DSP_H
#define DSP_H

#include "types.h"

int dsp_init_filters(struct channel_pipeline *pipeline);
int dsp_shift_frequency(struct channel_pipeline *pipeline, struct iq_buffer *buffer);
int dsp_decimate_channel(struct channel_pipeline *pipeline, struct iq_buffer *buffer);
int dsp_resample_output(struct channel_pipeline *pipeline, struct real_buffer *buffer);
void dsp_apply_deemphasis(struct channel_pipeline *pipeline, struct real_buffer *buffer);
void dsp_apply_dc_block(struct channel_pipeline *pipeline, struct real_buffer *buffer);
float dsp_polar_discriminant(float complex current, float complex previous);

#endif /* DSP_H */
