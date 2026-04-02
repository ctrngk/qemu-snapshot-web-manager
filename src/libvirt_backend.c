#include "libvirt_backend.h"
#include "util.h"
#include "snapshot.h"

#include <libvirt/libvirt.h>
#include <libvirt/libvirt-qemu.h>
#include <libvirt/virterror.h>
#include <jansson.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <time.h>
#include <pthread.h>
#include <linux/limits.h>   /* PATH_MAX */
#include <sys/wait.h>       /* WEXITSTATUS */

/* ------------------------------------------------------------------ */
/*  module state                                                      */
/* ------------------------------------------------------------------ */

static virConnectPtr conn = NULL;
static pthread_mutex_t conn_mutex = PTHREAD_MUTEX_INITIALIZER;

/* Thread-local buffer for passing libvirt error details to route handlers */
static _Thread_local char lv_last_error[1024] = "";

const char *lv_get_last_error(void) { return lv_last_error; }

/* ------------------------------------------------------------------ */
/*  XML parsing helpers (no libxml2 dependency)                       */
/* ------------------------------------------------------------------ */

/* Extract text content between <tag> and </tag>. Returns malloc'd string. */
static char *extract_xml_content(const char *xml, const char *tag)
{
    char open_tag[64], close_tag[64];
    snprintf(open_tag, sizeof(open_tag), "<%s>", tag);
    snprintf(close_tag, sizeof(close_tag), "</%s>", tag);

    const char *start = strstr(xml, open_tag);
    if (!start) return NULL;
    start += strlen(open_tag);

    const char *end = strstr(start, close_tag);
    if (!end) return NULL;

    size_t len = (size_t)(end - start);
    char *result = malloc(len + 1);
    if (!result) return NULL;
    memcpy(result, start, len);
    result[len] = '\0';
    return result;
}

/* Like extract_xml_content but handles tags with attributes: <tag attr='...'>content</tag> */
static char *extract_xml_content_ext(const char *xml, const char *tag)
{
    /* First try exact match <tag> */
    char *result = extract_xml_content(xml, tag);
    if (result) return result;

    /* Try <tag ... (space after tag name indicates attributes) */
    char pattern[64], close_tag[64];
    snprintf(pattern, sizeof(pattern), "<%s ", tag);
    snprintf(close_tag, sizeof(close_tag), "</%s>", tag);

    const char *tag_start = strstr(xml, pattern);
    if (!tag_start) return NULL;

    /* Find the closing > of the opening tag */
    const char *content_start = strchr(tag_start, '>');
    if (!content_start) return NULL;
    content_start++;  /* skip the '>' */

    const char *end = strstr(content_start, close_tag);
    if (!end) return NULL;

    size_t len = (size_t)(end - content_start);
    result = malloc(len + 1);
    if (!result) return NULL;
    memcpy(result, content_start, len);
    result[len] = '\0';
    return result;
}

/* Extract an attribute value from an XML tag: <tag ... attr='value' .../>
 * Searches within the given xml fragment only. Returns malloc'd string. */
static char *extract_xml_attr(const char *xml, const char *tag, const char *attr)
{
    char pattern[128];
    snprintf(pattern, sizeof(pattern), "<%s ", tag);
    const char *tag_start = strstr(xml, pattern);
    if (!tag_start) return NULL;

    /* Try single-quoted attribute first, then double-quoted */
    snprintf(pattern, sizeof(pattern), "%s='", attr);
    const char *attr_start = strstr(tag_start, pattern);
    if (!attr_start) {
        snprintf(pattern, sizeof(pattern), "%s=\"", attr);
        attr_start = strstr(tag_start, pattern);
    }
    if (!attr_start) return NULL;

    attr_start += strlen(pattern);
    char quote = *(attr_start - 1);   /* ' or " */
    const char *attr_end = strchr(attr_start, quote);
    if (!attr_end) return NULL;

    size_t len = (size_t)(attr_end - attr_start);
    char *result = malloc(len + 1);
    if (!result) return NULL;
    memcpy(result, attr_start, len);
    result[len] = '\0';
    return result;
}

/* Convert a unix timestamp string to an ISO-8601 string. */
static char *unix_to_iso(const char *unix_str)
{
    time_t t = (time_t)atol(unix_str);
    struct tm tm;
    gmtime_r(&t, &tm);
    char buf[32];
    strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
    return str_dup(buf);
}

/* ------------------------------------------------------------------ */
/*  mutex helpers                                                     */
/* ------------------------------------------------------------------ */

/* Lock mutex, check conn, lookup domain. Caller must call conn_unlock() when done with dom. */
static virDomainPtr lv_lookup_domain_locked(const char *vm_name)
{
    pthread_mutex_lock(&conn_mutex);
    if (!conn) {
        pthread_mutex_unlock(&conn_mutex);
        return NULL;
    }
    virDomainPtr dom = virDomainLookupByName(conn, vm_name);
    if (!dom) {
        pthread_mutex_unlock(&conn_mutex);
        return NULL;
    }
    return dom;  /* caller must call conn_unlock() after using dom */
}

static void conn_unlock(void)
{
    pthread_mutex_unlock(&conn_mutex);
}

static void conn_lock(void)
{
    pthread_mutex_lock(&conn_mutex);
}

/* ------------------------------------------------------------------ */
/*  connection lifecycle                                               */
/* ------------------------------------------------------------------ */

static int lv_connect(const char *uri)
{
    pthread_mutex_lock(&conn_mutex);
    conn = virConnectOpen(uri);
    if (!conn) {
        pthread_mutex_unlock(&conn_mutex);
        log_msg(LOG_ERROR, "libvirt: failed to connect to %s", uri);
        return -1;
    }
    pthread_mutex_unlock(&conn_mutex);
    log_msg(LOG_INFO, "libvirt: connected to %s", uri);
    return 0;
}

static void lv_disconnect(void)
{
    pthread_mutex_lock(&conn_mutex);
    if (conn) {
        virConnectClose(conn);
        conn = NULL;
    }
    pthread_mutex_unlock(&conn_mutex);
}

/* ------------------------------------------------------------------ */
/*  VM listing                                                        */
/* ------------------------------------------------------------------ */

static vm_state_t map_domain_state(int state)
{
    switch (state) {
    case VIR_DOMAIN_RUNNING: return VM_RUNNING;
    case VIR_DOMAIN_PAUSED:  return VM_PAUSED;
    case VIR_DOMAIN_SHUTOFF: return VM_SHUTOFF;
    default:                 return VM_OTHER;
    }
}

static int lv_list_vms(vm_info_t ***vms, int *count)
{
    pthread_mutex_lock(&conn_mutex);
    virDomainPtr *domains = NULL;
    int n = virConnectListAllDomains(conn, &domains,
                VIR_CONNECT_LIST_DOMAINS_ACTIVE |
                VIR_CONNECT_LIST_DOMAINS_INACTIVE);
    pthread_mutex_unlock(&conn_mutex);
    if (n < 0) {
        log_msg(LOG_ERROR, "libvirt: virConnectListAllDomains failed");
        *vms   = NULL;
        *count = 0;
        return -1;
    }

    vm_info_t **list = calloc((size_t)n, sizeof(vm_info_t *));
    if (!list) {
        for (int i = 0; i < n; i++)
            virDomainFree(domains[i]);
        free(domains);
        return -1;
    }

    for (int i = 0; i < n; i++) {
        vm_info_t *info = calloc(1, sizeof(vm_info_t));
        if (!info) {
            /* clean up what we've built so far */
            for (int j = 0; j < i; j++) {
                free(list[j]->name);
                free(list[j]->uuid);
                free(list[j]);
            }
            free(list);
            for (int j = i; j < n; j++)
                virDomainFree(domains[j]);
            free(domains);
            return -1;
        }

        info->name = str_dup(virDomainGetName(domains[i]));

        char uuid_buf[VIR_UUID_STRING_BUFLEN];
        virDomainGetUUIDString(domains[i], uuid_buf);
        info->uuid = str_dup(uuid_buf);

        int state = 0, reason = 0;
        virDomainGetState(domains[i], &state, &reason, 0);
        info->state = map_domain_state(state);

        virDomainInfo dinfo;
        if (virDomainGetInfo(domains[i], &dinfo) == 0) {
            info->vcpus     = (int)dinfo.nrVirtCpu;
            info->memory_kb = dinfo.memory;
        }

        list[i] = info;
        virDomainFree(domains[i]);
    }

    free(domains);
    *vms   = list;
    *count = n;
    return 0;
}

static void lv_free_vm_list(vm_info_t **vms, int count)
{
    if (!vms) return;
    for (int i = 0; i < count; i++) {
        if (vms[i]) {
            free(vms[i]->name);
            free(vms[i]->uuid);
            free(vms[i]);
        }
    }
    free(vms);
}

/* ------------------------------------------------------------------ */
/*  VM lifecycle                                                      */
/* ------------------------------------------------------------------ */

static int lv_vm_start(const char *vm_name)
{
    virDomainPtr dom = lv_lookup_domain_locked(vm_name);
    if (!dom) return -1;
    conn_unlock();
    int ret = virDomainCreate(dom);
    virDomainFree(dom);
    return ret;
}

static int lv_vm_stop(const char *vm_name)
{
    virDomainPtr dom = lv_lookup_domain_locked(vm_name);
    if (!dom) return -1;
    conn_unlock();
    int ret = virDomainShutdown(dom);
    virDomainFree(dom);
    return ret;
}

static int lv_vm_force_stop(const char *vm_name)
{
    virDomainPtr dom = lv_lookup_domain_locked(vm_name);
    if (!dom) return -1;
    conn_unlock();
    int ret = virDomainDestroy(dom);
    virDomainFree(dom);
    return ret;
}

static int lv_vm_pause(const char *vm_name)
{
    virDomainPtr dom = lv_lookup_domain_locked(vm_name);
    if (!dom) return -1;
    conn_unlock();
    int ret = virDomainSuspend(dom);
    virDomainFree(dom);
    return ret;
}

static int lv_vm_resume(const char *vm_name)
{
    virDomainPtr dom = lv_lookup_domain_locked(vm_name);
    if (!dom) return -1;
    conn_unlock();
    int ret = virDomainResume(dom);
    virDomainFree(dom);
    return ret;
}

/* ------------------------------------------------------------------ */
/*  snapshot operations                                               */
/* ------------------------------------------------------------------ */

