#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>

#include "html_render.h"
#include "util.h"

/* ------------------------------------------------------------------ */
/*  HTML-escape helper: escapes <, >, &, " in user-provided strings   */
/* ------------------------------------------------------------------ */

char *html_escape(const char *raw)
{
    if (!raw)
        return str_dup("");

    /* worst case: every char becomes &quot; (6 chars) */
    size_t len = strlen(raw);
    size_t alloc = len * 6 + 1;
    char *out = malloc(alloc);
    if (!out)
        return NULL;

    size_t j = 0;
    for (size_t i = 0; i < len; i++) {
        switch (raw[i]) {
        case '<':  memcpy(out + j, "&lt;",   4); j += 4; break;
        case '>':  memcpy(out + j, "&gt;",   4); j += 4; break;
        case '&':  memcpy(out + j, "&amp;",  5); j += 5; break;
        case '"':  memcpy(out + j, "&quot;", 6); j += 6; break;
        case '\'': memcpy(out + j, "&#39;",  5); j += 5; break;
        default:   out[j++] = raw[i]; break;
        }
    }
    out[j] = '\0';
    return out;
}

/* ------------------------------------------------------------------ */
/*  Simple string builder                                             */
/* ------------------------------------------------------------------ */

typedef struct {
    char  *buf;
    size_t len;
    size_t cap;
} strbuf_t;

static void sb_init(strbuf_t *sb, size_t initial)
{
    sb->cap = initial ? initial : 4096;
    sb->buf = malloc(sb->cap);
    sb->len = 0;
    if (sb->buf)
        sb->buf[0] = '\0';
}

static void sb_append(strbuf_t *sb, const char *s)
{
    if (!sb->buf || !s) return;
    size_t slen = strlen(s);
    if (sb->len + slen + 1 > sb->cap) {
        size_t new_cap = (sb->len + slen + 1) * 2;
        char *tmp = realloc(sb->buf, new_cap);
        if (!tmp) return;
        sb->buf = tmp;
        sb->cap = new_cap;
    }
    memcpy(sb->buf + sb->len, s, slen);
    sb->len += slen;
    sb->buf[sb->len] = '\0';
}

/* Append a formatted string. Uses a temporary buffer. */
static void sb_appendf(strbuf_t *sb, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

static void sb_appendf(strbuf_t *sb, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int needed = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (needed < 0) return;

    /* ensure capacity */
    size_t required = sb->len + (size_t)needed + 1;
    if (required > sb->cap) {
        size_t new_cap = required * 2;
        char *tmp = realloc(sb->buf, new_cap);
        if (!tmp) return;
        sb->buf = tmp;
        sb->cap = new_cap;
    }

    va_start(ap, fmt);
    vsnprintf(sb->buf + sb->len, (size_t)needed + 1, fmt, ap);
    va_end(ap);
    sb->len += (size_t)needed;
}

/* Return the built string (caller owns it). Invalidates the builder. */
static char *sb_finish(strbuf_t *sb)
{
    char *result = sb->buf;
    sb->buf = NULL;
    sb->len = sb->cap = 0;
    return result;
}

/* ------------------------------------------------------------------ */
/*  URL-encode helper for path strings                                */
/* ------------------------------------------------------------------ */

static char *url_encode(const char *s)
{
    /* Allocate worst case: every char becomes %XX */
    size_t len = strlen(s);
    char *out = malloc(len * 3 + 1);
    if (!out) return strdup(s);

    char *p = out;
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)s[i];
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '/' || c == '-' ||
            c == '_' || c == '.' || c == '~') {
            *p++ = c;
        } else {
            snprintf(p, 4, "%%%02X", c);
            p += 3;
        }
    }
    *p = '\0';
    return out;
}

/* ------------------------------------------------------------------ */
/*  State to CSS class mapping                                        */
/* ------------------------------------------------------------------ */

static const char *state_css_class(vm_state_t s)
{
    switch (s) {
    case VM_RUNNING: return "state-running";
    case VM_PAUSED:  return "state-paused";
    case VM_SHUTOFF: return "state-shutoff";
    case VM_OTHER:   return "state-other";
    default:         return "state-other";
    }
}

/* ------------------------------------------------------------------ */
/*  render_vm_list                                                    */
/* ------------------------------------------------------------------ */

