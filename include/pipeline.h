#ifndef PIPELINE_H
#define PIPELINE_H

#include "types.h"

void pipeline_init(struct channel_pipeline *pipeline);
void pipeline_cleanup(struct channel_pipeline *pipeline);
int pipeline_process(struct channel_pipeline *pipeline,
                     struct iq_buffer *input,
                     struct real_buffer *output);
int pipeline_is_squelched(const struct channel_pipeline *pipeline);

#endif /* PIPELINE_H */