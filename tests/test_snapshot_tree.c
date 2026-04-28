#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "../src/snapshot.h"
#include "../src/util.h"

/* Test 1: Create a single node */
static void test_node_creation(void)
{
    snapshot_node_t *n = snapshot_node_new("snap1", "First snapshot",
                                           "2026-01-01T00:00:00", SNAP_INTERNAL, 1);
    assert(n != NULL);
    assert(strcmp(n->id, "snap1") == 0);
    assert(strcmp(n->description, "First snapshot") == 0);
    assert(strcmp(n->creation_time, "2026-01-01T00:00:00") == 0);
    assert(n->type == SNAP_INTERNAL);
    assert(n->is_current == 1);
    assert(n->child_count == 0);
    assert(n->parent == NULL);
    snapshot_tree_free(n);
    printf("  PASS: test_node_creation\n");
}

/* Test 2: Build a tree and verify parent-child relationships */
static void test_tree_building(void)
{
    snapshot_node_t *root = snapshot_node_new("root", "Root", "2026-01-01", SNAP_INTERNAL, 0);
    snapshot_node_t *c1 = snapshot_node_new("child1", "Child 1", "2026-01-02", SNAP_INTERNAL, 0);
    snapshot_node_t *c2 = snapshot_node_new("child2", "Child 2", "2026-01-03", SNAP_EXTERNAL, 1);
    snapshot_node_t *gc1 = snapshot_node_new("grandchild1", "GC1", "2026-01-04", SNAP_INTERNAL, 0);

    snapshot_node_add_child(root, c1);
    snapshot_node_add_child(root, c2);
    snapshot_node_add_child(c1, gc1);

    assert(root->child_count == 2);
    assert(root->children[0] == c1);
    assert(root->children[1] == c2);
    assert(c1->parent == root);
    assert(c2->parent == root);
    assert(gc1->parent == c1);
    assert(c1->child_count == 1);
    assert(c1->children[0] == gc1);

    snapshot_tree_free(root);
    printf("  PASS: test_tree_building\n");
}

/* Test 3: Find nodes by id */
static void test_tree_find(void)
{
    snapshot_node_t *root = snapshot_node_new("root", "R", "t", SNAP_INTERNAL, 0);
    snapshot_node_t *c1 = snapshot_node_new("c1", "C1", "t", SNAP_INTERNAL, 0);
    snapshot_node_t *c2 = snapshot_node_new("c2", "C2", "t", SNAP_EXTERNAL, 0);
    snapshot_node_t *gc1 = snapshot_node_new("gc1", "GC1", "t", SNAP_INTERNAL, 0);
    snapshot_node_add_child(root, c1);
    snapshot_node_add_child(root, c2);
    snapshot_node_add_child(c1, gc1);

    assert(snapshot_tree_find(root, "root") == root);
    assert(snapshot_tree_find(root, "c1") == c1);
    assert(snapshot_tree_find(root, "c2") == c2);
    assert(snapshot_tree_find(root, "gc1") == gc1);
    assert(snapshot_tree_find(root, "nonexistent") == NULL);
    assert(snapshot_tree_find(NULL, "root") == NULL);

    snapshot_tree_free(root);
    printf("  PASS: test_tree_find\n");
}

/* Test 4: Count nodes */
static void test_tree_count(void)
{
    assert(snapshot_tree_count(NULL) == 0);

    snapshot_node_t *root = snapshot_node_new("root", "R", "t", SNAP_INTERNAL, 0);
    assert(snapshot_tree_count(root) == 1);

    snapshot_node_t *c1 = snapshot_node_new("c1", "", "t", SNAP_INTERNAL, 0);
    snapshot_node_t *c2 = snapshot_node_new("c2", "", "t", SNAP_INTERNAL, 0);
    snapshot_node_add_child(root, c1);
    snapshot_node_add_child(root, c2);
    assert(snapshot_tree_count(root) == 3);

    snapshot_node_t *gc = snapshot_node_new("gc", "", "t", SNAP_INTERNAL, 0);
    snapshot_node_add_child(c1, gc);
    assert(snapshot_tree_count(root) == 4);

    snapshot_tree_free(root);
    printf("  PASS: test_tree_count\n");
}

/* Test 5: JSON serialization */
static void test_tree_to_json(void)
{
    snapshot_node_t *root = snapshot_node_new("snap-root", "Root snapshot",
                                              "2026-01-01T10:00:00", SNAP_INTERNAL, 0);
    snapshot_node_t *child = snapshot_node_new("snap-child", "Child snapshot",
                                               "2026-01-02T10:00:00", SNAP_EXTERNAL, 1);
    snapshot_node_add_child(root, child);

    char *json = snapshot_tree_to_json(root);
    assert(json != NULL);

    /* Verify key fields are present */
    assert(strstr(json, "\"id\": \"snap-root\"") != NULL);
    assert(strstr(json, "\"name\": \"snap-root\"") != NULL);
    assert(strstr(json, "\"type\": \"internal\"") != NULL);
    assert(strstr(json, "\"isCurrent\": false") != NULL);
    assert(strstr(json, "\"children\"") != NULL);
    assert(strstr(json, "\"snap-child\"") != NULL);
    assert(strstr(json, "\"type\": \"external\"") != NULL);
    assert(strstr(json, "\"isCurrent\": true") != NULL);

    free(json);

    /* NULL root should return NULL */
    assert(snapshot_tree_to_json(NULL) == NULL);

    snapshot_tree_free(root);
    printf("  PASS: test_tree_to_json\n");
}

