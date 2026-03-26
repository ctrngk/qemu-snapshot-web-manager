#include "snapshot.h"
#include "util.h"

#define INITIAL_CHILD_CAPACITY 4

snapshot_node_t *snapshot_node_new(const char *id, const char *description,
                                   const char *creation_time, snap_type_t type,
                                   int is_current)
{
    snapshot_node_t *node = calloc(1, sizeof(snapshot_node_t));
    if (!node)
        return NULL;

    node->id            = str_dup(id);
    node->description   = str_dup(description);
    node->creation_time = str_dup(creation_time);
    node->type          = type;
    node->is_current    = is_current;
    node->parent        = NULL;
    node->child_count   = 0;
    node->child_capacity = INITIAL_CHILD_CAPACITY;
    node->children      = calloc(INITIAL_CHILD_CAPACITY, sizeof(snapshot_node_t *));

    return node;
}

void snapshot_node_add_child(snapshot_node_t *parent, snapshot_node_t *child)
{
    if (!parent || !child)
        return;

    if (parent->child_count == parent->child_capacity) {
        int new_cap = parent->child_capacity * 2;
        snapshot_node_t **tmp = realloc(parent->children,
                                        (size_t)new_cap * sizeof(snapshot_node_t *));
        if (!tmp)
            return;
        parent->children = tmp;
        parent->child_capacity = new_cap;
    }

    parent->children[parent->child_count++] = child;
    child->parent = parent;
}

void snapshot_tree_free(snapshot_node_t *root)
{
    if (!root)
        return;

    for (int i = 0; i < root->child_count; i++)
        snapshot_tree_free(root->children[i]);

    free(root->id);
    free(root->description);
    free(root->creation_time);
    free(root->children);
    free(root);
}

snapshot_node_t *snapshot_tree_find(snapshot_node_t *root, const char *id)
{
    if (!root || !id)
        return NULL;

    if (str_eq(root->id, id))
        return root;

    for (int i = 0; i < root->child_count; i++) {
        snapshot_node_t *found = snapshot_tree_find(root->children[i], id);
        if (found)
            return found;
    }

    return NULL;
}

static json_t *node_to_json(snapshot_node_t *node)
{
    json_t *obj = json_object();

    json_object_set_new(obj, "id",
                        json_string(node->id ? node->id : ""));
    json_object_set_new(obj, "name",
                        json_string(node->id ? node->id : ""));
    json_object_set_new(obj, "description",
                        json_string(node->description ? node->description : ""));
    json_object_set_new(obj, "date",
                        json_string(node->creation_time ? node->creation_time : ""));

    const char *type_str;
    switch (node->type) {
    case SNAP_INTERNAL:      type_str = "internal"; break;
    case SNAP_EXTERNAL:      type_str = "external"; break;
    case SNAP_CURRENT_STATE: type_str = "current-state"; break;
    default:                 type_str = "unknown"; break;
    }
    json_object_set_new(obj, "type", json_string(type_str));
    json_object_set_new(obj, "isCurrent",
                        json_boolean(node->is_current));

    json_t *children = json_array();
    for (int i = 0; i < node->child_count; i++)
        json_array_append_new(children, node_to_json(node->children[i]));
    json_object_set_new(obj, "children", children);

    return obj;
}

char *snapshot_tree_to_json(snapshot_node_t *root)
{
    if (!root)
        return NULL;

    json_t *obj = node_to_json(root);
    char *str = json_dumps(obj, JSON_INDENT(2));
    json_decref(obj);
    return str;
}

int snapshot_tree_count(snapshot_node_t *root)
{
    if (!root)
        return 0;

    int count = 1;
    for (int i = 0; i < root->child_count; i++)
        count += snapshot_tree_count(root->children[i]);
    return count;
}

void snapshot_tree_add_current_state(snapshot_node_t *root,
                                     const char *vm_state_str,
                                     int is_dirty)
{
    if (!root)
        return;

    /* Find the current snapshot node */
    snapshot_node_t *current = NULL;
    if (root->is_current) {
        current = root;
    } else {
        for (int i = 0; i < root->child_count; i++) {
            current = snapshot_tree_find(root->children[i],
                                         NULL); /* need DFS for is_current */
        }
    }

    /* DFS to find is_current node */
    if (!current) {
        /* Fallback: walk entire tree */
        snapshot_node_t **stack = malloc(sizeof(snapshot_node_t *) * 256);
        int top = 0;
        stack[top++] = root;
        while (top > 0) {
            snapshot_node_t *n = stack[--top];
            if (n->is_current) { current = n; break; }
            for (int i = 0; i < n->child_count && top < 255; i++)
                stack[top++] = n->children[i];
        }
        free(stack);
    }

    if (!current)
        return;

    /* Build description with state and dirty info */
    char desc[128];
    if (is_dirty)
        snprintf(desc, sizeof(desc), "%s (modified)", vm_state_str);
    else
        snprintf(desc, sizeof(desc), "%s", vm_state_str);

    snapshot_node_t *state_node = snapshot_node_new(
        "__current_state__",
        desc,
        "",
        SNAP_CURRENT_STATE,
        0);

    if (state_node)
        snapshot_node_add_child(current, state_node);
}
