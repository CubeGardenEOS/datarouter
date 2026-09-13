/* Exercise the Switch's actual atomic pending-state scan without wall-clock
 * scheduling or a resident Switch process. */
#define main datarouter_switch_program_main
#include "../src/switch.c"
#undef main
#include <assert.h>

int main(void) {
    datarouter_shared_memory *shm = calloc(1, sizeof(*shm));
    assert(shm);
    shm->control.max_ports = DATAROUTER_MAX_PORTS;
    assert(!maintenance_pending(shm));
    for (uint32_t i = 0; i < DATAROUTER_REGISTRATION_REQUEST_SLOTS; ++i) {
        atomic_store(&shm->control.register_requests[i].claim_token, 1ULL);
        assert(maintenance_pending(shm)); /* unpublished writer */
        atomic_store(&shm->control.register_requests[i].claim_token, 0ULL);
        atomic_store(&shm->control.register_requests[i].state, DATAROUTER_REG_ABANDONED);
        assert(maintenance_pending(shm)); /* request awaiting recovery */
        atomic_store(&shm->control.register_requests[i].state, DATAROUTER_REG_EMPTY);
        assert(!maintenance_pending(shm));
    }
    for (uint32_t i = 0; i < DATAROUTER_MAX_PORTS; ++i) {
        atomic_store(&shm->control.registry[i].state, DATAROUTER_PORT_DRAINING);
        assert(maintenance_pending(shm));
        atomic_store(&shm->control.registry[i].state, DATAROUTER_PORT_RELEASING);
        assert(maintenance_pending(shm));
        atomic_store(&shm->control.registry[i].state, DATAROUTER_PORT_ACTIVE);
        assert(!maintenance_pending(shm));
    }
    free(shm);
    return 0;
}