static int lv_list_snapshots(const char *vm_name, snapshot_node_t **tree)
{
    *tree = NULL;

    virDomainPtr dom = lv_lookup_domain_locked(vm_name);
    if (!dom) return -1;
    conn_unlock();

    virDomainSnapshotPtr *snaps = NULL;
    int n = virDomainListAllSnapshots(dom, &snaps, 0);
    if (n < 0) {
        virDomainFree(dom);
        return -1;
    }
    if (n == 0) {
        free(snaps);
        virDomainFree(dom);
        return 0;   /* success, just empty */
    }

    /* Determine the current snapshot name (may be NULL) */
    char *current_name = NULL;
    virDomainSnapshotPtr cur = virDomainSnapshotCurrent(dom, 0);
    if (cur) {
        current_name = str_dup(virDomainSnapshotGetName(cur));
        virDomainSnapshotFree(cur);
    }

    /* Temporary arrays for building the tree */
    snapshot_node_t **nodes = calloc((size_t)n, sizeof(snapshot_node_t *));
    virDomainSnapshotPtr *snap_ptrs = calloc((size_t)n, sizeof(virDomainSnapshotPtr));
    if (!nodes || !snap_ptrs) {
        free(nodes);
        free(snap_ptrs);
        free(current_name);
        for (int i = 0; i < n; i++)
            virDomainSnapshotFree(snaps[i]);
        free(snaps);
        virDomainFree(dom);
        return -1;
    }
    memcpy(snap_ptrs, snaps, (size_t)n * sizeof(virDomainSnapshotPtr));

    /* Phase 1: create a snapshot_node_t for each snapshot */
    for (int i = 0; i < n; i++) {
        const char *name = virDomainSnapshotGetName(snap_ptrs[i]);

        char *xml = virDomainSnapshotGetXMLDesc(snap_ptrs[i], 0);
        char *desc = xml ? extract_xml_content(xml, "description") : NULL;
        char *ctime_str = xml ? extract_xml_content(xml, "creationTime") : NULL;
        char *iso_time = ctime_str ? unix_to_iso(ctime_str) : str_dup("");

        /* Detect snapshot type from XML */
        snap_type_t stype = SNAP_INTERNAL;
        if (xml && (strstr(xml, "snapshot='external'") ||
                    strstr(xml, "snapshot=\"external\"") ||
                    strstr(xml, "disk-only"))) {
            stype = SNAP_EXTERNAL;
        }

        int is_cur = (current_name && str_eq(name, current_name)) ? 1 : 0;

        nodes[i] = snapshot_node_new(name,
                                     desc ? desc : "",
                                     iso_time ? iso_time : "",
                                     stype,
                                     is_cur);

        free(desc);
        free(ctime_str);
        free(iso_time);
        free(xml);
    }

    /* Phase 2: build parent-child relationships */
    snapshot_node_t *root = NULL;
    for (int i = 0; i < n; i++) {
        virDomainSnapshotPtr parent_snap =
            virDomainSnapshotGetParent(snap_ptrs[i], 0);

        if (parent_snap) {
            const char *parent_name = virDomainSnapshotGetName(parent_snap);
            /* Find matching node */
            for (int j = 0; j < n; j++) {
                if (nodes[j] && str_eq(nodes[j]->id, parent_name)) {
                    snapshot_node_add_child(nodes[j], nodes[i]);
                    break;
                }
            }
            virDomainSnapshotFree(parent_snap);
        } else {
            /* No parent — this is a root node */
            if (!root) {
                root = nodes[i];
            } else {
                /* Multiple roots: attach to existing root */
                snapshot_node_add_child(root, nodes[i]);
            }
        }
    }

    /* Clean up libvirt handles */
    for (int i = 0; i < n; i++)
        virDomainSnapshotFree(snap_ptrs[i]);
    free(snap_ptrs);
    free(snaps);
    free(nodes);
    free(current_name);
    virDomainFree(dom);

    *tree = root;
    return 0;
}

/* ------------------------------------------------------------------ */
/*  NVRAM format conversion (raw → qcow2 for internal snapshots)      */
/* ------------------------------------------------------------------ */

/*
 * Convert a VM's NVRAM from raw to qcow2 format.
 * Required for internal snapshots on UEFI VMs.
 * VM MUST be shut off before calling this.
 * Returns: 0 = converted, 1 = already qcow2, -1 = error.
 */

/* Check if a VM's NVRAM is in qcow2 format.
 * Returns: 1 = qcow2, 0 = not qcow2 (raw), -1 = no NVRAM (not UEFI). */
int lv_check_nvram_format(const char *vm_name)
{
    virDomainPtr dom = lv_lookup_domain_locked(vm_name);
    if (!dom) return -1;
    conn_unlock();

    char *dom_xml = virDomainGetXMLDesc(dom, VIR_DOMAIN_XML_INACTIVE);
    virDomainFree(dom);
    if (!dom_xml) return -1;

    char *nvram_path = extract_xml_content_ext(dom_xml, "nvram");
    if (!nvram_path || strlen(nvram_path) == 0) {
        free(nvram_path);
        free(dom_xml);
        return -1;  /* no NVRAM = not a UEFI VM */
    }
    free(nvram_path);

    char *fmt = extract_xml_attr(dom_xml, "nvram", "format");
    free(dom_xml);

    int result = (fmt && str_eq(fmt, "qcow2")) ? 1 : 0;
    free(fmt);
    return result;
}

int lv_convert_nvram(const char *vm_name)
{
    virDomainPtr dom = lv_lookup_domain_locked(vm_name);
    if (!dom) return -1;
    conn_unlock();

    /* Check VM is shut off */
    int info_state;
    virDomainInfo info;
    if (virDomainGetInfo(dom, &info) == 0)
        info_state = info.state;
    else
        info_state = -1;

    if (info_state != VIR_DOMAIN_SHUTOFF) {
        snprintf(lv_last_error, sizeof(lv_last_error),
                 "VM must be stopped before converting NVRAM");
        virDomainFree(dom);
        return -1;
    }

    /* Get inactive (persistent) XML */
    char *dom_xml = virDomainGetXMLDesc(dom, VIR_DOMAIN_XML_INACTIVE);
    if (!dom_xml) {
        snprintf(lv_last_error, sizeof(lv_last_error), "Failed to get domain XML");
        virDomainFree(dom);
        return -1;
    }

    /* Find <nvram ...>path</nvram> */
    char *nvram_path = extract_xml_content_ext(dom_xml, "nvram");
    if (!nvram_path || strlen(nvram_path) == 0) {
        snprintf(lv_last_error, sizeof(lv_last_error),
                 "No NVRAM path found in domain XML (not a UEFI VM?)");
        free(nvram_path);
        free(dom_xml);
        virDomainFree(dom);
        return -1;
    }

    /* Check current format — look for format='qcow2' on <nvram> tag */
    char *nvram_format = extract_xml_attr(dom_xml, "nvram", "format");
    if (nvram_format && str_eq(nvram_format, "qcow2")) {
        log_msg(LOG_INFO, "NVRAM already in qcow2 format: %s", nvram_path);
        free(nvram_format);
        free(nvram_path);
        free(dom_xml);
        virDomainFree(dom);
        return 1;  /* already qcow2 */
    }
    free(nvram_format);

    /* Build qcow2 path: replace .fd extension with .qcow2 */
    char qcow2_path[PATH_MAX];
    const char *dot = strrchr(nvram_path, '.');
    if (dot) {
        size_t prefix_len = (size_t)(dot - nvram_path);
        snprintf(qcow2_path, sizeof(qcow2_path), "%.*s.qcow2",
                 (int)prefix_len, nvram_path);
    } else {
        snprintf(qcow2_path, sizeof(qcow2_path), "%s.qcow2", nvram_path);
    }

    /* Convert: qemu-img convert -f raw -O qcow2 <old> <new> */
    char cmd[PATH_MAX * 2 + 128];
    snprintf(cmd, sizeof(cmd),
             "qemu-img convert -f raw -O qcow2 '%s' '%s'",
             nvram_path, qcow2_path);
    log_msg(LOG_INFO, "Converting NVRAM: %s", cmd);
    int rc = system(cmd);
    if (rc != 0) {
        snprintf(lv_last_error, sizeof(lv_last_error),
                 "qemu-img convert failed (exit %d)", WEXITSTATUS(rc));
        free(nvram_path);
        free(dom_xml);
        virDomainFree(dom);
        return -1;
    }

    /* Update domain XML: replace nvram path and format attributes */
    /* We need to change:
     *   format='raw'  → format='qcow2'
     *   >old_path</nvram>  → >new_path</nvram>
     */
    /* Strategy: find and replace in the XML string */
    size_t xml_len = strlen(dom_xml);
    char *new_xml = malloc(xml_len + 512);
    if (!new_xml) {
        snprintf(lv_last_error, sizeof(lv_last_error), "Out of memory");
        free(nvram_path);
        free(dom_xml);
        virDomainFree(dom);
        return -1;
    }

    /* Copy XML, replacing nvram path */
    char *nvram_start = strstr(dom_xml, nvram_path);
    if (!nvram_start) {
        snprintf(lv_last_error, sizeof(lv_last_error),
                 "Could not find NVRAM path in XML for replacement");
        free(new_xml);
        free(nvram_path);
        free(dom_xml);
        virDomainFree(dom);
        return -1;
    }

    size_t prefix = (size_t)(nvram_start - dom_xml);
    memcpy(new_xml, dom_xml, prefix);
    size_t offset = prefix;
    offset += (size_t)sprintf(new_xml + offset, "%s", qcow2_path);
    size_t old_path_len = strlen(nvram_path);
    strcpy(new_xml + offset, nvram_start + old_path_len);

    /* Now replace format='raw' with format='qcow2' in the <nvram> tag area */
    /* Find the <nvram tag in new_xml */
    char *nvram_tag = strstr(new_xml, "<nvram ");
    if (nvram_tag) {
        /* Find format='raw' or format="raw" within the tag */
        char *tag_end = strchr(nvram_tag, '>');
        if (tag_end) {
            char *fmt_raw = strstr(nvram_tag, "format='raw'");
            char *fmt_raw2 = strstr(nvram_tag, "format=\"raw\"");
            char *fmt_pos = fmt_raw ? fmt_raw : fmt_raw2;
            if (fmt_pos && fmt_pos < tag_end) {
                /* Replace "raw" with "qcow2" — need to shift rest of string */
                /* "format='raw'" (12 chars) → "format='qcow2'" (14 chars) */
                char quote = fmt_raw ? '\'' : '"';
                size_t old_len = (size_t)(12 + (fmt_raw2 ? 0 : 0)); /* format='raw' = 12 chars */
                char replacement[32];
                snprintf(replacement, sizeof(replacement), "format=%cqcow2%c", quote, quote);
                size_t new_len = strlen(replacement);
                size_t rest_len = strlen(fmt_pos + old_len);
                memmove(fmt_pos + new_len, fmt_pos + old_len, rest_len + 1);
                memcpy(fmt_pos, replacement, new_len);
            }
        }
    }

    log_msg(LOG_DEBUG, "Redefining domain with updated NVRAM path");
    conn_lock();
    virDomainPtr new_dom = virDomainDefineXML(conn, new_xml);
    conn_unlock();
    free(new_xml);

    if (!new_dom) {
        virErrorPtr err = virGetLastError();
        snprintf(lv_last_error, sizeof(lv_last_error),
                 "Failed to redefine domain: %s",
                 (err && err->message) ? err->message : "unknown");
        /* Revert: try to remove the qcow2 file */
        unlink(qcow2_path);
        free(nvram_path);
        free(dom_xml);
        virDomainFree(dom);
        return -1;
    }
    virDomainFree(new_dom);

    log_msg(LOG_INFO, "NVRAM converted: %s → %s", nvram_path, qcow2_path);

    free(nvram_path);
    free(dom_xml);
    virDomainFree(dom);
    return 0;
}

