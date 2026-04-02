#ifndef UTM_BACKEND_H
#define UTM_BACKEND_H

#include "vm_backend.h"

/* Get the UTM backend vtable. Returns NULL on non-macOS platforms. */
vm_backend_t *utm_backend_get(void);

#endif
