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

static int lv_delete_snapshot(const char *vm_name, const char *snap_name,
                              int auto_merge)
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

    int ret;
    if (auto_merge) {
        /* For external snapshots, metadata-only delete after merge */
        ret = virDomainSnapshotDelete(snap,
                VIR_DOMAIN_SNAPSHOT_DELETE_METADATA_ONLY);
    } else {
        ret = virDomainSnapshotDelete(snap, 0);
    }

    virDomainSnapshotFree(snap);
    virDomainFree(dom);

    if (ret == 0)
        log_msg(LOG_INFO, "libvirt: deleted snapshot '%s'", snap_name);
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

    char *xml = virDomainGetXMLDesc(dom, 0);
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
        int is_9p = (strstr(block, "9p") != NULL) || (strstr(block, "\"path\"") != NULL);

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
        return -1;
    }

    char cmd[256];
    snprintf(cmd, sizeof(cmd), "/usr/bin/umount /media/%s", mount_tag);

    char *msg = NULL;
    int ret = guest_exec(dom, cmd, &msg);
    if (ret != 0) {
        log_msg(LOG_ERROR, "Unmount in guest failed: %s", msg ? msg : "unknown error");
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
    int rc = guest_exec(dom, "systemctl is-active qemu-automount.timer", &msg);
    virDomainFree(dom);
    free(msg);

    if (rc == 0) return 1;   /* active */
    return 0;                 /* inactive or not installed */
}

/* ------------------------------------------------------------------ */
/*  Auto-mount: install systemd service in guest                      */
/* ------------------------------------------------------------------ */

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

    char *msg = NULL;
    int rc;

    /* Step 0: Make virt_qemu_ga_t permissive so guest agent can write system files.
     * This is needed because SELinux blocks writes from the guest agent context.
     * Ignore errors — semanage may not be installed, or already permissive. */
    rc = guest_exec(dom,
        "which semanage >/dev/null 2>&1 && semanage permissive -a virt_qemu_ga_t 2>/dev/null; true",
        &msg);
    free(msg); msg = NULL;
    /* Don't check rc — this step is best-effort */

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
        log_msg(LOG_ERROR, "automount: failed to write script: %s", msg ? msg : "unknown");
        if (out_msg) {
            if (msg && strstr(msg, "Permission denied")) {
                *out_msg = strdup(
                    "SELinux blocked writing to /usr/local/bin/. "
                    "Run this inside the guest VM: "
                    "sudo semanage permissive -a virt_qemu_ga_t "
                    "(requires: sudo dnf install policycoreutils-python-utils)");
            } else {
                *out_msg = msg;
                msg = NULL;
            }
        }
        free(msg);
        virDomainFree(dom);
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
        log_msg(LOG_ERROR, "automount: failed to write service: %s", msg ? msg : "unknown");
        if (out_msg) *out_msg = msg; else free(msg);
        virDomainFree(dom);
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
        log_msg(LOG_ERROR, "automount: failed to write timer: %s", msg ? msg : "unknown");
        if (out_msg) *out_msg = msg; else free(msg);
        virDomainFree(dom);
        return -1;
    }
    free(msg); msg = NULL;

    /* Step 4: Enable and start the timer */
    rc = guest_exec(dom,
        "systemctl daemon-reload && systemctl enable --now qemu-automount.timer",
        &msg);
    if (rc != 0) {
        log_msg(LOG_ERROR, "automount: failed to enable timer: %s", msg ? msg : "unknown");
        if (out_msg) *out_msg = msg; else free(msg);
        virDomainFree(dom);
        return -1;
    }
    free(msg);

    log_msg(LOG_INFO, "Auto-mount service installed and started in guest %s", vm_name);
    virDomainFree(dom);
    if (out_msg) *out_msg = NULL;
    return 0;
}

/* ------------------------------------------------------------------ */
/*  vtable                                                            */
/* ------------------------------------------------------------------ */

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
};

vm_backend_t *libvirt_backend_get(void)
{
    return &libvirt_be;
}
