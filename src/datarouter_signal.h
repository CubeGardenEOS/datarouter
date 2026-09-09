#ifndef DATAROUTER_SIGNAL_H
#define DATAROUTER_SIGNAL_H

#include "datarouter_internal.h"

int datarouter_signal_init(datarouter_signal *signal);
void datarouter_signal_post(datarouter_signal *signal);
/* Snapshot before testing the queue/mailbox predicate, then wait on that
 * snapshot. Notifications are coalesced hints, never a queued frame count. */
uint32_t datarouter_signal_prepare(const datarouter_signal *signal);
int datarouter_signal_wait_prepared(datarouter_signal *signal, uint32_t sequence, uint32_t timeout_ms);
int datarouter_signal_wait(datarouter_signal *signal, uint32_t timeout_ms);

#endif
