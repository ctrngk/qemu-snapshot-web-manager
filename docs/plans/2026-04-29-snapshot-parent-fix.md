# Snapshot Parent Resolution Fix Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Make snapshot tree generation preserve the correct parent-child hierarchy so new snapshots appear under the actual parent snapshot instead of the wrong branch.

**Architecture:** Extend the existing snapshot-tree tests to cover the bad parent-resolution case, then make the libvirt backend explicitly track whether a parent node was matched before deciding how to attach the node. Keep the API and frontend unchanged unless the backend fix proves insufficient.

**Tech Stack:** C11, libvirt, jansson, GNU make

---

### Task 1: Reproduce the broken hierarchy in a failing unit test

**Files:**
- Modify: `tests/test_snapshot_tree.c`
- Test: `tests/test_snapshot_tree.c`

**Step 1: Write the failing test**

```c
static void test_tree_preserves_expected_parent(void)
{
    snapshot_node_t *root = snapshot_node_new("add-shared-folder", "", "", SNAP_INTERNAL, 0);
    snapshot_node_t *fresh = snapshot_node_new("freshly-updated", "", "", SNAP_INTERNAL, 0);
    snapshot_node_t *newer = snapshot_node_new("test-purpose-on-usb-fedora", "", "", SNAP_INTERNAL, 0);

    snapshot_node_add_child(root, fresh);
    snapshot_node_add_child(root, newer);

    assert(newer->parent == root);
    snapshot_tree_free(root);
}
```

**Step 2: Run test to verify it fails**

Run: `make test`
Expected: FAIL once the test is updated to model the current broken backend attachment behavior more precisely than the placeholder above.

**Step 3: Write minimal implementation**

```c
/* Adjust the test fixture so it uses the same parent-linking path as the backend
 * and demonstrates the unresolved-parent case before production changes.
 */
```

**Step 4: Run test to verify it fails for the expected reason**

Run: `make test`
Expected: FAIL in the new snapshot-tree regression only.

**Step 5: Commit**

```bash
git add tests/test_snapshot_tree.c
git commit -m "test: reproduce snapshot parent regression"
```

### Task 2: Fix parent resolution in the libvirt backend

**Files:**
- Modify: `src/libvirt_backend.c:410-435`
- Test: `tests/test_snapshot_tree.c`

**Step 1: Write the failing test**

```c
assert(strcmp(root->children[0]->id, "freshly-updated") == 0);
assert(strcmp(root->children[1]->id, "test-purpose-on-usb-fedora") == 0);
```

**Step 2: Run test to verify it fails**

Run: `make test`
Expected: FAIL because backend parent matching still allows the new snapshot to be attached through the wrong branch or orphan path.

**Step 3: Write minimal implementation**

```c
int parent_found = 0;
for (int j = 0; j < n; j++) {
    if (nodes[j] && str_eq(nodes[j]->id, parent_name)) {
        snapshot_node_add_child(nodes[j], nodes[i]);
        parent_found = 1;
        break;
    }
}
if (!parent_found) {
    /* attach deliberately instead of leaving nodes[i] orphaned */
}
```

**Step 4: Run test to verify it passes**

Run: `make test`
Expected: PASS for the new regression and the existing snapshot-tree suite.

**Step 5: Commit**

```bash
git add src/libvirt_backend.c tests/test_snapshot_tree.c
git commit -m "fix: preserve snapshot parent relationships"
```

### Task 3: Run repo verification

**Files:**
- Modify: none
- Test: `tests/test_snapshot_tree.c`

**Step 1: Run the full test target**

Run: `make test`
Expected: PASS

**Step 2: Run a clean rebuild**

Run: `make clean && make test`
Expected: PASS

**Step 3: Commit**

```bash
git add src/libvirt_backend.c tests/test_snapshot_tree.c
git commit -m "test: verify snapshot tree parent fix"
```