/* Test 6: NULL description and creation_time */
static void test_null_fields(void)
{
    snapshot_node_t *n = snapshot_node_new("id1", NULL, NULL, SNAP_INTERNAL, 0);
    assert(n != NULL);
    assert(n->description == NULL);
    assert(n->creation_time == NULL);

    char *json = snapshot_tree_to_json(n);
    assert(json != NULL);
    assert(strstr(json, "\"description\": \"\"") != NULL);
    assert(strstr(json, "\"date\": \"\"") != NULL);
    free(json);

    snapshot_tree_free(n);
    printf("  PASS: test_null_fields\n");
}

/* Test 7: Dynamic array growth (add many children) */
static void test_many_children(void)
{
    snapshot_node_t *root = snapshot_node_new("root", "R", "t", SNAP_INTERNAL, 0);
    for (int i = 0; i < 20; i++) {
        char name[32];
        snprintf(name, sizeof(name), "child-%d", i);
        snapshot_node_t *c = snapshot_node_new(name, "", "t", SNAP_INTERNAL, 0);
        snapshot_node_add_child(root, c);
    }
    assert(root->child_count == 20);
    assert(snapshot_tree_count(root) == 21);

    /* Verify names are correct */
    assert(strcmp(root->children[0]->id, "child-0") == 0);
    assert(strcmp(root->children[19]->id, "child-19") == 0);

    snapshot_tree_free(root);
    printf("  PASS: test_many_children\n");
}

/*
 * Test 8: Regression — wrong parent when virDomainSnapshotGetParent() fails.
 *
 * Real-world failure: snapshot "test-purpose-on-usb-fedora" appeared under
 * "freshly-updated" instead of its true parent "add-shared-folder".
 *
 * Root cause: lv_list_snapshots Phase 2 relies solely on
 * virDomainSnapshotGetParent().  When that API call returns NULL (a transient
 * libvirt error), the snapshot has no api_parent_id and falls into the
 * "no parent → attach to root" branch, landing it directly under
 * "freshly-updated" (the tree root) instead of "add-shared-folder".
 *
 * snapshot_tree_build_from_flat() exposes the same bug: it accepts both an
 * api_parent_ids[] array (mirrors the unreliable API) and an xml_parent_ids[]
 * array (mirrors the reliable XML <parent> element), but currently ignores
 * xml_parent_ids entirely.  This test sets api_parent_ids[2] = NULL to
 * simulate the API failure and asserts the correct parent — which the buggy
 * implementation fails to produce.
 *
 * Expected hierarchy:
 *   freshly-updated
 *     └── add-shared-folder
 *           └── test-purpose-on-usb-fedora   ← must be here, not at top level
 */
static void test_regression_wrong_parent_usb_fedora(void)
{
    /* The three snapshot names from the actual bug report */
    const char *ids[3] = {
        "freshly-updated",
        "add-shared-folder",
        "test-purpose-on-usb-fedora",
    };

    /*
     * Simulate virDomainSnapshotGetParent() results.
     * api_parent_ids[2] == NULL: the API call failed for
     * "test-purpose-on-usb-fedora" even though a parent exists in XML.
     */
    const char *api_parent_ids[3] = {
        NULL,               /* freshly-updated: genuine root */
        "freshly-updated",  /* add-shared-folder: API returned correct parent */
        NULL,               /* test-purpose-on-usb-fedora: API failure → NULL */
    };

    /*
     * Simulate XML-parsed <parent> values — the reliable fallback.
     * xml_parent_ids[2] carries the true parent "add-shared-folder".
     */
    const char *xml_parent_ids[3] = {
        NULL,
        "freshly-updated",
        "add-shared-folder",  /* correct parent known from XML */
    };

    snapshot_node_t *root = snapshot_tree_build_from_flat(
        3, ids, api_parent_ids, xml_parent_ids);

    assert(root != NULL);
    assert(strcmp(root->id, "freshly-updated") == 0);

    snapshot_node_t *add = snapshot_tree_find(root, "add-shared-folder");
    assert(add != NULL);

    snapshot_node_t *usb = snapshot_tree_find(root, "test-purpose-on-usb-fedora");
    assert(usb != NULL);

    /*
     * Core regression assertion: test-purpose-on-usb-fedora must be a child
     * of add-shared-folder, NOT a direct child of freshly-updated.
     *
     * The buggy implementation ignores xml_parent_ids and treats the NULL
     * api_parent_ids[2] as "no parent", so it wrongly attaches the node to
     * the root.  This assert therefore FAILS against the current code.
     */
    assert(usb->parent == add);           /* must be under add-shared-folder */
    assert(usb->parent != root);          /* must NOT be directly under root  */

    snapshot_tree_free(root);
    printf("  PASS: test_regression_wrong_parent_usb_fedora\n");
}

int main(void)
{
    printf("Running snapshot tree tests...\n");
    test_node_creation();
    test_tree_building();
    test_tree_find();
    test_tree_count();
    test_tree_to_json();
    test_null_fields();
    test_many_children();
    test_regression_wrong_parent_usb_fedora();
    printf("All %d tests passed!\n", 8);
    return 0;
}
