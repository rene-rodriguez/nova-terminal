// pane — Binary split tree of terminal panes (§16.3).
// Each tab owns a PaneNode tree; leaves carry a Session; internal nodes are
// H/V splits with a ratio. Pure + testable (no window, no Raylib).
#ifndef FANGS_PANE_H
#define FANGS_PANE_H

#include <stdbool.h>

typedef struct Session Session;

typedef enum {
    PANE_LEAF,
    PANE_HSPLIT,   // horizontal split (left/right)
    PANE_VSPLIT    // vertical split   (top/bottom)
} PaneKind;

typedef struct PaneNode {
    PaneKind kind;
    struct PaneNode *parent;
    union {
        struct { struct PaneNode *left, *right; float ratio; } split;
        struct { Session *session; } leaf;
    };
} PaneNode;

// Create a leaf node owning the given session. Takes ownership of `s`.
PaneNode *pane_leaf(Session *s);

// Split the focused leaf: `focused` must be a PANE_LEAF. Replaces it with a
// new split node; `new_leaf` is the newly created session for the opposite
// pane. Returns the new split root node.
PaneNode *pane_split(PaneNode *root, PaneNode *focused, PaneKind dir,
                     Session *new_leaf, float ratio);

// Close a leaf. The parent split is collapsed; if the tree becomes a single
// leaf, it replaces the root. `*new_focus` is set to the nearest remaining
// leaf. Returns the new root (may be same, may be the other child).
PaneNode *pane_close(PaneNode *root, PaneNode *leaf, PaneNode **new_focus);

// Directional focus move. Returns the leaf in direction dx/dy from `cur`.
PaneNode *pane_focus_move(const PaneNode *root, const PaneNode *cur,
                          int dx, int dy);

// Walk the tree to find the leaf at the given pixel coordinates.
// `leaf_rect` is the callback that yields rect for each leaf (user-defined).
PaneNode *pane_at_pos(PaneNode *root, int x, int y,
                      void (*leaf_rect)(const PaneNode *, int *x, int *y,
                                        int *w, int *h));

// Set the ratio of a split node (clamped 0.15–0.85).
void pane_set_ratio(PaneNode *split, float ratio);

// Recursively free the tree and destroy all sessions.
void pane_destroy(PaneNode *root);

// Count leaves and splits.
int  pane_count_leaves(const PaneNode *root);
int  pane_count_splits(const PaneNode *root);

// Find the first leaf (useful when root is a leaf).
PaneNode *pane_first_leaf(PaneNode *root);

// Find the next/prev leaf in a flat traversal.
PaneNode *pane_next_leaf(PaneNode *root, PaneNode *cur);

// Collect all leaf nodes into a flat array. `max` is the array capacity;
// `*n` is set to the number collected (capped at max).
void pane_collect_leaves(PaneNode *root, PaneNode **out, int max, int *n);

#endif // FANGS_PANE_H