char *render_vm_list(vm_info_t **vms, int count)
{
    if (!vms || count <= 0)
        return str_dup("<p class=\"placeholder\">No virtual machines found</p>");

    strbuf_t sb;
    sb_init(&sb, (size_t)count * 1024);

    for (int i = 0; i < count; i++) {
        vm_info_t *vm = vms[i];
        if (!vm) continue;

        char *esc_name = html_escape(vm->name);
        const char *state_str = vm_state_str(vm->state);
        const char *state_cls = state_css_class(vm->state);
        unsigned long mem_mb = vm->memory_kb / 1024;

        sb_appendf(&sb,
            "<div class=\"vm-item\" data-vm=\"%s\" onclick=\"selectVm('%s')\">\n"
            "    <div class=\"vm-header\">\n"
            "        <div class=\"vm-name\">%s</div>\n"
            "        <span class=\"vm-state %s\">%s</span>\n"
            "    </div>\n"
            "    <div class=\"vm-meta\">\n"
            "        <span class=\"vm-info\">%d vCPU · %lu MB</span>\n"
            "    </div>\n"
            "    <div class=\"vm-actions\" onclick=\"event.stopPropagation()\">\n",
            esc_name, esc_name, esc_name,
            state_cls, state_str,
            vm->vcpus, mem_mb);

        /* Context-aware action buttons */
        switch (vm->state) {
        case VM_RUNNING:
            sb_appendf(&sb,
                "        <button class=\"btn btn-xs btn-warning\""
                " hx-post=\"/api/vms/%s/stop\""
                " hx-target=\"#vm-notification\""
                " hx-swap=\"innerHTML\""
                " title=\"Shutdown (graceful)\">⏹ Stop</button>\n"
                "        <button class=\"btn btn-xs btn-secondary\""
                " hx-post=\"/api/vms/%s/pause\""
                " hx-target=\"#vm-notification\""
                " hx-swap=\"innerHTML\""
                " title=\"Pause\">⏸ Pause</button>\n"
                "        <button class=\"btn btn-xs btn-danger\""
                " hx-post=\"/api/vms/%s/force-stop\""
                " hx-target=\"#vm-notification\""
                " hx-swap=\"innerHTML\""
                " hx-confirm=\"Force stop %s? This may cause data loss.\""
                " title=\"Force power off\">⚡ Force</button>\n",
                esc_name, esc_name, esc_name, esc_name);
            break;
        case VM_PAUSED:
            sb_appendf(&sb,
                "        <button class=\"btn btn-xs btn-success\""
                " hx-post=\"/api/vms/%s/resume\""
                " hx-target=\"#vm-notification\""
                " hx-swap=\"innerHTML\""
                " title=\"Resume\">▶ Resume</button>\n"
                "        <button class=\"btn btn-xs btn-danger\""
                " hx-post=\"/api/vms/%s/force-stop\""
                " hx-target=\"#vm-notification\""
                " hx-swap=\"innerHTML\""
                " hx-confirm=\"Force stop %s? This may cause data loss.\""
                " title=\"Force power off\">⚡ Force</button>\n",
                esc_name, esc_name, esc_name);
            break;
        case VM_SHUTOFF:
            sb_appendf(&sb,
                "        <button class=\"btn btn-xs btn-success btn-vm-start\""
                " hx-post=\"/api/vms/%s/start\""
                " hx-target=\"#vm-notification\""
                " hx-swap=\"innerHTML\""
                " title=\"Start VM\">▶ Start</button>\n",
                esc_name);
            break;
        default:
            sb_appendf(&sb,
                "        <button class=\"btn btn-xs btn-success btn-vm-start\""
                " hx-post=\"/api/vms/%s/start\""
                " hx-target=\"#vm-notification\""
                " hx-swap=\"innerHTML\""
                " title=\"Start VM\">▶ Start</button>\n"
                "        <button class=\"btn btn-xs btn-danger\""
                " hx-post=\"/api/vms/%s/force-stop\""
                " hx-target=\"#vm-notification\""
                " hx-swap=\"innerHTML\""
                " hx-confirm=\"Force stop %s?\""
                " title=\"Force power off\">⚡ Force</button>\n",
                esc_name, esc_name, esc_name);
            break;
        }

        sb_appendf(&sb,
            "    </div>\n"
            "</div>\n");

        free(esc_name);
    }

    return sb_finish(&sb);
}

/* ------------------------------------------------------------------ */
/*  render_snapshot_detail                                            */
/* ------------------------------------------------------------------ */

