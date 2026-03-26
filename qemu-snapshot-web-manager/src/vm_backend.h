#ifndef VM_BACKEND_H
#define VM_BACKEND_H

#include "snapshot.h"

/* ─── VM state enum ─── */
typedef enum {
    VM_RUNNING,
    VM_PAUSED,
    VM_SHUTOFF,
    VM_OTHER
} vm_state_t;

/* ─── VM info (returned by list_vms) ─── */
typedef struct {
    char *name;
    char *uuid;
    vm_state_t state;
    int vcpus;
    unsigned long memory_kb;
} vm_info_t;

/* ─── Shared folder info ─── */
typedef struct {
    char *source_dir;     /* host path */
    char *mount_tag;      /* guest mount tag */
    char *fs_type;        /* "virtiofs" or "9p" */
    int read_only;
    int mounted;          /* 1 if currently mounted in guest, 0 otherwise, -1 unknown */
} shared_folder_t;

/* ─── Backend vtable ─── */
typedef struct vm_backend {
    const char *name;   /* "libvirt" or "utm" */

    /* Connection lifecycle */
    int  (*connect)(const char *uri);
    void (*disconnect)(void);

    /* VM listing */
    int  (*list_vms)(vm_info_t ***vms, int *count);
    void (*free_vm_list)(vm_info_t **vms, int count);

    /* VM lifecycle */
    int  (*vm_start)(const char *vm_name);
    int  (*vm_stop)(const char *vm_name);
    int  (*vm_pause)(const char *vm_name);

    /* Snapshot operations */
    int  (*list_snapshots)(const char *vm_name, snapshot_node_t **tree);
    int  (*create_snapshot)(const char *vm_name, const char *snap_name,
                            const char *description, snap_type_t type);
    int  (*delete_snapshot)(const char *vm_name, const char *snap_name,
                            int auto_merge);
    int  (*revert_snapshot)(const char *vm_name, const char *snap_name);
    int  (*merge_snapshot)(const char *vm_name, const char *snap_name);

    /* Shared folders */
    int  (*list_shared_folders)(const char *vm_name,
                                shared_folder_t **folders, int *count);
    void (*free_shared_folders)(shared_folder_t *folders, int count);

    /* Shared folder management */
    int  (*add_shared_folder)(const char *vm_name, const char *source_dir,
                              const char *mount_tag, int read_only,
                              const char *fs_type);  /* "virtiofs" or "9p" */
    int  (*remove_shared_folder)(const char *vm_name, const char *mount_tag);

    /* Mount/unmount shared folder inside guest via guest agent */
    int  (*mount_shared_folder)(const char *vm_name, const char *mount_tag,
                                 const char *fs_type);
    int  (*unmount_shared_folder)(const char *vm_name, const char *mount_tag);

    /* Check mount status inside guest via guest agent */
    int  (*check_mount_status)(const char *vm_name,
                                shared_folder_t *folders, int count);

    /* Auto-mount: check if auto-mount timer is installed in guest */
    int  (*check_automount_status)(const char *vm_name);  /* returns 1=active, 0=inactive, -1=error */

    /* Auto-mount: install auto-mount systemd service in guest */
    int  (*setup_automount)(const char *vm_name, char **out_msg);
} vm_backend_t;

/* ─── Backend registry ─── */

/* Get the currently active backend. Returns NULL if none set. */
vm_backend_t *backend_get(void);

/* Set the active backend. Called at startup. */
void backend_set(vm_backend_t *be);

/* Helper: convert vm_state_t to display string */
static inline const char *vm_state_str(vm_state_t s) {
    switch (s) {
    case VM_RUNNING: return "running";
    case VM_PAUSED:  return "paused";
    case VM_SHUTOFF: return "shut off";
    case VM_OTHER:   return "other";
    default:         return "unknown";
    }
}

#endif
