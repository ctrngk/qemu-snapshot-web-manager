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

/* Enable shared memory (memoryBacking) on a VM for VirtioFS support.
 * Returns 0=enabled, -1=error. Idempotent if already enabled. */
int lv_enable_shared_memory(const char *vm_name);

/* Edit snapshot description using libvirt REDEFINE.
 * Returns 0=success, -1=error. */
int lv_edit_snapshot_description(const char *vm_name, const char *snap_name,
                                 const char *new_description);

#endif