char *render_snapshot_detail(const char *vm_name, snapshot_node_t *snap,
                             vm_state_t vm_state)
{
    if (!snap)
        return str_dup("<p class=\"placeholder\">Snapshot not found</p>");

    int vm_active = (vm_state == VM_RUNNING || vm_state == VM_PAUSED);

    char *esc_vm   = html_escape(vm_name);
    char *esc_id   = html_escape(snap->id);
    char *esc_desc = html_escape(snap->description);
    char *esc_time = html_escape(snap->creation_time);

    const char *badge_cls  = (snap->type == SNAP_INTERNAL) ? "badge-internal" : "badge-external";
    const char *badge_text = (snap->type == SNAP_INTERNAL) ? "Internal" : "External";

    strbuf_t sb;
    sb_init(&sb, 4096);

    sb_append(&sb, "<div class=\"snapshot-info\">\n");
    sb_appendf(&sb, "    <h3>%s</h3>\n", esc_id);
    sb_append(&sb, "    <div class=\"snapshot-meta\">\n");
    sb_appendf(&sb, "        <span class=\"badge %s\">%s</span>\n", badge_cls, badge_text);
    sb_appendf(&sb, "        <span class=\"snap-date\">%s</span>\n", esc_time);
    sb_append(&sb, "    </div>\n");
    sb_appendf(&sb, "    <p class=\"snap-desc\">%s</p>\n", esc_desc);

    /* Edit description button */
    sb_appendf(&sb,
        "    <button class=\"btn btn-sm btn-secondary edit-desc-btn\"\n"
        "            hx-get=\"/api/vms/%s/snapshots/%s/edit\"\n"
        "            hx-target=\"#snapshot-detail\"\n"
        "            hx-swap=\"innerHTML\"\n"
        "            hx-indicator=\"#snap-action-loading\">\n"
        "        \xe2\x9c\x8f Edit Description\n"
        "    </button>\n",
        esc_vm, esc_id);

    if (snap->is_current) {
        sb_append(&sb,
            "    <div class=\"snap-current\">\xe2\x98\x85 Current Snapshot</div>\n");
    }

    sb_append(&sb, "    <div class=\"snap-actions\">\n");

    /* Revert button — loads confirmation dialog */
    sb_appendf(&sb,
        "        <button class=\"btn btn-primary\"\n"
        "                hx-get=\"/api/vms/%s/snapshots/%s/revert-confirm\"\n"
        "                hx-target=\"#snapshot-detail\"\n"
        "                hx-swap=\"innerHTML\"\n"
        "                hx-indicator=\"#snap-action-loading\">\n"
        "            \xe2\x86\xa9 Revert\n"
        "        </button>\n",
        esc_vm, esc_id);

    /* Delete button — disabled while VM is running/paused */
    if (vm_active) {
        sb_appendf(&sb,
            "        <button class=\"btn btn-danger\" disabled\n"
            "                title=\"Shut down the VM to delete snapshots\">\n"
            "            \xf0\x9f\x97\x91 Delete\n"
            "        </button>\n");
    } else {
        sb_appendf(&sb,
            "        <button class=\"btn btn-danger\"\n"
            "                hx-delete=\"/api/vms/%s/snapshots/%s\"\n"
            "                hx-target=\"#snapshot-tree\"\n"
            "                hx-swap=\"innerHTML\"\n"
            "                hx-indicator=\"#snap-action-loading\"\n"
            "                hx-confirm=\"Delete snapshot '%s'?\">\n"
            "            \xf0\x9f\x97\x91 Delete\n"
            "        </button>\n",
            esc_vm, esc_id, esc_id);
    }

    /* Merge button (only for external snapshots) */
    if (snap->type == SNAP_EXTERNAL) {
        if (vm_active) {
            sb_append(&sb,
                "        <button class=\"btn btn-warning\" disabled\n"
                "                title=\"Shut down the VM to merge snapshots\">\n"
                "            \xe2\x8a\x95 Merge\n"
                "        </button>\n");
        } else {
            sb_appendf(&sb,
                "        <button class=\"btn btn-warning\"\n"
                "                hx-post=\"/api/vms/%s/snapshots/%s/merge\"\n"
                "                hx-target=\"#snapshot-tree\"\n"
                "                hx-swap=\"innerHTML\"\n"
                "                hx-indicator=\"#snap-action-loading\"\n"
                "                hx-confirm=\"Merge snapshot '%s'?\">\n"
                "            \xe2\x8a\x95 Merge\n"
                "        </button>\n",
                esc_vm, esc_id, esc_id);
        }
    }

    sb_append(&sb, "    </div>\n");

    /* Loading indicator for action buttons */
    sb_append(&sb,
        "    <div id=\"snap-action-loading\" class=\"htmx-indicator snap-action-busy\">\n"
        "        <span class=\"spinner\"></span> Processing\xe2\x80\xa6\n"
        "    </div>\n");

    sb_append(&sb, "</div>\n");

    free(esc_vm);
    free(esc_id);
    free(esc_desc);
    free(esc_time);

    return sb_finish(&sb);
}

/* ------------------------------------------------------------------ */
/*  render_create_snapshot_form                                       */
/* ------------------------------------------------------------------ */