static int lv_create_snapshot(const char *vm_name, const char *snap_name,
                              const char *description, snap_type_t type)
{
    virDomainPtr dom = lv_lookup_domain_locked(vm_name);
    if (!dom) return -1;
    conn_unlock();

    char xml[4096];
    if (type == SNAP_EXTERNAL) {
        snprintf(xml, sizeof(xml),
            "<domainsnapshot>"
            "<name>%s</name>"
            "<description>%s</description>"
            "<disks><disk name='vda' snapshot='external'/></disks>"
            "</domainsnapshot>",
            snap_name, description ? description : "");
    } else {
        snprintf(xml, sizeof(xml),
            "<domainsnapshot>"
            "<name>%s</name>"
            "<description>%s</description>"
            "</domainsnapshot>",
            snap_name, description ? description : "");
    }

    unsigned int flags = (type == SNAP_EXTERNAL)
        ? (VIR_DOMAIN_SNAPSHOT_CREATE_DISK_ONLY |
           VIR_DOMAIN_SNAPSHOT_CREATE_ATOMIC)
        : 0;

    virDomainSnapshotPtr snap = virDomainSnapshotCreateXML(dom, xml, flags);

    if (!snap) {
        /* Save the libvirt error before virDomainFree clears it */
        virErrorPtr err = virGetLastError();
        if (err && err->message)
            snprintf(lv_last_error, sizeof(lv_last_error), "%s", err->message);
        else
            snprintf(lv_last_error, sizeof(lv_last_error), "Unknown error");
        log_msg(LOG_ERROR, "libvirt: failed to create snapshot '%s': %s",
                snap_name, lv_last_error);
        virDomainFree(dom);
        return -1;
    }
    virDomainFree(dom);
    virDomainSnapshotFree(snap);
    log_msg(LOG_INFO, "libvirt: created %s snapshot '%s' on '%s'",
            type == SNAP_EXTERNAL ? "external" : "internal",
            snap_name, vm_name);
    return 0;
}

/* Edit snapshot description using REDEFINE flag.
 * Gets current XML, replaces <description> content, redefines. */
int lv_edit_snapshot_description(const char *vm_name, const char *snap_name,
                                 const char *new_description)
{
    virDomainPtr dom = lv_lookup_domain_locked(vm_name);
    if (!dom) return -1;
    conn_unlock();

    virDomainSnapshotPtr snap =
        virDomainSnapshotLookupByName(dom, snap_name, 0);
    if (!snap) {
        snprintf(lv_last_error, sizeof(lv_last_error),
                 "Snapshot '%s' not found", snap_name);
        virDomainFree(dom);
        return -1;
    }

    /* Check if this is the current snapshot */
    int is_current = 0;
    virDomainSnapshotPtr cur = virDomainSnapshotCurrent(dom, 0);
    if (cur) {
        const char *cur_name = virDomainSnapshotGetName(cur);
        if (cur_name && strcmp(cur_name, snap_name) == 0)
            is_current = 1;
        virDomainSnapshotFree(cur);
    }

    char *xml = virDomainSnapshotGetXMLDesc(snap,
                    VIR_DOMAIN_SNAPSHOT_XML_SECURE);
    virDomainSnapshotFree(snap);
    if (!xml) {
        snprintf(lv_last_error, sizeof(lv_last_error),
                 "Failed to get snapshot XML");
        virDomainFree(dom);
        return -1;
    }

    /* Build new XML: replace or insert <description> */
    char *new_xml = NULL;
    const char *desc_start = strstr(xml, "<description>");
    const char *desc_end = strstr(xml, "</description>");

    if (desc_start && desc_end) {
        /* Replace existing description */
        size_t prefix_len = (size_t)(desc_start - xml) + strlen("<description>");
        size_t suffix_start = (size_t)(desc_end - xml);
        size_t new_len = prefix_len + strlen(new_description) +
                         strlen(xml) - suffix_start + 1;
        new_xml = malloc(new_len);
        if (new_xml) {
            memcpy(new_xml, xml, prefix_len);
            memcpy(new_xml + prefix_len, new_description, strlen(new_description));
            strcpy(new_xml + prefix_len + strlen(new_description),
                   xml + suffix_start);
        }
    } else {
        /* No <description> tag — insert after <name>...</name> */
        const char *name_end = strstr(xml, "</name>");
        if (name_end) {
            name_end += strlen("</name>");
            size_t prefix_len = (size_t)(name_end - xml);
            char desc_tag[2048];
            snprintf(desc_tag, sizeof(desc_tag),
                     "<description>%s</description>", new_description);
            size_t new_len = prefix_len + strlen(desc_tag) +
                             strlen(xml) - prefix_len + 1;
            new_xml = malloc(new_len);
            if (new_xml) {
                memcpy(new_xml, xml, prefix_len);
                memcpy(new_xml + prefix_len, desc_tag, strlen(desc_tag));
                strcpy(new_xml + prefix_len + strlen(desc_tag),
                       xml + prefix_len);
            }
        }
    }
    free(xml);

    if (!new_xml) {
        snprintf(lv_last_error, sizeof(lv_last_error),
                 "Failed to build modified snapshot XML");
        virDomainFree(dom);
        return -1;
    }

    unsigned int flags = VIR_DOMAIN_SNAPSHOT_CREATE_REDEFINE;
    if (is_current)
        flags |= VIR_DOMAIN_SNAPSHOT_CREATE_CURRENT;

    virDomainSnapshotPtr new_snap =
        virDomainSnapshotCreateXML(dom, new_xml, flags);
    free(new_xml);

    if (!new_snap) {
        virErrorPtr err = virGetLastError();
        if (err && err->message)
            snprintf(lv_last_error, sizeof(lv_last_error), "%s", err->message);
        else
            snprintf(lv_last_error, sizeof(lv_last_error), "REDEFINE failed");
        virDomainFree(dom);
        return -1;
    }

    virDomainSnapshotFree(new_snap);
    virDomainFree(dom);
    log_msg(LOG_INFO, "libvirt: updated description for snapshot '%s' on '%s'",
            snap_name, vm_name);
    return 0;
}

static int lv_delete_snapshot(const char *vm_name, const char *snap_name,
                              int auto_merge)
{
    (void)auto_merge;
    virDomainPtr dom = lv_lookup_domain_locked(vm_name);
    if (!dom) return -1;
    conn_unlock();

    virDomainSnapshotPtr snap =
        virDomainSnapshotLookupByName(dom, snap_name, 0);
    if (!snap) {
        virDomainFree(dom);
        return -1;
    }

    /* Detect snapshot type from XML to choose correct delete strategy.
     * Internal snapshots: flags=0 → deletes both metadata AND qcow2 data.
     * External snapshots: METADATA_ONLY → data lives in separate overlay files. */
    int is_external = 0;
    char *xml = virDomainSnapshotGetXMLDesc(snap, 0);
    if (xml) {
        if (strstr(xml, "snapshot='external'") ||
            strstr(xml, "snapshot=\"external\"") ||
            strstr(xml, "disk-only"))
            is_external = 1;
        free(xml);
    }

    unsigned int flags = is_external
        ? VIR_DOMAIN_SNAPSHOT_DELETE_METADATA_ONLY
        : 0;

    int ret = virDomainSnapshotDelete(snap, flags);

    virDomainSnapshotFree(snap);
    virDomainFree(dom);

    if (ret == 0)
        log_msg(LOG_INFO, "libvirt: deleted %s snapshot '%s'",
                is_external ? "external" : "internal", snap_name);
    else
        log_msg(LOG_ERROR, "libvirt: failed to delete snapshot '%s'", snap_name);
    return ret;
}

static int lv_revert_snapshot(const char *vm_name, const char *snap_name)
{
    virDomainPtr dom = lv_lookup_domain_locked(vm_name);
    if (!dom) return -1;
    conn_unlock();

    virDomainSnapshotPtr snap =
        virDomainSnapshotLookupByName(dom, snap_name, 0);
    if (!snap) {
        virDomainFree(dom);
        return -1;
    }

    int ret = virDomainRevertToSnapshot(snap, VIR_DOMAIN_SNAPSHOT_REVERT_FORCE);
    virDomainSnapshotFree(snap);
    virDomainFree(dom);
    return ret;
}

static int lv_merge_snapshot(const char *vm_name, const char *snap_name)
{
    virDomainPtr dom = lv_lookup_domain_locked(vm_name);
    if (!dom) return -1;
    conn_unlock();

    /* Verify snapshot exists */
    virDomainSnapshotPtr snap =
        virDomainSnapshotLookupByName(dom, snap_name, 0);
    if (!snap) {
        virDomainFree(dom);
        return -1;
    }
    virDomainSnapshotFree(snap);

    /* Block commit on vda — merges active layer into base */
    int ret = virDomainBlockCommit(dom, "vda", NULL, NULL, 0,
        VIR_DOMAIN_BLOCK_COMMIT_ACTIVE | VIR_DOMAIN_BLOCK_COMMIT_SHALLOW);

    virDomainFree(dom);

    if (ret == 0)
        log_msg(LOG_INFO, "libvirt: merge started for '%s'", snap_name);
    else
        log_msg(LOG_ERROR, "libvirt: merge failed for '%s'", snap_name);
    return ret;
}

/* ------------------------------------------------------------------ */
/*  shared folders                                                    */
/* ------------------------------------------------------------------ */

static int lv_list_shared_folders(const char *vm_name,
                                  shared_folder_t **out, int *count)
{
    *out   = NULL;
    *count = 0;

    virDomainPtr dom = lv_lookup_domain_locked(vm_name);
    if (!dom) return -1;
    conn_unlock();

    /* Use inactive (persistent) config so we see folders added with
     * VIR_DOMAIN_AFFECT_CONFIG that aren't yet in the live domain. */
    char *xml = virDomainGetXMLDesc(dom, VIR_DOMAIN_XML_INACTIVE);
    virDomainFree(dom);
    if (!xml) return -1;

    int capacity = 4;
    shared_folder_t *folders = calloc((size_t)capacity, sizeof(shared_folder_t));
    if (!folders) { free(xml); return -1; }
    int n = 0;

    const char *pos = xml;
    while ((pos = strstr(pos, "<filesystem")) != NULL) {
        const char *fs_end = strstr(pos, "</filesystem>");
        if (!fs_end) break;

        size_t block_len = (size_t)(fs_end - pos);
        char *block = malloc(block_len + 1);
        if (!block) break;
        memcpy(block, pos, block_len);
        block[block_len] = '\0';

        int is_virtiofs = (strstr(block, "virtiofs") != NULL);
        int is_9p = (strstr(block, "9p") != NULL) || (strstr(block, "'path'") != NULL);

        if (is_virtiofs || is_9p) {
            char *source = extract_xml_attr(block, "source", "dir");
            char *target = extract_xml_attr(block, "target", "dir");

            if (source && target) {
                if (n >= capacity) {
                    capacity *= 2;
                    shared_folder_t *tmp = realloc(folders,
                        (size_t)capacity * sizeof(shared_folder_t));
                    if (!tmp) {
                        free(source);
                        free(target);
                        free(block);
                        break;
                    }
                    folders = tmp;
                }
                folders[n].source_dir = source;
                folders[n].mount_tag  = target;
                folders[n].fs_type    = str_dup(is_virtiofs ? "virtiofs" : "9p");
                folders[n].read_only  = (strstr(block, "readonly") != NULL) ? 1 : 0;
                n++;
            } else {
                free(source);
                free(target);
            }
        }

        free(block);
        pos = fs_end + 1;
    }

    free(xml);
    *out   = folders;
    *count = n;
    return 0;
}

static void lv_free_shared_folders(shared_folder_t *folders, int count)
{
    if (!folders) return;
    for (int i = 0; i < count; i++) {
        free(folders[i].source_dir);
        free(folders[i].mount_tag);
        free(folders[i].fs_type);
    }
    free(folders);
}

/* ------------------------------------------------------------------ */
/*  shared folder management                                          */
/* ------------------------------------------------------------------ */

