#ifndef LIBVIRT_BACKEND_H
#define LIBVIRT_BACKEND_H

#include "vm_backend.h"

/* Get the libvirt backend vtable. */
vm_backend_t *libvirt_backend_get(void);

#endif