char *render_create_snapshot_form(const char *vm_name, int nvram_is_qcow2)
{
    char *esc_vm = html_escape(vm_name);

    strbuf_t sb;
    sb_init(&sb, 4096);

    sb_append(&sb, "<div class=\"modal-overlay\" onclick=\"this.remove()\">\n");
    sb_append(&sb, "    <div class=\"modal\" onclick=\"event.stopPropagation()\">\n");
    sb_append(&sb, "        <h3>Create Snapshot</h3>\n");

    /* NVRAM warning banner */
    if (nvram_is_qcow2 == 0) {
        sb_appendf(&sb,
            "        <div class=\"alert alert-warning\" style=\"margin-bottom:0.75rem\">\n"
            "            ⚠️ <strong>NVRAM is in raw format.</strong> "
            "Internal snapshots require qcow2 NVRAM. "
            "Stop the VM first, then convert.\n"
            "            <div style=\"margin-top:0.5rem\">\n"
            "              <button type=\"button\" class=\"btn btn-sm btn-warning\"\n"
            "                onclick=\"event.preventDefault();"
            "this.closest('.modal-overlay').remove();"
            "htmx.ajax('POST','/api/vms/%s/convert-nvram',"
            "{target:'#vm-notification',swap:'innerHTML'})\">\n"
            "                🔧 Convert NVRAM to qcow2\n"
            "              </button>\n"
            "            </div>\n"
            "        </div>\n",
            esc_vm);
    }

    sb_appendf(&sb,
        "        <div id=\"snap-form-error\"></div>\n"
        "        <form hx-post=\"/api/vms/%s/snapshots\"\n"
        "              hx-target=\"#snap-form-error\"\n"
        "              hx-swap=\"innerHTML\"\n"
        "              hx-indicator=\"#snap-creating\"\n"
        "              hx-on::after-request=\"if(event.detail.successful){"
        "document.getElementById('vm-notification').innerHTML=event.detail.xhr.responseText;"
        "this.closest('.modal-overlay').remove();}\">\n",
        esc_vm);
    sb_append(&sb,
        "            <div class=\"form-group\">\n"
        "                <label class=\"form-label\">Name</label>\n"
        "                <input class=\"form-input\" name=\"name\" placeholder=\"my-snapshot\" required>\n"
        "            </div>\n");
    sb_append(&sb,
        "            <div class=\"form-group\">\n"
        "                <label class=\"form-label\">Description</label>\n"
        "                <textarea class=\"form-input\" name=\"description\" rows=\"3\" \n"
        "                          placeholder=\"Optional description\"></textarea>\n"
        "            </div>\n");
    sb_append(&sb,
        "            <div class=\"form-group\">\n"
        "                <label class=\"form-label\">Type</label>\n"
        "                <select class=\"form-select\" name=\"type\">\n"
        "                    <option value=\"internal\">Internal</option>\n"
        "                    <option value=\"external\">External</option>\n"
        "                </select>\n"
        "                <p class=\"form-hint\">\n"
        "                    <strong>Internal</strong> — saves full VM state "
        "(memory + disk). Like VirtualBox &ldquo;save state&rdquo;. "
        "Requires qcow2 NVRAM on UEFI VMs.<br>\n"
        "                    <strong>External</strong> — disk-only snapshot. "
        "Faster, but does not capture memory. "
        "Must be merged later to reclaim space.\n"
        "                </p>\n"
        "            </div>\n");
    sb_append(&sb,
        "            <div class=\"form-actions\">\n"
        "                <button type=\"submit\" class=\"btn btn-primary\""
        "                        id=\"snap-create-btn\">Create</button>\n"
        "                <button type=\"button\" class=\"btn btn-ghost\" \n"
        "                        onclick=\"this.closest('.modal-overlay').remove()\">Cancel</button>\n"
        "            </div>\n"
        "            <div id=\"snap-creating\" class=\"htmx-indicator snap-loading\">\n"
        "                <span class=\"spinner\"></span> Creating snapshot\xe2\x80\xa6 "
        "This may take a moment for running VMs.\n"
        "            </div>\n");
    sb_append(&sb, "        </form>\n");
    sb_append(&sb, "    </div>\n");
    sb_append(&sb, "</div>\n");

    free(esc_vm);
    return sb_finish(&sb);
}

/* ------------------------------------------------------------------ */
/*  render_shared_folders                                             */
/* ------------------------------------------------------------------ */