int lv_enable_shared_memory(const char *vm_name)
{
    virDomainPtr dom = lv_lookup_domain_locked(vm_name);
    if (!dom) return -1;
    conn_unlock();

    char *domxml = virDomainGetXMLDesc(dom, VIR_DOMAIN_XML_INACTIVE);
    if (!domxml) {
        virDomainFree(dom);
        return -1;
    }

    if (strstr(domxml, "<access mode='shared'")) {
        /* Already enabled */
        free(domxml);
        virDomainFree(dom);
        return 0;
    }

    log_msg(LOG_INFO, "libvirt: enabling shared memory for '%s'", vm_name);

    const char *mem_backing =
        "<memoryBacking>\n"
        "    <source type='memfd'/>\n"
        "    <access mode='shared'/>\n"
        "  </memoryBacking>\n";

    char *insert_point = strstr(domxml, "</domain>");
    if (!insert_point) {
        free(domxml);
        virDomainFree(dom);
        return -1;
    }

    size_t prefix_len = (size_t)(insert_point - domxml);
    size_t mb_len = strlen(mem_backing);
    size_t suffix_len = strlen(insert_point);
    char *newxml = malloc(prefix_len + mb_len + 4 + suffix_len + 1);
    if (!newxml) {
        free(domxml);
        virDomainFree(dom);
        return -1;
    }

    memcpy(newxml, domxml, prefix_len);
    memcpy(newxml + prefix_len, "  ", 2);
    memcpy(newxml + prefix_len + 2, mem_backing, mb_len);
    memcpy(newxml + prefix_len + 2 + mb_len, insert_point, suffix_len);
    newxml[prefix_len + 2 + mb_len + suffix_len] = '\0';
    free(domxml);

    conn_lock();
    virDomainPtr newdom = virDomainDefineXML(conn, newxml);
    conn_unlock();
    free(newxml);

    if (!newdom) {
        snprintf(lv_last_error, sizeof(lv_last_error),
                 "Failed to enable shared memory: %s",
                 virGetLastError() ? virGetLastError()->message : "unknown");
        virDomainFree(dom);
        return -1;
    }

    virDomainFree(newdom);
    virDomainFree(dom);
    log_msg(LOG_INFO, "libvirt: shared memory enabled on '%s'", vm_name);
    return 0;
}

/* Forward declaration — defined below in the guest agent section */
static int guest_exec(virDomainPtr dom, const char *command, char **out_msg);

static int lv_add_shared_folder(const char *vm_name, const char *source_dir,
                                const char *mount_tag, int read_only,
                                const char *fs_type)
{
    virDomainPtr dom = lv_lookup_domain_locked(vm_name);
    if (!dom) {
        log_msg(LOG_ERROR, "libvirt: VM '%s' not found", vm_name);
        return -1;
    }
    conn_unlock();

    /* virtiofs requires shared memory backing — check if missing */
    if (!str_eq(fs_type, "9p")) {
        char *domxml = virDomainGetXMLDesc(dom, VIR_DOMAIN_XML_INACTIVE);
        if (domxml && !strstr(domxml, "<access mode='shared'")) {
            log_msg(LOG_INFO, "libvirt: VM '%s' needs shared memory for virtiofs",
                    vm_name);
            free(domxml);
            virDomainFree(dom);
            snprintf(lv_last_error, sizeof(lv_last_error),
                     "VirtioFS requires shared memory to be enabled on this VM");
            return -2;  /* signal: shared memory required */
        }
        free(domxml);
    }

    /* Build filesystem XML */
    char xml[2048];
    if (str_eq(fs_type, "9p")) {
        snprintf(xml, sizeof(xml),
            "<filesystem type='mount' accessmode='mapped'>\n"
            "  <driver type='path' wrpolicy='immediate'/>\n"
            "  <source dir='%s'/>\n"
            "  <target dir='%s'/>\n"
            "%s"
            "</filesystem>",
            source_dir, mount_tag,
            read_only ? "  <readonly/>\n" : "");
    } else {
        /* Default to virtiofs */
        snprintf(xml, sizeof(xml),
            "<filesystem type='mount' accessmode='passthrough'>\n"
            "  <driver type='virtiofs'/>\n"
            "  <source dir='%s'/>\n"
            "  <target dir='%s'/>\n"
            "%s"
            "</filesystem>",
            source_dir, mount_tag,
            read_only ? "  <readonly/>\n" : "");
    }

    /* Determine VM state and attach accordingly */
    int state = 0, reason = 0;
    virDomainGetState(dom, &state, &reason, 0);

    unsigned int flags;
    int needs_restart = 0;
    if (state == VIR_DOMAIN_RUNNING || state == VIR_DOMAIN_PAUSED) {
        /* Check if shared memory is in the live config (required for virtiofs hot-add) */
        if (!str_eq(fs_type, "9p")) {
            char *livexml = virDomainGetXMLDesc(dom, 0);
            if (livexml && !strstr(livexml, "<access mode='shared'")) {
                /* Shared memory was just added to persistent config but the running
                 * VM doesn't have it yet — can only do config-only */
                flags = VIR_DOMAIN_AFFECT_CONFIG;
                needs_restart = 1;
                log_msg(LOG_INFO, "libvirt: VM '%s' needs restart for shared memory — "
                        "adding folder to config only", vm_name);
            } else {
                flags = VIR_DOMAIN_AFFECT_LIVE | VIR_DOMAIN_AFFECT_CONFIG;
                log_msg(LOG_WARN, "libvirt: hot-adding shared folder '%s' to running VM '%s'",
                        mount_tag, vm_name);
            }
            free(livexml);
        } else {
            flags = VIR_DOMAIN_AFFECT_LIVE | VIR_DOMAIN_AFFECT_CONFIG;
            log_msg(LOG_WARN, "libvirt: hot-adding shared folder '%s' to running VM '%s'",
                    mount_tag, vm_name);
        }
    } else {
        flags = VIR_DOMAIN_AFFECT_CONFIG;
    }

    int ret = virDomainAttachDeviceFlags(dom, xml, flags);
    if (ret != 0) {
        virErrorPtr verr = virGetLastError();
        const char *detail = (verr && verr->message) ? verr->message : "unknown error";
        snprintf(lv_last_error, sizeof(lv_last_error), "%s", detail);
        log_msg(LOG_ERROR, "libvirt: failed to add shared folder '%s' to '%s': %s",
                mount_tag, vm_name, detail);
    } else {
        lv_last_error[0] = '\0';
        if (needs_restart) {
            log_msg(LOG_INFO, "libvirt: folder '%s' saved to config — VM restart required",
                    mount_tag);
            virDomainFree(dom);
            return 1;
        }
        log_msg(LOG_INFO, "libvirt: added shared folder '%s' (%s) to '%s'",
                mount_tag, fs_type, vm_name);

        /* Hot-added to running VM — try to mount inside guest immediately */
        if (flags & VIR_DOMAIN_AFFECT_LIVE) {
            const char *mnt_fs = str_eq(fs_type, "9p") ? "9p" : "virtiofs";
            char cmd[1024];
            snprintf(cmd, sizeof(cmd),
                "mkdir -p /media/%s && mount -t %s %s /media/%s 2>/dev/null",
                mount_tag, mnt_fs, mount_tag, mount_tag);
            char *msg = NULL;
            int mrc = guest_exec(dom, cmd, &msg);
            if (mrc == 0)
                log_msg(LOG_INFO, "libvirt: auto-mounted '%s' inside guest", mount_tag);
            else
                log_msg(LOG_DEBUG, "libvirt: guest mount of '%s' skipped: %s",
                        mount_tag, msg ? msg : "(no agent)");
            free(msg);
        }
    }
    virDomainFree(dom);
    return ret;
}

static int lv_remove_shared_folder(const char *vm_name, const char *mount_tag)
{
    virDomainPtr dom = lv_lookup_domain_locked(vm_name);
    if (!dom) {
        log_msg(LOG_ERROR, "libvirt: VM '%s' not found", vm_name);
        return -1;
    }
    conn_unlock();

    /* Get domain XML and find the matching <filesystem> block */
    char *full_xml = virDomainGetXMLDesc(dom, 0);
    if (!full_xml) {
        virDomainFree(dom);
        return -1;
    }

    /* Search for the filesystem block containing this mount_tag */
    char target_pattern[256];
    snprintf(target_pattern, sizeof(target_pattern), "dir='%s'", mount_tag);
    char target_pattern2[256];
    snprintf(target_pattern2, sizeof(target_pattern2), "dir=\"%s\"", mount_tag);

    char *fs_xml = NULL;
    const char *pos = full_xml;
    while ((pos = strstr(pos, "<filesystem")) != NULL) {
        const char *fs_end = strstr(pos, "</filesystem>");
        if (!fs_end) break;

        size_t block_len = (size_t)(fs_end + strlen("</filesystem>") - pos);
        char *block = malloc(block_len + 1);
        if (!block) break;
        memcpy(block, pos, block_len);
        block[block_len] = '\0';

        /* Check if this block has a <target dir="mount_tag"/> */
        char *target_tag = strstr(block, "<target ");
        if (target_tag &&
            (strstr(target_tag, target_pattern) || strstr(target_tag, target_pattern2))) {
            fs_xml = block;
            break;
        }

        free(block);
        pos = fs_end + 1;
    }

    free(full_xml);

    if (!fs_xml) {
        virDomainFree(dom);
        log_msg(LOG_ERROR, "libvirt: shared folder '%s' not found on VM '%s'",
                mount_tag, vm_name);
        return -1;
    }

    /* Determine VM state and detach accordingly */
    int state = 0, reason = 0;
    virDomainGetState(dom, &state, &reason, 0);

    unsigned int flags;
    if (state == VIR_DOMAIN_RUNNING || state == VIR_DOMAIN_PAUSED) {
        flags = VIR_DOMAIN_AFFECT_LIVE | VIR_DOMAIN_AFFECT_CONFIG;
        log_msg(LOG_WARN, "libvirt: hot-removing shared folder '%s' from running VM '%s'",
                mount_tag, vm_name);
    } else {
        flags = VIR_DOMAIN_AFFECT_CONFIG;
    }

    int ret = virDomainDetachDeviceFlags(dom, fs_xml, flags);
    free(fs_xml);
    virDomainFree(dom);

    if (ret != 0) {
        log_msg(LOG_ERROR, "libvirt: failed to remove shared folder '%s' from '%s'",
                mount_tag, vm_name);
        return -1;
    }
    log_msg(LOG_INFO, "libvirt: removed shared folder '%s' from '%s'",
            mount_tag, vm_name);
    return 0;
}

/* ------------------------------------------------------------------ */
/*  Guest agent helpers                                               */
/* ------------------------------------------------------------------ */

/* Execute a command inside the guest via the QEMU Guest Agent.
 * Returns 0 on success, -1 on failure.
 * If out_msg is not NULL, writes an error/success message (caller must free). */
