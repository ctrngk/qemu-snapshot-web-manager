#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "../src/html_render.h"

static void test_render_vm_list(void)
{
    /* Test with VMs */
    vm_info_t vm1 = { .name = "testvm", .uuid = "abc-123",
                      .state = VM_RUNNING, .vcpus = 2, .memory_kb = 2048000 };
    vm_info_t vm2 = { .name = "devvm", .uuid = "def-456",
                      .state = VM_SHUTOFF, .vcpus = 4, .memory_kb = 4096000 };
    vm_info_t *vms[] = { &vm1, &vm2 };

    char *html = render_vm_list(vms, 2);
    assert(html != NULL);
    assert(strstr(html, "testvm") != NULL);
    assert(strstr(html, "devvm") != NULL);
    assert(strstr(html, "vm-item") != NULL);
    assert(strstr(html, "selectVm") != NULL);
    assert(strstr(html, "state-running") != NULL);
    assert(strstr(html, "state-shutoff") != NULL);
    free(html);

    /* Test with no VMs */
    char *empty = render_vm_list(NULL, 0);
    assert(empty != NULL);
    assert(strstr(empty, "No virtual machines") != NULL);
    free(empty);

    printf("  PASS: test_render_vm_list\n");
}

static void test_render_snapshot_detail(void)
{
    snapshot_node_t snap = {
        .id = "snap1", .description = "Test snapshot",
        .creation_time = "2026-01-01T00:00:00Z",
        .type = SNAP_INTERNAL, .is_current = 1,
        .parent = NULL, .children = NULL,
        .child_count = 0, .child_capacity = 0
    };

    char *html = render_snapshot_detail("testvm", &snap, VM_SHUTOFF);
    assert(html != NULL);
    assert(strstr(html, "snap1") != NULL);
    assert(strstr(html, "Test snapshot") != NULL);
    assert(strstr(html, "badge-internal") != NULL);
    assert(strstr(html, "Current") != NULL);
    assert(strstr(html, "hx-get=\"/api/vms/testvm/snapshots/snap1/revert-confirm\"") != NULL);
    assert(strstr(html, "hx-delete") != NULL);
    assert(strstr(html, "/api/vms/testvm/snapshots/snap1/revert") != NULL);
    free(html);

    /* Test with NULL snap */
    char *null_html = render_snapshot_detail("testvm", NULL, VM_SHUTOFF);
    assert(null_html != NULL);
    assert(strstr(null_html, "not found") != NULL);
    free(null_html);

    /* Test external snapshot (should show merge button) */
    snapshot_node_t ext_snap = {
        .id = "ext1", .description = "External",
        .creation_time = "2026-02-01", .type = SNAP_EXTERNAL, .is_current = 0,
        .parent = NULL, .children = NULL,
        .child_count = 0, .child_capacity = 0
    };
    char *ext_html = render_snapshot_detail("testvm", &ext_snap, VM_SHUTOFF);
    assert(strstr(ext_html, "badge-external") != NULL);
    assert(strstr(ext_html, "Merge") != NULL);
    free(ext_html);

    printf("  PASS: test_render_snapshot_detail\n");
}

static void test_render_create_form(void)
{
    char *html = render_create_snapshot_form("testvm", 1);
    assert(html != NULL);
    assert(strstr(html, "hx-post=\"/api/vms/testvm/snapshots\"") != NULL);
    assert(strstr(html, "name=\"name\"") != NULL);
    assert(strstr(html, "name=\"type\"") != NULL);
    assert(strstr(html, "internal") != NULL);
    assert(strstr(html, "external") != NULL);
    free(html);
    printf("  PASS: test_render_create_form\n");
}

static void test_render_shared_folders(void)
{
    shared_folder_t f1 = { .source_dir = "/home/user/share",
                           .mount_tag = "myshare", .fs_type = "virtiofs", .read_only = 0 };
    shared_folder_t f2 = { .source_dir = "/data/tmp",
                           .mount_tag = "tmpdata", .fs_type = "9p", .read_only = 1 };
    shared_folder_t folders[] = { f1, f2 };

    char *html = render_shared_folders("testvm", folders, 2, 0);
    assert(html != NULL);
    assert(strstr(html, "/home/user/share") != NULL);
    assert(strstr(html, "myshare") != NULL);
    assert(strstr(html, "badge-readonly") != NULL);
    free(html);

    /* Empty */
    char *empty = render_shared_folders("testvm", NULL, 0, -1);
    assert(strstr(empty, "No shared folders") != NULL);
    free(empty);

    printf("  PASS: test_render_shared_folders\n");
}

static void test_render_messages(void)
{
    char *ok = render_success("Snapshot created");
    assert(strstr(ok, "alert-success") != NULL);
    assert(strstr(ok, "Snapshot created") != NULL);
    free(ok);

    char *err = render_error("Connection failed");
    assert(strstr(err, "alert-error") != NULL);
    assert(strstr(err, "Connection failed") != NULL);
    free(err);

    printf("  PASS: test_render_messages\n");
}

static void test_html_escaping(void)
{
    /* Verify user-provided strings are escaped */
    vm_info_t vm = { .name = "<script>alert('xss')</script>",
                     .uuid = "u", .state = VM_RUNNING,
                     .vcpus = 1, .memory_kb = 1024 };
    vm_info_t *vms[] = { &vm };

    char *html = render_vm_list(vms, 1);
    assert(html != NULL);
    /* Raw <script> must NOT appear */
    assert(strstr(html, "<script>") == NULL);
    /* Escaped version must appear */
    assert(strstr(html, "&lt;script&gt;") != NULL);
    free(html);

    printf("  PASS: test_html_escaping\n");
}

int main(void)
{
    printf("Running HTML render tests...\n");
    test_render_vm_list();
    test_render_snapshot_detail();
    test_render_create_form();
    test_render_shared_folders();
    test_render_messages();
    test_html_escaping();
    printf("All %d tests passed!\n", 6);
    return 0;
}
