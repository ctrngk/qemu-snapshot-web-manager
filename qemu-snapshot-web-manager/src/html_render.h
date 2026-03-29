#ifndef HTML_RENDER_H
#define HTML_RENDER_H

#include "vm_backend.h"

/* HTML-escape a string (malloc'd result, caller frees). */
char *html_escape(const char *raw);

/* Render the VM list as clickable items for the left panel.
 * Each item triggers selectVm(name) on click and loads snapshot tree.
 * Returns malloc'd HTML string. */
char *render_vm_list(vm_info_t **vms, int count);

/* Render snapshot detail panel for a selected snapshot.
 * Includes revert/delete/merge action buttons with HTMX attributes.
 * vm_name is needed to construct the API URLs.
 * vm_state controls button availability (e.g., delete disabled while running).
 * Returns malloc'd HTML string. */
char *render_snapshot_detail(const char *vm_name, snapshot_node_t *snap,
                             vm_state_t vm_state);

/* Render the create-snapshot form (shown in modal or detail panel).
 * Returns malloc'd HTML string. */
char *render_create_snapshot_form(const char *vm_name, int nvram_is_qcow2);

/* Render the shared folders list.
 * Returns malloc'd HTML string. */
char *render_shared_folders(const char *vm_name, shared_folder_t *folders, int count, int automount_active);

/* Render the add-shared-folder form.
 * Returns malloc'd HTML string. */
char *render_add_shared_folder_form(const char *vm_name);

/* Render a success message. Returns malloc'd HTML string. */
char *render_success(const char *message);

/* Render an error message. Returns malloc'd HTML string. */
char *render_error(const char *message);

/* Render an error message with raw HTML (no escaping). Returns malloc'd HTML string. */
char *render_error_html(const char *html_message);

/* Render a directory browser listing for the browse modal.
 * Returns malloc'd HTML string. */
char *render_directory_listing(const char *current_path, char **entries, int count);

/* Render orphan snapshot warning banner.
 * Shows count and names of orphaned snapshots with a cleanup button.
 * Returns malloc'd HTML string (empty string if no orphans). */
char *render_orphan_warning(const char *vm_name, char **orphan_names, int count);

/* Render guest-agent help message with OS-specific install instructions.
 * operation is the action that failed (e.g., "mount", "unmount", "auto-mount setup").
 * detail is the specific error from libvirt (may be NULL).
 * Returns malloc'd HTML string. */
char *render_guest_agent_help(const char *operation, const char *detail);

#endif
