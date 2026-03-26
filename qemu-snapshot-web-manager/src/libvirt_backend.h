#ifndef LIBVIRT_BACKEND_H
#define LIBVIRT_BACKEND_H

#include "vm_backend.h"

/* Get the libvirt backend vtable. */
vm_backend_t *libvirt_backend_get(void);

/* Get the last libvirt error message (thread-local). */
const char *lv_get_last_error(void);

/* Convert NVRAM from raw to qcow2 format (required for internal snapshots
 * on UEFI VMs). VM must be shut off. Returns 0=converted, 1=already qcow2, -1=error. */
int lv_convert_nvram(const char *vm_name);

#endif
