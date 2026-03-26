#ifndef LIBVIRT_BACKEND_H
#define LIBVIRT_BACKEND_H

#include "vm_backend.h"

/* Get the libvirt backend vtable. */
vm_backend_t *libvirt_backend_get(void);

/* Get the last libvirt error message (thread-local). */
const char *lv_get_last_error(void);

#endif
