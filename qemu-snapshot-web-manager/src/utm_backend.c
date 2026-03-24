#include "utm_backend.h"
#include "util.h"
#include <string.h>
#include <stdlib.h>
#include <errno.h>

#ifdef __APPLE__

#include <unistd.h>
#include <sys/wait.h>

/* ─── utmctl CLI helper ─── */

/* Path to utmctl binary. Try app bundle first, then PATH. */
static const char *utmctl_path(void)
{
    if (access("/Applications/UTM.app/Contents/MacOS/utmctl", X_OK) == 0)
        return "/Applications/UTM.app/Contents/MacOS/utmctl";
    if (access("/usr/local/bin/utmctl", X_OK) == 0)
        return "/usr/local/bin/utmctl";
    return NULL;
}

/*
 * Run a command with args, capture stdout+stderr into output buffer.
 * Returns exit code (0 = success), or -1 on fork/exec failure.
 */
static int run_cmd(const char **args, char *output, size_t output_size)
{
    int pipefd[2];
    if (pipe(pipefd) < 0)
        return -1;

    pid_t pid = fork();
    if (pid < 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        return -1;
    }

    if (pid == 0) {
        /* Child */
        close(pipefd[0]);
        dup2(pipefd[1], STDOUT_FILENO);
        dup2(pipefd[1], STDERR_FILENO);
        close(pipefd[1]);
        execv(args[0], (char *const *)args);
        _exit(127);
    }

    /* Parent */
    close(pipefd[1]);
    size_t total = 0;
    ssize_t n;
    while (total < output_size - 1 &&
           (n = read(pipefd[0], output + total, output_size - 1 - total)) > 0) {
        total += (size_t)n;
    }
    output[total] = '\0';
    close(pipefd[0]);

    int status;
    waitpid(pid, &status, 0);
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

/* ─── Map utmctl status string to vm_state_t ─── */

static vm_state_t map_status(const char *s)
{
    if (strcmp(s, "started") == 0 || strcmp(s, "running") == 0)
        return VM_RUNNING;
    if (strcmp(s, "paused") == 0)
        return VM_PAUSED;
    if (strcmp(s, "stopped") == 0)
        return VM_SHUTOFF;
    return VM_OTHER;
}

/* ─── Connection lifecycle ─── */

static int utm_connect(const char *uri)
{
    (void)uri; /* UTM doesn't use a connection URI */
    const char *path = utmctl_path();
    if (!path) {
        log_msg(LOG_ERROR, "utm: utmctl not found");
        return -1;
    }
    log_msg(LOG_INFO, "utm: using utmctl at %s", path);
    return 0;
}

static void utm_disconnect(void)
{
    /* Nothing to clean up */
}

/* ─── VM listing ─── */

/*
 * Parse `utmctl list` output. Expected format:
 *   UUID                                  Status   Name
 *   13e3e4c0-1234-5678-abcd-ef1234567890  stopped  UbuntuVM
 *
 * Skip the header line. For each subsequent non-empty line:
 * extract UUID (first field), Status (second field), Name (rest).
 */
static int utm_list_vms(vm_info_t ***vms, int *count)
{
    const char *path = utmctl_path();
    if (!path)
        return -1;

    const char *args[] = {path, "list", NULL};
    char output[8192];
    int ret = run_cmd(args, output, sizeof(output));
    if (ret != 0) {
        log_msg(LOG_ERROR, "utm: utmctl list failed: %s", output);
        return -1;
    }

    /* Count lines (excluding header) to size the array */
    int capacity = 8;
    vm_info_t **list = calloc((size_t)capacity, sizeof(vm_info_t *));
    if (!list)
        return -1;
    int n = 0;

    char *saveptr = NULL;
    char *line = strtok_r(output, "\n", &saveptr);
    int first = 1;

    while (line) {
        if (first) {
            first = 0; /* skip header line */
            line = strtok_r(NULL, "\n", &saveptr);
            continue;
        }

        /* Skip empty lines */
        while (*line == ' ' || *line == '\t')
            line++;
        if (*line == '\0') {
            line = strtok_r(NULL, "\n", &saveptr);
            continue;
        }

        /* Parse: UUID  Status  Name (whitespace-separated) */
        char *uuid_str = line;
        char *p = line;

        /* Advance past UUID */
        while (*p && *p != ' ' && *p != '\t')
            p++;
        if (*p)
            *p++ = '\0';

        /* Skip whitespace to Status */
        while (*p == ' ' || *p == '\t')
            p++;
        char *status_str = p;

        /* Advance past Status */
        while (*p && *p != ' ' && *p != '\t')
            p++;
        if (*p)
            *p++ = '\0';

        /* Skip whitespace to Name (rest of line) */
        while (*p == ' ' || *p == '\t')
            p++;
        char *name_str = p;

        /* Trim trailing whitespace from name */
        size_t len = strlen(name_str);
        while (len > 0 && (name_str[len - 1] == ' ' || name_str[len - 1] == '\t' ||
                           name_str[len - 1] == '\r'))
            name_str[--len] = '\0';

        if (*uuid_str && *status_str && *name_str) {
            if (n >= capacity) {
                capacity *= 2;
                vm_info_t **tmp = realloc(list, (size_t)capacity * sizeof(vm_info_t *));
                if (!tmp)
                    break;
                list = tmp;
            }

            vm_info_t *vm = calloc(1, sizeof(vm_info_t));
            if (!vm)
                break;

            vm->name = str_dup(name_str);
            vm->uuid = str_dup(uuid_str);
            vm->state = map_status(status_str);
            vm->vcpus = 0;       /* utmctl doesn't report CPU/memory */
            vm->memory_kb = 0;
            list[n++] = vm;
        }

        line = strtok_r(NULL, "\n", &saveptr);
    }

    *vms = list;
    *count = n;
    return 0;
}

static void utm_free_vm_list(vm_info_t **vms, int count)
{
    if (!vms)
        return;
    for (int i = 0; i < count; i++) {
        if (vms[i]) {
            free(vms[i]->name);
            free(vms[i]->uuid);
            free(vms[i]);
        }
    }
    free(vms);
}

/* ─── VM lifecycle ─── */

static int utm_vm_start(const char *vm_name)
{
    const char *path = utmctl_path();
    if (!path)
        return -1;
    const char *args[] = {path, "start", vm_name, NULL};
    char output[1024];
    int ret = run_cmd(args, output, sizeof(output));
    if (ret != 0)
        log_msg(LOG_ERROR, "utm: start failed: %s", output);
    return ret;
}

static int utm_vm_stop(const char *vm_name)
{
    const char *path = utmctl_path();
    if (!path)
        return -1;
    const char *args[] = {path, "stop", vm_name, NULL};
    char output[1024];
    int ret = run_cmd(args, output, sizeof(output));
    if (ret != 0)
        log_msg(LOG_ERROR, "utm: stop failed: %s", output);
    return ret;
}

static int utm_vm_pause(const char *vm_name)
{
    const char *path = utmctl_path();
    if (!path)
        return -1;
    /* utmctl uses "suspend" for pause */
    const char *args[] = {path, "suspend", vm_name, NULL};
    char output[1024];
    int ret = run_cmd(args, output, sizeof(output));
    if (ret != 0)
        log_msg(LOG_ERROR, "utm: suspend (pause) failed: %s", output);
    return ret;
}

/* ─── Snapshot operations — all unsupported via utmctl ─── */

static int utm_list_snapshots(const char *vm_name, snapshot_node_t **tree)
{
    (void)vm_name;
    *tree = NULL;
    log_msg(LOG_WARN, "utm: snapshot listing not supported via utmctl");
    return -ENOTSUP;
}

static int utm_create_snapshot(const char *vm_name, const char *snap_name,
                               const char *description, snap_type_t type)
{
    (void)vm_name;
    (void)snap_name;
    (void)description;
    (void)type;
    log_msg(LOG_WARN, "utm: snapshot creation not supported via utmctl");
    return -ENOTSUP;
}

static int utm_delete_snapshot(const char *vm_name, const char *snap_name,
                               int auto_merge)
{
    (void)vm_name;
    (void)snap_name;
    (void)auto_merge;
    log_msg(LOG_WARN, "utm: snapshot deletion not supported via utmctl");
    return -ENOTSUP;
}

static int utm_revert_snapshot(const char *vm_name, const char *snap_name)
{
    (void)vm_name;
    (void)snap_name;
    log_msg(LOG_WARN, "utm: snapshot revert not supported via utmctl");
    return -ENOTSUP;
}

static int utm_merge_snapshot(const char *vm_name, const char *snap_name)
{
    (void)vm_name;
    (void)snap_name;
    log_msg(LOG_WARN, "utm: snapshot merge not supported via utmctl");
    return -ENOTSUP;
}

/* ─── Shared folders — parse config.plist via plutil ─── */

static int utm_list_shared_folders(const char *vm_name,
                                   shared_folder_t **folders, int *count)
{
    *folders = NULL;
    *count = 0;

    const char *home = getenv("HOME");
    if (!home)
        return -1;

    char plist_path[1024];
    snprintf(plist_path, sizeof(plist_path),
             "%s/Library/Containers/com.utmapp.UTM/Data/Documents/%s.utm/config.plist",
             home, vm_name);

    /* Use plutil to dump plist as human-readable text */
    char cmd_output[8192];
    const char *args[] = {"/usr/bin/plutil", "-p", plist_path, NULL};
    int ret = run_cmd(args, cmd_output, sizeof(cmd_output));
    if (ret != 0)
        return 0; /* No config found or unreadable — return empty list */

    /*
     * Best-effort parse: look for lines containing directory share paths.
     * UTM plist may contain keys like "directoryShareUrl" or entries under
     * a "sharing" dictionary. Since the plist structure varies by UTM version
     * and we don't want to add a plist parsing library, we do a simple
     * line-by-line search for share-related keys.
     *
     * This returns an empty list if no recognizable share entries are found,
     * which is the safe default.
     */
    int capacity = 4;
    shared_folder_t *list = calloc((size_t)capacity, sizeof(shared_folder_t));
    if (!list)
        return -1;
    int n = 0;

    char *saveptr = NULL;
    char *line = strtok_r(cmd_output, "\n", &saveptr);
    while (line) {
        /* Look for lines like: "directoryShareUrl" => "file:///path/to/dir" */
        char *key = strstr(line, "directoryShareUrl");
        if (!key)
            key = strstr(line, "DirectoryShare");
        if (key) {
            char *quote1 = strstr(key, "\"");
            if (quote1) {
                quote1++; /* skip past opening quote of key */
                /* Find the value after => */
                char *arrow = strstr(quote1, "=>");
                if (arrow) {
                    char *vq1 = strchr(arrow, '"');
                    if (vq1) {
                        vq1++;
                        char *vq2 = strchr(vq1, '"');
                        if (vq2) {
                            *vq2 = '\0';
                            /* Strip file:// prefix if present */
                            const char *share_path = vq1;
                            if (strncmp(share_path, "file://", 7) == 0)
                                share_path += 7;

                            if (n >= capacity) {
                                capacity *= 2;
                                shared_folder_t *tmp =
                                    realloc(list, (size_t)capacity * sizeof(shared_folder_t));
                                if (!tmp)
                                    break;
                                list = tmp;
                            }

                            list[n].source_dir = str_dup(share_path);
                            list[n].mount_tag = str_dup("virtiofs");
                            list[n].read_only = 0;
                            n++;
                        }
                    }
                }
            }
        }
        line = strtok_r(NULL, "\n", &saveptr);
    }

    *folders = list;
    *count = n;
    return 0;
}

static void utm_free_shared_folders(shared_folder_t *folders, int count)
{
    if (!folders)
        return;
    for (int i = 0; i < count; i++) {
        free(folders[i].source_dir);
        free(folders[i].mount_tag);
    }
    free(folders);
}

/* ─── Shared folder management — not supported via utmctl ─── */

static int utm_add_shared_folder(const char *vm_name, const char *source_dir,
                                 const char *mount_tag, int read_only,
                                 const char *fs_type)
{
    (void)vm_name;
    (void)source_dir;
    (void)mount_tag;
    (void)read_only;
    (void)fs_type;
    log_msg(LOG_WARN, "utm: adding shared folders not supported via utmctl");
    return -ENOTSUP;
}

static int utm_remove_shared_folder(const char *vm_name, const char *mount_tag)
{
    (void)vm_name;
    (void)mount_tag;
    log_msg(LOG_WARN, "utm: removing shared folders not supported via utmctl");
    return -ENOTSUP;
}

/* ─── Backend vtable ─── */

static vm_backend_t utm_be = {
    .name               = "utm",
    .connect            = utm_connect,
    .disconnect         = utm_disconnect,
    .list_vms           = utm_list_vms,
    .free_vm_list       = utm_free_vm_list,
    .vm_start           = utm_vm_start,
    .vm_stop            = utm_vm_stop,
    .vm_pause           = utm_vm_pause,
    .list_snapshots     = utm_list_snapshots,
    .create_snapshot    = utm_create_snapshot,
    .delete_snapshot    = utm_delete_snapshot,
    .revert_snapshot    = utm_revert_snapshot,
    .merge_snapshot     = utm_merge_snapshot,
    .list_shared_folders  = utm_list_shared_folders,
    .free_shared_folders  = utm_free_shared_folders,
    .add_shared_folder    = utm_add_shared_folder,
    .remove_shared_folder  = utm_remove_shared_folder,
    .mount_shared_folder   = NULL,
    .unmount_shared_folder = NULL,
};

vm_backend_t *utm_backend_get(void)
{
    return &utm_be;
}

#else /* !__APPLE__ */

/* On non-macOS platforms, UTM is not available */
vm_backend_t *utm_backend_get(void)
{
    return NULL;
}

#endif /* __APPLE__ */
