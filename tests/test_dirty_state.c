#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "../src/vm_backend.h"

int dirty_state_init(const char *path);
void dirty_state_shutdown(void);
void dirty_state_mark_clean(const char *name);
void dirty_state_mark_dirty(const char *name);
int dirty_state_is_dirty(const char *name, vm_state_t state);

static char *make_temp_state_path(void)
{
    char dir_template[] = "/tmp/qswm-dirty-state-XXXXXX";
    char *dir = mkdtemp(dir_template);
    assert(dir != NULL);

    size_t len = strlen(dir) + strlen("/dirty-state.db") + 1;
    char *path = malloc(len);
    assert(path != NULL);
    snprintf(path, len, "%s/dirty-state.db", dir);
    return path;
}

static void cleanup_temp_state_path(char *path)
{
    if (!path)
        return;

    char *dir = strdup(path);
    assert(dir != NULL);

    char *slash = strrchr(dir, '/');
    assert(slash != NULL);
    *slash = '\0';

    unlink(path);
    rmdir(dir);
    free(dir);
    free(path);
}

static void test_dirty_state_persists_across_restart(void)
{
    char *path = make_temp_state_path();

    assert(dirty_state_init(path) == 0);
    assert(dirty_state_is_dirty("fedora43", VM_SHUTOFF) == 0);

    dirty_state_mark_dirty("fedora43");
    assert(dirty_state_is_dirty("fedora43", VM_SHUTOFF) == 1);

    dirty_state_shutdown();

    assert(dirty_state_init(path) == 0);
    assert(dirty_state_is_dirty("fedora43", VM_SHUTOFF) == 1);

    dirty_state_mark_clean("fedora43");
    assert(dirty_state_is_dirty("fedora43", VM_SHUTOFF) == 0);

    dirty_state_shutdown();

    assert(dirty_state_init(path) == 0);
    assert(dirty_state_is_dirty("fedora43", VM_SHUTOFF) == 0);
    dirty_state_shutdown();

    cleanup_temp_state_path(path);
    printf("  PASS: test_dirty_state_persists_across_restart\n");
}

int main(void)
{
    printf("Running dirty state tests...\n");
    test_dirty_state_persists_across_restart();
    printf("All %d tests passed!\n", 1);
    return 0;
}
