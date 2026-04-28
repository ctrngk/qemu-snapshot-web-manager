#include "dirty_state.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <jansson.h>

#include "util.h"

#define MAX_TRACKED_VMS 128

typedef struct {
    char name[256];
    int is_clean;
} dirty_state_entry_t;

static dirty_state_entry_t dirty_state_table[MAX_TRACKED_VMS];
static int dirty_state_count = 0;
static char dirty_state_path[PATH_MAX];

static int dirty_state_find(const char *name)
{
    for (int i = 0; i < dirty_state_count; i++) {
        if (strcmp(dirty_state_table[i].name, name) == 0)
            return i;
    }
    return -1;
}

static void dirty_state_clear(void)
{
    dirty_state_count = 0;
}

static int ensure_parent_dir(const char *path)
{
    char dir[PATH_MAX];

    if (!path || !path[0])
        return 0;

    if (snprintf(dir, sizeof(dir), "%s", path) >= (int)sizeof(dir)) {
        log_msg(LOG_ERROR, "Dirty state path is too long");
        return -1;
    }

    char *slash = strrchr(dir, '/');
    if (!slash)
        return 0;
    *slash = '\0';

    if (dir[0] == '\0')
        return 0;

    for (char *p = dir + 1; *p; p++) {
        if (*p != '/')
            continue;

        *p = '\0';
        if (mkdir(dir, 0755) != 0 && errno != EEXIST) {
            log_msg(LOG_ERROR, "Failed to create directory '%s': %s",
                    dir, strerror(errno));
            return -1;
        }
        *p = '/';
    }

    if (mkdir(dir, 0755) != 0 && errno != EEXIST) {
        log_msg(LOG_ERROR, "Failed to create directory '%s': %s",
                dir, strerror(errno));
        return -1;
    }

    return 0;
}

static int dirty_state_save(void)
{
    if (!dirty_state_path[0])
        return 0;

    if (ensure_parent_dir(dirty_state_path) != 0)
        return -1;

    char tmp_path[PATH_MAX];
    if (snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", dirty_state_path) >= (int)sizeof(tmp_path)) {
        log_msg(LOG_ERROR, "Dirty state temp path is too long");
        return -1;
    }

    json_t *root = json_array();
    if (!root) {
        log_msg(LOG_ERROR, "Failed to allocate dirty state JSON array");
        return -1;
    }

    for (int i = 0; i < dirty_state_count; i++) {
        json_t *entry = json_object();
        if (!entry ||
            json_object_set_new(entry, "name",
                                json_string(dirty_state_table[i].name)) != 0 ||
            json_object_set_new(entry, "is_clean",
                                json_boolean(dirty_state_table[i].is_clean)) != 0 ||
            json_array_append_new(root, entry) != 0) {
            json_decref(entry);
            json_decref(root);
            log_msg(LOG_ERROR, "Failed to build dirty state JSON");
            return -1;
        }
    }

    FILE *fp = fopen(tmp_path, "w");
    if (!fp) {
        json_decref(root);
        log_msg(LOG_ERROR, "Failed to open dirty state file '%s': %s",
                tmp_path, strerror(errno));
        return -1;
    }

    if (json_dumpf(root, fp, JSON_INDENT(2)) != 0 || fputc('\n', fp) == EOF) {
        json_decref(root);
        fclose(fp);
        unlink(tmp_path);
        log_msg(LOG_ERROR, "Failed to write dirty state file '%s'",
                tmp_path);
        return -1;
    }
    json_decref(root);

    if (fclose(fp) != 0) {
        unlink(tmp_path);
        log_msg(LOG_ERROR, "Failed to close dirty state file '%s': %s",
                tmp_path, strerror(errno));
        return -1;
    }

    if (rename(tmp_path, dirty_state_path) != 0) {
        unlink(tmp_path);
        log_msg(LOG_ERROR, "Failed to replace dirty state file '%s': %s",
                dirty_state_path, strerror(errno));
        return -1;
    }

    return 0;
}