static int guest_exec(virDomainPtr dom, const char *command, char **out_msg)
{
    /* JSON-escape the command to safely embed in the JSON string */
    char *escaped_cmd = json_escape(command);
    if (!escaped_cmd) {
        if (out_msg) *out_msg = strdup("Failed to allocate memory for command");
        return -1;
    }

    /* Build guest-exec JSON command.
     * Use /usr/bin/bash with explicit PATH for full command availability. */
    size_t json_len = strlen(escaped_cmd) + 256;
    char *cmd_json = malloc(json_len);
    if (!cmd_json) {
        free(escaped_cmd);
        if (out_msg) *out_msg = strdup("Failed to allocate memory for JSON command");
        return -1;
    }
    snprintf(cmd_json, json_len,
        "{\"execute\":\"guest-exec\","
        "\"arguments\":{\"path\":\"/usr/bin/bash\","
        "\"arg\":[\"-c\",\"%s\"],"
        "\"env\":[\"PATH=/usr/sbin:/usr/bin:/sbin:/bin\"],"
        "\"capture-output\":true}}",
        escaped_cmd);
    free(escaped_cmd);

    char *result = virDomainQemuAgentCommand(dom, cmd_json, 30, 0);
    free(cmd_json);
    if (!result) {
        if (out_msg) {
            virErrorPtr err = virGetLastError();
            if (err && err->message)
                *out_msg = strdup(err->message);
            else
                *out_msg = strdup("Guest agent not available. Is qemu-guest-agent installed and running in the VM?");
        }
        return -1;
    }

    /* Parse the PID from the response: {"return":{"pid":12345}} */
    json_error_t jerr;
    json_t *resp = json_loads(result, 0, &jerr);
    free(result);

    if (!resp) {
        if (out_msg) *out_msg = strdup("Failed to parse guest agent response");
        return -1;
    }

    json_t *ret_obj = json_object_get(resp, "return");
    int pid = (int)json_integer_value(json_object_get(ret_obj, "pid"));
    json_decref(resp);

    if (pid <= 0) {
        if (out_msg) *out_msg = strdup("Guest agent returned invalid PID");
        return -1;
    }

    /* Poll for command completion — retry up to 10 times (1s each) */
    int exited = 0;
    int exitcode = 0;
    const char *err_data = NULL;
    resp = NULL;
    ret_obj = NULL;

    for (int attempt = 0; attempt < 10; attempt++) {
        sleep(1);

        char status_json[256];
        snprintf(status_json, sizeof(status_json),
            "{\"execute\":\"guest-exec-status\",\"arguments\":{\"pid\":%d}}", pid);

        result = virDomainQemuAgentCommand(dom, status_json, 30, 0);
        if (!result) {
            if (out_msg) *out_msg = strdup("Failed to get command status from guest agent");
            return -1;
        }

        resp = json_loads(result, 0, &jerr);
        free(result);

        if (!resp) {
            if (out_msg) *out_msg = strdup("Failed to parse status response");
            return -1;
        }

        ret_obj = json_object_get(resp, "return");
        exited = json_is_true(json_object_get(ret_obj, "exited"));

        if (exited) {
            exitcode = (int)json_integer_value(json_object_get(ret_obj, "exitcode"));
            err_data = json_string_value(json_object_get(ret_obj, "err-data"));
            break;
        }

        json_decref(resp);
        resp = NULL;
    }

    if (!exited) {
        json_decref(resp);
        if (out_msg) *out_msg = strdup("Command still running in guest");
        return -1;
    }

    if (exitcode != 0) {
        if (out_msg) {
            /* Decode base64 stderr/stdout for meaningful error messages */
            const char *out_data_b64 = json_string_value(json_object_get(ret_obj, "out-data"));

            /* Try stderr first, then stdout */
            const char *b64 = (err_data && strlen(err_data) > 0) ? err_data :
                              (out_data_b64 && strlen(out_data_b64) > 0) ? out_data_b64 : NULL;

            if (exitcode == 126) {
                /* Permission denied — likely SELinux */
                *out_msg = strdup("Permission denied inside guest (SELinux may be blocking mount from the guest agent context).");
            } else if (exitcode == 127) {
                *out_msg = strdup("Command not found inside guest. Required packages may not be installed.");
            } else if (b64) {
                char *decoded = base64_decode(b64, NULL);
                if (decoded) {
                    char buf[1024];
                    snprintf(buf, sizeof(buf), "Command exited with code %d: %s", exitcode, decoded);
                    *out_msg = strdup(buf);
                    free(decoded);
                } else {
                    char buf[256];
                    snprintf(buf, sizeof(buf), "Command exited with code %d inside guest", exitcode);
                    *out_msg = strdup(buf);
                }
            } else {
                char buf[256];
                snprintf(buf, sizeof(buf), "Command exited with code %d inside guest", exitcode);
                *out_msg = strdup(buf);
            }
        }
        json_decref(resp);
        return -1;
    }

    /* Success — decode and return stdout */
    if (out_msg) {
        const char *out_data_b64 = json_string_value(json_object_get(ret_obj, "out-data"));
        if (out_data_b64 && strlen(out_data_b64) > 0) {
            *out_msg = base64_decode(out_data_b64, NULL);
        } else {
            *out_msg = strdup("");
        }
    }

    json_decref(resp);
    return 0;
}

static int lv_mount_shared_folder(const char *vm_name, const char *mount_tag,
                                   const char *fs_type)
{
    virDomainPtr dom = lv_lookup_domain_locked(vm_name);
    if (!dom) return -1;
    conn_unlock();

    /* Check VM is running */
    virDomainInfo info;
    if (virDomainGetInfo(dom, &info) != 0 || info.state != VIR_DOMAIN_RUNNING) {
        virDomainFree(dom);
        log_msg(LOG_ERROR, "VM %s is not running — cannot mount via guest agent", vm_name);
        snprintf(lv_last_error, sizeof(lv_last_error), "VM is not running");
        return -1;
    }

    /* Build mount command with full paths (guest agent has minimal PATH) */
    const char *fst = (fs_type && strlen(fs_type) > 0) ? fs_type : "virtiofs";
    char cmd[512];
    if (strcmp(fst, "9p") == 0) {
        snprintf(cmd, sizeof(cmd),
            "/usr/bin/mkdir -p /media/%s && /usr/bin/mount -t 9p -o trans=virtio %s /media/%s",
            mount_tag, mount_tag, mount_tag);
    } else {
        snprintf(cmd, sizeof(cmd),
            "/usr/bin/mkdir -p /media/%s && /usr/bin/mount -t virtiofs %s /media/%s",
            mount_tag, mount_tag, mount_tag);
    }

    char *msg = NULL;
    int ret = guest_exec(dom, cmd, &msg);
    if (ret != 0) {
        log_msg(LOG_ERROR, "Mount in guest failed: %s", msg ? msg : "unknown error");
        if (msg) snprintf(lv_last_error, sizeof(lv_last_error), "%s", msg);
    } else {
        log_msg(LOG_INFO, "Mounted %s at /media/%s in guest %s", mount_tag, mount_tag, vm_name);
    }
    free(msg);
    virDomainFree(dom);
    return ret;
}

static int lv_unmount_shared_folder(const char *vm_name, const char *mount_tag)
{
    virDomainPtr dom = lv_lookup_domain_locked(vm_name);
    if (!dom) return -1;
    conn_unlock();

    /* Check VM is running */
    virDomainInfo info;
    if (virDomainGetInfo(dom, &info) != 0 || info.state != VIR_DOMAIN_RUNNING) {
        virDomainFree(dom);
        log_msg(LOG_ERROR, "VM %s is not running — cannot unmount via guest agent", vm_name);
        snprintf(lv_last_error, sizeof(lv_last_error), "VM is not running");
        return -1;
    }

    char cmd[256];
    snprintf(cmd, sizeof(cmd), "/usr/bin/umount /media/%s", mount_tag);

    char *msg = NULL;
    int ret = guest_exec(dom, cmd, &msg);
    if (ret != 0) {
        log_msg(LOG_ERROR, "Unmount in guest failed: %s", msg ? msg : "unknown error");
        if (msg) snprintf(lv_last_error, sizeof(lv_last_error), "%s", msg);
    } else {
        log_msg(LOG_INFO, "Unmounted /media/%s in guest %s", mount_tag, vm_name);
    }
    free(msg);
    virDomainFree(dom);
    return ret;
}

static int lv_check_mount_status(const char *vm_name,
                                  shared_folder_t *folders, int count)
{
    if (!folders || count <= 0) return -1;

    virDomainPtr dom = lv_lookup_domain_locked(vm_name);
    if (!dom) return -1;
    conn_unlock();

    /* Default all to unknown (-1) */
    for (int i = 0; i < count; i++)
        folders[i].mounted = -1;

    /* VM must be running to query guest agent */
    virDomainInfo info;
    if (virDomainGetInfo(dom, &info) != 0 || info.state != VIR_DOMAIN_RUNNING) {
        virDomainFree(dom);
        return 0; /* not an error — just can't check */
    }

    /* Use findmnt to check mounted filesystems.
     * guest-get-fsinfo only reports block-device filesystems and misses
     * virtiofs/9p mounts. findmnt sees all mount types. */
    char *msg = NULL;
    int rc = guest_exec(dom, "findmnt -n -o TARGET,FSTYPE --list", &msg);
    virDomainFree(dom);

    if (rc != 0 || !msg) {
        /* Guest agent not available or findmnt failed — leave as unknown */
        log_msg(LOG_DEBUG, "check_mount_status: findmnt failed for %s: %s",
                vm_name, msg ? msg : "no output");
        free(msg);
        return 0;
    }

    log_msg(LOG_DEBUG, "check_mount_status: findmnt output:\n%s", msg);

    /* Mark all as not mounted, then check findmnt output */
    for (int i = 0; i < count; i++)
        folders[i].mounted = 0;

    /* Parse each line of findmnt output: "TARGET  FSTYPE" */
    for (int i = 0; i < count; i++) {
        if (!folders[i].mount_tag) continue;
        char expected[512];
        snprintf(expected, sizeof(expected), "/media/%s", folders[i].mount_tag);

        /* Search for the mountpoint in the findmnt output */
        char *line = msg;
        while (line && *line) {
            /* Skip leading whitespace */
            while (*line == ' ' || *line == '\t') line++;

            /* Find end of line */
            char *eol = strchr(line, '\n');
            size_t linelen = eol ? (size_t)(eol - line) : strlen(line);

            /* Check if this line starts with our expected mountpoint */
            size_t explen = strlen(expected);
            if (linelen >= explen &&
                strncmp(line, expected, explen) == 0 &&
                (line[explen] == ' ' || line[explen] == '\t' ||
                 line[explen] == '\n' || line[explen] == '\0')) {
                folders[i].mounted = 1;
                log_msg(LOG_DEBUG, "check_mount_status: MATCH: %s is mounted",
                        folders[i].mount_tag);
                break;
            }

            line = eol ? eol + 1 : NULL;
        }
    }

    free(msg);
    return 0;
}

/* ------------------------------------------------------------------ */
/*  Auto-mount: check if qemu-automount.timer is active in guest      */
/* ------------------------------------------------------------------ */

