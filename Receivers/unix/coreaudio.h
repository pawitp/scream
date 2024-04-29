#ifndef SCREAM_COREAUDIO_H
#define SCREAM_COREAUDIO_H

#include "scream.h"

int coreaudio_output_init(unsigned int max_latency_ms);
int coreaudio_output_send(receiver_data_t *data);

#endif
