#ifndef DEMOD_H
#define DEMOD_H

#include "types.h"

void demodulate_fm(
    struct channel_pipeline *pipeline, const struct iq_buffer *input, struct real_buffer *output
);
void demodulate_am(
    struct channel_pipeline *pipeline, const struct iq_buffer *input, struct real_buffer *output
);
void demodulate_usb(
    struct channel_pipeline *pipeline, const struct iq_buffer *input, struct real_buffer *output
);
void demodulate_lsb(
    struct channel_pipeline *pipeline, const struct iq_buffer *input, struct real_buffer *output
);
void demodulate_raw(
    struct channel_pipeline *pipeline, const struct iq_buffer *input, struct real_buffer *output
);

#endif /* DEMOD_H */
