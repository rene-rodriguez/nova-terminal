#include "cmdblocks.h"
#include "cmdblocks_osc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ghostty/vt.h>

// Max command blocks remembered (older ones drop off the top of scrollback
// anyway; their tracked refs are freed on eviction).
#define CB_RING 256

typedef struct {
    GhosttyTrackedGridRef top;   // anchor at the command's prompt row
    int  code;                   // exit code, -1 if unknown
    bool done;                   // OSC D seen
} CbBlock;

struct CmdBlocks {
    CbParser parser;

    CbBlock ring[CB_RING];       // finished blocks (circular)
    int     head;                // index of the oldest
    int     count;               // number stored (<= CB_RING)

    // The live block: started at the most recent prompt (OSC A), not yet
    // followed by another prompt. Holds the running/idle command at the bottom.
    GhosttyTrackedGridRef cur_top;
    bool cur_has;
    int  cur_code;
    bool cur_done;
};

CmdBlocks *cmdblocks_create(void)
{
    CmdBlocks *cb = (CmdBlocks *)calloc(1, sizeof(CmdBlocks));
    return cb;
}

void cmdblocks_destroy(CmdBlocks *cb)
{
    if (!cb) return;
    for (int k = 0; k < cb->count; k++) {
        int idx = (cb->head + k) % CB_RING;
        if (cb->ring[idx].top) ghostty_tracked_grid_ref_free(cb->ring[idx].top);
    }
    if (cb->cur_top) ghostty_tracked_grid_ref_free(cb->cur_top);
    free(cb);
}

// --- model -------------------------------------------------------------------

static void push_finished(CmdBlocks *cb, GhosttyTrackedGridRef top, int code, bool done)
{
    int idx = (cb->head + cb->count) % CB_RING;
    if (cb->count == CB_RING) {
        if (cb->ring[idx].top) ghostty_tracked_grid_ref_free(cb->ring[idx].top);
        cb->head = (cb->head + 1) % CB_RING;
    } else {
        cb->count++;
    }
    cb->ring[idx].top  = top;
    cb->ring[idx].code = code;
    cb->ring[idx].done = done;
}

// A new prompt: retire the previous live block to the ring, anchor a fresh
// tracked ref at the current prompt row.
static void on_prompt(CmdBlocks *cb, GhosttyTerminal term, uint16_t row)
{
    if (cb->cur_has)
        push_finished(cb, cb->cur_top, cb->cur_code, cb->cur_done);

    cb->cur_top  = NULL;
    cb->cur_has  = false;
    cb->cur_code = -1;
    cb->cur_done = false;

    GhosttyPoint pt = { .tag = GHOSTTY_POINT_TAG_VIEWPORT };
    pt.value.coordinate.x = 0;
    pt.value.coordinate.y = row;

    GhosttyTrackedGridRef ref = NULL;
    if (ghostty_terminal_grid_ref_track(term, pt, &ref) == GHOSTTY_SUCCESS && ref) {
        cb->cur_top = ref;
        cb->cur_has = true;
    }
}

void cmdblocks_feed(CmdBlocks *cb, TermEngine *te, const uint8_t *data, size_t len)
{
    if (!cb) return;

    GhosttyTerminal    term = term_engine_terminal(te);
    GhosttyRenderState rs   = term_engine_render_state(te);

    size_t flush = 0, pos = 0;
    CbHit hit;
    while (cb_parse_next(&cb->parser, data, len, &pos, &hit)) {
        if (hit.mark == CB_MARK_PROMPT) {
            // Flush through the A terminator so the engine's cursor is parked at
            // the new prompt row, then anchor a tracked ref there.
            if (hit.end > flush) {
                term_engine_write(te, data + flush, hit.end - flush);
                flush = hit.end;
            }
            ghostty_render_state_update(rs, term);
            uint16_t cy = 0;
            ghostty_render_state_get(rs, GHOSTTY_RENDER_STATE_DATA_CURSOR_VIEWPORT_Y, &cy);
            on_prompt(cb, term, cy);
        } else if (hit.mark == CB_MARK_DONE) {
            if (cb->cur_has) {
                cb->cur_code = hit.code;
                cb->cur_done = true;
            }
        }
        // CB_MARK_CMD / CB_MARK_EXEC: reserved, no-op for now.
    }

    if (len > flush)
        term_engine_write(te, data + flush, len - flush);
}

// --- helpers -----------------------------------------------------------------

static Color tc_color(ThemeColor c, unsigned char a)
{
    return (Color){ c.r, c.g, c.b, a };
}