/* Guest OS types for auto-mount dispatch */
/* Detect guest OS using guest-agent osinfo or binary probing */
static guest_os_t detect_guest_os_dom(virDomainPtr dom)
{
    char *msg = NULL;
    int rc;

    /* Try guest-get-osinfo first (QGA 2.10+) */
    char *osinfo_json = virDomainQemuAgentCommand(dom,
        "{\"execute\":\"guest-get-osinfo\"}", 10, 0);
    if (osinfo_json) {
        json_error_t jerr;
        json_t *resp = json_loads(osinfo_json, 0, &jerr);
        free(osinfo_json);
        if (resp) {
            json_t *ret = json_object_get(resp, "return");
            const char *kernel = json_string_value(json_object_get(ret, "kernel-release"));
            const char *os_id = json_string_value(json_object_get(ret, "id"));
            const char *name = json_string_value(json_object_get(ret, "name"));

            guest_os_t detected = GUEST_OS_UNKNOWN;

            /* Check Windows first */
            if ((name && (strcasestr(name, "windows") || strcasestr(name, "Microsoft"))) ||
                (os_id && strcasestr(os_id, "mswindows"))) {
                detected = GUEST_OS_WINDOWS;
            }
            /* Check macOS */
            else if ((name && strcasestr(name, "macOS")) ||
                     (kernel && strstr(kernel, "Darwin"))) {
                detected = GUEST_OS_MACOS;
            }
            /* Check FreeBSD */
            else if ((os_id && strcasestr(os_id, "freebsd")) ||
                     (name && strcasestr(name, "FreeBSD")) ||
                     (kernel && strstr(kernel, "FreeBSD"))) {
                detected = GUEST_OS_FREEBSD;
            }
            /* Otherwise assume Linux */
            else {
                detected = GUEST_OS_LINUX_SYSTEMD; /* refined below */
            }

            json_decref(resp);

            if (detected == GUEST_OS_LINUX_SYSTEMD) {
                /* Check which init system */
                rc = guest_exec(dom, "systemctl --version >/dev/null 2>&1", &msg);
                free(msg); msg = NULL;
                if (rc == 0) return GUEST_OS_LINUX_SYSTEMD;

                rc = guest_exec(dom, "rc-service --version >/dev/null 2>&1", &msg);
                free(msg); msg = NULL;
                if (rc == 0) return GUEST_OS_LINUX_OPENRC;

                return GUEST_OS_LINUX_OTHER;
            }
            return detected;
        }
    }

    /* Fallback: probe for OS-specific binaries */
    log_msg(LOG_DEBUG, "guest-get-osinfo not available, probing binaries");

    /* Check for Windows (cmd.exe) */
    rc = guest_exec(dom, "cmd.exe /c echo ok", &msg);
    free(msg); msg = NULL;
    if (rc == 0) return GUEST_OS_WINDOWS;

    /* Check for FreeBSD (freebsd-version) */
    rc = guest_exec(dom, "freebsd-version >/dev/null 2>&1", &msg);
    free(msg); msg = NULL;
    if (rc == 0) return GUEST_OS_FREEBSD;

    /* Check for macOS (sw_vers) */
    rc = guest_exec(dom, "sw_vers >/dev/null 2>&1", &msg);
    free(msg); msg = NULL;
    if (rc == 0) return GUEST_OS_MACOS;

    /* Assume Linux — check init system */
    rc = guest_exec(dom, "systemctl --version >/dev/null 2>&1", &msg);
    free(msg); msg = NULL;
    if (rc == 0) return GUEST_OS_LINUX_SYSTEMD;

    rc = guest_exec(dom, "rc-service --version >/dev/null 2>&1", &msg);
    free(msg); msg = NULL;
    if (rc == 0) return GUEST_OS_LINUX_OPENRC;

    return GUEST_OS_LINUX_OTHER;
}

static const char *guest_os_name(guest_os_t os)
{
    switch (os) {
    case GUEST_OS_LINUX_SYSTEMD: return "Linux (systemd)";
    case GUEST_OS_LINUX_OPENRC:  return "Linux (OpenRC)";
    case GUEST_OS_LINUX_OTHER:   return "Linux (other)";
    case GUEST_OS_WINDOWS:       return "Windows";
    case GUEST_OS_FREEBSD:       return "FreeBSD";
    case GUEST_OS_MACOS:         return "macOS";
    case GUEST_OS_UNKNOWN:       return "Unknown";
    }
    return "Unknown";
}

static int lv_check_automount_status(const char *vm_name)
{
    virDomainPtr dom = lv_lookup_domain_locked(vm_name);
    if (!dom) return -1;
    conn_unlock();

    virDomainInfo info;
    if (virDomainGetInfo(dom, &info) != 0 || info.state != VIR_DOMAIN_RUNNING) {
        virDomainFree(dom);
        return -1;
    }

    char *msg = NULL;
    int rc;

    /* Check systemd timer (Linux) */
    rc = guest_exec(dom, "systemctl is-active qemu-automount.timer 2>/dev/null", &msg);
    free(msg); msg = NULL;
    if (rc == 0) { virDomainFree(dom); return 1; }

    /* Check OpenRC service (Alpine Linux) */
    rc = guest_exec(dom, "rc-service qemu-automount status 2>/dev/null | grep -q started", &msg);
    free(msg); msg = NULL;
    if (rc == 0) { virDomainFree(dom); return 1; }

    /* Check Windows scheduled task */
    rc = guest_exec(dom, "schtasks.exe /Query /TN \"QEMU-AutoMount\" >NUL 2>&1", &msg);
    free(msg); msg = NULL;
    if (rc == 0) { virDomainFree(dom); return 1; }

    /* Check FreeBSD rc service */
    rc = guest_exec(dom, "service qemu_automount status 2>/dev/null | grep -q running", &msg);
    free(msg); msg = NULL;
    if (rc == 0) { virDomainFree(dom); return 1; }

    /* Check macOS launchd */
    rc = guest_exec(dom, "launchctl list com.qemu.automount 2>/dev/null", &msg);
    free(msg); msg = NULL;
    if (rc == 0) { virDomainFree(dom); return 1; }

    virDomainFree(dom);
    return 0;  /* not active */
}

/* ------------------------------------------------------------------ */
/*  Auto-mount: install systemd service in guest                      */
/* ------------------------------------------------------------------ */

/* ------------------------------------------------------------------ */
/*  Auto-mount setup: OS-specific installers                          */
/* ------------------------------------------------------------------ */

/* Linux (systemd): timer + script — existing approach */
static int setup_automount_systemd(virDomainPtr dom, char **out_msg)
{
    char *msg = NULL;
    int rc;

    /* Step 0: Make virt_qemu_ga_t permissive (best-effort) */
    rc = guest_exec(dom,
        "which semanage >/dev/null 2>&1 && semanage permissive -a virt_qemu_ga_t 2>/dev/null; true",
        &msg);
    free(msg); msg = NULL;

    /* Step 1: Write the auto-mount script */
    rc = guest_exec(dom,
        "cat > /usr/local/bin/qemu-automount << 'SCRIPT'\n"
        "#!/bin/bash\n"
        "for tag_path in /sys/fs/virtiofs/*/tag; do\n"
        "    [ -e \"$tag_path\" ] || continue\n"
        "    TAG=$(cat \"$tag_path\")\n"
        "    MOUNT_POINT=\"/media/$TAG\"\n"
        "    mkdir -p \"$MOUNT_POINT\"\n"
        "    if ! mountpoint -q \"$MOUNT_POINT\"; then\n"
        "        mount -t virtiofs \"$TAG\" \"$MOUNT_POINT\" >/dev/null 2>&1\n"
        "    fi\n"
        "done\n"
        "SCRIPT\n"
        "chmod +x /usr/local/bin/qemu-automount",
        &msg);
    if (rc != 0) {
        if (out_msg) {
            if (msg && strstr(msg, "Permission denied")) {
                *out_msg = strdup(
                    "SELinux blocked writing to /usr/local/bin/. "
                    "Run this inside the guest VM: "
                    "sudo semanage permissive -a virt_qemu_ga_t "
                    "(requires: sudo dnf install policycoreutils-python-utils)");
            } else {
                *out_msg = msg; msg = NULL;
            }
        }
        free(msg);
        return -1;
    }
    free(msg); msg = NULL;

    /* Step 2: Write the systemd service file */
    rc = guest_exec(dom,
        "cat > /etc/systemd/system/qemu-automount.service << 'EOF'\n"
        "[Unit]\n"
        "Description=QEMU/KVM VirtioFS Auto-Mounter\n"
        "\n"
        "[Service]\n"
        "Type=oneshot\n"
        "ExecStart=/usr/local/bin/qemu-automount\n"
        "EOF",
        &msg);
    if (rc != 0) {
        if (out_msg) { *out_msg = msg; msg = NULL; }
        free(msg);
        return -1;
    }
    free(msg); msg = NULL;

    /* Step 3: Write the timer file */
    rc = guest_exec(dom,
        "cat > /etc/systemd/system/qemu-automount.timer << 'EOF'\n"
        "[Unit]\n"
        "Description=Run QEMU Auto-Mounter every 5 seconds\n"
        "\n"
        "[Timer]\n"
        "OnBootSec=5s\n"
        "OnUnitActiveSec=5s\n"
        "AccuracySec=100ms\n"
        "\n"
        "[Install]\n"
        "WantedBy=timers.target\n"
        "EOF",
        &msg);
    if (rc != 0) {
        if (out_msg) { *out_msg = msg; msg = NULL; }
        free(msg);
        return -1;
    }
    free(msg); msg = NULL;

    /* Step 4: Enable and start the timer */
    rc = guest_exec(dom,
        "systemctl daemon-reload && systemctl enable --now qemu-automount.timer",
        &msg);
    if (rc != 0) {
        if (out_msg) { *out_msg = msg; msg = NULL; }
        free(msg);
        return -1;
    }
    free(msg);
    return 0;
}

/* Linux (OpenRC): service script + crontab */
static int setup_automount_openrc(virDomainPtr dom, char **out_msg)
{
    char *msg = NULL;
    int rc;

    /* Step 1: Write the auto-mount script (same as systemd) */
    rc = guest_exec(dom,
        "cat > /usr/local/bin/qemu-automount << 'SCRIPT'\n"
        "#!/bin/sh\n"
        "for tag_path in /sys/fs/virtiofs/*/tag; do\n"
        "    [ -e \"$tag_path\" ] || continue\n"
        "    TAG=$(cat \"$tag_path\")\n"
        "    MOUNT_POINT=\"/media/$TAG\"\n"
        "    mkdir -p \"$MOUNT_POINT\"\n"
        "    if ! mountpoint -q \"$MOUNT_POINT\"; then\n"
        "        mount -t virtiofs \"$TAG\" \"$MOUNT_POINT\" 2>/dev/null\n"
        "    fi\n"
        "done\n"
        "SCRIPT\n"
        "chmod +x /usr/local/bin/qemu-automount",
        &msg);
    if (rc != 0) {
        if (out_msg) { *out_msg = msg; msg = NULL; }
        free(msg);
        return -1;
    }
    free(msg); msg = NULL;

    /* Step 2: Write OpenRC init script */
    rc = guest_exec(dom,
        "cat > /etc/init.d/qemu-automount << 'EOF'\n"
        "#!/sbin/openrc-run\n"
        "description=\"QEMU/KVM VirtioFS Auto-Mounter\"\n"
        "command=/usr/local/bin/qemu-automount\n"
        "command_background=false\n"
        "\n"
        "depend() {\n"
        "    need localmount\n"
        "}\n"
        "EOF\n"
        "chmod +x /etc/init.d/qemu-automount",
        &msg);
    if (rc != 0) {
        if (out_msg) { *out_msg = msg; msg = NULL; }
        free(msg);
        return -1;
    }
    free(msg); msg = NULL;

    /* Step 3: Enable and start + add crontab for periodic re-check */
    rc = guest_exec(dom,
        "rc-update add qemu-automount default 2>/dev/null; "
        "rc-service qemu-automount start 2>/dev/null; "
        "(crontab -l 2>/dev/null | grep -v qemu-automount; "
        "echo '*/1 * * * * /usr/local/bin/qemu-automount') | crontab -",
        &msg);
    if (rc != 0) {
        if (out_msg) { *out_msg = msg; msg = NULL; }
        free(msg);
        return -1;
    }
    free(msg);
    return 0;
}