char *render_shared_folders(const char *vm_name, shared_folder_t *folders, int count, int automount_active)
{
    if (!folders || count <= 0)
        return str_dup("<p class=\"placeholder\">No shared folders configured</p>");

    char *esc_vm = html_escape(vm_name);

    strbuf_t sb;
    sb_init(&sb, (size_t)count * 512);

    sb_append(&sb, "<div id=\"folder-notification\"></div>\n");

    /* Auto-mount status bar */
    sb_append(&sb, "<div class=\"automount-status\">\n");
    if (automount_active == 1) {
        sb_append(&sb,
            "    <span class=\"badge badge-mounted\">\xe2\x9c\x93 Auto-Mount Active</span>\n"
            "    <span class=\"automount-hint\">VirtioFS shares are automatically mounted to /media/</span>\n");
    } else if (automount_active == 0) {
        sb_appendf(&sb,
            "    <button class=\"btn btn-sm btn-primary\"\n"
            "            hx-post=\"/api/vms/%s/shared-folders/automount\"\n"
            "            hx-target=\"#folder-notification-persistent\"\n"
            "            hx-swap=\"innerHTML\"\n"
            "            hx-indicator=\"#automount-spinner\"\n"
            "            hx-disabled-elt=\"this\"\n"
            "            hx-confirm=\"Install auto-mount service in the guest VM?"
            " This adds a systemd timer that mounts VirtioFS shares to /media/ automatically.\">\n"
            "        \xe2\x9a\x99\xef\xb8\x8f Setup Auto-Mount\n"
            "    </button>\n"
            "    <span id=\"automount-spinner\" class=\"spinner htmx-indicator\"></span>\n", esc_vm);
    }
    /* If automount_active == -1 (unknown/error), show nothing */
    sb_append(&sb, "</div>\n");

    sb_append(&sb, "<div class=\"shared-folders-list\">\n");

    for (int i = 0; i < count; i++) {
        char *esc_tag  = html_escape(folders[i].mount_tag);
        char *esc_path = html_escape(folders[i].source_dir);

        sb_append(&sb, "    <div class=\"folder-item\">\n");
        sb_append(&sb, "        <div class=\"folder-header\">\n");
        sb_appendf(&sb, "            <div class=\"folder-icon\">\xf0\x9f\x93\x81</div>\n");
        sb_append(&sb, "            <div class=\"folder-info\">\n");
        sb_appendf(&sb, "                <div class=\"folder-tag\">%s</div>\n", esc_tag);
        sb_appendf(&sb, "                <div class=\"folder-path\" title=\"%s\">%s</div>\n", esc_path, esc_path);
        if (folders[i].read_only) {
            sb_append(&sb, "                <span class=\"badge badge-readonly\">Read Only</span>\n");
        }
        if (folders[i].mounted == 1) {
            sb_append(&sb, "                <span class=\"badge badge-mounted\">✓ Mounted</span>\n");
        } else if (folders[i].mounted == 0) {
            sb_append(&sb, "                <span class=\"badge badge-unmounted\">Not Mounted</span>\n");
        }

        /* Mount instructions */
        const char *ftype = folders[i].fs_type ? folders[i].fs_type : "virtiofs";
        if (strcmp(ftype, "9p") == 0) {
            sb_appendf(&sb,
                "                <div class=\"folder-mount-hint\">"
                "<code>sudo mkdir -p /media/%s &amp;&amp; sudo mount -t 9p -o trans=virtio %s /media/%s</code></div>\n",
                esc_tag, esc_tag, esc_tag);
        } else {
            sb_appendf(&sb,
                "                <div class=\"folder-mount-hint\">"
                "<code>sudo mkdir -p /media/%s &amp;&amp; sudo mount -t virtiofs %s /media/%s</code></div>\n",
                esc_tag, esc_tag, esc_tag);
        }

        sb_append(&sb, "            </div>\n");  /* close folder-info */
        sb_append(&sb, "        </div>\n");  /* close folder-header */

        int mounted = folders[i].mounted; /* 1=mounted, 0=not mounted, -1=unknown (VM off) */

        sb_append(&sb, "        <div class=\"folder-actions\">\n");

        if (folders[i].needs_restart) {
            /* Folder is in config but not in the live VM */
            sb_append(&sb,
                "            <span class=\"badge badge-warning\">"
                "\xe2\x9a\xa0\xef\xb8\x8f Stop &amp; Start from web UI to activate</span>\n"
                "            " STOP_START_EXPLANATION_HTML "\n");
            sb_appendf(&sb,
                "        <button class=\"btn btn-sm btn-danger\"\n"
                "                hx-delete=\"/api/vms/%s/shared-folders/%s\"\n"
                "                hx-target=\"#folder-notification-persistent\"\n"
                "                hx-swap=\"innerHTML\"\n"
                "                hx-confirm=\"Detach shared folder '%s'? (Files on host are not deleted)\">\n"
                "            \xe2\x9c\x96 Detach\n"
                "        </button>\n",
                esc_vm, esc_tag, esc_tag);
        } else {

        /* Mount button */
        if (mounted == 1) {
            sb_append(&sb,
                "            <button class=\"btn btn-sm btn-success\" disabled\n"
                "                    title=\"Already mounted inside guest\">\n"
                "                \xe2\xac\x86\xef\xb8\x8f Mounted\n"
                "            </button>\n");
        } else {
            sb_appendf(&sb,
                "            <button class=\"btn btn-sm btn-success\"\n"
                "                    hx-post=\"/api/vms/%s/shared-folders/%s/mount\"\n"
                "                    hx-target=\"#folder-notification-persistent\"\n"
                "                    hx-swap=\"innerHTML\"\n"
                "                    hx-confirm=\"Mount %s inside the guest VM?\"\n"
                "                    title=\"Mount inside guest via QEMU Guest Agent\">\n"
                "                \xe2\xac\x86\xef\xb8\x8f Mount\n"
                "            </button>\n",
                esc_vm, esc_tag, esc_tag);
        }

        /* Unmount button — only active when confirmed mounted */
        if (mounted == 1) {
            sb_appendf(&sb,
                "            <button class=\"btn btn-sm btn-warning\"\n"
                "                    hx-post=\"/api/vms/%s/shared-folders/%s/unmount\"\n"
                "                    hx-target=\"#folder-notification-persistent\"\n"
                "                    hx-swap=\"innerHTML\"\n"
                "                    hx-confirm=\"Unmount %s from the guest VM?\"\n"
                "                    title=\"Unmount inside guest via QEMU Guest Agent\">\n"
                "                \xe2\xac\x87\xef\xb8\x8f Unmount\n"
                "            </button>\n",
                esc_vm, esc_tag, esc_tag);
        } else if (mounted == 0) {
            sb_append(&sb,
                "            <button class=\"btn btn-sm btn-warning\" disabled\n"
                "                    title=\"Not currently mounted\">\n"
                "                \xe2\xac\x87\xef\xb8\x8f Unmount\n"
                "            </button>\n");
        } else {
            /* Unknown — show both enabled, let the action fail gracefully */
            sb_appendf(&sb,
                "            <button class=\"btn btn-sm btn-warning\"\n"
                "                    hx-post=\"/api/vms/%s/shared-folders/%s/unmount\"\n"
                "                    hx-target=\"#folder-notification-persistent\"\n"
                "                    hx-swap=\"innerHTML\"\n"
                "                    hx-confirm=\"Unmount %s from the guest VM?\"\n"
                "                    title=\"Unmount inside guest via QEMU Guest Agent\">\n"
                "                \xe2\xac\x87\xef\xb8\x8f Unmount\n"
                "            </button>\n",
                esc_vm, esc_tag, esc_tag);
        }
        sb_appendf(&sb,
            "        <button class=\"btn btn-sm btn-danger\"\n"
            "                hx-delete=\"/api/vms/%s/shared-folders/%s\"\n"
            "                hx-target=\"#folder-notification-persistent\"\n"
            "                hx-swap=\"innerHTML\"\n"
            "                hx-confirm=\"Detach shared folder '%s'? (Files on host are not deleted)\">\n"
            "            \xe2\x9c\x96 Detach\n"
            "        </button>\n",
            esc_vm, esc_tag, esc_tag);
        } /* end else (not needs_restart) */
        sb_append(&sb, "        </div>\n");
        sb_append(&sb, "    </div>\n");

        free(esc_tag);
        free(esc_path);
    }

    sb_append(&sb, "</div>\n");
    free(esc_vm);
    return sb_finish(&sb);
}

