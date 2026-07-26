#ifndef DATAROUTER_SCHEDULER_H
#define DATAROUTER_SCHEDULER_H

#include "datarouter_internal.h"

void datarouter_scheduler_clear(datarouter_shared_memory *shm);
void datarouter_scheduler_mark_tx_ready(datarouter_shared_memory *shm, uint32_t slot_index);
uint32_t datarouter_scheduler_take_next_ready(datarouter_shared_memory *shm);
void datarouter_scheduler_mark_tx_blocked(datarouter_shared_memory *shm, uint32_t slot_index);
void datarouter_scheduler_clear_tx_blocked(datarouter_shared_memory *shm, uint32_t slot_index);
int datarouter_scheduler_has_tx_blocked(datarouter_shared_memory *shm);
uint32_t datarouter_scheduler_wake_blocked(datarouter_shared_memory *shm);
void datarouter_scheduler_forget_slot(datarouter_shared_memory *shm, uint32_t slot_index);
void datarouter_scheduler_mark_corrupt(datarouter_shared_memory *shm, uint32_t slot_index);
uint64_t datarouter_scheduler_take_corrupt(datarouter_shared_memory *shm);

#endif
