#ifndef DATAROUTER_SIGNAL_H
#define DATAROUTER_SIGNAL_H

#include "datarouter_internal.h"

int datarouter_signal_init(datarouter_signal *signal);
void datarouter_signal_post(datarouter_signal *signal);
int datarouter_signal_wait(datarouter_signal *signal, uint32_t timeout_ms);

#endif