/* ------------------------------------------------------------------ */
/*  render_add_shared_folder_form                                     */
/* ------------------------------------------------------------------ */

char *render_add_shared_folder_form(const char *vm_name)
{
    char *esc_vm = html_escape(vm_name);

    strbuf_t sb;
    sb_init(&sb, 4096);

    sb_append(&sb, "<div class=\"modal-overlay\" onclick=\"this.remove()\">\n");
    sb_append(&sb, "    <div class=\"modal\" onclick=\"event.stopPropagation()\">\n");
    sb_append(&sb, "        <h3>Add Shared Folder</h3>\n");
    sb_appendf(&sb,
        "        <form hx-post=\"/api/vms/%s/shared-folders\"\n"
        "              hx-target=\"#folder-notification-persistent\"\n"
        "              hx-swap=\"innerHTML\"\n"
        "              hx-on::after-request=\"this.closest('.modal-overlay').remove()\">\n",
        esc_vm);
    sb_append(&sb,
        "            <div class=\"form-group\">\n"
        "                <label class=\"form-label\">Source Directory (host path)</label>\n"
        "                <div class=\"input-with-button\">\n"
        "                    <input class=\"form-input\" name=\"source_dir\" id=\"sf-source-dir\"\n"
        "                           placeholder=\"/home/user/Documents\" required\n"
        "                           oninput=\"var p=this.value.replace(/\\/$/,''); "
        "var n=p.split('/').pop()||'share'; "
        "document.getElementById('sf-mount-tag').value='sf_'+n.replace(/[^a-zA-Z0-9_-]/g,'_');\">\n"
        "                    <button type=\"button\" class=\"btn btn-sm\" onclick=\"openDirBrowser()\">Browse...</button>\n"
        "                </div>\n"
        "                <small class=\"form-hint\">Full path on the host machine (e.g. /home/user/Downloads)</small>\n"
        "            </div>\n");
    sb_append(&sb,
        "            <div class=\"form-group\">\n"
        "                <label class=\"form-label\">Mount Tag</label>\n"
        "                <input class=\"form-input\" name=\"mount_tag\" id=\"sf-mount-tag\"\n"
        "                       placeholder=\"sf_Downloads\" required>\n"
        "                <small class=\"form-hint\">Auto-generated from directory name. Used to mount inside VM.</small>\n"
        "            </div>\n");
    sb_append(&sb,
        "            <div class=\"form-group\">\n"
        "                <label class=\"form-label\">Filesystem Type</label>\n"
        "                <select class=\"form-select\" name=\"fs_type\">\n"
        "                    <option value=\"virtiofs\">virtiofs</option>\n"
        "                    <option value=\"9p\">9p</option>\n"
        "                </select>\n"
        "            </div>\n");
    sb_append(&sb,
        "            <div class=\"form-group\">\n"
        "                <label class=\"form-label\">\n"
        "                    <input type=\"checkbox\" name=\"read_only\" value=\"1\"> Read Only\n"
        "                </label>\n"
        "            </div>\n");
    sb_append(&sb,
        "            <div class=\"form-actions\">\n"
        "                <button type=\"submit\" class=\"btn btn-primary\">Add Folder</button>\n"
        "                <button type=\"button\" class=\"btn btn-ghost\"\n"
        "                        onclick=\"this.closest('.modal-overlay').remove()\">Cancel</button>\n"
        "            </div>\n");
    sb_append(&sb, "        </form>\n");
    sb_append(&sb, "    </div>\n");
    sb_append(&sb, "</div>\n");

    free(esc_vm);
    return sb_finish(&sb);
}

/* ------------------------------------------------------------------ */
/*  render_directory_listing                                          */
/* ------------------------------------------------------------------ */

