#include "vm_backend.h"

static vm_backend_t *active_backend = NULL;

vm_backend_t *backend_get(void) {
    return active_backend;
}

void backend_set(vm_backend_t *be) {
    active_backend = be;
}