// Tracked ref → current viewport row. False if it's scrolled out of view.
static bool top_vrow(GhosttyTrackedGridRef ref, int *out)
{
    if (!ref) return false;
    GhosttyPointCoordinate c;
    if (ghostty_tracked_grid_ref_point(ref, GHOSTTY_POINT_TAG_VIEWPORT, &c) != GHOSTTY_SUCCESS)
        return false;
    *out = (int)c.y;
    return true;
}

// Extract a finished command's output text via the engine's select_output API.
// Scans the block's body rows for the first that the engine reports as command
// output, then formats that contiguous output region. Caller frees the result.
static char *block_output_text(GhosttyTerminal term, int top_v, int next_v, int rows)
{
    int hi = next_v < rows ? next_v : rows;
    for (int r = top_v + 1; r < hi; r++) {
        GhosttyPoint pt = { .tag = GHOSTTY_POINT_TAG_VIEWPORT };
        pt.value.coordinate.x = 0;
        pt.value.coordinate.y = (uint32_t)r;

        GhosttyGridRef ref;
        if (ghostty_terminal_grid_ref(term, pt, &ref) != GHOSTTY_SUCCESS)
            continue;

        GhosttySelection sel = GHOSTTY_INIT_SIZED(GhosttySelection);
        if (ghostty_terminal_select_output(term, ref, &sel) != GHOSTTY_SUCCESS)
            continue;

        GhosttyTerminalSelectionFormatOptions o =
            GHOSTTY_INIT_SIZED(GhosttyTerminalSelectionFormatOptions);
        o.emit      = GHOSTTY_FORMATTER_FORMAT_PLAIN;
        o.unwrap    = true;
        o.trim      = true;
        o.selection = &sel;

        uint8_t *out = NULL;
        size_t   n   = 0;
        if (ghostty_terminal_selection_format_alloc(term, NULL, o, &out, &n) == GHOSTTY_SUCCESS
            && out) {
            char *s = malloc(n + 1);
            if (s) { memcpy(s, out, n); s[n] = '\0'; }
            ghostty_free(NULL, out, n);
            return s;
        }
    }
    return NULL;
}

// --- draw --------------------------------------------------------------------

typedef struct { int row; int code; bool live; } CbItem;

// Extract command text from a block's prompt row: the first line of text
// at the prompt row (includes shell prompt + typed command).
// Caller frees the result. Returns NULL on failure.
static char *block_command_text(GhosttyTerminal term, int top_v)
{
    GhosttyPoint pt = { .tag = GHOSTTY_POINT_TAG_VIEWPORT };
    pt.value.coordinate.x = 0;
    pt.value.coordinate.y = (uint32_t)top_v;

    GhosttyGridRef ref;
    if (ghostty_terminal_grid_ref(term, pt, &ref) != GHOSTTY_SUCCESS)
        return NULL;

    // The prompt row is the command *input* line (prompt prefix + typed
    // command), not command output. select_output returns NO_VALUE on a
    // non-output row, which would leave the command empty — use select_line.
    GhosttyTerminalSelectLineOptions lo =
        GHOSTTY_INIT_SIZED(GhosttyTerminalSelectLineOptions);
    lo.ref = ref;
    GhosttySelection sel = GHOSTTY_INIT_SIZED(GhosttySelection);
    if (ghostty_terminal_select_line(term, &lo, &sel) != GHOSTTY_SUCCESS)
        return NULL;

    GhosttyTerminalSelectionFormatOptions o =
        GHOSTTY_INIT_SIZED(GhosttyTerminalSelectionFormatOptions);
    o.emit      = GHOSTTY_FORMATTER_FORMAT_PLAIN;
    o.unwrap    = true;
    o.trim      = true;
    o.selection = &sel;

    uint8_t *out = NULL;
    size_t   n   = 0;
    if (ghostty_terminal_selection_format_alloc(term, NULL, o, &out, &n) == GHOSTTY_SUCCESS
        && out && n > 0) {
        // Trim trailing newline/spaces.
        while (n > 0 && (out[n-1] == '\n' || out[n-1] == '\r' || out[n-1] == ' '))
            n--;
        char *s = malloc(n + 1);
        if (s) { memcpy(s, out, n); s[n] = '\0'; }
        ghostty_free(NULL, out, n);
        return s;
    }
    if (out) ghostty_free(NULL, out, n);
    return NULL;
}