char *render_directory_listing(const char *current_path, char **entries, int count)
{
    strbuf_t sb;
    sb_init(&sb, 4096);

    char *esc_path = html_escape(current_path);
    char *url_path = url_encode(current_path);

    sb_append(&sb, "<div class=\"dir-browser\">\n");
    sb_appendf(&sb, "    <div class=\"dir-current-path\">\xf0\x9f\x93\x81 %s</div>\n", esc_path);
    sb_append(&sb, "    <div class=\"dir-list\">\n");

    for (int i = 0; i < count; i++) {
        char *esc_name = html_escape(entries[i]);
        int is_parent = strcmp(entries[i], "..") == 0;

        /* Build full path for this entry */
        char fullpath[4096];
        if (is_parent) {
            /* Go up one level */
            const char *last_slash = strrchr(current_path, '/');
            if (last_slash && last_slash != current_path) {
                snprintf(fullpath, sizeof(fullpath), "%.*s",
                         (int)(last_slash - current_path), current_path);
            } else {
                snprintf(fullpath, sizeof(fullpath), "/");
            }
        } else {
            if (strcmp(current_path, "/") == 0)
                snprintf(fullpath, sizeof(fullpath), "/%s", entries[i]);
            else
                snprintf(fullpath, sizeof(fullpath), "%s/%s", current_path, entries[i]);
        }
        char *url_fullpath = url_encode(fullpath);
        char *esc_fullpath = html_escape(fullpath);

        sb_appendf(&sb,
            "        <div class=\"dir-entry\" "
            "hx-get=\"/api/browse?path=%s\" "
            "hx-target=\"#dir-browser-content\" "
            "hx-swap=\"innerHTML\">"
            "%s %s</div>\n",
            url_fullpath,
            is_parent ? "\xe2\xac\x86\xef\xb8\x8f" : "\xf0\x9f\x93\x82",
            esc_name);

        free(esc_name);
        free(url_fullpath);
        free(esc_fullpath);
    }

    sb_append(&sb, "    </div>\n");

    /* "Select this directory" button */
    sb_appendf(&sb,
        "    <div class=\"dir-actions\">\n"
        "        <button class=\"btn btn-primary\" "
        "onclick=\"selectDirectory(&#39;%s&#39;)\">Select This Folder</button>\n"
        "        <button class=\"btn btn-ghost\" "
        "onclick=\"document.getElementById(&#39;dir-browser-modal&#39;).remove()\">Cancel</button>\n"
        "    </div>\n",
        esc_path);

    sb_append(&sb, "</div>\n");

    free(esc_path);
    free(url_path);
    return sb_finish(&sb);
}

/* ------------------------------------------------------------------ */
/*  render_success / render_error                                     */
/* ------------------------------------------------------------------ */

char *render_success(const char *message)
{
    char *esc = html_escape(message);
    char *html = str_fmt("<div class=\"alert alert-success\">\xe2\x9c\x93 %s</div>", esc);
    free(esc);
    return html;
}

char *render_error(const char *message)
{
    char *esc = html_escape(message);
    char *html = str_fmt("<div class=\"alert alert-error\">\xe2\x9c\x97 %s</div>", esc);
    free(esc);
    return html;
}

char *render_error_html(const char *html_message)
{
    return str_fmt("<div class=\"alert alert-error\">\xe2\x9c\x97 %s</div>", html_message);
}

char *render_orphan_warning(const char *vm_name, char **orphan_names, int count)
{
    if (!orphan_names || count <= 0)
        return str_dup("");

    char *esc_vm = html_escape(vm_name);

    strbuf_t sb;
    sb_init(&sb, 1024);

    sb_append(&sb,
        "<div class=\"orphan-warning\">\n"
        "  <div class=\"orphan-header\">\n"
        "    <span class=\"orphan-icon\">⚠️</span>\n");
    sb_appendf(&sb,
        "    <strong>Found %d orphaned snapshot%s in disk files</strong>\n",
        count, count == 1 ? "" : "s");
    sb_append(&sb,
        "  </div>\n"
        "  <p class=\"orphan-detail\">These waste disk space and may cause naming "
        "conflicts when creating new snapshots.</p>\n"
        "  <details class=\"orphan-list-details\">\n"
        "    <summary>Show orphan names</summary>\n"
        "    <ul class=\"orphan-list\">\n");

    for (int i = 0; i < count; i++) {
        char *esc = html_escape(orphan_names[i]);
        sb_appendf(&sb, "      <li>%s</li>\n", esc);
        free(esc);
    }

    sb_append(&sb, "    </ul>\n  </details>\n");
    sb_appendf(&sb,
        "  <button class=\"btn btn-sm btn-warning\"\n"
        "          hx-post=\"/api/vms/%s/orphan-cleanup\"\n"
        "          hx-target=\"#orphan-check\"\n"
        "          hx-swap=\"innerHTML\"\n"
        "          hx-confirm=\"Delete %d orphaned snapshot%s from disk files?\">\n"
        "    🧹 Clean Up\n"
        "  </button>\n"
        "</div>\n",
        esc_vm, count, count == 1 ? "" : "s");

    free(esc_vm);
    return sb_finish(&sb);
}

/* ------------------------------------------------------------------ */
/*  render_guest_agent_help                                           */
/* ------------------------------------------------------------------ */