/* Windows: PowerShell script + Scheduled Task */
static int setup_automount_windows(virDomainPtr dom, char **out_msg)
{
    char *msg = NULL;
    int rc;

    /* Step 1: Write PowerShell auto-mount script.
     * On Windows, VirtioFS shares appear as network drives or can be
     * mounted via the WinFsp/VirtioFS driver. The script checks for
     * VirtioFS devices and mounts them. */
    rc = guest_exec(dom,
        "cmd.exe /c \"mkdir C:\\ProgramData\\QEMU 2>NUL & "
        "echo # QEMU VirtioFS Auto-Mounter > C:\\ProgramData\\QEMU\\automount.ps1 & "
        "echo $vfs = Get-PnpDevice -FriendlyName '*VirtIO FS*' -ErrorAction SilentlyContinue >> C:\\ProgramData\\QEMU\\automount.ps1 & "
        "echo if (-not $vfs) { exit } >> C:\\ProgramData\\QEMU\\automount.ps1 & "
        "echo $shares = Get-CimInstance -ClassName Win32_NetworkConnection -ErrorAction SilentlyContinue >> C:\\ProgramData\\QEMU\\automount.ps1 & "
        "echo # Mount any unmounted VirtioFS shares >> C:\\ProgramData\\QEMU\\automount.ps1 & "
        "echo Get-PSDrive -PSProvider FileSystem -ErrorAction SilentlyContinue ^| Out-Null >> C:\\ProgramData\\QEMU\\automount.ps1\"",
        &msg);
    if (rc != 0) {
        if (out_msg) {
            *out_msg = strdup("Failed to create auto-mount script. "
                "Windows VirtioFS support requires the VirtIO-Win drivers "
                "and WinFsp to be installed.");
            (void)msg;
        }
        free(msg);
        return -1;
    }
    free(msg); msg = NULL;

    /* Step 2: Create a scheduled task to run every 5 minutes */
    rc = guest_exec(dom,
        "cmd.exe /c \"schtasks.exe /Create /TN QEMU-AutoMount "
        "/SC MINUTE /MO 5 "
        "/TR \\\"powershell.exe -ExecutionPolicy Bypass -File C:\\ProgramData\\QEMU\\automount.ps1\\\" "
        "/RU SYSTEM /F\"",
        &msg);
    if (rc != 0) {
        if (out_msg) { *out_msg = msg; msg = NULL; }
        free(msg);
        return -1;
    }
    free(msg);
    return 0;
}

/* FreeBSD: rc.d script */
static int setup_automount_freebsd(virDomainPtr dom, char **out_msg)
{
    char *msg = NULL;
    int rc;

    /* Step 1: Write the auto-mount script */
    rc = guest_exec(dom,
        "cat > /usr/local/bin/qemu-automount << 'SCRIPT'\n"
        "#!/bin/sh\n"
        "# Mount all VirtioFS shares\n"
        "for tag in $(sysctl -n vfs.virtiofs.tags 2>/dev/null); do\n"
        "    MOUNT_POINT=\"/media/$tag\"\n"
        "    mkdir -p \"$MOUNT_POINT\"\n"
        "    mount | grep -q \"$MOUNT_POINT\" || mount -t virtiofs \"$tag\" \"$MOUNT_POINT\" 2>/dev/null\n"
        "done\n"
        "SCRIPT\n"
        "chmod +x /usr/local/bin/qemu-automount",
        &msg);
    if (rc != 0) {
        if (out_msg) { *out_msg = msg; msg = NULL; }
        free(msg);
        return -1;
    }
    free(msg); msg = NULL;

    /* Step 2: Write rc.d script */
    rc = guest_exec(dom,
        "cat > /usr/local/etc/rc.d/qemu_automount << 'EOF'\n"
        "#!/bin/sh\n"
        "# PROVIDE: qemu_automount\n"
        "# REQUIRE: FILESYSTEMS\n"
        ". /etc/rc.subr\n"
        "name=qemu_automount\n"
        "rcvar=qemu_automount_enable\n"
        "start_cmd=\"/usr/local/bin/qemu-automount\"\n"
        "load_rc_config $name\n"
        "run_rc_command \"$1\"\n"
        "EOF\n"
        "chmod +x /usr/local/etc/rc.d/qemu_automount",
        &msg);
    if (rc != 0) {
        if (out_msg) { *out_msg = msg; msg = NULL; }
        free(msg);
        return -1;
    }
    free(msg); msg = NULL;

    /* Step 3: Enable and start */
    rc = guest_exec(dom,
        "sysrc qemu_automount_enable=YES 2>/dev/null; "
        "service qemu_automount start 2>/dev/null; "
        "(crontab -l 2>/dev/null | grep -v qemu-automount; "
        "echo '*/1 * * * * /usr/local/bin/qemu-automount') | crontab -",
        &msg);
    if (rc != 0) {
        if (out_msg) { *out_msg = msg; msg = NULL; }
        free(msg);
        return -1;
    }
    free(msg);
    return 0;
}

/* macOS: launchd plist */
static int setup_automount_macos(virDomainPtr dom, char **out_msg)
{
    char *msg = NULL;
    int rc;

    /* Step 1: Write the auto-mount script */
    rc = guest_exec(dom,
        "cat > /usr/local/bin/qemu-automount << 'SCRIPT'\n"
        "#!/bin/bash\n"
        "# Mount VirtioFS shares on macOS guest\n"
        "# Note: macOS VirtioFS support is limited; this is best-effort\n"
        "for tag in $(mount -t virtiofs 2>/dev/null | awk '{print $1}'); do\n"
        "    MOUNT_POINT=\"/Volumes/$tag\"\n"
        "    mkdir -p \"$MOUNT_POINT\" 2>/dev/null\n"
        "    mount | grep -q \"$MOUNT_POINT\" || mount -t virtiofs \"$tag\" \"$MOUNT_POINT\" 2>/dev/null\n"
        "done\n"
        "SCRIPT\n"
        "chmod +x /usr/local/bin/qemu-automount",
        &msg);
    if (rc != 0) {
        if (out_msg) { *out_msg = msg; msg = NULL; }
        free(msg);
        return -1;
    }
    free(msg); msg = NULL;

    /* Step 2: Write launchd plist */
    rc = guest_exec(dom,
        "cat > /Library/LaunchDaemons/com.qemu.automount.plist << 'EOF'\n"
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" "
        "\"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
        "<plist version=\"1.0\">\n"
        "<dict>\n"
        "    <key>Label</key>\n"
        "    <string>com.qemu.automount</string>\n"
        "    <key>ProgramArguments</key>\n"
        "    <array>\n"
        "        <string>/usr/local/bin/qemu-automount</string>\n"
        "    </array>\n"
        "    <key>StartInterval</key>\n"
        "    <integer>5</integer>\n"
        "    <key>RunAtLoad</key>\n"
        "    <true/>\n"
        "</dict>\n"
        "</plist>\n"
        "EOF",
        &msg);
    if (rc != 0) {
        if (out_msg) { *out_msg = msg; msg = NULL; }
        free(msg);
        return -1;
    }
    free(msg); msg = NULL;

    /* Step 3: Load the daemon */
    rc = guest_exec(dom,
        "launchctl load /Library/LaunchDaemons/com.qemu.automount.plist 2>/dev/null",
        &msg);
    if (rc != 0) {
        if (out_msg) { *out_msg = msg; msg = NULL; }
        free(msg);
        return -1;
    }
    free(msg);
    return 0;
}

static int lv_setup_automount(const char *vm_name, char **out_msg)
{
    virDomainPtr dom = lv_lookup_domain_locked(vm_name);
    if (!dom) {
        if (out_msg) *out_msg = strdup("VM not found or not connected to libvirt");
        return -1;
    }
    conn_unlock();

    virDomainInfo info;
    if (virDomainGetInfo(dom, &info) != 0 || info.state != VIR_DOMAIN_RUNNING) {
        virDomainFree(dom);
        if (out_msg) *out_msg = strdup("VM is not running");
        return -1;
    }

    /* Detect guest OS */
    guest_os_t os = detect_guest_os_dom(dom);
    log_msg(LOG_INFO, "Detected guest OS for %s: %s", vm_name, guest_os_name(os));

    int rc;
    switch (os) {
    case GUEST_OS_LINUX_SYSTEMD:
        rc = setup_automount_systemd(dom, out_msg);
        break;
    case GUEST_OS_LINUX_OPENRC:
        rc = setup_automount_openrc(dom, out_msg);
        break;
    case GUEST_OS_WINDOWS:
        rc = setup_automount_windows(dom, out_msg);
        break;
    case GUEST_OS_FREEBSD:
        rc = setup_automount_freebsd(dom, out_msg);
        break;
    case GUEST_OS_MACOS:
        rc = setup_automount_macos(dom, out_msg);
        break;
    case GUEST_OS_LINUX_OTHER:
    case GUEST_OS_UNKNOWN:
    default:
        /* Try systemd first as fallback, it's the most common */
        rc = setup_automount_systemd(dom, out_msg);
        if (rc != 0) {
            free(out_msg ? *out_msg : NULL);
            if (out_msg) {
                *out_msg = strdup(
                    "Could not detect init system. "
                    "Tried systemd but it's not available. "
                    "Supported: Linux (systemd, OpenRC), Windows, FreeBSD, macOS.");
            }
        }
        break;
    }

    if (rc == 0) {
        log_msg(LOG_INFO, "Auto-mount service installed in guest %s (%s)",
                vm_name, guest_os_name(os));
    }

    virDomainFree(dom);
    if (rc == 0 && out_msg) *out_msg = NULL;
    return rc;
}

/* ------------------------------------------------------------------ */
/*  orphan snapshot scanner                                           */
/* ------------------------------------------------------------------ */

/* Helper: get list of snapshot names inside a qcow2 file via qemu-img.
 * Returns count of names found, fills *names (caller frees each + array).
 * Returns -1 on error. VM must be shut off. */
static int qcow2_snapshot_names(const char *disk_path,
                                char ***names_out, int *count_out)
{
    *names_out = NULL;
    *count_out = 0;

    /* Build command: qemu-img snapshot -l <path> */
    char cmd[PATH_MAX + 64];
    snprintf(cmd, sizeof(cmd), "qemu-img snapshot -l '%s' 2>/dev/null", disk_path);

    FILE *fp = popen(cmd, "r");
    if (!fp) return -1;

    /* Parse output lines. Format:
     * Snapshot list:
     * ID  TAG  VM_SIZE  DATE  VM_CLOCK  ICOUNT
     * 1   name-here   0 B  2026-...  ...
     * Lines with numeric ID in first column have snapshot data. */
    char **names = NULL;
    int count = 0, capacity = 0;
    char line[1024];
    while (fgets(line, sizeof(line), fp)) {
        /* Skip header lines — snapshot lines start with digits */
        const char *p = line;
        while (*p == ' ') p++;
        if (*p < '0' || *p > '9') continue;

        /* Skip ID field */
        while (*p >= '0' && *p <= '9') p++;
        while (*p == ' ') p++;

        /* TAG field: everything up to the VM_SIZE column.
         * VM_SIZE is a number followed by B/KiB/MiB/GiB.
         * We find the LAST occurrence of a size pattern to delimit TAG. */
        const char *tag_start = p;

        /* Find the size pattern: digits followed by space and unit */
        const char *size_ptr = NULL;
        const char *q = tag_start;
        while (*q) {
            if (*q >= '0' && *q <= '9') {
                /* Check if this looks like a size: number followed by
                 * optional decimal, then space/unit */
                const char *num_start = q;
                while (*q >= '0' && *q <= '9') q++;
                if (*q == '.') { q++; while (*q >= '0' && *q <= '9') q++; }
                if (*q == ' ') {
                    const char *after = q + 1;
                    if (strncmp(after, "B ", 2) == 0 ||
                        strncmp(after, "KiB ", 4) == 0 ||
                        strncmp(after, "MiB ", 4) == 0 ||
                        strncmp(after, "GiB ", 4) == 0) {
                        size_ptr = num_start;
                    }
                }
            } else {
                q++;
            }
        }

        if (!size_ptr) continue;

        /* TAG = text from tag_start up to size_ptr, trimmed */
        size_t tag_len = (size_t)(size_ptr - tag_start);
        while (tag_len > 0 && tag_start[tag_len - 1] == ' ') tag_len--;
        if (tag_len == 0) continue;

        char *tag = malloc(tag_len + 1);
        if (!tag) continue;
        memcpy(tag, tag_start, tag_len);
        tag[tag_len] = '\0';

        /* Append to array */
        if (count >= capacity) {
            capacity = capacity ? capacity * 2 : 16;
            char **tmp = realloc(names, sizeof(char *) * (size_t)capacity);
            if (!tmp) { free(tag); continue; }
            names = tmp;
        }
        names[count++] = tag;
    }
    pclose(fp);

    *names_out = names;
    *count_out = count;
    return count;
}