bool cmdblocks_draw(CmdBlocks *cb, TermEngine *te, Font font, const Theme *th,
                    int cell_w, int cell_h, int font_size,
                    int pad, int term_area_w, int rows,
                    int mouse_x, int mouse_y, bool click,
                    CmdBlockAction *action)
{
    if (!cb) return false;
    (void)cell_w;
    GhosttyTerminal term = term_engine_terminal(te);

    // Initialize the action to NONE so the host can detect whether a button fired.
    if (action)
        action->action = CB_ACTION_NONE;

    CbItem items[CB_RING + 1];
    int n = 0;

    for (int k = 0; k < cb->count; k++) {
        CbBlock *b = &cb->ring[(cb->head + k) % CB_RING];
        int r;
        if (!top_vrow(b->top, &r) || r < 0 || r >= rows) continue;
        items[n].row = r; items[n].code = b->code; items[n].live = false; n++;
    }
    if (cb->cur_has) {
        int r;
        if (top_vrow(cb->cur_top, &r) && r >= 0 && r < rows) {
            items[n].row = r; items[n].code = -1; items[n].live = true; n++;
        }
    }
    if (n == 0) return false;

    // Sort ascending by viewport row (small n; insertion sort).
    for (int i = 1; i < n; i++) {
        CbItem t = items[i];
        int j = i - 1;
        while (j >= 0 && items[j].row > t.row) { items[j + 1] = items[j]; j--; }
        items[j + 1] = t;
    }

    Color ok   = tc_color(th->ansi[2], 255);   // green
    Color bad  = tc_color(th->ansi[1], 255);   // red
    Color neut = tc_color(th->ansi[8], 255);   // grey
    Color bg   = tc_color(th->bg, 255);

    int badge_r  = cell_h / 3;
    if (badge_r < 4) badge_r = 4;
    if (badge_r > 7) badge_r = 7;
    int badge_cx = term_area_w - pad - 8 - badge_r;   // left of the scrollbar margin

    int hovered = -1;
    bool consumed = false;

    for (int i = 0; i < n; i++) {
        int row    = items[i].row;
        int y      = pad + row * cell_h;
        int next_y = (i + 1 < n) ? pad + items[i + 1].row * cell_h : pad + rows * cell_h;
        Color stc  = items[i].live ? neut
                   : (items[i].code == 0 ? ok : (items[i].code > 0 ? bad : neut));

        // Left gutter strip spanning the whole block region.
        DrawRectangle(0, y, 3, next_y - y, Fade(stc, 0.55f));

        // Separator line across the block top.
        int sep_x1 = badge_cx - badge_r - 6;
        if (sep_x1 > pad)
            DrawRectangle(pad, y, sep_x1 - pad, 1, Fade(stc, 0.45f));

        // Status badge on the top row (none for the in-progress live block).
        if (!items[i].live) {
            float cxb = (float)badge_cx;
            float cyb = (float)y + (float)cell_h / 2.0f;
            DrawCircle((int)cxb, (int)cyb, (float)badge_r, stc);
            if (items[i].code == 0) {           // ✓ success
                DrawLineEx((Vector2){cxb - badge_r * 0.45f, cyb + badge_r * 0.05f},
                           (Vector2){cxb - badge_r * 0.10f, cyb + badge_r * 0.42f}, 1.7f, bg);
                DrawLineEx((Vector2){cxb - badge_r * 0.10f, cyb + badge_r * 0.42f},
                           (Vector2){cxb + badge_r * 0.50f, cyb - badge_r * 0.42f}, 1.7f, bg);
            } else if (items[i].code > 0) {     // ✗ non-zero exit
                float o = badge_r * 0.42f;
                DrawLineEx((Vector2){cxb - o, cyb - o}, (Vector2){cxb + o, cyb + o}, 1.7f, bg);
                DrawLineEx((Vector2){cxb + o, cyb - o}, (Vector2){cxb - o, cyb + o}, 1.7f, bg);
            }
            // code < 0 (unknown exit): leave the neutral circle bare.
        }

        if (mouse_x >= 0 && mouse_x < term_area_w && mouse_y >= y && mouse_y < next_y)
            hovered = i;
    }

    // Hover affordances: "copy" and "Ask AI" buttons.
    if (hovered >= 0 && !items[hovered].live) {
        int row    = items[hovered].row;
        int y      = pad + row * cell_h;
        int next_v = (hovered + 1 < n) ? items[hovered + 1].row : rows;

        int btn_fs = (int)(font_size * 0.82f);
        if (btn_fs < 8) btn_fs = 8;
        int padx = 6;
        int btn_h = cell_h - 2;

        Color st = items[hovered].code == 0 ? ok
                 : (items[hovered].code > 0 ? bad : neut);

        // "copy" button (rightmost, next to the badge)
        const char *copy_label = "copy";
        Vector2 copy_ts = MeasureTextEx(font, copy_label, (float)btn_fs, 0);
        int copy_w = (int)copy_ts.x + 2 * padx;
        int copy_x = badge_cx - badge_r - 8 - copy_w;
        int copy_y = y + 1;
        Rectangle copy_btn = { (float)copy_x, (float)copy_y, (float)copy_w, (float)btn_h };

        bool copy_over = (mouse_x >= copy_btn.x && mouse_x < copy_btn.x + copy_btn.width
                          && mouse_y >= copy_btn.y && mouse_y < copy_btn.y + copy_btn.height);
        DrawRectangleRounded(copy_btn, 0.35f, 6, Fade(st, copy_over ? 0.45f : 0.22f));
        DrawTextEx(font, copy_label,
                   (Vector2){ copy_btn.x + padx, copy_btn.y + (btn_h - copy_ts.y) / 2 },
                   (float)btn_fs, 0, tc_color(th->fg, 230));

        // "Ask AI" button (left of copy button). Failed blocks get an
        // emphasized "Explain error" label (the button already red-tints via
        // `st`); a literal ⚡ glyph isn't in the embedded font atlas, so the
        // emphasis is the label + status color, not an emoji.
        const char *ai_label = items[hovered].code > 0 ? "Explain error" : "Ask AI";
        Vector2 ai_ts = MeasureTextEx(font, ai_label, (float)btn_fs, 0);
        int ai_w = (int)ai_ts.x + 2 * padx;
        int ai_gap = 4;
        int ai_x = copy_x - ai_gap - ai_w;
        int ai_y = y + 1;
        Rectangle ai_btn = { (float)ai_x, (float)ai_y, (float)ai_w, (float)btn_h };

        bool ai_over = (mouse_x >= ai_btn.x && mouse_x < ai_btn.x + ai_btn.width
                        && mouse_y >= ai_btn.y && mouse_y < ai_btn.y + ai_btn.height);
        DrawRectangleRounded(ai_btn, 0.35f, 6, Fade(st, ai_over ? 0.45f : 0.22f));
        DrawTextEx(font, ai_label,
                   (Vector2){ ai_btn.x + padx, ai_btn.y + (btn_h - ai_ts.y) / 2 },
                   (float)btn_fs, 0, tc_color(th->fg, 230));

        if (click) {
            if (copy_over) {
                char *txt = block_output_text(term, row, next_v, rows);
                if (txt) { SetClipboardText(txt); free(txt); }
                consumed = true;
            } else if (ai_over) {
                // Fill in the action so main.c can respond.
                if (action) {
                    action->action = CB_ACTION_ASK_AI;
                    action->exit_code = items[hovered].code;

                    // Extract command text from the prompt row.
                    char *cmd = block_command_text(term, row);
                    if (cmd) {
                        snprintf(action->command, sizeof(action->command), "%s", cmd);
                        free(cmd);
                    } else {
                        action->command[0] = '\0';
                    }

                    // Extract output text.
                    action->output = block_output_text(term, row, next_v, rows);
                    action->output_len = action->output ? (int)strlen(action->output) : 0;
                }
                consumed = true;
            }
        }
    }

    return consumed;
}