char *render_guest_agent_help(const char *operation, const char *detail,
                             guest_os_t detected_os)
{
    strbuf_t sb;
    sb_init(&sb, 4096);

    char *esc_op = html_escape(operation);
    char *esc_detail = detail ? html_escape(detail) : NULL;

    sb_append(&sb,
        "<div class=\"alert alert-error\">\n"
        "  <strong>\xe2\x9c\x97 ");
    sb_appendf(&sb, "Failed to %s</strong>", esc_op);

    if (esc_detail) {
        sb_appendf(&sb, "<br><code style=\"font-size:0.85em\">%s</code>", esc_detail);
    }

    /* Determine which sections to auto-expand based on detected OS */
    int linux_open = (detected_os == GUEST_OS_LINUX_SYSTEMD ||
                      detected_os == GUEST_OS_LINUX_OPENRC ||
                      detected_os == GUEST_OS_LINUX_OTHER);
    int windows_open = (detected_os == GUEST_OS_WINDOWS);
    int freebsd_open = (detected_os == GUEST_OS_FREEBSD);
    /* If unknown, show all collapsed */

    if (esc_detail) {
        sb_appendf(&sb, "<br><code style=\"font-size:0.85em\">%s</code>", esc_detail);
    }

    sb_append(&sb,
        "<br><br>"
        "  This feature uses the <strong>QEMU Guest Agent</strong> to run commands "
        "inside the VM. The guest agent must be installed and running in the guest OS.\n"
        "  <br><br>\n");

    sb_appendf(&sb, "  <details%s>\n", linux_open ? " open" : "");
    sb_append(&sb,
        "    <summary><strong>\xf0\x9f\x90\xa7 Linux</strong></summary>\n"
        "    <div style=\"padding:8px 0 4px 16px\">\n"
        "      <details>\n"
        "        <summary>Fedora / RHEL / CentOS</summary>\n"
        "        <pre style=\"margin:4px 0\">sudo dnf install qemu-guest-agent\n"
        "sudo systemctl enable --now qemu-guest-agent</pre>\n"
        "      </details>\n"
        "      <details>\n"
        "        <summary>Ubuntu / Debian</summary>\n"
        "        <pre style=\"margin:4px 0\">sudo apt install qemu-guest-agent\n"
        "sudo systemctl enable --now qemu-guest-agent</pre>\n"
        "      </details>\n"
        "      <details>\n"
        "        <summary>Arch Linux</summary>\n"
        "        <pre style=\"margin:4px 0\">sudo pacman -S qemu-guest-agent\n"
        "sudo systemctl enable --now qemu-guest-agent</pre>\n"
        "      </details>\n"
        "      <details>\n"
        "        <summary>Alpine Linux</summary>\n"
        "        <pre style=\"margin:4px 0\">apk add qemu-guest-agent\n"
        "rc-update add qemu-guest-agent\n"
        "rc-service qemu-guest-agent start</pre>\n"
        "      </details>\n"
        "      <details>\n"
        "        <summary>openSUSE</summary>\n"
        "        <pre style=\"margin:4px 0\">sudo zypper install qemu-guest-agent\n"
        "sudo systemctl enable --now qemu-guest-agent</pre>\n"
        "      </details>\n"
        "    </div>\n"
        "  </details>\n");

    sb_appendf(&sb, "  <details%s>\n", windows_open ? " open" : "");
    sb_append(&sb,
        "    <summary><strong>\xf0\x9f\xaa\x9f Windows</strong></summary>\n"
        "    <div style=\"padding:8px 0 4px 16px\">\n"
        "      Download and install the <a href=\"https://fedorapeople.org/groups/virt/virtio-win/direct-downloads/stable-virtio/\" "
        "target=\"_blank\">VirtIO guest tools (virtio-win)</a>.<br>\n"
        "      The QEMU Guest Agent is included in the installer.\n"
        "    </div>\n"
        "  </details>\n");

    sb_appendf(&sb, "  <details%s>\n", freebsd_open ? " open" : "");
    sb_append(&sb,
        "    <summary><strong>\xf0\x9f\x98\x88 FreeBSD</strong></summary>\n"
        "    <div style=\"padding:8px 0 4px 16px\">\n"
        "      <pre style=\"margin:4px 0\">pkg install qemu-guest-agent\n"
        "sysrc qemu_guest_agent_enable=YES\n"
        "service qemu-guest-agent start</pre>\n"
        "    </div>\n"
        "  </details>\n"
        "  <br>\n"
        "  <details>\n"
        "    <summary><strong>\xf0\x9f\x94\x92 SELinux issue?</strong> (Fedora/RHEL)</summary>\n"
        "    <div style=\"padding:8px 0 4px 16px\">\n"
        "      If the agent is installed but operations still fail, SELinux may be blocking it:\n"
        "      <pre style=\"margin:4px 0\">sudo semanage permissive -a virt_qemu_ga_t</pre>\n"
        "      Requires: <code>sudo dnf install policycoreutils-python-utils</code>\n"
        "    </div>\n"
        "  </details>\n"
        "</div>\n");

    free(esc_op);
    free(esc_detail);
    return sb_finish(&sb);
}
