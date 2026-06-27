#ifndef FANGS_LAYOUT_H
#define FANGS_LAYOUT_H

#include <stdbool.h>

// Forward declaration — defined in pane.h (included by caller at point of use).
typedef struct PaneNode PaneNode;

typedef struct {
    int x;
    int y;
    int w;
    int h;
} Rect;

typedef struct {
    Rect terminal;
    Rect sidebar;
    bool sidebar_visible;
} Layout;

Layout layout_compute(int window_w, int window_h, bool sidebar_visible,
                      int sidebar_width, int pad, int min_terminal_w);

// Callback invoked for each leaf pane with its assigned pixel rect.
typedef void (*PaneRectFn)(const PaneNode *, int x, int y, int w, int h, void *user);

// Compute pixel rects for every leaf in the pane tree within the given
// terminal area. Calls `cb` for each leaf with its assigned rect and userdata.
// Root is the current tab's pane tree; x/y/w/h is the terminal area in pixels.
void layout_compute_panes(const PaneNode *root,
                          int term_x, int term_y, int term_w, int term_h,
                          PaneRectFn cb, void *user);

#endif // FANGS_LAYOUT_H