// --- navigation --------------------------------------------------------------

bool cmdblocks_navigate(CmdBlocks *cb, TermEngine *te, int dir)
{
    if (!cb) return false;
    GhosttyTerminal term = term_engine_terminal(te);

    // Current viewport top in absolute screen coordinates.
    GhosttyPoint top = { .tag = GHOSTTY_POINT_TAG_VIEWPORT };
    top.value.coordinate.x = 0;
    top.value.coordinate.y = 0;
    GhosttyGridRef tref;
    if (ghostty_terminal_grid_ref(term, top, &tref) != GHOSTTY_SUCCESS) return false;
    GhosttyPointCoordinate tc;
    if (ghostty_terminal_point_from_grid_ref(term, &tref, GHOSTTY_POINT_TAG_SCREEN, &tc)
        != GHOSTTY_SUCCESS) return false;
    long vp = (long)tc.y;

    bool found = false;
    long best = 0;
    long max_long = (dir < 0) ? -999999 : 999999;
    best = max_long;
    for (int k = 0; k <= cb->count; k++) {
        GhosttyTrackedGridRef ref = (k < cb->count)
            ? cb->ring[(cb->head + k) % CB_RING].top
            : cb->cur_top;
        if (!ref) continue;

        GhosttyPointCoordinate sc;
        if (ghostty_tracked_grid_ref_point(ref, GHOSTTY_POINT_TAG_SCREEN, &sc) != GHOSTTY_SUCCESS)
            continue;
        long sy = (long)sc.y;

        if (dir < 0) {                       // previous: nearest above
            if (sy < vp && (!found || sy > best)) { best = sy; found = true; }
        } else {                             // next: nearest below
            if (sy > vp && (!found || sy < best)) { best = sy; found = true; }
        }
    }
    if (!found) return false;

    GhosttyTerminalScrollViewport sv = {
        .tag   = GHOSTTY_SCROLL_VIEWPORT_DELTA,
        .value = { .delta = (int)(best - vp) },
    };
    ghostty_terminal_scroll_viewport(term, sv);
    return true;
}