/* Helper: get all disk paths (qcow2) from domain XML.
 * Returns count, fills *paths (caller frees each + array). */
static int get_domain_disk_paths(virDomainPtr dom,
                                 char ***paths_out, int *count_out)
{
    *paths_out = NULL;
    *count_out = 0;

    char *xml = virDomainGetXMLDesc(dom, VIR_DOMAIN_XML_INACTIVE);
    if (!xml) return -1;

    char **paths = NULL;
    int count = 0, capacity = 0;

    /* Find all <disk type='file' device='disk'> with qcow2 driver */
    const char *pos = xml;
    while (pos && *pos) {
        /* Try both quote styles */
        const char *p1 = strstr(pos, "<disk type='file'");
        const char *p2 = strstr(pos, "<disk type=\"file\"");
        /* Pick the earlier match */
        if (p1 && p2) pos = (p1 < p2) ? p1 : p2;
        else if (p1)  pos = p1;
        else if (p2)  pos = p2;
        else          break;
        /* Find end of this <disk> element */
        const char *disk_end = strstr(pos, "</disk>");
        if (!disk_end) break;

        /* Only include qcow2 disks (not cdrom, not raw) */
        size_t disk_len = (size_t)(disk_end - pos);
        char *disk_xml = malloc(disk_len + 1);
        if (!disk_xml) { pos = disk_end; continue; }
        memcpy(disk_xml, pos, disk_len);
        disk_xml[disk_len] = '\0';

        if (strstr(disk_xml, "type='qcow2'") ||
            strstr(disk_xml, "type=\"qcow2\"")) {
            /* Extract source file path */
            char *source_file = extract_xml_attr(disk_xml, "source", "file");
            if (source_file) {
                if (count >= capacity) {
                    capacity = capacity ? capacity * 2 : 4;
                    char **tmp = realloc(paths, sizeof(char *) * (size_t)capacity);
                    if (tmp) paths = tmp;
                }
                if (count < capacity)
                    paths[count++] = source_file;
                else
                    free(source_file);
            }
        }
        free(disk_xml);
        pos = disk_end + 7;
    }

    /* Also check NVRAM (UEFI VMs have snapshots in NVRAM qcow2 too) */
    char *nvram_path = extract_xml_content_ext(xml, "nvram");
    if (nvram_path && strlen(nvram_path) > 0) {
        char *nvram_fmt = extract_xml_attr(xml, "nvram", "format");
        if (nvram_fmt && str_eq(nvram_fmt, "qcow2")) {
            if (count >= capacity) {
                capacity = capacity ? capacity * 2 : 4;
                char **tmp = realloc(paths, sizeof(char *) * (size_t)capacity);
                if (tmp) paths = tmp;
            }
            if (count < capacity)
                paths[count++] = nvram_path;
            else
                free(nvram_path);
            nvram_path = NULL;  /* ownership transferred */
        }
        free(nvram_fmt);
    }
    free(nvram_path);
    free(xml);

    *paths_out = paths;
    *count_out = count;
    return count;
}

int lv_scan_orphan_snapshots(const char *vm_name,
                              char ***orphan_names_out, int *orphan_count_out)
{
    *orphan_names_out = NULL;
    *orphan_count_out = 0;

    virDomainPtr dom = lv_lookup_domain_locked(vm_name);
    if (!dom) return -1;
    conn_unlock();

    /* Only scan shut-off VMs (qemu-img can't read locked files) */
    int state = 0;
    virDomainGetState(dom, &state, NULL, 0);
    if (state != VIR_DOMAIN_SHUTOFF) {
        virDomainFree(dom);
        return 0;  /* not an error, just can't scan */
    }

    /* Get libvirt snapshot names */
    char **lv_names = NULL;
    int lv_count = 0;
    virDomainSnapshotPtr *snaps = NULL;
    int n = virDomainSnapshotNum(dom, 0);
    if (n > 0) {
        lv_names = calloc((size_t)n, sizeof(char *));
        snaps = calloc((size_t)n, sizeof(virDomainSnapshotPtr));
        if (lv_names && snaps) {
            virDomainSnapshotListNames(dom, lv_names, n, 0);
            lv_count = n;
        }
    }

    /* Get disk paths */
    char **disk_paths = NULL;
    int disk_count = 0;
    get_domain_disk_paths(dom, &disk_paths, &disk_count);

    /* For each disk, get qcow2 names and find orphans */
    char **orphans = NULL;
    int orphan_count = 0, orphan_cap = 0;

    for (int d = 0; d < disk_count; d++) {
        char **qcow2_names = NULL;
        int qcow2_count = 0;
        if (qcow2_snapshot_names(disk_paths[d], &qcow2_names, &qcow2_count) < 0)
            continue;

        for (int q = 0; q < qcow2_count; q++) {
            /* Is this name in libvirt metadata? */
            int found = 0;
            for (int l = 0; l < lv_count; l++) {
                if (str_eq(qcow2_names[q], lv_names[l])) {
                    found = 1;
                    break;
                }
            }
            if (!found) {
                /* Check if we already added this orphan (from another disk) */
                int dup = 0;
                for (int o = 0; o < orphan_count; o++) {
                    if (str_eq(orphans[o], qcow2_names[q])) { dup = 1; break; }
                }
                if (!dup) {
                    if (orphan_count >= orphan_cap) {
                        orphan_cap = orphan_cap ? orphan_cap * 2 : 16;
                        char **tmp = realloc(orphans, sizeof(char *) * (size_t)orphan_cap);
                        if (tmp) orphans = tmp;
                    }
                    if (orphan_count < orphan_cap)
                        orphans[orphan_count++] = str_dup(qcow2_names[q]);
                }
            }
            free(qcow2_names[q]);
        }
        free(qcow2_names);
        free(disk_paths[d]);
    }
    free(disk_paths);

    /* Cleanup libvirt names */
    for (int i = 0; i < lv_count; i++) free(lv_names[i]);
    free(lv_names);
    free(snaps);
    virDomainFree(dom);

    *orphan_names_out = orphans;
    *orphan_count_out = orphan_count;
    return orphan_count;
}

int lv_cleanup_orphan_snapshots(const char *vm_name)
{
    virDomainPtr dom = lv_lookup_domain_locked(vm_name);
    if (!dom) return -1;
    conn_unlock();

    /* Only on shut-off VMs */
    int state = 0;
    virDomainGetState(dom, &state, NULL, 0);
    if (state != VIR_DOMAIN_SHUTOFF) {
        snprintf(lv_last_error, sizeof(lv_last_error),
                 "VM must be shut off to clean orphan snapshots");
        virDomainFree(dom);
        return -1;
    }

    /* Get disk paths */
    char **disk_paths = NULL;
    int disk_count = 0;
    get_domain_disk_paths(dom, &disk_paths, &disk_count);

    /* Get libvirt snapshot names */
    char **lv_names = NULL;
    int lv_count = virDomainSnapshotNum(dom, 0);
    if (lv_count > 0) {
        lv_names = calloc((size_t)lv_count, sizeof(char *));
        if (lv_names)
            virDomainSnapshotListNames(dom, lv_names, lv_count, 0);
    }

    int total_cleaned = 0;

    for (int d = 0; d < disk_count; d++) {
        char **qcow2_names = NULL;
        int qcow2_count = 0;
        if (qcow2_snapshot_names(disk_paths[d], &qcow2_names, &qcow2_count) < 0) {
            free(disk_paths[d]);
            continue;
        }

        for (int q = 0; q < qcow2_count; q++) {
            /* Is this name in libvirt metadata? */
            int found = 0;
            for (int l = 0; l < lv_count; l++) {
                if (str_eq(qcow2_names[q], lv_names[l])) { found = 1; break; }
            }
            if (!found) {
                /* Delete orphan from qcow2 */
                char cmd[PATH_MAX + 256];
                snprintf(cmd, sizeof(cmd),
                         "qemu-img snapshot -d '%s' '%s' 2>&1",
                         qcow2_names[q], disk_paths[d]);
                int rc = system(cmd);
                if (rc == 0) {
                    log_msg(LOG_INFO, "Cleaned orphan snapshot '%s' from %s",
                            qcow2_names[q], disk_paths[d]);
                    total_cleaned++;
                } else {
                    log_msg(LOG_ERROR, "Failed to clean orphan '%s' from %s",
                            qcow2_names[q], disk_paths[d]);
                }
            }
            free(qcow2_names[q]);
        }
        free(qcow2_names);
        free(disk_paths[d]);
    }
    free(disk_paths);

    for (int i = 0; i < lv_count; i++) free(lv_names[i]);
    free(lv_names);
    virDomainFree(dom);

    log_msg(LOG_INFO, "Cleaned %d orphan snapshot(s) from VM '%s'",
            total_cleaned, vm_name);
    return total_cleaned;
}

/* ------------------------------------------------------------------ */
/*  vtable                                                            */
/* ------------------------------------------------------------------ */

/* Public wrapper: detect guest OS by VM name */
static guest_os_t lv_detect_guest_os(const char *vm_name)
{
    virDomainPtr dom = lv_lookup_domain_locked(vm_name);
    if (!dom) return GUEST_OS_UNKNOWN;
    conn_unlock();

    virDomainInfo info;
    if (virDomainGetInfo(dom, &info) != 0 || info.state != VIR_DOMAIN_RUNNING) {
        virDomainFree(dom);
        return GUEST_OS_UNKNOWN;
    }

    guest_os_t os = detect_guest_os_dom(dom);
    virDomainFree(dom);
    return os;
}

static vm_backend_t libvirt_be = {
    .name                = "libvirt",
    .connect             = lv_connect,
    .disconnect          = lv_disconnect,
    .list_vms            = lv_list_vms,
    .free_vm_list        = lv_free_vm_list,
    .vm_start            = lv_vm_start,
    .vm_stop             = lv_vm_stop,
    .vm_force_stop       = lv_vm_force_stop,
    .vm_pause            = lv_vm_pause,
    .vm_resume           = lv_vm_resume,
    .list_snapshots      = lv_list_snapshots,
    .create_snapshot     = lv_create_snapshot,
    .delete_snapshot     = lv_delete_snapshot,
    .revert_snapshot     = lv_revert_snapshot,
    .merge_snapshot      = lv_merge_snapshot,
    .list_shared_folders  = lv_list_shared_folders,
    .free_shared_folders  = lv_free_shared_folders,
    .add_shared_folder    = lv_add_shared_folder,
    .remove_shared_folder  = lv_remove_shared_folder,
    .mount_shared_folder   = lv_mount_shared_folder,
    .unmount_shared_folder = lv_unmount_shared_folder,
    .check_mount_status    = lv_check_mount_status,
    .check_automount_status = lv_check_automount_status,
    .setup_automount        = lv_setup_automount,
    .detect_guest_os        = lv_detect_guest_os,
};

vm_backend_t *libvirt_backend_get(void)
{
    return &libvirt_be;
}
