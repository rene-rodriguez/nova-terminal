#include "layout.h"
#include "pane.h"

static int clamp_nonnegative(int value)
{
    return value < 0 ? 0 : value;
}

Layout layout_compute(int window_w, int window_h, bool sidebar_visible,
                      int sidebar_width, int pad, int min_terminal_w)
{
    (void)pad;

    window_w = clamp_nonnegative(window_w);
    window_h = clamp_nonnegative(window_h);
    sidebar_width = clamp_nonnegative(sidebar_width);
    min_terminal_w = clamp_nonnegative(min_terminal_w);

    Layout lo = {
        .terminal = {0, 0, window_w, window_h},
        .sidebar = {window_w, 0, 0, window_h},
        .sidebar_visible = false,
    };

    if (!sidebar_visible || window_w <= min_terminal_w || sidebar_width == 0)
        return lo;

    int available_sidebar_w = window_w - min_terminal_w;
    int actual_sidebar_w = sidebar_width < available_sidebar_w
        ? sidebar_width
        : available_sidebar_w;

    if (actual_sidebar_w <= 0)
        return lo;

    lo.sidebar_visible = true;
    lo.terminal.w = window_w - actual_sidebar_w;
    lo.sidebar.x = lo.terminal.w;
    lo.sidebar.w = actual_sidebar_w;
    return lo;
}

// ---------------------------------------------------------------------------
// Pane layout (§16.4): compute pixel rects for each leaf in a PaneNode tree
// ---------------------------------------------------------------------------

static void compute_panes_rec(const PaneNode *n,
                              int x, int y, int w, int h,
                              PaneRectFn cb, void *user)
{
    if (!n) return;

    if (n->kind == PANE_LEAF) {
        cb(n, x, y, w, h, user);
        return;
    }

    // Internal node — split. Reserve a small gap between children.
    const int gap = 2; // 2-pixel gutter between panes

    if (n->kind == PANE_HSPLIT) {
        // Horizontal split: left/right
        int left_w = (int)((float)(w - gap) * n->split.ratio);
        if (left_w < 1) left_w = 1;
        int right_w = w - gap - left_w;
        if (right_w < 1) right_w = 1;

        compute_panes_rec(n->split.left,  x, y, left_w, h, cb, user);
        compute_panes_rec(n->split.right, x + left_w + gap, y, right_w, h, cb, user);
    } else if (n->kind == PANE_VSPLIT) {
        // Vertical split: top/bottom
        int top_h = (int)((float)(h - gap) * n->split.ratio);
        if (top_h < 1) top_h = 1;
        int bot_h = h - gap - top_h;
        if (bot_h < 1) bot_h = 1;

        compute_panes_rec(n->split.left,  x, y, w, top_h, cb, user);
        compute_panes_rec(n->split.right, x, y + top_h + gap, w, bot_h, cb, user);
    }
}

void layout_compute_panes(const PaneNode *root,
                          int term_x, int term_y, int term_w, int term_h,
                          PaneRectFn cb, void *user)
{
    compute_panes_rec(root, term_x, term_y, term_w, term_h, cb, user);
}