static int dirty_state_set(const char *name, int is_clean)
{
    if (!name || !name[0]) {
        log_msg(LOG_WARN, "Ignoring empty VM name in dirty state update");
        return -1;
    }

    int idx = dirty_state_find(name);
    if (idx >= 0) {
        if (dirty_state_table[idx].is_clean == is_clean)
            return 0;
        dirty_state_table[idx].is_clean = is_clean;
        return dirty_state_save();
    }

    if (dirty_state_count >= MAX_TRACKED_VMS) {
        log_msg(LOG_WARN, "Dirty state table is full; cannot track '%s'", name);
        return -1;
    }

    snprintf(dirty_state_table[dirty_state_count].name,
             sizeof(dirty_state_table[dirty_state_count].name), "%s", name);
    dirty_state_table[dirty_state_count].is_clean = is_clean;
    dirty_state_count++;
    return dirty_state_save();
}

int dirty_state_init(const char *path)
{
    dirty_state_shutdown();

    if (!path || !path[0]) {
        log_msg(LOG_WARN, "Dirty state persistence disabled: no path configured");
        return 0;
    }

    if (snprintf(dirty_state_path, sizeof(dirty_state_path), "%s", path) >=
        (int)sizeof(dirty_state_path)) {
        dirty_state_path[0] = '\0';
        log_msg(LOG_ERROR, "Dirty state path is too long");
        return -1;
    }

    if (access(dirty_state_path, F_OK) != 0) {
        if (errno == ENOENT)
            return 0;

        log_msg(LOG_ERROR, "Failed to access dirty state file '%s': %s",
                dirty_state_path, strerror(errno));
        return -1;
    }

    json_error_t error;
    json_t *root = json_load_file(dirty_state_path, 0, &error);
    if (!root) {
        log_msg(LOG_ERROR, "Failed to load dirty state file '%s': %s (line %d)",
                dirty_state_path, error.text, error.line);
        return -1;
    }

    if (!json_is_array(root)) {
        json_decref(root);
        log_msg(LOG_ERROR, "Dirty state file '%s' must contain a JSON array",
                dirty_state_path);
        return -1;
    }

    size_t count = json_array_size(root);
    for (size_t i = 0; i < count; i++) {
        json_t *entry = json_array_get(root, i);
        if (!json_is_object(entry)) {
            log_msg(LOG_WARN, "Skipping malformed dirty state entry at index %zu", i);
            continue;
        }

        json_t *name = json_object_get(entry, "name");
        json_t *is_clean = json_object_get(entry, "is_clean");
        if (!json_is_string(name) || !json_is_boolean(is_clean)) {
            log_msg(LOG_WARN, "Skipping invalid dirty state entry at index %zu", i);
            continue;
        }

        if (dirty_state_count >= MAX_TRACKED_VMS) {
            log_msg(LOG_WARN, "Dirty state file exceeds capacity; extra entries ignored");
            break;
        }

        const char *vm_name = json_string_value(name);
        if (!vm_name || !vm_name[0]) {
            log_msg(LOG_WARN, "Skipping dirty state entry with empty VM name");
            continue;
        }

        snprintf(dirty_state_table[dirty_state_count].name,
                 sizeof(dirty_state_table[dirty_state_count].name), "%s", vm_name);
        dirty_state_table[dirty_state_count].is_clean = json_is_true(is_clean) ? 1 : 0;
        dirty_state_count++;
    }

    json_decref(root);
    return 0;
}

void dirty_state_shutdown(void)
{
    dirty_state_clear();
    dirty_state_path[0] = '\0';
}

void dirty_state_mark_clean(const char *name)
{
    dirty_state_set(name, 1);
}

void dirty_state_mark_dirty(const char *name)
{
    dirty_state_set(name, 0);
}

int dirty_state_is_dirty(const char *name, vm_state_t state)
{
    if (state == VM_RUNNING) {
        dirty_state_mark_dirty(name);
        return 1;
    }

    int idx = dirty_state_find(name);
    if (idx >= 0)
        return !dirty_state_table[idx].is_clean;

    if (state == VM_PAUSED) {
        dirty_state_mark_dirty(name);
        return 1;
    }

    return 0;
}
