#ifndef SNAPSHOT_H
#define SNAPSHOT_H

#include <jansson.h>

typedef enum { SNAP_INTERNAL, SNAP_EXTERNAL } snap_type_t;

typedef struct snapshot_node {
    char *id;                      /* unique snapshot name/id */
    char *description;             /* user description */
    char *creation_time;           /* ISO-8601 timestamp string */
    snap_type_t type;              /* internal or external */
    int is_current;                /* 1 if this is the current/active snapshot */

    struct snapshot_node *parent;  /* parent node (NULL for root) */
    struct snapshot_node **children;
    int child_count;
    int child_capacity;
} snapshot_node_t;

/* Create a new snapshot node. All strings are duplicated internally. */
snapshot_node_t *snapshot_node_new(const char *id, const char *description,
                                   const char *creation_time, snap_type_t type,
                                   int is_current);

/* Add a child to a parent node. Sets child->parent automatically. */
void snapshot_node_add_child(snapshot_node_t *parent, snapshot_node_t *child);

/* Recursively free the entire tree starting from root. */
void snapshot_tree_free(snapshot_node_t *root);

/* Find a node by id using DFS. Returns NULL if not found. */
snapshot_node_t *snapshot_tree_find(snapshot_node_t *root, const char *id);

/* Serialize the tree to a JSON string for D3.js consumption.
 * Returns a malloc'd JSON string. Caller must free().
 *
 * JSON format:
 * {
 *   "id": "snap1",
 *   "name": "snap1",           (same as id, for D3 compatibility)
 *   "description": "...",
 *   "date": "2026-01-01T00:00:00",
 *   "type": "internal",        ("internal" or "external")
 *   "isCurrent": true,
 *   "children": [ ... ]
 * }
 */
char *snapshot_tree_to_json(snapshot_node_t *root);

/* Count total nodes in the tree. */
int snapshot_tree_count(snapshot_node_t *root);

#endif
