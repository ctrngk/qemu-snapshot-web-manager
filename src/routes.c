#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <dirent.h>
#include <sys/stat.h>
#include <limits.h>

#include <microhttpd.h>
#include "libvirt_backend.h"

#include "routes.h"
#include "util.h"
#include "snapshot.h"
#include "vm_backend.h"
#include "html_render.h"

/* ------------------------------------------------------------------ */
/*  request context for POST body accumulation                        */
/* ------------------------------------------------------------------ */

/* ------------------------------------------------------------------ */
/*  Per-VM dirty state tracking (in-memory)                           */
/*  After revert or create-snapshot → clean.                          */
/*  When VM is detected running → dirty.                              */
/*  Default for unknown shut-off VMs → clean.                         */
/* ------------------------------------------------------------------ */

#define MAX_TRACKED_VMS 128

static struct {
    char name[256];
    int  is_clean;   /* 1 = state matches a snapshot, 0 = diverged */
} vm_dirty_table[MAX_TRACKED_VMS];
static int vm_dirty_count = 0;

static void vm_mark_clean(const char *name)
{
    for (int i = 0; i < vm_dirty_count; i++) {
        if (strcmp(vm_dirty_table[i].name, name) == 0) {
            vm_dirty_table[i].is_clean = 1;
            return;
        }
    }
    if (vm_dirty_count < MAX_TRACKED_VMS) {
        snprintf(vm_dirty_table[vm_dirty_count].name,
                 sizeof(vm_dirty_table[0].name), "%s", name);
        vm_dirty_table[vm_dirty_count].is_clean = 1;
        vm_dirty_count++;
    }
}

static void vm_mark_dirty(const char *name)
{
    for (int i = 0; i < vm_dirty_count; i++) {
        if (strcmp(vm_dirty_table[i].name, name) == 0) {
            vm_dirty_table[i].is_clean = 0;
            return;
        }
    }
    if (vm_dirty_count < MAX_TRACKED_VMS) {
        snprintf(vm_dirty_table[vm_dirty_count].name,
                 sizeof(vm_dirty_table[0].name), "%s", name);
        vm_dirty_table[vm_dirty_count].is_clean = 0;
        vm_dirty_count++;
    }
}

/* Returns 1 if VM state has diverged from last snapshot. */
static int vm_is_dirty(const char *name, vm_state_t state)
{
    /* Running VMs are always dirty — actively accumulating changes */
    if (state == VM_RUNNING) {
        vm_mark_dirty(name);
        return 1;
    }

    /* Paused and shut-off VMs: check tracking table.
     * Paused VMs are not accumulating changes, so respect clean markers
     * (e.g., right after taking a snapshot while paused). */
    for (int i = 0; i < vm_dirty_count; i++) {
        if (strcmp(vm_dirty_table[i].name, name) == 0)
            return !vm_dirty_table[i].is_clean;
    }

    /* Not in table: paused VMs are dirty (state diverged from last snapshot),
     * shut-off VMs are clean (never started since server boot) */
    if (state == VM_PAUSED) {
        vm_mark_dirty(name);
        return 1;
    }
    return 0;
}

/* ------------------------------------------------------------------ */

typedef struct {
    char  *body;
    size_t body_size;
    size_t body_alloc;
} request_context_t;

static request_context_t *request_context_new(void)
{
    request_context_t *ctx = calloc(1, sizeof(*ctx));
    return ctx;
}

static void request_context_free(request_context_t *ctx)
{
    if (!ctx) return;
    free(ctx->body);
    free(ctx);
}

static int request_context_append(request_context_t *ctx,
                                  const char *data, size_t size)
{
    if (size == 0) return 0;
    size_t needed = ctx->body_size + size + 1;
    if (needed > ctx->body_alloc) {
        size_t new_alloc = (needed < 1024) ? 1024 : needed * 2;
        char *tmp = realloc(ctx->body, new_alloc);
        if (!tmp) return -1;
        ctx->body = tmp;
        ctx->body_alloc = new_alloc;
    }
    memcpy(ctx->body + ctx->body_size, data, size);
    ctx->body_size += size;
    ctx->body[ctx->body_size] = '\0';
    return 0;
}

/* ------------------------------------------------------------------ */
/*  response helpers                                                  */
/* ------------------------------------------------------------------ */

static enum MHD_Result send_html(struct MHD_Connection *conn,
                                 unsigned int status, const char *html)
{
    size_t len = strlen(html);
    char *buf = malloc(len);
    if (!buf) return MHD_NO;
    memcpy(buf, html, len);

    struct MHD_Response *resp =
        MHD_create_response_from_buffer(len, buf, MHD_RESPMEM_MUST_FREE);
    if (!resp) { free(buf); return MHD_NO; }

    MHD_add_response_header(resp, "Content-Type", "text/html; charset=utf-8");
    enum MHD_Result ret = MHD_queue_response(conn, status, resp);
    MHD_destroy_response(resp);
    return ret;
}

static enum MHD_Result send_json(struct MHD_Connection *conn,
                                 unsigned int status, const char *json)
{
    size_t len = strlen(json);
    char *buf = malloc(len);
    if (!buf) return MHD_NO;
    memcpy(buf, json, len);

    struct MHD_Response *resp =
        MHD_create_response_from_buffer(len, buf, MHD_RESPMEM_MUST_FREE);
    if (!resp) { free(buf); return MHD_NO; }

    MHD_add_response_header(resp, "Content-Type", "application/json");
    enum MHD_Result ret = MHD_queue_response(conn, status, resp);
    MHD_destroy_response(resp);
    return ret;
}

static enum MHD_Result send_404(struct MHD_Connection *conn)
{
    return send_html(conn, MHD_HTTP_NOT_FOUND,
                     "<html><body><h1>404 Not Found</h1></body></html>");
}

static enum MHD_Result send_405(struct MHD_Connection *conn)
{
    return send_html(conn, MHD_HTTP_METHOD_NOT_ALLOWED,
                     "<html><body><h1>405 Method Not Allowed</h1></body></html>");
}

/* ------------------------------------------------------------------ */
/*  HTML response with HX-Trigger header for HTMX mutation responses  */
/* ------------------------------------------------------------------ */

static enum MHD_Result send_html_trigger(struct MHD_Connection *conn,
                                         unsigned int status,
                                         const char *html,
                                         const char *trigger)
{
    size_t len = strlen(html);
    char *buf = malloc(len);
    if (!buf) return MHD_NO;
    memcpy(buf, html, len);

    struct MHD_Response *resp =
        MHD_create_response_from_buffer(len, buf, MHD_RESPMEM_MUST_FREE);
    if (!resp) { free(buf); return MHD_NO; }

    MHD_add_response_header(resp, "Content-Type", "text/html; charset=utf-8");
    if (trigger)
        MHD_add_response_header(resp, "HX-Trigger", trigger);
    enum MHD_Result ret = MHD_queue_response(conn, status, resp);
    MHD_destroy_response(resp);
    return ret;
}

/* ------------------------------------------------------------------ */
/*  form body parser                                                  */
/* ------------------------------------------------------------------ */

static char *form_value(const char *body, const char *key)
{
    if (!body || !key) return NULL;
    size_t klen = strlen(key);
    const char *p = body;
    while ((p = strstr(p, key)) != NULL) {
        /* ensure we're at start of body or preceded by '&' */
        if (p != body && *(p - 1) != '&') { p += klen; continue; }
        if (p[klen] != '=') { p += klen; continue; }
        /* found "key=" at valid position */
        const char *val = p + klen + 1;
        const char *end = strchr(val, '&');
        size_t vlen = end ? (size_t)(end - val) : strlen(val);
        char *raw = malloc(vlen + 1);
        if (!raw) return NULL;
        memcpy(raw, val, vlen);
        raw[vlen] = '\0';
        char *decoded = url_decode(raw);
        free(raw);
        return decoded;
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/*  route handlers                                                    */
/* ------------------------------------------------------------------ */

static enum MHD_Result handle_list_vms(struct MHD_Connection *conn)
{
    vm_backend_t *be = backend_get();
    vm_info_t **vms = NULL;
    int count = 0;
    if (!be || be->list_vms(&vms, &count) != 0) {
        char *html = render_error("Failed to list VMs");
        enum MHD_Result ret = send_html(conn, MHD_HTTP_INTERNAL_SERVER_ERROR, html);
        free(html);
        return ret;
    }
    char *html = render_vm_list(vms, count);
    enum MHD_Result ret = send_html(conn, MHD_HTTP_OK, html);
    free(html);
    be->free_vm_list(vms, count);
    return ret;
}

static enum MHD_Result handle_vm_start(struct MHD_Connection *conn,
                                       const char *vm_name)
{
    vm_backend_t *be = backend_get();
    if (!be || be->vm_start(vm_name) != 0) {
        const char *err = lv_get_last_error();
        char msg[512];
        snprintf(msg, sizeof(msg),
            "Failed to start VM: %s", err[0] ? err : "unknown error");
        char *html = render_error(msg);
        enum MHD_Result ret = send_html_trigger(conn, MHD_HTTP_INTERNAL_SERVER_ERROR,
                                                html, "vmStateChanged");
        free(html);
        return ret;
    }
    char *html = render_success("VM started");
    enum MHD_Result ret = send_html_trigger(conn, MHD_HTTP_OK, html, "vmStateChanged");
    free(html);
    return ret;
}

static enum MHD_Result handle_vm_stop(struct MHD_Connection *conn,
                                      const char *vm_name)
{
    vm_backend_t *be = backend_get();
    if (!be || be->vm_stop(vm_name) != 0) {
        const char *err = lv_get_last_error();
        char msg[512];
        snprintf(msg, sizeof(msg),
            "Failed to stop VM: %s", err[0] ? err : "unknown error");
        char *html = render_error(msg);
        enum MHD_Result ret = send_html_trigger(conn, MHD_HTTP_INTERNAL_SERVER_ERROR,
                                                html, "vmStateChanged");
        free(html);
        return ret;
    }
    char *html = render_success("VM stopped");
    enum MHD_Result ret = send_html_trigger(conn, MHD_HTTP_OK, html, "vmStateChanged");
    free(html);
    return ret;
}

static enum MHD_Result handle_vm_pause(struct MHD_Connection *conn,
                                       const char *vm_name)
{
    vm_backend_t *be = backend_get();
    if (!be || be->vm_pause(vm_name) != 0) {
        char *html = render_error("Failed to pause VM");
        enum MHD_Result ret = send_html_trigger(conn, MHD_HTTP_INTERNAL_SERVER_ERROR,
                                                html, "vmStateChanged");
        free(html);
        return ret;
    }
    char *html = render_success("VM paused");
    enum MHD_Result ret = send_html_trigger(conn, MHD_HTTP_OK, html, "vmStateChanged");
    free(html);
    return ret;
}

static enum MHD_Result handle_vm_resume(struct MHD_Connection *conn,
                                        const char *vm_name)
{
    vm_backend_t *be = backend_get();
    if (!be || !be->vm_resume || be->vm_resume(vm_name) != 0) {
        char *html = render_error("Failed to resume VM");
        enum MHD_Result ret = send_html_trigger(conn, MHD_HTTP_INTERNAL_SERVER_ERROR,
                                                html, "vmStateChanged");
        free(html);
        return ret;
    }
    char *html = render_success("VM resumed");
    enum MHD_Result ret = send_html_trigger(conn, MHD_HTTP_OK, html, "vmStateChanged");
    free(html);
    return ret;
}

static enum MHD_Result handle_vm_force_stop(struct MHD_Connection *conn,
                                            const char *vm_name)
{
    vm_backend_t *be = backend_get();
    if (!be || !be->vm_force_stop || be->vm_force_stop(vm_name) != 0) {
        char *html = render_error("Failed to force-stop VM");
        enum MHD_Result ret = send_html_trigger(conn, MHD_HTTP_INTERNAL_SERVER_ERROR,
                                                html, "vmStateChanged");
        free(html);
        return ret;
    }
    char *html = render_success("VM force-stopped");
    enum MHD_Result ret = send_html_trigger(conn, MHD_HTTP_OK, html, "vmStateChanged");
    free(html);
    return ret;
}

static enum MHD_Result handle_convert_nvram(struct MHD_Connection *conn,
                                            const char *vm_name)
{
    int rc = lv_convert_nvram(vm_name);
    if (rc == 1) {
        char *html = render_success("NVRAM is already in qcow2 format — no conversion needed");
        enum MHD_Result ret = send_html(conn, MHD_HTTP_OK, html);
        free(html);
        return ret;
    }
    if (rc != 0) {
        const char *err = lv_get_last_error();
        char msg[1200];
        snprintf(msg, sizeof(msg), "NVRAM conversion failed: %s", err);
        char *html = render_error(msg);
        enum MHD_Result ret = send_html(conn, MHD_HTTP_INTERNAL_SERVER_ERROR, html);
        free(html);
        return ret;
    }
    char *html = render_success("✓ NVRAM converted to qcow2. You can now start the VM and take internal snapshots.");
    enum MHD_Result ret = send_html_trigger(conn, MHD_HTTP_OK, html, "vmStateChanged");
    free(html);
    return ret;
}

/* ------------------------------------------------------------------ */
/*  orphan snapshot scan & cleanup                                     */
/* ------------------------------------------------------------------ */

static enum MHD_Result handle_orphan_check(struct MHD_Connection *conn,
                                            const char *vm_name)
{
    char **orphan_names = NULL;
    int orphan_count = 0;
    int rc = lv_scan_orphan_snapshots(vm_name, &orphan_names, &orphan_count);

    if (rc < 0) {
        /* Error scanning — return empty (don't show warning on error) */
        return send_html(conn, MHD_HTTP_OK, "");
    }

    char *html = render_orphan_warning(vm_name, orphan_names, orphan_count);

    for (int i = 0; i < orphan_count; i++) free(orphan_names[i]);
    free(orphan_names);

    enum MHD_Result ret = send_html(conn, MHD_HTTP_OK, html);
    free(html);
    return ret;
}

static enum MHD_Result handle_orphan_cleanup(struct MHD_Connection *conn,
                                              const char *vm_name)
{
    int cleaned = lv_cleanup_orphan_snapshots(vm_name);
    if (cleaned < 0) {
        const char *err = lv_get_last_error();
        char msg[512];
        snprintf(msg, sizeof(msg), "Cleanup failed: %s", err);
        char *html = render_error(msg);
        enum MHD_Result ret = send_html(conn, MHD_HTTP_INTERNAL_SERVER_ERROR, html);
        free(html);
        return ret;
    }

    if (cleaned == 0) {
        char *html = render_success("No orphan snapshots found");
        enum MHD_Result ret = send_html(conn, MHD_HTTP_OK, html);
        free(html);
        return ret;
    }

    char msg[128];
    snprintf(msg, sizeof(msg), "Cleaned %d orphan snapshot%s from disk files",
             cleaned, cleaned == 1 ? "" : "s");
    char *html = render_success(msg);
    enum MHD_Result ret = send_html_trigger(conn, MHD_HTTP_OK, html, "vmStateChanged");
    free(html);
    return ret;
}

static enum MHD_Result handle_snapshot_tree(struct MHD_Connection *conn,
                                            const char *vm_name)
{
    vm_backend_t *be = backend_get();
    snapshot_node_t *tree = NULL;
    if (!be || be->list_snapshots(vm_name, &tree) != 0) {
        return send_json(conn, MHD_HTTP_OK,
            "{\"id\":\"error\",\"name\":\"Error\","
            "\"description\":\"Failed to load snapshots\","
            "\"date\":\"\",\"type\":\"internal\","
            "\"isCurrent\":false,\"children\":[]}");
    }
    if (!tree) {
        return send_json(conn, MHD_HTTP_OK,
            "{\"id\":\"empty\",\"name\":\"No Snapshots\","
            "\"description\":\"Create your first snapshot\","
            "\"date\":\"\",\"type\":\"internal\","
            "\"isCurrent\":false,\"children\":[]}");
    }

    /* Get VM state and add "Current State" virtual node */
    vm_info_t **vms = NULL;
    int vcount = 0;
    if (be->list_vms(&vms, &vcount) == 0) {
        for (int i = 0; i < vcount; i++) {
            if (str_eq(vms[i]->name, vm_name)) {
                int is_dirty = vm_is_dirty(vm_name, vms[i]->state);
                snapshot_tree_add_current_state(tree,
                    vm_state_str(vms[i]->state), is_dirty);
                break;
            }
        }
        be->free_vm_list(vms, vcount);
    }

    char *json = snapshot_tree_to_json(tree);
    enum MHD_Result ret = send_json(conn, MHD_HTTP_OK, json);
    free(json);
    snapshot_tree_free(tree);
    return ret;
}

static enum MHD_Result handle_snapshot_detail(struct MHD_Connection *conn,
                                              const char *vm_name,
                                              const char *snap_name)
{
    vm_backend_t *be = backend_get();
    snapshot_node_t *tree = NULL;
    if (!be || be->list_snapshots(vm_name, &tree) != 0 || !tree) {
        char *html = render_error("Failed to load snapshots");
        enum MHD_Result ret = send_html(conn, MHD_HTTP_INTERNAL_SERVER_ERROR, html);
        free(html);
        return ret;
    }
    snapshot_node_t *snap = snapshot_tree_find(tree, snap_name);
    if (!snap) {
        snapshot_tree_free(tree);
        char *html = render_error("Snapshot not found");
        enum MHD_Result ret = send_html(conn, MHD_HTTP_NOT_FOUND, html);
        free(html);
        return ret;
    }

    /* Get VM state for button availability */
    vm_state_t vm_state = VM_OTHER;
    vm_info_t **vms = NULL;
    int vcount = 0;
    if (be->list_vms(&vms, &vcount) == 0) {
        for (int i = 0; i < vcount; i++) {
            if (str_eq(vms[i]->name, vm_name)) {
                vm_state = vms[i]->state;
                break;
            }
        }
        be->free_vm_list(vms, vcount);
    }

    char *html = render_snapshot_detail(vm_name, snap, vm_state);
    enum MHD_Result ret = send_html(conn, MHD_HTTP_OK, html);
    free(html);
    snapshot_tree_free(tree);
    return ret;
}

static enum MHD_Result handle_create_snapshot(struct MHD_Connection *conn,
                                              const char *vm_name,
                                              const char *body)
{
    vm_backend_t *be = backend_get();
    if (!be) {
        char *html = render_error("No backend available");
        enum MHD_Result ret = send_html_trigger(conn, MHD_HTTP_INTERNAL_SERVER_ERROR,
                                                html, "vmStateChanged");
        free(html);
        return ret;
    }

    char *name = form_value(body, "name");
    char *description = form_value(body, "description");
    char *type_str = form_value(body, "type");

    if (!name || strlen(name) == 0) {
        free(name); free(description); free(type_str);
        char *html = render_error("Snapshot name is required");
        enum MHD_Result ret = send_html_trigger(conn, MHD_HTTP_BAD_REQUEST,
                                                html, "vmStateChanged");
        free(html);
        return ret;
    }

    /* Replace spaces with hyphens — QEMU job IDs break on spaces
     * when taking internal snapshots of running VMs */
    for (char *p = name; *p; p++) {
        if (*p == ' ') *p = '-';
    }

    snap_type_t type = SNAP_INTERNAL;
    if (type_str && str_eq(type_str, "external"))
        type = SNAP_EXTERNAL;

    int rc = be->create_snapshot(vm_name, name, description ? description : "", type);
    free(name);
    free(description);
    free(type_str);

    if (rc != 0) {
        const char *err = lv_get_last_error();
        char msg[1300];
        if (strstr(err, "pflash") || strstr(err, "QCOW2 nvram") || strstr(err, "qcow2")) {
            snprintf(msg, sizeof(msg),
                "Internal snapshot requires QCOW2 NVRAM format. "
                "Stop the VM and click Convert NVRAM, then start it again."
                "<div style=\"margin-top:8px\">"
                "<button class=\"btn btn-sm btn-primary\""
                " hx-post=\"/api/vms/%s/convert-nvram\""
                " hx-target=\"#vm-notification\""
                " hx-swap=\"innerHTML\""
                " hx-confirm=\"Convert NVRAM from raw to qcow2? "
                "This is safe and required for internal snapshots on UEFI VMs. "
                "Your data will NOT be lost.\""
                ">\xf0\x9f\x94\xa7 Convert NVRAM</button>"
                "</div>",
                vm_name);
        } else if (strstr(err, "Invalid job ID")) {
            snprintf(msg, sizeof(msg),
                "Failed to create snapshot: QEMU rejected the snapshot name. "
                "Try a simpler name without special characters.");
        } else {
            snprintf(msg, sizeof(msg), "Failed to create snapshot: %s", err);
        }
        char *html = render_error(msg);
        enum MHD_Result ret = send_html_trigger(conn, MHD_HTTP_INTERNAL_SERVER_ERROR,
                                                html, "vmStateChanged");
        free(html);
        return ret;
    }
    char *html = render_success("Snapshot created");
    /* Creating a snapshot captures the current state — mark clean */
    vm_mark_clean(vm_name);
    enum MHD_Result ret = send_html_trigger(conn, MHD_HTTP_OK, html, "vmStateChanged");
    free(html);
    return ret;
}

/* GET /api/vms/{vm}/snapshots/{snap}/edit — return edit form */
static enum MHD_Result handle_edit_snapshot_form(struct MHD_Connection *conn,
                                                  const char *vm_name,
                                                  const char *snap_name)
{
    vm_backend_t *be = backend_get();
    snapshot_node_t *tree = NULL;
    if (!be || be->list_snapshots(vm_name, &tree) != 0 || !tree) {
        char *html = render_error("Failed to load snapshots");
        enum MHD_Result ret = send_html(conn, MHD_HTTP_INTERNAL_SERVER_ERROR, html);
        free(html);
        return ret;
    }
    snapshot_node_t *snap = snapshot_tree_find(tree, snap_name);
    if (!snap) {
        snapshot_tree_free(tree);
        char *html = render_error("Snapshot not found");
        enum MHD_Result ret = send_html(conn, MHD_HTTP_NOT_FOUND, html);
        free(html);
        return ret;
    }

    char *esc_name = html_escape(snap_name);
    char *esc_vm = html_escape(vm_name);
    char *esc_desc = html_escape(snap->description ? snap->description : "");

    char html[4096];
    snprintf(html, sizeof(html),
        "<div class='edit-snapshot-form'>"
        "<h3>Edit Description</h3>"
        "<p class='edit-snap-name'><strong>%s</strong></p>"
        "<form hx-put='/api/vms/%s/snapshots/%s'"
        "      hx-target='#snapshot-detail'"
        "      hx-swap='innerHTML'>"
        "<textarea name='description' rows='4' "
        "placeholder='Enter description...'>%s</textarea>"
        "<div class='form-actions'>"
        "<button type='submit' class='btn btn-sm btn-primary'>Save</button>"
        "<button type='button' class='btn btn-sm btn-secondary'"
        "        hx-get='/api/vms/%s/snapshots/%s'"
        "        hx-target='#snapshot-detail'"
        "        hx-swap='innerHTML'>Cancel</button>"
        "</div>"
        "</form>"
        "</div>",
        esc_name, esc_vm, esc_name, esc_desc, esc_vm, esc_name);

    free(esc_name);
    free(esc_vm);
    free(esc_desc);
    snapshot_tree_free(tree);

    return send_html(conn, MHD_HTTP_OK, html);
}

/* PUT /api/vms/{vm}/snapshots/{snap} — update description */
static enum MHD_Result handle_edit_snapshot(struct MHD_Connection *conn,
                                            const char *vm_name,
                                            const char *snap_name,
                                            const char *body)
{
    /* Parse description from form body */
    char *desc = form_value(body, "description");
    if (!desc) desc = str_dup("");  /* allow clearing description */

    if (lv_edit_snapshot_description(vm_name, snap_name, desc) != 0) {
        free(desc);
        const char *err = lv_get_last_error();
        char msg[512];
        snprintf(msg, sizeof(msg), "Failed to update description: %s", err);
        char *html = render_error(msg);
        enum MHD_Result ret = send_html(conn, MHD_HTTP_INTERNAL_SERVER_ERROR, html);
        free(html);
        return ret;
    }
    free(desc);

    /* Return success message + updated detail */
    vm_backend_t *be = backend_get();
    snapshot_node_t *tree = NULL;
    char *detail_html = NULL;

    if (be && be->list_snapshots(vm_name, &tree) == 0 && tree) {
        snapshot_node_t *snap = snapshot_tree_find(tree, snap_name);
        if (snap) {
            /* Get current VM state for button rendering */
            vm_state_t vs = VM_OTHER;
            vm_info_t **vms = NULL;
            int vc = 0;
            if (be->list_vms(&vms, &vc) == 0) {
                for (int i = 0; i < vc; i++) {
                    if (str_eq(vms[i]->name, vm_name)) { vs = vms[i]->state; break; }
                }
                be->free_vm_list(vms, vc);
            }
            detail_html = render_snapshot_detail(vm_name, snap, vs);
        }
        snapshot_tree_free(tree);
    }

    if (detail_html) {
        char *html = str_fmt(
            "<div class='alert success'>✅ Description updated</div>%s",
            detail_html);
        free(detail_html);
        /* Also trigger tree refresh so labels update */
        enum MHD_Result ret = send_html_trigger(conn, MHD_HTTP_OK, html,
                                                 "vmStateChanged");
        free(html);
        return ret;
    }

    char *html = render_success("Description updated");
    enum MHD_Result ret = send_html_trigger(conn, MHD_HTTP_OK, html, "vmStateChanged");
    free(html);
    return ret;
}

static enum MHD_Result handle_delete_snapshot(struct MHD_Connection *conn,
                                              const char *vm_name,
                                              const char *snap_name)
{
    vm_backend_t *be = backend_get();
    if (!be) {
        char *html = render_error("No backend available");
        enum MHD_Result ret = send_html(conn, MHD_HTTP_INTERNAL_SERVER_ERROR, html);
        free(html);
        return ret;
    }

    /* Block deleting internal snapshots while VM is running/paused.
     * libvirt only removes metadata — the actual data stays orphaned
     * inside the qcow2 file, causing "already exists" errors later. */
    vm_info_t **vms = NULL;
    int vcount = 0;
    if (be->list_vms(&vms, &vcount) == 0) {
        for (int i = 0; i < vcount; i++) {
            if (str_eq(vms[i]->name, vm_name)) {
                if (vms[i]->state == VM_RUNNING ||
                    vms[i]->state == VM_PAUSED) {
                    be->free_vm_list(vms, vcount);
                    char *html = render_error(
                        "Cannot delete snapshots while VM is running or paused. "
                        "Please shut down the VM first to fully remove snapshot data.");
                    enum MHD_Result ret = send_html_trigger(conn,
                        MHD_HTTP_BAD_REQUEST, html, "vmStateChanged");
                    free(html);
                    return ret;
                }
                break;
            }
        }
        be->free_vm_list(vms, vcount);
    }

    if (be->delete_snapshot(vm_name, snap_name, 1) != 0) {
        const char *err = lv_get_last_error();
        char msg[512];
        snprintf(msg, sizeof(msg), "Failed to delete snapshot: %s", err);
        char *html = render_error(msg);
        enum MHD_Result ret = send_html_trigger(conn, MHD_HTTP_INTERNAL_SERVER_ERROR,
                                                html, "vmStateChanged");
        free(html);
        return ret;
    }
    char *html = render_success("Snapshot deleted");
    enum MHD_Result ret = send_html_trigger(conn, MHD_HTTP_OK, html, "vmStateChanged");
    free(html);
    return ret;
}

static enum MHD_Result handle_revert_confirm(struct MHD_Connection *conn,
                                              const char *vm_name,
                                              const char *snap_name)
{
    vm_backend_t *be = backend_get();
    if (!be) {
        char *html = render_error("No backend available");
        return send_html(conn, MHD_HTTP_INTERNAL_SERVER_ERROR, html);
    }

    /* Check VM state */
    int is_running = 0;
    int is_dirty = 0;
    vm_info_t **vms = NULL;
    int vcount = 0;
    if (be->list_vms(&vms, &vcount) == 0) {
        for (int i = 0; i < vcount; i++) {
            if (str_eq(vms[i]->name, vm_name)) {
                is_running = (vms[i]->state == VM_RUNNING);
                is_dirty = vm_is_dirty(vm_name, vms[i]->state);
                break;
            }
        }
        be->free_vm_list(vms, vcount);
    }

    char *esc_vm   = html_escape(vm_name);
    char *esc_snap = html_escape(snap_name);
    char html[6144];

    if (is_running) {
        /* VM is running — must be stopped first */
        snprintf(html, sizeof(html),
            "<div class=\"revert-confirm\">\n"
            "  <h3>\xe2\x9a\xa0\xef\xb8\x8f VM is running</h3>\n"
            "  <p>Please shut down or pause the VM before reverting to a snapshot.</p>\n"
            "  <div class=\"snap-actions\">\n"
            "    <button class=\"btn btn-ghost\"\n"
            "            hx-get=\"/api/vms/%s/snapshots/%s\"\n"
            "            hx-target=\"#snapshot-detail\"\n"
            "            hx-swap=\"innerHTML\">\n"
            "      OK\n"
            "    </button>\n"
            "  </div>\n"
            "</div>\n",
            esc_vm, esc_snap);
    } else if (is_dirty) {
        /* VM is shut off but state has diverged from last snapshot */
        snprintf(html, sizeof(html),
            "<div class=\"revert-confirm\">\n"
            "  <h3>\xe2\x9a\xa0\xef\xb8\x8f Current state has unsaved changes</h3>\n"
            "  <p>The VM state has changed since the last snapshot. "
            "You can save the current state before reverting.</p>\n"
            "  <div class=\"snap-actions\">\n"
            "    <button class=\"btn btn-primary\"\n"
            "            hx-post=\"/api/vms/%s/snapshots/%s/revert?save_current=1\"\n"
            "            hx-target=\"#snapshot-detail\"\n"
            "            hx-swap=\"innerHTML\"\n"
            "            hx-indicator=\"#revert-loading\">\n"
            "      \xf0\x9f\x92\xbe Save &amp; Revert\n"
            "    </button>\n"
            "    <button class=\"btn btn-danger\"\n"
            "            hx-post=\"/api/vms/%s/snapshots/%s/revert\"\n"
            "            hx-target=\"#snapshot-detail\"\n"
            "            hx-swap=\"innerHTML\"\n"
            "            hx-indicator=\"#revert-loading\">\n"
            "      \xe2\x86\xa9 Revert Without Saving\n"
            "    </button>\n"
            "    <button class=\"btn btn-ghost\"\n"
            "            hx-get=\"/api/vms/%s/snapshots/%s\"\n"
            "            hx-target=\"#snapshot-detail\"\n"
            "            hx-swap=\"innerHTML\">\n"
            "      Cancel\n"
            "    </button>\n"
            "  </div>\n"
            "  <div id=\"revert-loading\" class=\"htmx-indicator snap-action-busy\">\n"
            "    <span class=\"spinner\"></span> Reverting\xe2\x80\xa6\n"
            "  </div>\n"
            "</div>\n",
            esc_vm, esc_snap, esc_vm, esc_snap, esc_vm, esc_snap);
    } else {
        /* VM is shut off and clean — simple confirmation */
        snprintf(html, sizeof(html),
            "<div class=\"revert-confirm\">\n"
            "  <h3>Revert to snapshot '%s'?</h3>\n"
            "  <p>This will restore the VM to the state when this snapshot was taken.</p>\n"
            "  <div class=\"snap-actions\">\n"
            "    <button class=\"btn btn-primary\"\n"
            "            hx-post=\"/api/vms/%s/snapshots/%s/revert\"\n"
            "            hx-target=\"#snapshot-detail\"\n"
            "            hx-swap=\"innerHTML\"\n"
            "            hx-indicator=\"#revert-loading\">\n"
            "      \xe2\x86\xa9 Revert\n"
            "    </button>\n"
            "    <button class=\"btn btn-ghost\"\n"
            "            hx-get=\"/api/vms/%s/snapshots/%s\"\n"
            "            hx-target=\"#snapshot-detail\"\n"
            "            hx-swap=\"innerHTML\">\n"
            "      Cancel\n"
            "    </button>\n"
            "  </div>\n"
            "  <div id=\"revert-loading\" class=\"htmx-indicator snap-action-busy\">\n"
            "    <span class=\"spinner\"></span> Reverting\xe2\x80\xa6\n"
            "  </div>\n"
            "</div>\n",
            esc_snap, esc_vm, esc_snap, esc_vm, esc_snap);
    }

    free(esc_vm);
    free(esc_snap);
    return send_html(conn, MHD_HTTP_OK, html);
}

static enum MHD_Result handle_revert_snapshot(struct MHD_Connection *conn,
                                              const char *vm_name,
                                              const char *snap_name)
{
    vm_backend_t *be = backend_get();
    if (!be) {
        char *html = render_error("No backend available");
        enum MHD_Result ret = send_html(conn, MHD_HTTP_INTERNAL_SERVER_ERROR, html);
        free(html);
        return ret;
    }

    /* Block revert while VM is running — paused/shut-off are OK
     * (VIR_DOMAIN_SNAPSHOT_REVERT_FORCE handles them) */
    vm_info_t **vms = NULL;
    int vcount = 0;
    if (be->list_vms(&vms, &vcount) == 0) {
        for (int i = 0; i < vcount; i++) {
            if (str_eq(vms[i]->name, vm_name)) {
                if (vms[i]->state == VM_RUNNING) {
                    be->free_vm_list(vms, vcount);
                    char *html = render_error(
                        "Cannot revert while VM is running. "
                        "Please shut down or pause the VM first.");
                    enum MHD_Result ret = send_html_trigger(conn,
                        MHD_HTTP_BAD_REQUEST, html, "vmStateChanged");
                    free(html);
                    return ret;
                }
                break;
            }
        }
        be->free_vm_list(vms, vcount);
    }

    /* Check if user wants to save current state first */
    const char *save = MHD_lookup_connection_value(conn,
        MHD_GET_ARGUMENT_KIND, "save_current");
    if (save && str_eq(save, "1")) {
        /* Create auto-snapshot of current state before reverting */
        time_t now = time(NULL);
        struct tm *tm = localtime(&now);
        char auto_name[128];
        strftime(auto_name, sizeof(auto_name),
                 "pre-revert-%Y%m%d-%H%M%S", tm);
        if (be->create_snapshot(vm_name, auto_name,
                                "Auto-saved before revert", SNAP_INTERNAL) != 0) {
            char *html = render_error("Failed to save current state before revert");
            enum MHD_Result ret = send_html_trigger(conn,
                MHD_HTTP_INTERNAL_SERVER_ERROR, html, "vmStateChanged");
            free(html);
            return ret;
        }
    }

    if (be->revert_snapshot(vm_name, snap_name) != 0) {
        const char *err = lv_get_last_error();
        char errmsg[512];
        snprintf(errmsg, sizeof(errmsg),
            "Failed to revert: %s", err[0] ? err : "unknown error");
        char *html = render_error(errmsg);
        enum MHD_Result ret = send_html_trigger(conn, MHD_HTTP_INTERNAL_SERVER_ERROR,
                                                html, "vmStateChanged");
        free(html);
        return ret;
    }

    /* Mark VM as clean — state now matches the snapshot */
    vm_mark_clean(vm_name);

    char msg[256];
    if (save && str_eq(save, "1"))
        snprintf(msg, sizeof(msg), "Current state saved. Reverted to '%s'", snap_name);
    else
        snprintf(msg, sizeof(msg), "Reverted to '%s'", snap_name);
    char *success_html = render_success(msg);

    /* Also render the snapshot detail so the panel shows useful info */
    snapshot_node_t *tree = NULL;
    char *detail_html = NULL;
    if (be->list_snapshots(vm_name, &tree) == 0 && tree) {
        snapshot_node_t *snap = snapshot_tree_find(tree, snap_name);
        if (snap) {
            /* After revert, re-check VM state (may have auto-resumed) */
            vm_state_t post_state = VM_SHUTOFF;
            vm_info_t **vms2 = NULL;
            int vc2 = 0;
            if (be->list_vms(&vms2, &vc2) == 0) {
                for (int i = 0; i < vc2; i++) {
                    if (str_eq(vms2[i]->name, vm_name)) {
                        post_state = vms2[i]->state;
                        break;
                    }
                }
                be->free_vm_list(vms2, vc2);
            }
            detail_html = render_snapshot_detail(vm_name, snap, post_state);
        }
        snapshot_tree_free(tree);
    }

    /* Combine: success message + snapshot detail */
    size_t total = strlen(success_html) + (detail_html ? strlen(detail_html) : 0) + 1;
    char *combined = malloc(total);
    snprintf(combined, total, "%s%s", success_html, detail_html ? detail_html : "");
    free(success_html);
    free(detail_html);

    enum MHD_Result ret = send_html_trigger(conn, MHD_HTTP_OK, combined, "vmStateChanged");
    free(combined);
    return ret;
}

static enum MHD_Result handle_merge_snapshot(struct MHD_Connection *conn,
                                             const char *vm_name,
                                             const char *snap_name)
{
    vm_backend_t *be = backend_get();
    if (!be || be->merge_snapshot(vm_name, snap_name) != 0) {
        const char *err = lv_get_last_error();
        char msg[512];
        snprintf(msg, sizeof(msg),
            "Failed to merge: %s. Make sure the VM is shut off.",
            err[0] ? err : "unknown error");
        char *html = render_error(msg);
        enum MHD_Result ret = send_html_trigger(conn, MHD_HTTP_INTERNAL_SERVER_ERROR,
                                                html, "vmStateChanged");
        free(html);
        return ret;
    }
    char *html = render_success("Snapshot merged");
    enum MHD_Result ret = send_html_trigger(conn, MHD_HTTP_OK, html, "vmStateChanged");
    free(html);
    return ret;
}

static enum MHD_Result handle_shared_folders(struct MHD_Connection *conn,
                                             const char *vm_name)
{
    vm_backend_t *be = backend_get();
    shared_folder_t *folders = NULL;
    int count = 0;
    if (!be || be->list_shared_folders(vm_name, &folders, &count) != 0) {
        char *html = render_error("Failed to list shared folders");
        enum MHD_Result ret = send_html(conn, MHD_HTTP_INTERNAL_SERVER_ERROR, html);
        free(html);
        return ret;
    }

    /* Check mount status inside guest (if VM running + guest agent available) */
    if (be->check_mount_status)
        be->check_mount_status(vm_name, folders, count);

    /* Check auto-mount timer status */
    int automount_active = -1;
    if (be->check_automount_status)
        automount_active = be->check_automount_status(vm_name);

    char *html = render_shared_folders(vm_name, folders, count, automount_active);
    enum MHD_Result ret = send_html(conn, MHD_HTTP_OK, html);
    free(html);
    be->free_shared_folders(folders, count);
    return ret;
}

static enum MHD_Result handle_snapshot_form(struct MHD_Connection *conn,
                                            const char *vm_name)
{
    int nvram_ok = lv_check_nvram_format(vm_name);
    char *html = render_create_snapshot_form(vm_name, nvram_ok);
    enum MHD_Result ret = send_html(conn, MHD_HTTP_OK, html);
    free(html);
    return ret;
}

static enum MHD_Result handle_shared_folder_form(struct MHD_Connection *conn,
                                                  const char *vm_name)
{
    char *html = render_add_shared_folder_form(vm_name);
    enum MHD_Result ret = send_html(conn, MHD_HTTP_OK, html);
    free(html);
    return ret;
}

static enum MHD_Result handle_add_shared_folder(struct MHD_Connection *conn,
                                                 const char *vm_name,
                                                 const char *body)
{
    vm_backend_t *be = backend_get();
    if (!be) {
        char *html = render_error("No backend available");
        enum MHD_Result ret = send_html(conn, MHD_HTTP_INTERNAL_SERVER_ERROR, html);
        free(html);
        return ret;
    }

    char *source_dir = form_value(body, "source_dir");
    char *mount_tag  = form_value(body, "mount_tag");
    char *fs_type    = form_value(body, "fs_type");
    char *ro_str     = form_value(body, "read_only");
    char *confirm_sm = form_value(body, "confirm_shared_memory");
    int read_only    = (ro_str && str_eq(ro_str, "1")) ? 1 : 0;
    int sm_confirmed = (confirm_sm && str_eq(confirm_sm, "1"));

    if (!source_dir || strlen(source_dir) == 0 ||
        !mount_tag  || strlen(mount_tag) == 0) {
        free(source_dir); free(mount_tag); free(fs_type); free(ro_str);
        free(confirm_sm);
        char *html = render_error("Source directory and mount tag are required");
        enum MHD_Result ret = send_html(conn, MHD_HTTP_BAD_REQUEST, html);
        free(html);
        return ret;
    }

    const char *effective_fs = (fs_type && strlen(fs_type) > 0) ? fs_type : "virtiofs";

    /* If user confirmed shared memory, enable it first */
    if (sm_confirmed) {
        lv_enable_shared_memory(vm_name);
    }

    int rc = be->add_shared_folder(vm_name, source_dir, mount_tag,
                                   read_only, effective_fs);

    if (rc == -2) {
        /* Shared memory required — show confirmation prompt */
        char *esc_vm  = html_escape(vm_name);
        char *esc_src = html_escape(source_dir);
        char *esc_tag = html_escape(mount_tag);
        char *esc_fs  = html_escape(effective_fs);
        char msg[4096];
        snprintf(msg, sizeof(msg),
            "\xe2\x9a\xa0\xef\xb8\x8f VirtioFS requires <strong>shared memory</strong> "
            "to be enabled on this VM. This modifies the VM configuration."
            "<div style=\"margin-top:8px\">"
            "<form hx-post=\"/api/vms/%s/shared-folders\""
            " hx-target=\"#folder-notification-persistent\""
            " hx-swap=\"innerHTML\">"
            "<input type=\"hidden\" name=\"source_dir\" value=\"%s\">"
            "<input type=\"hidden\" name=\"mount_tag\" value=\"%s\">"
            "<input type=\"hidden\" name=\"fs_type\" value=\"%s\">"
            "<input type=\"hidden\" name=\"read_only\" value=\"%d\">"
            "<input type=\"hidden\" name=\"confirm_shared_memory\" value=\"1\">"
            "<button class=\"btn btn-sm btn-primary\" type=\"submit\">"
            "\xe2\x9c\x85 Enable Shared Memory &amp; Add Folder</button>"
            " <button type=\"button\" class=\"btn btn-sm btn-ghost\""
            " onclick=\"this.closest('.alert').remove()\">"
            "Cancel</button>"
            "</form></div>",
            esc_vm, esc_src, esc_tag, esc_fs, read_only);
        free(esc_vm); free(esc_src); free(esc_tag); free(esc_fs);
        free(source_dir); free(mount_tag); free(fs_type); free(ro_str);
        free(confirm_sm);

        char *html = render_error_html(msg);
        enum MHD_Result ret = send_html(conn, MHD_HTTP_OK, html);
        free(html);
        return ret;
    }

    free(source_dir);
    free(mount_tag);
    free(fs_type);
    free(ro_str);
    free(confirm_sm);

    if (rc < 0) {
        const char *detail = lv_get_last_error();
        char *html;
        if (detail && detail[0]) {
            char buf[1280];
            snprintf(buf, sizeof(buf), "Failed to add shared folder: %s", detail);
            html = render_error(buf);
        } else {
            html = render_error("Failed to add shared folder (check server logs for details)");
        }
        enum MHD_Result ret = send_html(conn, MHD_HTTP_INTERNAL_SERVER_ERROR, html);
        free(html);
        return ret;
    }
    if (rc == 1) {
        /* Folder saved to config but VM needs restart for shared memory */
        char *html = render_success(
            "Shared folder saved to VM config. "
            "\xe2\x9a\xa0\xef\xb8\x8f Restart the VM for it to take effect "
            "(VirtioFS requires shared memory, which was just enabled).");
        enum MHD_Result ret = send_html_trigger(conn, MHD_HTTP_OK, html, "sharedFoldersChanged");
        free(html);
        return ret;
    }
    char *html = render_success("Shared folder added");
    enum MHD_Result ret = send_html_trigger(conn, MHD_HTTP_OK, html, "sharedFoldersChanged");
    free(html);
    return ret;
}

static enum MHD_Result handle_remove_shared_folder(struct MHD_Connection *conn,
                                                    const char *vm_name,
                                                    const char *mount_tag)
{
    vm_backend_t *be = backend_get();
    if (!be || be->remove_shared_folder(vm_name, mount_tag) != 0) {
        char *html = render_error("Failed to remove shared folder");
        enum MHD_Result ret = send_html(conn, MHD_HTTP_INTERNAL_SERVER_ERROR, html);
        free(html);
        return ret;
    }
    char *html = render_success("Shared folder removed");
    enum MHD_Result ret = send_html_trigger(conn, MHD_HTTP_OK, html, "sharedFoldersChanged");
    free(html);
    return ret;
}

static enum MHD_Result handle_mount_shared_folder(struct MHD_Connection *conn,
                                                    const char *vm_name,
                                                    const char *mount_tag)
{
    vm_backend_t *be = backend_get();
    if (!be || !be->mount_shared_folder) {
        char *html = render_error("Mount not supported by this backend");
        enum MHD_Result ret = send_html(conn, MHD_HTTP_BAD_REQUEST, html);
        free(html);
        return ret;
    }

    /* Find the fs_type for this folder from the shared folders list */
    shared_folder_t *folders = NULL;
    int count = 0;
    const char *fs_type = "virtiofs"; /* default */

    if (be->list_shared_folders && be->list_shared_folders(vm_name, &folders, &count) == 0) {
        for (int i = 0; i < count; i++) {
            if (folders[i].mount_tag && strcmp(folders[i].mount_tag, mount_tag) == 0) {
                fs_type = folders[i].fs_type ? folders[i].fs_type : "virtiofs";
                break;
            }
        }
    }

    int rc = be->mount_shared_folder(vm_name, mount_tag, fs_type);

    if (folders && be->free_shared_folders)
        be->free_shared_folders(folders, count);

    if (rc != 0) {
        const char *err = lv_get_last_error();
        guest_os_t gos = GUEST_OS_UNKNOWN;
        if (be->detect_guest_os)
            gos = be->detect_guest_os(vm_name);
        char *html = render_guest_agent_help("mount shared folder", err, gos);
        enum MHD_Result ret = send_html(conn, MHD_HTTP_OK, html);
        free(html);
        return ret;
    }

    char msg[128];
    snprintf(msg, sizeof(msg), "Mounted successfully at /media/%s", mount_tag);
    char *html = render_success(msg);
    enum MHD_Result ret = send_html_trigger(conn, MHD_HTTP_OK, html, "sharedFoldersChanged");
    free(html);
    return ret;
}

static enum MHD_Result handle_unmount_shared_folder(struct MHD_Connection *conn,
                                                      const char *vm_name,
                                                      const char *mount_tag)
{
    vm_backend_t *be = backend_get();
    if (!be || !be->unmount_shared_folder) {
        char *html = render_error("Unmount not supported by this backend");
        enum MHD_Result ret = send_html(conn, MHD_HTTP_BAD_REQUEST, html);
        free(html);
        return ret;
    }

    int rc = be->unmount_shared_folder(vm_name, mount_tag);
    if (rc != 0) {
        const char *err = lv_get_last_error();
        guest_os_t gos = GUEST_OS_UNKNOWN;
        if (be->detect_guest_os)
            gos = be->detect_guest_os(vm_name);
        char *html = render_guest_agent_help("unmount shared folder", err, gos);
        enum MHD_Result ret = send_html(conn, MHD_HTTP_OK, html);
        free(html);
        return ret;
    }

    char *html = render_success("Unmounted successfully inside guest");
    enum MHD_Result ret = send_html_trigger(conn, MHD_HTTP_OK, html, "sharedFoldersChanged");
    free(html);
    return ret;
}

/* ------------------------------------------------------------------ */
/*  route dispatcher                                                  */
/* ------------------------------------------------------------------ */

void routes_init(void)
{
    log_msg(LOG_INFO, "Routes initialized");
}

/* ------------------------------------------------------------------ */
/*  POST /api/vms/{name}/shared-folders/automount                     */
/* ------------------------------------------------------------------ */

static enum MHD_Result handle_automount_setup(struct MHD_Connection *conn,
                                               const char *vm_name)
{
    vm_backend_t *be = backend_get();
    if (!be || !be->setup_automount) {
        char *html = render_error("Backend does not support auto-mount setup");
        enum MHD_Result ret = send_html(conn, MHD_HTTP_INTERNAL_SERVER_ERROR, html);
        free(html);
        return ret;
    }

    char *msg = NULL;
    int rc = be->setup_automount(vm_name, &msg);
    if (rc != 0) {
        const char *err_detail = msg ? msg : lv_get_last_error();
        guest_os_t gos = GUEST_OS_UNKNOWN;
        if (be->detect_guest_os)
            gos = be->detect_guest_os(vm_name);
        char *html = render_guest_agent_help("setup auto-mount", err_detail, gos);
        /* Don't trigger sharedFoldersChanged on error — that would reload the
         * panel and wipe out this error message before the user can read it. */
        enum MHD_Result ret = send_html(conn, MHD_HTTP_INTERNAL_SERVER_ERROR, html);
        free(html);
        free(msg);
        return ret;
    }

    char *html = render_success("Auto-mount service installed and started in guest VM");
    enum MHD_Result ret = send_html_trigger(conn, MHD_HTTP_OK, html, "sharedFoldersChanged");
    free(html);
    free(msg);
    return ret;
}

/* ------------------------------------------------------------------ */
/*  GET /api/browse?path=...                                          */
/* ------------------------------------------------------------------ */

static enum MHD_Result handle_browse_dir(struct MHD_Connection *conn,
                                          const char *path)
{
    if (!path || path[0] != '/') {
        /* Default to the real user's home when running under sudo */
        const char *sudo_user = getenv("SUDO_USER");
        if (sudo_user) {
            static char home_buf[PATH_MAX];
            snprintf(home_buf, sizeof(home_buf), "/home/%s", sudo_user);
            path = home_buf;
        } else {
            const char *home = getenv("HOME");
            path = (home && home[0] == '/') ? home : "/home";
        }
    }

    /* Security: resolve realpath, reject anything containing ".." */
    char resolved[PATH_MAX];
    if (!realpath(path, resolved)) {
        char *html = render_error("Directory not found");
        enum MHD_Result ret = send_html(conn, MHD_HTTP_NOT_FOUND, html);
        free(html);
        return ret;
    }

    DIR *dir = opendir(resolved);
    if (!dir) {
        char *html = render_error("Cannot open directory");
        enum MHD_Result ret = send_html(conn, MHD_HTTP_FORBIDDEN, html);
        free(html);
        return ret;
    }

    /* Collect directory entries */
    char **entries = NULL;
    int count = 0;
    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        if (ent->d_name[0] == '.' && strcmp(ent->d_name, "..") != 0)
            continue;
        if (strcmp(ent->d_name, ".") == 0)
            continue;

        /* Check if it's a directory */
        char fullpath[PATH_MAX + 256];
        snprintf(fullpath, sizeof(fullpath), "%s/%s", resolved, ent->d_name);
        struct stat st;
        if (stat(fullpath, &st) != 0 || !S_ISDIR(st.st_mode))
            continue;

        entries = realloc(entries, (count + 1) * sizeof(char *));
        entries[count++] = strdup(ent->d_name);
    }
    closedir(dir);

    /* Sort alphabetically (with ".." always first) */
    for (int i = 0; i < count - 1; i++) {
        for (int j = i + 1; j < count; j++) {
            int i_is_parent = strcmp(entries[i], "..") == 0;
            int j_is_parent = strcmp(entries[j], "..") == 0;
            if (j_is_parent || (!i_is_parent && strcmp(entries[i], entries[j]) > 0)) {
                char *tmp = entries[i];
                entries[i] = entries[j];
                entries[j] = tmp;
            }
        }
    }

    char *html = render_directory_listing(resolved, entries, count);

    for (int i = 0; i < count; i++) free(entries[i]);
    free(entries);

    enum MHD_Result ret = send_html(conn, MHD_HTTP_OK, html);
    free(html);
    return ret;
}

enum MHD_Result route_dispatch(struct MHD_Connection *connection,
                                const char *url,
                                const char *method,
                                const char *upload_data,
                                size_t *upload_data_size,
                                void **con_cls)
{
    /* -- POST body accumulation ------------------------------------ */
    int is_post   = str_eq(method, "POST");
    int is_delete = str_eq(method, "DELETE");
    int is_put    = str_eq(method, "PUT");

    if (is_post || is_delete || is_put) {
        if (*con_cls == NULL) {
            request_context_t *ctx = request_context_new();
            if (!ctx) return MHD_NO;
            *con_cls = ctx;
            return MHD_YES;
        }

        request_context_t *ctx = (request_context_t *)*con_cls;

        if (*upload_data_size > 0) {
            if (request_context_append(ctx, upload_data, *upload_data_size) < 0)
                return MHD_NO;
            *upload_data_size = 0;
            return MHD_YES;
        }
        /* upload_data_size == 0 → body complete, fall through to dispatch */
    }

    /* -- extract URL segments -------------------------------------- */
    char *seg1 = path_segment(url, 1);  /* "vms" */
    char *seg2 = path_segment(url, 2);  /* vm_name */
    char *seg3 = path_segment(url, 3);  /* "snapshots" | "start" | ... */
    char *seg4 = path_segment(url, 4);  /* snap_name */
    char *seg5 = path_segment(url, 5);  /* "revert" | "merge" */

    enum MHD_Result result;

    /* GET /api/browse?path=... */
    if (seg1 && str_eq(seg1, "browse") && str_eq(method, "GET")) {
        const char *browse_path = MHD_lookup_connection_value(connection, MHD_GET_ARGUMENT_KIND, "path");
        result = handle_browse_dir(connection, browse_path);
        goto cleanup;
    }

    if (!seg1 || !str_eq(seg1, "vms")) {
        result = send_404(connection);
        goto cleanup;
    }

    /* GET /api/vms */
    if (!seg2) {
        if (str_eq(method, "GET")) {
            result = handle_list_vms(connection);
        } else {
            result = send_405(connection);
        }
        goto cleanup;
    }

    /* Routes requiring vm_name */
    const char *vm_name = seg2;

    if (!seg3) {
        result = send_404(connection);
        goto cleanup;
    }

    /* /api/vms/{name}/snapshots/... */
    if (str_eq(seg3, "snapshots")) {
        if (!seg4) {
            /* /api/vms/{name}/snapshots */
            if (str_eq(method, "GET")) {
                result = handle_snapshot_tree(connection, vm_name);
            } else if (is_post) {
                request_context_t *ctx = (request_context_t *)*con_cls;
                result = handle_create_snapshot(connection, vm_name,
                                                ctx ? ctx->body : NULL);
            } else {
                result = send_405(connection);
            }
            goto cleanup;
        }

        /* /api/vms/{name}/snapshots/form */
        if (str_eq(seg4, "form")) {
            if (str_eq(method, "GET")) {
                result = handle_snapshot_form(connection, vm_name);
            } else {
                result = send_405(connection);
            }
            goto cleanup;
        }

        const char *snap_name = seg4;

        if (!seg5) {
            /* /api/vms/{name}/snapshots/{snap} */
            if (str_eq(method, "GET")) {
                result = handle_snapshot_detail(connection, vm_name, snap_name);
            } else if (is_put) {
                request_context_t *ctx = (request_context_t *)*con_cls;
                result = handle_edit_snapshot(connection, vm_name, snap_name,
                                              ctx ? ctx->body : NULL);
            } else if (is_delete) {
                result = handle_delete_snapshot(connection, vm_name, snap_name);
            } else {
                result = send_405(connection);
            }
            goto cleanup;
        }

        /* /api/vms/{name}/snapshots/{snap}/edit */
        if (str_eq(seg5, "edit") && str_eq(method, "GET")) {
            result = handle_edit_snapshot_form(connection, vm_name, snap_name);
            goto cleanup;
        }

        /* /api/vms/{name}/snapshots/{snap}/revert */
        if (str_eq(seg5, "revert") && is_post) {
            result = handle_revert_snapshot(connection, vm_name, snap_name);
            goto cleanup;
        }

        /* /api/vms/{name}/snapshots/{snap}/revert-confirm */
        if (str_eq(seg5, "revert-confirm") && str_eq(method, "GET")) {
            result = handle_revert_confirm(connection, vm_name, snap_name);
            goto cleanup;
        }

        /* /api/vms/{name}/snapshots/{snap}/merge */
        if (str_eq(seg5, "merge") && is_post) {
            result = handle_merge_snapshot(connection, vm_name, snap_name);
            goto cleanup;
        }

        result = send_404(connection);
        goto cleanup;
    }

    /* /api/vms/{name}/shared-folders */
    if (str_eq(seg3, "shared-folders")) {
        if (!seg4 && str_eq(method, "GET")) {
            result = handle_shared_folders(connection, vm_name);
        } else if (!seg4 && is_post) {
            request_context_t *ctx = (request_context_t *)*con_cls;
            result = handle_add_shared_folder(connection, vm_name,
                                              ctx ? ctx->body : NULL);
        } else if (seg4 && str_eq(seg4, "form") && str_eq(method, "GET")) {
            result = handle_shared_folder_form(connection, vm_name);
        } else if (seg4 && seg5 && str_eq(seg5, "mount") && is_post) {
            result = handle_mount_shared_folder(connection, vm_name, seg4);
        } else if (seg4 && seg5 && str_eq(seg5, "unmount") && is_post) {
            result = handle_unmount_shared_folder(connection, vm_name, seg4);
        } else if (seg4 && str_eq(seg4, "automount") && is_post) {
            result = handle_automount_setup(connection, vm_name);
        } else if (seg4 && is_delete) {
            result = handle_remove_shared_folder(connection, vm_name, seg4);
        } else {
            result = send_405(connection);
        }
        goto cleanup;
    }

    /* /api/vms/{name}/start */
    if (str_eq(seg3, "start") && is_post) {
        result = handle_vm_start(connection, vm_name);
        goto cleanup;
    }

    /* /api/vms/{name}/stop */
    if (str_eq(seg3, "stop") && is_post) {
        result = handle_vm_stop(connection, vm_name);
        goto cleanup;
    }

    /* /api/vms/{name}/pause */
    if (str_eq(seg3, "pause") && is_post) {
        result = handle_vm_pause(connection, vm_name);
        goto cleanup;
    }

    /* /api/vms/{name}/resume */
    if (str_eq(seg3, "resume") && is_post) {
        result = handle_vm_resume(connection, vm_name);
        goto cleanup;
    }

    /* /api/vms/{name}/force-stop */
    if (str_eq(seg3, "force-stop") && is_post) {
        result = handle_vm_force_stop(connection, vm_name);
        goto cleanup;
    }

    /* /api/vms/{name}/convert-nvram */
    if (str_eq(seg3, "convert-nvram") && is_post) {
        result = handle_convert_nvram(connection, vm_name);
        goto cleanup;
    }

    /* /api/vms/{name}/orphan-check */
    if (str_eq(seg3, "orphan-check") && str_eq(method, "GET")) {
        result = handle_orphan_check(connection, vm_name);
        goto cleanup;
    }

    /* /api/vms/{name}/orphan-cleanup */
    if (str_eq(seg3, "orphan-cleanup") && is_post) {
        result = handle_orphan_cleanup(connection, vm_name);
        goto cleanup;
    }

    result = send_404(connection);

cleanup:
    free(seg1);
    free(seg2);
    free(seg3);
    free(seg4);
    free(seg5);
    return result;
}

void route_request_completed(void *cls, struct MHD_Connection *connection,
                              void **con_cls,
                              enum MHD_RequestTerminationCode toe)
{
    (void)cls;
    (void)connection;
    (void)toe;

    if (*con_cls && *con_cls != (void *)1) {
        request_context_free((request_context_t *)*con_cls);
        *con_cls = NULL;
    }
}
