#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>

#include "raylib.h"
#include "raygui.h"
#include <ghostty/vt.h>

#include "ai_block.h"
#include "ai_provider.h"
#include "cmdblocks.h"
#include "config.h"
#include "context.h"
#include "inline_cmd.h"
#include "layout.h"
#include "pane.h"
#include "pty.h"
#include "redact.h"
#include "session.h"
#include "term_engine.h"
#include "theme.h"
#include "ui_inline.h"
#include "ui_sidebar.h"
#include "ui_sidebar_model.h"
#include "ui_settings.h"
#include "ui_theme.h"
#include "ui_toast.h"
#include "ui_effects.h"

// Font embedded into the binary at compile time (CMake bin2header from
// assets/JetBrainsMono-Regular.ttf and JetBrainsMono-Bold.ttf).
#include "font_jetbrains_mono.h"
#include "font_jetbrains_mono_bold.h"

// Font-zoom (Ctrl +/-/0) bounds. Default matches config_defaults().
#define FANGS_DEFAULT_FONT_SIZE 16
#define FANGS_MIN_FONT_SIZE     6
#define FANGS_MAX_FONT_SIZE     96

// Max tabs (Cmd+1..9 selects tab N; 0 reserved).
#define FANGS_MAX_TABS 9

// Tab structure: owns a pane tree of terminal sessions.
typedef struct {
    PaneNode *root;
    PaneNode *focused;
    char title[64];
} Tab;

// App: the whole terminal window.
typedef struct {
    Tab tabs[FANGS_MAX_TABS];
    int n_tabs;
    int active;  // index into tabs[]
} App;

// Global App: owns all tabs and their pane trees of Sessions (§16.5).
static App app = {0};

// Pointer to the active session's CmdBlocks, kept in sync by
// sync_active_session(). Used by feed_engine() and the draw loop.
static CmdBlocks *g_cmdblocks = NULL;

// ---------------------------------------------------------------------------
// Pane-rect collector for multi-pane rendering (§16.4)
// ---------------------------------------------------------------------------

typedef struct { PaneNode *leaf; int x, y, w, h; } PaneRectEntry;

typedef struct {
    PaneRectEntry *entries;
    int count, capacity;
} PaneRectCollector;

static void pane_rect_collect_cb(const PaneNode *n, int x, int y, int w, int h, void *user)
{
    PaneRectCollector *c = (PaneRectCollector *)user;
    if (c->count < c->capacity) {
        c->entries[c->count].leaf = (PaneNode *)n;
        c->entries[c->count].x = x;
        c->entries[c->count].y = y;
        c->entries[c->count].w = w;
        c->entries[c->count].h = h;
        c->count++;
    }
}

// ---------------------------------------------------------------------------
// Session handle mgmt
// ---------------------------------------------------------------------------

// Re-extract the active session's handles into the main-loop locals.
// Call on tab switch, pane split/close/focus change, and at startup.
// Returns the active Session*, or NULL if no tab/session exists.
// Also updates the module-global g_cmdblocks pointer.
static Session *sync_active_session(TermEngine **te_out,
                                     int *pty_fd_out, pid_t *child_out,
                                     bool *child_exited_out)
{
    if (app.n_tabs < 1 || app.active < 0 || app.active >= app.n_tabs) {
        g_cmdblocks = NULL;
        if (te_out) *te_out = NULL;
        if (pty_fd_out) *pty_fd_out = -1;
        if (child_out) *child_out = -1;
        if (child_exited_out) *child_exited_out = true;
        return NULL;
    }
    Tab *tab = &app.tabs[app.active];
    if (!tab->focused)
        tab->focused = pane_first_leaf(tab->root);
    PaneNode *leaf = tab->focused;
    if (!leaf || leaf->kind != PANE_LEAF) {
        leaf = pane_first_leaf(tab->root);
        tab->focused = leaf;
    }
    if (!leaf) {
        g_cmdblocks = NULL;
        if (te_out) *te_out = NULL;
        if (pty_fd_out) *pty_fd_out = -1;
        if (child_out) *child_out = -1;
        if (child_exited_out) *child_exited_out = true;
        return NULL;
    }
    Session *s = leaf->leaf.session;
    g_cmdblocks = (CmdBlocks *)session_cmdblocks(s);
    if (te_out)      *te_out      = (TermEngine *)session_engine(s);
    if (pty_fd_out)  *pty_fd_out  = session_pty_fd(s);
    if (child_out)   *child_out   = session_child_pid(s);
    if (child_exited_out) *child_exited_out = !session_child_alive(s);
    return s;
}

// Initialize the App with a single tab containing one leaf Session.
// Returns the Session pointer for handle extraction, or NULL on failure.
static Session *app_init_first_tab(uint16_t cols, uint16_t rows,
                                    int cell_w, int cell_h,
                                    int max_scrollback)
{
    Session *s = session_create(cols, rows, cell_w, cell_h, max_scrollback, NULL);
    if (!s) return NULL;

    Tab *tab = &app.tabs[0];
    tab->root    = pane_leaf(s);
    tab->focused = tab->root;
    snprintf(tab->title, sizeof(tab->title), "1");
    app.n_tabs  = 1;
    app.active  = 0;
    return s;
}

// Switch to a different tab. Returns the new active Session*, or NULL.
static Session *app_switch_tab(int idx, TermEngine **te, int *pty_fd,
                                pid_t *child, bool *child_exited)
{
    if (idx < 0 || idx >= app.n_tabs || idx == app.active)
        return sync_active_session(te, pty_fd, child, child_exited);
    app.active = idx;
    return sync_active_session(te, pty_fd, child, child_exited);
}

// Add a new empty tab with an initial Session.
static Session *app_add_tab(uint16_t cols, uint16_t rows,
                             int cell_w, int cell_h,
                             int max_scrollback, const char *cwd,
                             TermEngine **te, int *pty_fd,
                             pid_t *child, bool *child_exited)
{
    if (app.n_tabs >= FANGS_MAX_TABS) return NULL;

    Session *s = session_create(cols, rows, cell_w, cell_h, max_scrollback, cwd);
    if (!s) return NULL;

    Tab *tab = &app.tabs[app.n_tabs];
    tab->root    = pane_leaf(s);
    tab->focused = tab->root;
    snprintf(tab->title, sizeof(tab->title), "%d", app.n_tabs + 1);
    app.n_tabs++;
    app.active = app.n_tabs - 1;  // switch to the new tab
    return sync_active_session(te, pty_fd, child, child_exited);
}

// Close the active tab or focused pane in it.
static bool app_close_active(void)
{
    if (app.n_tabs <= 0) return false;

    Tab *tab = &app.tabs[app.active];
    if (!tab->root) {
        // Empty tab; remove it.
        for (int i = app.active; i < app.n_tabs - 1; i++)
            app.tabs[i] = app.tabs[i + 1];
        app.n_tabs--;
        if (app.active >= app.n_tabs) app.active = app.n_tabs - 1;
        if (app.active < 0) app.active = 0;
        return true;
    }

    // If the focused pane is the only leaf, close the whole tab.
    PaneNode *focused = tab->focused;
    if (pane_count_leaves(tab->root) <= 1 || !focused) {
        // Destroy the pane tree and clear the tab.
        pane_destroy(tab->root);
        tab->root = NULL;
        tab->focused = NULL;
        // Remove the tab from the array.
        for (int i = app.active; i < app.n_tabs - 1; i++)
            app.tabs[i] = app.tabs[i + 1];
        app.n_tabs--;
        if (app.active >= app.n_tabs) app.active = app.n_tabs - 1;
        if (app.active < 0) app.active = 0;
        return true;
    }

    // Close the focused pane in a multi-pane tab.
    PaneNode *new_focus = NULL;
    PaneNode *new_root = pane_close(tab->root, focused, &new_focus);
    tab->root = new_root;
    tab->focused = new_focus ? new_focus : pane_first_leaf(new_root);
    return true;
}

// Split the focused pane of the active tab.
static Session *app_split_focused(PaneKind dir, uint16_t cols, uint16_t rows,
                                   int cell_w, int cell_h,
                                   int max_scrollback, const char *cwd,
                                   TermEngine **te, int *pty_fd,
                                   pid_t *child, bool *child_exited)
{
    if (app.n_tabs <= 0) return NULL;
    Tab *tab = &app.tabs[app.active];
    if (!tab->root || !tab->focused) return NULL;

    Session *new_s = session_create(cols, rows, cell_w, cell_h, max_scrollback, cwd);
    if (!new_s) return NULL;

    PaneNode *new_root = pane_split(tab->root, tab->focused, dir, new_s, 0.5f);
    tab->root = new_root;

    // Focus follows to the newly created pane (the leaf owning new_s).
    PaneNode *leaves[FANGS_MAX_TABS > 64 ? FANGS_MAX_TABS : 64];
    int nl = 0;
    pane_collect_leaves(new_root, leaves, (int)(sizeof(leaves)/sizeof(leaves[0])), &nl);
    tab->focused = pane_first_leaf(new_root);
    for (int i = 0; i < nl; i++) {
        if (leaves[i]->kind == PANE_LEAF && leaves[i]->leaf.session == new_s) {
            tab->focused = leaves[i];
            break;
        }
    }

    return sync_active_session(te, pty_fd, child, child_exited);
}

// Destroy all tabs and their pane-trees.
static void app_destroy_all(void)
{
    for (int i = 0; i < app.n_tabs; i++) {
        if (app.tabs[i].root) {
            pane_destroy(app.tabs[i].root);
            app.tabs[i].root = NULL;
            app.tabs[i].focused = NULL;
        }
    }
    app.n_tabs = 0;
    app.active = 0;
}

// Snap a raw scale factor to the nearest 0.25 step (1.49 -> 1.50, 1.62 -> 1.50).
static float snap_quarter(float v)
{
    return (float)((int)(v / 0.25f + 0.5f)) * 0.25f;
}

// Resolve the effective content scale used to rasterize the font at native
// pixel density, so font_size stays a *logical* size across machines.
//
// Resolution order:
//   1. FANGS_SCALE env — explicit override, always wins (e.g. FANGS_SCALE=1.5).
//   2. GetWindowScaleDPI() when it reports a real scale (macOS Retina = 2.0, and
//      Wayland/X11 setups whose GLFW does surface the content scale).
//   3. Monitor physical DPI fallback: GLFW's Wayland backend reports 1.0 even
//      under fractional scaling, so derive scale ~= DPI/96 from the monitor's
//      physical size and snap to a 0.25 step. Only departs from 1.0 for clearly
//      HiDPI panels (>= ~125 DPI) so normal ~96-DPI displays are never enlarged.
static Vector2 fangs_content_scale(void)
{
    const char *env = getenv("FANGS_SCALE");
    if (env && env[0] != '\0') {
        float v = (float)atof(env);
        if (v > 0.0f)
            return (Vector2){v, v};
    }

    Vector2 dpi = GetWindowScaleDPI();
    if (dpi.y > 1.01f)
        return dpi;

    int mon = GetCurrentMonitor();
    int res_w = GetMonitorWidth(mon), res_h = GetMonitorHeight(mon);
    int mm_w  = GetMonitorPhysicalWidth(mon), mm_h = GetMonitorPhysicalHeight(mon);
    if (mm_w > 0 && mm_h > 0 && res_w > 0 && res_h > 0) {
        float avg_dpi = 0.5f * (res_w * 25.4f / mm_w + res_h * 25.4f / mm_h);
        float raw = avg_dpi / 96.0f;
        if (raw >= 1.30f) {              // ~125+ DPI: genuinely HiDPI
            float snapped = snap_quarter(raw);
            if (snapped < 1.0f) snapped = 1.0f;
            if (snapped > 3.0f) snapped = 3.0f;
            return (Vector2){snapped, snapped};
        }
    }
    return (Vector2){1.0f, 1.0f};
}

static Font load_terminal_font(int font_size, int *cell_width, int *cell_height)
{
    Vector2 dpi_scale = fangs_content_scale();
    int font_size_px = (int)(font_size * dpi_scale.y);
    if (font_size_px < 1)
        font_size_px = 1;

    Font font = LoadFontFromMemory(".ttf", font_jetbrains_mono,
                         (int)sizeof(font_jetbrains_mono), font_size_px, NULL, 0);
    if (font.texture.id == 0)
        return font;

    // The texture is rasterized at native pixel size; bilinear filtering keeps
    // fractional positioning from looking jagged without introducing blur.
    SetTextureFilter(font.texture, TEXTURE_FILTER_BILINEAR);

    Vector2 glyph_size = MeasureTextEx(font, "M", font_size_px, 0);
    int measured_width  = (int)(glyph_size.x / dpi_scale.x);
    int measured_height = (int)(glyph_size.y / dpi_scale.y);

    *cell_width = measured_width < 1 ? 1 : measured_width;
    *cell_height = measured_height < 1 ? 1 : measured_height;
    return font;
}

static void compute_terminal_grid(int term_area_w, int pad,
                                  int cell_width, int cell_height,
                                  uint16_t *cols_out, uint16_t *rows_out)
{
    int scr_h = GetScreenHeight();
    int cols = (term_area_w - 2 * pad) / cell_width;
    int rows = (scr_h - 2 * pad) / cell_height;
    if (cols < 1) cols = 1;
    if (rows < 1) rows = 1;

    *cols_out = (uint16_t)cols;
    *rows_out = (uint16_t)rows;
}

static bool export_screen_image(const char *path)
{
    if (!path || path[0] == '\0')
        return false;

    Image img = LoadImageFromScreen();
    if (!img.data)
        return false;

    bool ok = ExportImage(img, path);
    UnloadImage(img);
    return ok;
}

static bool write_phase3_smoke_report(const char *path,
                                      Layout lo,
                                      int term_area_w,
                                      uint16_t term_cols,
                                      uint16_t term_rows,
                                      bool screenshot_written)
{
    if (!path || path[0] == '\0')
        return false;

    FILE *f = fopen(path, "w");
    if (!f)
        return false;

    bool pty_visible_unfocused = ui_sidebar_allows_pty_input(
        false, false, lo.sidebar_visible, false, false, false);
    bool pty_visible_focused = ui_sidebar_allows_pty_input(
        false, false, lo.sidebar_visible, true, false, false);

    fprintf(f,
        "phase3_smoke=ok\n"
        "window_w=%d\n"
        "window_h=%d\n"
        "sidebar_visible=%d\n"
        "sidebar_focused=%d\n"
        "layout_sidebar_visible=%d\n"
        "terminal_x=%d\n"
        "terminal_y=%d\n"
        "terminal_w=%d\n"
        "terminal_h=%d\n"
        "sidebar_x=%d\n"
        "sidebar_y=%d\n"
        "sidebar_w=%d\n"
        "sidebar_h=%d\n"
        "term_area_w=%d\n"
        "term_cols=%u\n"
        "term_rows=%u\n"
        "pty_allowed_visible_unfocused=%d\n"
        "pty_allowed_visible_focused=%d\n"
        "screenshot_written=%d\n",
        GetScreenWidth(),
        GetScreenHeight(),
        ui_sidebar_visible() ? 1 : 0,
        ui_sidebar_focused() ? 1 : 0,
        lo.sidebar_visible ? 1 : 0,
        lo.terminal.x,
        lo.terminal.y,
        lo.terminal.w,
        lo.terminal.h,
        lo.sidebar.x,
        lo.sidebar.y,
        lo.sidebar.w,
        lo.sidebar.h,
        term_area_w,
        (unsigned)term_cols,
        (unsigned)term_rows,
        pty_visible_unfocused ? 1 : 0,
        pty_visible_focused ? 1 : 0,
        screenshot_written ? 1 : 0);

    return fclose(f) == 0;
}


// ---------------------------------------------------------------------------
// Input handling
// ---------------------------------------------------------------------------

// Map a raylib key constant to a GhosttyKey code.
// Returns GHOSTTY_KEY_UNIDENTIFIED for keys we don't handle.
static GhosttyKey raylib_key_to_ghostty(int rl_key)
{
    // Letters — raylib KEY_A..KEY_Z are contiguous, and so are
    // GHOSTTY_KEY_A..GHOSTTY_KEY_Z.
    if (rl_key >= KEY_A && rl_key <= KEY_Z)
        return GHOSTTY_KEY_A + (rl_key - KEY_A);

    // Digits — raylib KEY_ZERO..KEY_NINE are contiguous.
    if (rl_key >= KEY_ZERO && rl_key <= KEY_NINE)
        return GHOSTTY_KEY_DIGIT_0 + (rl_key - KEY_ZERO);

    // Function keys — raylib KEY_F1..KEY_F12 are contiguous.
    if (rl_key >= KEY_F1 && rl_key <= KEY_F12)
        return GHOSTTY_KEY_F1 + (rl_key - KEY_F1);

    switch (rl_key) {
    case KEY_SPACE:       return GHOSTTY_KEY_SPACE;
    case KEY_ENTER:       return GHOSTTY_KEY_ENTER;
    case KEY_TAB:         return GHOSTTY_KEY_TAB;
    case KEY_BACKSPACE:   return GHOSTTY_KEY_BACKSPACE;
    case KEY_DELETE:      return GHOSTTY_KEY_DELETE;
    case KEY_ESCAPE:      return GHOSTTY_KEY_ESCAPE;
    case KEY_UP:          return GHOSTTY_KEY_ARROW_UP;
    case KEY_DOWN:        return GHOSTTY_KEY_ARROW_DOWN;
    case KEY_LEFT:        return GHOSTTY_KEY_ARROW_LEFT;
    case KEY_RIGHT:       return GHOSTTY_KEY_ARROW_RIGHT;
    case KEY_HOME:        return GHOSTTY_KEY_HOME;
    case KEY_END:         return GHOSTTY_KEY_END;
    case KEY_PAGE_UP:     return GHOSTTY_KEY_PAGE_UP;
    case KEY_PAGE_DOWN:   return GHOSTTY_KEY_PAGE_DOWN;
    case KEY_INSERT:      return GHOSTTY_KEY_INSERT;
    case KEY_MINUS:       return GHOSTTY_KEY_MINUS;
    case KEY_EQUAL:       return GHOSTTY_KEY_EQUAL;
    case KEY_LEFT_BRACKET:  return GHOSTTY_KEY_BRACKET_LEFT;
    case KEY_RIGHT_BRACKET: return GHOSTTY_KEY_BRACKET_RIGHT;
    case KEY_BACKSLASH:   return GHOSTTY_KEY_BACKSLASH;
    case KEY_SEMICOLON:   return GHOSTTY_KEY_SEMICOLON;
    case KEY_APOSTROPHE:  return GHOSTTY_KEY_QUOTE;
    case KEY_COMMA:       return GHOSTTY_KEY_COMMA;
    case KEY_PERIOD:      return GHOSTTY_KEY_PERIOD;
    case KEY_SLASH:       return GHOSTTY_KEY_SLASH;
    case KEY_GRAVE:       return GHOSTTY_KEY_BACKQUOTE;
    default:              return GHOSTTY_KEY_UNIDENTIFIED;
    }
}

// Build a GhosttyMods bitmask from the current raylib modifier key state.
static GhosttyMods get_ghostty_mods(void)
{
    GhosttyMods mods = 0;
    if (IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT))
        mods |= GHOSTTY_MODS_SHIFT;
    if (IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL))
        mods |= GHOSTTY_MODS_CTRL;
    if (IsKeyDown(KEY_LEFT_ALT) || IsKeyDown(KEY_RIGHT_ALT))
        mods |= GHOSTTY_MODS_ALT;
    if (IsKeyDown(KEY_LEFT_SUPER) || IsKeyDown(KEY_RIGHT_SUPER))
        mods |= GHOSTTY_MODS_SUPER;
    return mods;
}

// Return the unshifted Unicode codepoint for a raylib key, i.e. the
// character the key produces with no modifiers on a US layout.  The
// Kitty keyboard protocol requires this to identify keys.  Returns 0
// for keys that don't have a natural codepoint (arrows, F-keys, etc.).
static uint32_t raylib_key_unshifted_codepoint(int rl_key)
{
    if (rl_key >= KEY_A && rl_key <= KEY_Z)
        return 'a' + (uint32_t)(rl_key - KEY_A);
    if (rl_key >= KEY_ZERO && rl_key <= KEY_NINE)
        return '0' + (uint32_t)(rl_key - KEY_ZERO);

    switch (rl_key) {
    case KEY_SPACE:          return ' ';
    case KEY_MINUS:          return '-';
    case KEY_EQUAL:          return '=';
    case KEY_LEFT_BRACKET:   return '[';
    case KEY_RIGHT_BRACKET:  return ']';
    case KEY_BACKSLASH:      return '\\';
    case KEY_SEMICOLON:      return ';';
    case KEY_APOSTROPHE:     return '\'';
    case KEY_COMMA:          return ',';
    case KEY_PERIOD:         return '.';
    case KEY_SLASH:          return '/';
    case KEY_GRAVE:          return '`';
    default:                 return 0;
    }
}

// Encode a single Unicode codepoint into a UTF-8 byte buffer.
// Returns the number of bytes written (1–4).
// Invalid codepoints (> U+10FFFF) are replaced with U+FFFD.
static int utf8_encode(uint32_t cp, char out[4])
{
    // Unicode defines the maximum valid codepoint as U+10FFFF.
    // Codepoints above this value are invalid and should be replaced
    // with the Unicode replacement character U+FFFD.
    const uint32_t MAX_UNICODE = 0x10FFFF;
    const uint32_t REPLACEMENT_CHAR = 0xFFFD;

    if (cp > MAX_UNICODE) {
        cp = REPLACEMENT_CHAR;
    }

    if (cp < 0x80) {
        out[0] = (char)cp;
        return 1;
    } else if (cp < 0x800) {
        out[0] = (char)(0xC0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    } else if (cp < 0x10000) {
        out[0] = (char)(0xE0 | (cp >> 12));
        out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    } else {
        out[0] = (char)(0xF0 | (cp >> 18));
        out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
        out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[3] = (char)(0x80 | (cp & 0x3F));
        return 4;
    }
}

// Map a raylib mouse button to a GhosttyMouseButton.
static GhosttyMouseButton raylib_mouse_to_ghostty(int rl_button)
{
    switch (rl_button) {
    case MOUSE_BUTTON_LEFT:    return GHOSTTY_MOUSE_BUTTON_LEFT;
    case MOUSE_BUTTON_RIGHT:   return GHOSTTY_MOUSE_BUTTON_RIGHT;
    case MOUSE_BUTTON_MIDDLE:  return GHOSTTY_MOUSE_BUTTON_MIDDLE;
    case MOUSE_BUTTON_SIDE:    return GHOSTTY_MOUSE_BUTTON_FOUR;
    case MOUSE_BUTTON_EXTRA:   return GHOSTTY_MOUSE_BUTTON_FIVE;
    case MOUSE_BUTTON_FORWARD: return GHOSTTY_MOUSE_BUTTON_SIX;
    case MOUSE_BUTTON_BACK:    return GHOSTTY_MOUSE_BUTTON_SEVEN;
    default:                   return GHOSTTY_MOUSE_BUTTON_UNKNOWN;
    }
}

// Encode a mouse event and write the resulting escape sequence to the pty.
// If the encoder produces no output (e.g. tracking is disabled), this is
// a no-op.
static void mouse_encode_and_write(int pty_fd, GhosttyMouseEncoder encoder,
                                   GhosttyMouseEvent event)
{
    char buf[128];
    size_t written = 0;
    GhosttyResult res = ghostty_mouse_encoder_encode(
        encoder, event, buf, sizeof(buf), &written);
    if (res == GHOSTTY_SUCCESS && written > 0)
        pty_write(pty_fd, buf, written);
}

// Poll raylib for mouse events and use the libghostty mouse encoder
// to produce the correct VT escape sequences, which are then written
// to the pty.  The encoder handles tracking mode (X10, normal, button,
// any-event) and output format (X10, UTF8, SGR, URxvt, SGR-Pixels)
// based on what the terminal application has requested.
static void handle_mouse(int pty_fd, GhosttyMouseEncoder encoder,
                         GhosttyMouseEvent event, GhosttyTerminal terminal,
                         int cell_width, int cell_height, int pad,
                         int term_area_w)
{
    if (GetMouseX() >= term_area_w)
        return;

    // Sync encoder tracking mode and format from terminal state so
    // mode changes (e.g. applications enabling SGR mouse reporting)
    // are honoured automatically.
    ghostty_mouse_encoder_setopt_from_terminal(encoder, terminal);

    // Provide the encoder with the current terminal geometry so it
    // can convert pixel positions to cell coordinates.
    int scr_h = GetScreenHeight();
    GhosttyMouseEncoderSize enc_size = {
        .size          = sizeof(GhosttyMouseEncoderSize),
        .screen_width  = (uint32_t)term_area_w,
        .screen_height = (uint32_t)scr_h,
        .cell_width    = (uint32_t)cell_width,
        .cell_height   = (uint32_t)cell_height,
        .padding_top   = (uint32_t)pad,
        .padding_bottom = (uint32_t)pad,
        .padding_left  = (uint32_t)pad,
        .padding_right = (uint32_t)pad,
    };
    ghostty_mouse_encoder_setopt(encoder,
        GHOSTTY_MOUSE_ENCODER_OPT_SIZE, &enc_size);

    // Track whether any button is currently held — the encoder uses
    // this to distinguish drags from plain motion.
    bool any_pressed = IsMouseButtonDown(MOUSE_BUTTON_LEFT)
                    || IsMouseButtonDown(MOUSE_BUTTON_RIGHT)
                    || IsMouseButtonDown(MOUSE_BUTTON_MIDDLE);
    ghostty_mouse_encoder_setopt(encoder,
        GHOSTTY_MOUSE_ENCODER_OPT_ANY_BUTTON_PRESSED, &any_pressed);

    // Enable motion deduplication so the encoder suppresses redundant
    // motion events within the same cell.
    bool track_cell = true;
    ghostty_mouse_encoder_setopt(encoder,
        GHOSTTY_MOUSE_ENCODER_OPT_TRACK_LAST_CELL, &track_cell);

    GhosttyMods mods = get_ghostty_mods();
    Vector2 pos = GetMousePosition();
    ghostty_mouse_event_set_mods(event, mods);
    ghostty_mouse_event_set_position(event,
        (GhosttyMousePosition){ .x = pos.x, .y = pos.y });

    // Check each mouse button for press/release events.
    static const int buttons[] = {
        MOUSE_BUTTON_LEFT, MOUSE_BUTTON_RIGHT, MOUSE_BUTTON_MIDDLE,
        MOUSE_BUTTON_SIDE, MOUSE_BUTTON_EXTRA, MOUSE_BUTTON_FORWARD,
        MOUSE_BUTTON_BACK,
    };
    for (size_t i = 0; i < sizeof(buttons) / sizeof(buttons[0]); i++) {
        int rl_btn = buttons[i];
        GhosttyMouseButton gbtn = raylib_mouse_to_ghostty(rl_btn);
        if (gbtn == GHOSTTY_MOUSE_BUTTON_UNKNOWN)
            continue;

        if (IsMouseButtonPressed(rl_btn)) {
            ghostty_mouse_event_set_action(event, GHOSTTY_MOUSE_ACTION_PRESS);
            ghostty_mouse_event_set_button(event, gbtn);
            mouse_encode_and_write(pty_fd, encoder, event);
        } else if (IsMouseButtonReleased(rl_btn)) {
            ghostty_mouse_event_set_action(event, GHOSTTY_MOUSE_ACTION_RELEASE);
            ghostty_mouse_event_set_button(event, gbtn);
            mouse_encode_and_write(pty_fd, encoder, event);
        }
    }

    // Mouse motion — send a motion event with whatever button is held
    // (or no button for pure motion in any-event tracking mode).
    Vector2 delta = GetMouseDelta();
    if (delta.x != 0.0f || delta.y != 0.0f) {
        ghostty_mouse_event_set_action(event, GHOSTTY_MOUSE_ACTION_MOTION);
        if (IsMouseButtonDown(MOUSE_BUTTON_LEFT))
            ghostty_mouse_event_set_button(event, GHOSTTY_MOUSE_BUTTON_LEFT);
        else if (IsMouseButtonDown(MOUSE_BUTTON_RIGHT))
            ghostty_mouse_event_set_button(event, GHOSTTY_MOUSE_BUTTON_RIGHT);
        else if (IsMouseButtonDown(MOUSE_BUTTON_MIDDLE))
            ghostty_mouse_event_set_button(event, GHOSTTY_MOUSE_BUTTON_MIDDLE);
        else
            ghostty_mouse_event_clear_button(event);
        mouse_encode_and_write(pty_fd, encoder, event);
    }

    // Scroll wheel handling.  When a mouse tracking mode is active the
    // wheel events are forwarded to the application as button 4/5
    // press+release pairs.  Otherwise we scroll the viewport through
    // the scrollback buffer so the user can review history.
    float wheel = GetMouseWheelMove();
    if (wheel != 0.0f) {
        // Check whether any mouse tracking mode is enabled.  If so,
        // the application wants to handle scroll events itself.
        bool mouse_tracking = false;
        ghostty_terminal_get(terminal, GHOSTTY_TERMINAL_DATA_MOUSE_TRACKING, &mouse_tracking);

        if (mouse_tracking) {
            // Forward to the application via the mouse encoder.
            GhosttyMouseButton scroll_btn = (wheel > 0.0f)
                ? GHOSTTY_MOUSE_BUTTON_FOUR
                : GHOSTTY_MOUSE_BUTTON_FIVE;
            ghostty_mouse_event_set_button(event, scroll_btn);
            ghostty_mouse_event_set_action(event, GHOSTTY_MOUSE_ACTION_PRESS);
            mouse_encode_and_write(pty_fd, encoder, event);
            ghostty_mouse_event_set_action(event, GHOSTTY_MOUSE_ACTION_RELEASE);
            mouse_encode_and_write(pty_fd, encoder, event);
        } else {
            // Scroll the viewport through scrollback.  Scroll 3 rows
            // per wheel tick for a comfortable pace.  Delta is negative
            // to scroll up (into history), positive to scroll down.
            int delta = (wheel > 0.0f) ? -3 : 3;
            GhosttyTerminalScrollViewport sv = {
                .tag = GHOSTTY_SCROLL_VIEWPORT_DELTA,
                .value = { .delta = delta },
            };
            ghostty_terminal_scroll_viewport(terminal, sv);
        }
    }
}

// Poll raylib for keyboard events and use the libghostty key encoder
// to produce the correct VT escape sequences, which are then written
// to the pty.  The encoder respects terminal modes (cursor key
// application mode, Kitty keyboard protocol, etc.) so we don't need
// to maintain our own escape-sequence tables.
static void handle_input(int pty_fd, GhosttyKeyEncoder encoder,
                         GhosttyKeyEvent event, GhosttyTerminal terminal)
{
    // Sync encoder options from the terminal so mode changes (e.g.
    // application cursor keys, Kitty keyboard protocol) are honoured.
    ghostty_key_encoder_setopt_from_terminal(encoder, terminal);

    // Drain printable characters from raylib's input queue.  We collect
    // them into a single UTF-8 buffer so the encoder can attach text
    // to the key event.
    char char_utf8[64];
    int char_utf8_len = 0;
    int ch;
    while ((ch = GetCharPressed()) != 0) {
        char u8[4];
        int n = utf8_encode(ch, u8);
        if (char_utf8_len + n < (int)sizeof(char_utf8)) {
            memcpy(&char_utf8[char_utf8_len], u8, n);
            char_utf8_len += n;
        }
    }

    // All raylib keys we want to check for press/repeat events.
    // Letters and digits are handled via ranges; everything else is
    // enumerated explicitly.
    static const int special_keys[] = {
        KEY_SPACE, KEY_ENTER, KEY_TAB, KEY_BACKSPACE, KEY_DELETE,
        KEY_ESCAPE, KEY_UP, KEY_DOWN, KEY_LEFT, KEY_RIGHT,
        KEY_HOME, KEY_END, KEY_PAGE_UP, KEY_PAGE_DOWN, KEY_INSERT,
        KEY_MINUS, KEY_EQUAL, KEY_LEFT_BRACKET, KEY_RIGHT_BRACKET,
        KEY_BACKSLASH, KEY_SEMICOLON, KEY_APOSTROPHE, KEY_COMMA,
        KEY_PERIOD, KEY_SLASH, KEY_GRAVE,
        KEY_F1, KEY_F2, KEY_F3, KEY_F4, KEY_F5, KEY_F6,
        KEY_F7, KEY_F8, KEY_F9, KEY_F10, KEY_F11, KEY_F12,
    };

    // Build the set of raylib keys to scan: letters + digits + specials.
    int keys_to_check[26 + 10 + sizeof(special_keys) / sizeof(special_keys[0])];
    int num_keys = 0;
    for (int k = KEY_A; k <= KEY_Z; k++)
        keys_to_check[num_keys++] = k;
    for (int k = KEY_ZERO; k <= KEY_NINE; k++)
        keys_to_check[num_keys++] = k;
    for (size_t i = 0; i < sizeof(special_keys) / sizeof(special_keys[0]); i++)
        keys_to_check[num_keys++] = special_keys[i];

    GhosttyMods mods = get_ghostty_mods();

    for (int i = 0; i < num_keys; i++) {
        int rl_key = keys_to_check[i];
        bool pressed  = IsKeyPressed(rl_key);
        bool repeated = IsKeyPressedRepeat(rl_key);
        bool released = IsKeyReleased(rl_key);
        if (!pressed && !repeated && !released)
            continue;

        GhosttyKey gkey = raylib_key_to_ghostty(rl_key);
        if (gkey == GHOSTTY_KEY_UNIDENTIFIED)
            continue;

        GhosttyKeyAction action = released  ? GHOSTTY_KEY_ACTION_RELEASE
                                : pressed   ? GHOSTTY_KEY_ACTION_PRESS
                                            : GHOSTTY_KEY_ACTION_REPEAT;

        ghostty_key_event_set_key(event, gkey);
        ghostty_key_event_set_action(event, action);
        ghostty_key_event_set_mods(event, mods);

        // The unshifted codepoint is the character the key produces
        // with no modifiers.  The Kitty protocol needs it to identify
        // keys independent of the current shift state.
        uint32_t ucp = raylib_key_unshifted_codepoint(rl_key);
        ghostty_key_event_set_unshifted_codepoint(event, ucp);

        // Consumed mods are modifiers the platform's text input
        // already accounted for when producing the UTF-8 text.
        // For printable keys, shift is consumed (it turns 'a' → 'A').
        // For non-printable keys nothing is consumed.
        GhosttyMods consumed = 0;
        if (ucp != 0 && (mods & GHOSTTY_MODS_SHIFT))
            consumed |= GHOSTTY_MODS_SHIFT;
        ghostty_key_event_set_consumed_mods(event, consumed);

        // Attach any UTF-8 text that raylib produced for this frame.
        // For unmodified printable keys this is the character itself;
        // for special keys or ctrl combos there's typically no text.
        // Release events never carry text.
        if (char_utf8_len > 0 && !released) {
            ghostty_key_event_set_utf8(event, char_utf8, (size_t)char_utf8_len);
            // Only attach the text to the first key event this frame
            // to avoid duplicating it.
            char_utf8_len = 0;
        } else {
            ghostty_key_event_set_utf8(event, NULL, 0);
        }

        char buf[128];
        size_t written = 0;
        GhosttyResult res = ghostty_key_encoder_encode(
            encoder, event, buf, sizeof(buf), &written);
        if (res == GHOSTTY_SUCCESS && written > 0) {
            pty_write(pty_fd, buf, written);
            // Text was consumed by the encoder — clear it so the
            // fallback below doesn't double-send.
            char_utf8_len = 0;
        }
    }

    // Fallback: on some platforms (e.g. VMs) the character event arrives
    // a frame after the key-press event.  If we collected UTF-8 text but
    // no key event consumed it, write it directly to the PTY so input
    // isn't silently dropped.
    if (char_utf8_len > 0)
        pty_write(pty_fd, char_utf8, char_utf8_len);
}

// Handle scrollbar drag-to-scroll.  When the user clicks in the
// scrollbar region we begin tracking; while held we map the mouse Y
// position directly to an absolute scroll offset so the thumb follows
// the cursor exactly.
//
// Returns true while a drag is in progress so the caller can skip
// normal mouse handling if desired.
static bool handle_scrollbar(GhosttyTerminal terminal,
                             GhosttyRenderState render_state,
                             bool *dragging,
                             int term_area_w)
{
    // Query scrollbar geometry from the terminal.
    GhosttyTerminalScrollbar scrollbar = {0};
    if (ghostty_terminal_get(terminal, GHOSTTY_TERMINAL_DATA_SCROLLBAR,
                             &scrollbar) != GHOSTTY_SUCCESS)
        return false;

    // Nothing to drag when the viewport covers all content.
    if (scrollbar.total <= scrollbar.len) {
        *dragging = false;
        return false;
    }

    int scr_h = GetScreenHeight();
    const int bar_width = 6;
    const int bar_margin = 2;
    int bar_left = term_area_w - bar_width - bar_margin;
    // Use a wider hit region for easier grabbing.
    int hit_left = bar_left - 8;
    Vector2 mpos = GetMousePosition();

    // Start a drag when the user clicks inside the hit region.
    if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT)
        && mpos.x >= hit_left && mpos.x <= term_area_w) {
        *dragging = true;
    }

    if (*dragging && IsMouseButtonDown(MOUSE_BUTTON_LEFT)) {
        // Map mouse Y directly to an absolute scroll offset.
        // Y=0 → top of scrollback (offset 0), Y=scr_h → bottom
        // (offset = total - len).
        uint64_t scrollable = scrollbar.total - scrollbar.len;
        double frac = (double)mpos.y / (double)scr_h;
        if (frac < 0.0) frac = 0.0;
        if (frac > 1.0) frac = 1.0;
        int64_t target = (int64_t)(frac * (double)scrollable);

        intptr_t delta = (intptr_t)(target - (int64_t)scrollbar.offset);
        if (delta != 0) {
            GhosttyTerminalScrollViewport sv = {
                .tag = GHOSTTY_SCROLL_VIEWPORT_DELTA,
                .value = { .delta = delta },
            };
            ghostty_terminal_scroll_viewport(terminal, sv);
            ghostty_render_state_update(render_state, terminal);
        }
    }

    if (IsMouseButtonReleased(MOUSE_BUTTON_LEFT))
        *dragging = false;

    return *dragging;
}

// Deferred texture cleanup — textures uploaded during a frame can't be
// freed until after EndDrawing() flushes the draw commands to the GPU.
#define MAX_DEFERRED_TEXTURES 256
static Texture2D deferred_textures[MAX_DEFERRED_TEXTURES];
static int deferred_texture_count = 0;

static void defer_unload_texture(Texture2D tex)
{
    if (deferred_texture_count < MAX_DEFERRED_TEXTURES)
        deferred_textures[deferred_texture_count++] = tex;
    else
        UnloadTexture(tex); // overflow fallback — may glitch but won't leak
}

static void flush_deferred_textures(void)
{
    for (int i = 0; i < deferred_texture_count; i++)
        UnloadTexture(deferred_textures[i]);
    deferred_texture_count = 0;
}

// Draw all Kitty graphics placements for a given z-layer.
//
// The layer filter is applied by the iterator itself via
// ghostty_kitty_graphics_placement_iterator_set(), so we only see
// placements matching the requested layer.
//
// WARNING: This is deliberately simple but very inefficient.  Every
// visible image is re-uploaded to the GPU every frame and destroyed
// right after.  A real implementation should cache Texture2D objects
// keyed by image ID and only re-upload when the image is re-transmitted
// or evicted from the terminal's storage.
static void render_kitty_images(GhosttyTerminal terminal,
                                GhosttyKittyGraphics graphics,
                                GhosttyKittyGraphicsPlacementIterator placement_iter,
                                int cell_width, int cell_height, int pad,
                                GhosttyKittyPlacementLayer layer)
{
    // Configure the layer filter on the iterator so
    // placement_next() only yields matching placements.
    ghostty_kitty_graphics_placement_iterator_set(placement_iter,
        GHOSTTY_KITTY_GRAPHICS_PLACEMENT_ITERATOR_OPTION_LAYER, &layer);

    // Re-populate the iterator for this layer scan.
    if (ghostty_kitty_graphics_get(graphics,
            GHOSTTY_KITTY_GRAPHICS_DATA_PLACEMENT_ITERATOR,
            &placement_iter) != GHOSTTY_SUCCESS)
        return;

    while (ghostty_kitty_graphics_placement_next(placement_iter)) {
        // Look up the image for this placement.
        uint32_t image_id = 0;
        ghostty_kitty_graphics_placement_get(placement_iter,
            GHOSTTY_KITTY_GRAPHICS_PLACEMENT_DATA_IMAGE_ID, &image_id);

        GhosttyKittyGraphicsImage image_handle =
            ghostty_kitty_graphics_image(graphics, image_id);
        if (!image_handle)
            continue;

        // Get viewport-relative position.  Returns NO_VALUE when the
        // placement is entirely off-screen or is a virtual (unicode
        // placeholder) placement, so both cases are handled in one call.
        int32_t vp_col = 0, vp_row = 0;
        if (ghostty_kitty_graphics_placement_viewport_pos(
                placement_iter, image_handle, terminal,
                &vp_col, &vp_row) != GHOSTTY_SUCCESS)
            continue;

        // Read image dimensions and pixel data.  We only handle RGBA
        // (the PNG decoder we registered converts everything to RGBA).
        uint32_t img_w = 0, img_h = 0;
        ghostty_kitty_graphics_image_get(image_handle,
            GHOSTTY_KITTY_IMAGE_DATA_WIDTH, &img_w);
        ghostty_kitty_graphics_image_get(image_handle,
            GHOSTTY_KITTY_IMAGE_DATA_HEIGHT, &img_h);
        if (img_w == 0 || img_h == 0)
            continue;

        GhosttyKittyImageFormat fmt = GHOSTTY_KITTY_IMAGE_FORMAT_RGBA;
        ghostty_kitty_graphics_image_get(image_handle,
            GHOSTTY_KITTY_IMAGE_DATA_FORMAT, &fmt);
        if (fmt != GHOSTTY_KITTY_IMAGE_FORMAT_RGBA)
            continue;

        const uint8_t *data_ptr = NULL;
        size_t data_len = 0;
        ghostty_kitty_graphics_image_get(image_handle,
            GHOSTTY_KITTY_IMAGE_DATA_DATA_PTR, &data_ptr);
        ghostty_kitty_graphics_image_get(image_handle,
            GHOSTTY_KITTY_IMAGE_DATA_DATA_LEN, &data_len);
        if (!data_ptr || data_len < (size_t)img_w * img_h * 4)
            continue;

        // Compute grid cell count for rendered size.
        uint32_t grid_cols = 0, grid_rows = 0;
        if (ghostty_kitty_graphics_placement_grid_size(
                placement_iter, image_handle, terminal,
                &grid_cols, &grid_rows) != GHOSTTY_SUCCESS)
            continue;
        if (grid_cols == 0 || grid_rows == 0)
            continue;

        uint32_t dest_w = grid_cols * (uint32_t)cell_width;
        uint32_t dest_h = grid_rows * (uint32_t)cell_height;

        // Get the resolved source rectangle (handles "0 = full image"
        // semantics and clamps to image bounds).
        uint32_t src_x = 0, src_y = 0, src_w = 0, src_h = 0;
        if (ghostty_kitty_graphics_placement_source_rect(
                placement_iter, image_handle,
                &src_x, &src_y, &src_w, &src_h) != GHOSTTY_SUCCESS)
            continue;

        // Read the sub-cell pixel offsets.
        uint32_t x_offset = 0, y_offset = 0;
        ghostty_kitty_graphics_placement_get(placement_iter,
            GHOSTTY_KITTY_GRAPHICS_PLACEMENT_DATA_X_OFFSET, &x_offset);
        ghostty_kitty_graphics_placement_get(placement_iter,
            GHOSTTY_KITTY_GRAPHICS_PLACEMENT_DATA_Y_OFFSET, &y_offset);

        // Upload the RGBA data to a temporary texture, draw, and free.
        Image img = {
            .data    = (void *)(uintptr_t)data_ptr,
            .width   = (int)img_w,
            .height  = (int)img_h,
            .mipmaps = 1,
            .format  = PIXELFORMAT_UNCOMPRESSED_R8G8B8A8,
        };
        Texture2D tex = LoadTextureFromImage(img);
        SetTextureFilter(tex, TEXTURE_FILTER_BILINEAR);

        int dest_x = pad + (int)vp_col * cell_width  + (int)x_offset;
        int dest_y = pad + (int)vp_row * cell_height + (int)y_offset;

        Rectangle src_rect = {
            (float)src_x, (float)src_y,
            (float)src_w, (float)src_h
        };
        Rectangle dst_rect = {
            (float)dest_x, (float)dest_y,
            (float)dest_w, (float)dest_h
        };
        DrawTexturePro(tex, src_rect, dst_rect,
                       (Vector2){0, 0}, 0.0f, WHITE);

        defer_unload_texture(tex);
    }
}

// ---------------------------------------------------------------------------
// Rendering
// ---------------------------------------------------------------------------

// Render the current terminal screen using the RenderState API.
//
// For each row/cell we read the grapheme codepoints and the cell's style,
// resolve foreground/background colors via the palette, and draw each
// character individually with DrawTextEx.  This supports per-cell colors
// from SGR sequences (bold, 256-color, 24-bit RGB, etc.).
//
// cell_width and cell_height are the measured dimensions of a single
// monospace glyph at the current font size, in screen (logical) pixels.
// font_size is the logical font size (before DPI scaling).
// pad is the pixel margin between the window edges and the terminal grid.
//
// If scrollbar is non-NULL, a scrollbar indicator is drawn on the right
// edge of the terminal area.
// --- Host-side text selection + clipboard ----------------------------------
// The engine's selection API is snapshot-based; for full control of the
// highlight + copied text we track a linear viewport selection in the host and
// capture the selected cells' text during the render pass.
typedef struct { bool active, dragging; int sr, sc, er, ec; } Selection;
static Selection g_sel = {0};
static char g_sel_text[1 << 16];
static int  g_sel_len = 0;
static int  g_sel_row = -1;   // last row appended during capture

static void sel_ordered(const Selection *s, int *r0, int *c0, int *r1, int *c1)
{
    if (s->sr < s->er || (s->sr == s->er && s->sc <= s->ec)) {
        *r0 = s->sr; *c0 = s->sc; *r1 = s->er; *c1 = s->ec;
    } else {
        *r0 = s->er; *c0 = s->ec; *r1 = s->sr; *c1 = s->sc;
    }
}

static bool sel_contains(int row, int col)
{
    if (!g_sel.active) return false;
    int r0, c0, r1, c1; sel_ordered(&g_sel, &r0, &c0, &r1, &c1);
    if (row < r0 || row > r1) return false;
    if (r0 == r1) return col >= c0 && col <= c1;
    if (row == r0) return col >= c0;
    if (row == r1) return col <= c1;
    return true;
}

// Append a selected cell's text during the render pass; '\n' on row change.
static void sel_capture(int row, const char *s, int n)
{
    if (!g_sel.active) return;
    if (g_sel_row != -1 && row != g_sel_row && g_sel_len < (int)sizeof(g_sel_text) - 1)
        g_sel_text[g_sel_len++] = '\n';
    g_sel_row = row;
    if (g_sel_len + n < (int)sizeof(g_sel_text) - 1) {
        memcpy(g_sel_text + g_sel_len, s, (size_t)n);
        g_sel_len += n;
    }
    g_sel_text[g_sel_len] = '\0';
}

// Copy the captured selection to the clipboard, trimming trailing spaces/line.
static void sel_copy_to_clipboard(void)
{
    if (!g_sel.active || g_sel_len == 0) return;
    static char out[sizeof(g_sel_text)];
    int n = 0, line_start = 0;
    for (int i = 0; i <= g_sel_len; i++) {
        char c = (i < g_sel_len) ? g_sel_text[i] : '\n';
        if (c == '\n') {
            while (n > line_start && out[n - 1] == ' ') n--;
            if (i < g_sel_len) out[n++] = '\n';
            line_start = n;
        } else {
            out[n++] = c;
        }
    }
    out[n] = '\0';
    if (n > 0) SetClipboardText(out);
}

// Paste clipboard text into the pty, bracketed-paste-encoded when the app
// enabled DECSET 2004 (so newlines don't auto-execute in a supporting shell).
static void do_paste(int pty_fd, GhosttyTerminal terminal)
{
    const char *clip = GetClipboardText();
    if (!clip || !clip[0]) return;
    size_t len = strlen(clip);

    bool bracketed = false;
    ghostty_terminal_mode_get(terminal, GHOSTTY_MODE_BRACKETED_PASTE, &bracketed);

    size_t need = 0;
    char *data = malloc(len + 1);
    if (!data) return;
    memcpy(data, clip, len + 1);
    ghostty_paste_encode(data, len, bracketed, NULL, 0, &need);   // query size

    char *buf = malloc(need > 0 ? need : len + 1);
    if (buf) {
        size_t written = 0;
        memcpy(data, clip, len + 1);   // refresh (encode may modify data)
        if (ghostty_paste_encode(data, len, bracketed, buf, need, &written) == GHOSTTY_SUCCESS)
            pty_write(pty_fd, buf, written);
        free(buf);
    }
    free(data);
}

// --- Clickable URLs --------------------------------------------------------
// The viewport text grid is captured each render so a Ctrl/Cmd+click can be
// hit-tested against URLs without re-walking the engine state.
#define UI_MAX_ROWS 300
#define UI_MAX_COLS 500
static char g_row_text[UI_MAX_ROWS][UI_MAX_COLS * 4 + 4];
static int  g_row_off[UI_MAX_ROWS][UI_MAX_COLS + 1];   // byte offset per column
static int  g_row_len[UI_MAX_ROWS];
static int  g_row_cols[UI_MAX_ROWS];
static int  g_rows_captured = 0;

static void row_capture(int row, int col, const char *s, int n)
{
    if (row < 0 || row >= UI_MAX_ROWS || col < 0 || col >= UI_MAX_COLS) return;
    if (col == 0) { g_row_len[row] = 0; g_row_cols[row] = 0; }
    g_row_off[row][col] = g_row_len[row];
    if (g_row_len[row] + n < UI_MAX_COLS * 4) {
        memcpy(&g_row_text[row][g_row_len[row]], s, (size_t)n);
        g_row_len[row] += n;
    }
    g_row_text[row][g_row_len[row]] = '\0';
    g_row_cols[row] = col + 1;
    if (row + 1 > g_rows_captured) g_rows_captured = row + 1;
}

static bool url_is_char(unsigned char c)
{
    return (c >= '!' && c <= '~') && c != '"' && c != '<' && c != '>'
        && c != '`' && c != '{' && c != '}' && c != '\\' && c != '|' && c != '^';
}

static bool url_starts(const char *s, int n)
{
    return (n >= 7 && strncmp(s, "http://", 7) == 0)
        || (n >= 8 && strncmp(s, "https://", 8) == 0)
        || (n >= 7 && strncmp(s, "file://", 7) == 0)
        || (n >= 6 && strncmp(s, "ftp://", 6) == 0);
}

// If (row,col) sits on a URL in the captured grid, copy it to out and return true.
static bool url_at(int row, int col, char *out, int out_size)
{
    if (row < 0 || row >= g_rows_captured || col < 0 || col >= g_row_cols[row])
        return false;
    const char *line = g_row_text[row];
    int len = g_row_len[row];
    int click = g_row_off[row][col];
    for (int i = 0; i < len; i++) {
        if (!url_starts(line + i, len - i)) continue;
        int j = i;
        while (j < len && url_is_char((unsigned char)line[j])) j++;
        while (j > i && strchr(".,;:!?)]}'", line[j - 1])) j--;   // trim trailing punctuation
        if (click >= i && click < j) {
            int m = j - i;
            if (m >= out_size) m = out_size - 1;
            memcpy(out, line + i, (size_t)m);
            out[m] = '\0';
            return true;
        }
        i = j;
    }
    return false;
}

// Open a URL with the OS handler, double-forked + setsid so it detaches cleanly
// (no zombie, no controlling terminal). No shell, so the URL isn't interpreted.
static void open_url(const char *url)
{
#if defined(__APPLE__)
    const char *opener = "open";
#else
    const char *opener = "xdg-open";
#endif
    pid_t pid = fork();
    if (pid == 0) {
        pid_t pid2 = fork();
        if (pid2 == 0) {
            setsid();
            execlp(opener, opener, url, (char *)NULL);
            _exit(127);
        }
        _exit(0);
    }
    if (pid > 0)
        waitpid(pid, NULL, 0);
}

// --- Find (Ctrl+F): highlight matches in the visible scrollback -------------
static bool g_search_active = false;
static char g_search_query[128] = "";

static char lc(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; }

static int ci_find(const char *hay, int haylen, const char *needle, int needlen, int from)
{
    if (needlen == 0) return -1;
    for (int i = from; i + needlen <= haylen; i++) {
        int k = 0;
        while (k < needlen && lc(hay[i + k]) == lc(needle[k])) k++;
        if (k == needlen) return i;
    }
    return -1;
}

// Map a byte offset within a captured row back to its column.
static int col_of_byte(int row, int off)
{
    for (int c = 0; c < g_row_cols[row]; c++) {
        int start = g_row_off[row][c];
        int end = (c + 1 < g_row_cols[row]) ? g_row_off[row][c + 1] : g_row_len[row];
        if (off >= start && off < end) return c;
    }
    return g_row_cols[row] > 0 ? g_row_cols[row] - 1 : 0;
}

// Highlight every visible occurrence of the query; returns the match count.
static int draw_search_highlights(int pad, int cell_width, int cell_height)
{
    int qlen = (int)strlen(g_search_query);
    if (qlen == 0) return 0;
    int total = 0;
    for (int r = 0; r < g_rows_captured; r++) {
        const char *line = g_row_text[r];
        int len = g_row_len[r];
        int from = 0, m;
        while ((m = ci_find(line, len, g_search_query, qlen, from)) >= 0) {
            int c0 = col_of_byte(r, m);
            int c1 = col_of_byte(r, m + qlen - 1);
            for (int c = c0; c <= c1; c++)
                DrawRectangle(pad + c * cell_width, pad + r * cell_height,
                              cell_width, cell_height, UI2RAY(g_ui_theme.search_hit));
            total++;
            from = m + qlen;
        }
    }
    return total;
}

static void search_input(void)
{
    int ch;
    while ((ch = GetCharPressed()) != 0) {
        int l = (int)strlen(g_search_query);
        if (ch >= 32 && ch < 127 && l < (int)sizeof(g_search_query) - 1) {
            g_search_query[l] = (char)ch;
            g_search_query[l + 1] = '\0';
        }
    }
    if (IsKeyPressed(KEY_BACKSPACE)) {
        int l = (int)strlen(g_search_query);
        if (l > 0) g_search_query[l - 1] = '\0';
    }
}

static void draw_search_box(Font font, int term_area_w, int matches)
{
    int w = 300, h = 34;
    int x = term_area_w - w - 16;
    if (x < 8) x = 8;
    int y = 12;
    DrawRectangle(x, y, w, h, UI2RAY(g_ui_theme.search_bg));
    DrawRectangleLines(x, y, w, h, UI2RAY(g_ui_theme.search_border));
    char label[200];
    snprintf(label, sizeof(label), "Find: %s", g_search_query);
    DrawTextEx(font, label, (Vector2){(float)x + 10, (float)y + 8}, 16.0f, 0,
               UI2RAY(g_ui_theme.search_text));
    char cnt[32];
    snprintf(cnt, sizeof(cnt), "%d", matches);
    Vector2 cs = MeasureTextEx(font, cnt, 14.0f, 0);
    DrawTextEx(font, cnt, (Vector2){(float)x + w - cs.x - 10, (float)y + 10}, 14.0f, 0,
               UI2RAY(g_ui_theme.search_count));
}

static void render_terminal(GhosttyRenderState render_state,
                            GhosttyRenderStateRowIterator row_iter,
                            GhosttyRenderStateRowCells cells,
                            Font font, Font bold_font,
                            int cell_width, int cell_height,
                            int font_size,
                            int pad,
                            int term_area_w,
                            const GhosttyTerminalScrollbar *scrollbar,
                            GhosttyTerminal terminal,
                            GhosttyKittyGraphicsPlacementIterator placement_iter,
                            int origin_x, int origin_y,
                            AppConfig *cfg,
                            int frame_count)
{
    // Grab colors (palette, default fg/bg) from the render state so we
    // can resolve palette-indexed cell colors.
    GhosttyRenderStateColors colors = GHOSTTY_INIT_SIZED(GhosttyRenderStateColors);
    if (ghostty_render_state_colors_get(render_state, &colors) != GHOSTTY_SUCCESS)
        return;

    // Reset per-frame capture: selection text + the URL hit-test grid.
    g_rows_captured = 0;
    if (g_sel.active) { g_sel_len = 0; g_sel_row = -1; g_sel_text[0] = '\0'; }

    // Obtain the Kitty graphics storage from the terminal.  This is a
    // borrowed pointer valid until the next mutating terminal call.
    GhosttyKittyGraphics kitty_gfx = NULL;
    bool has_kitty = (ghostty_terminal_get(terminal,
        GHOSTTY_TERMINAL_DATA_KITTY_GRAPHICS, &kitty_gfx) == GHOSTTY_SUCCESS
        && kitty_gfx != NULL);

    // Populate the row iterator from the current render state snapshot.
    if (ghostty_render_state_get(render_state,
            GHOSTTY_RENDER_STATE_DATA_ROW_ITERATOR, &row_iter) != GHOSTTY_SUCCESS)
        return;

    // --- Layer 1: images below cell backgrounds (z < INT32_MIN/2) ---
    if (has_kitty && placement_iter) {
        render_kitty_images(terminal, kitty_gfx, placement_iter,
                            cell_width, cell_height, pad,
                            GHOSTTY_KITTY_PLACEMENT_LAYER_BELOW_BG);
    }

    // Small padding from the pane rect edges.
    int y = origin_y + pad;

    while (ghostty_render_state_row_iterator_next(row_iter)) {
        // Get the cells for this row (reuses the same cells handle).
        if (ghostty_render_state_row_get(row_iter,
                GHOSTTY_RENDER_STATE_ROW_DATA_CELLS, &cells) != GHOSTTY_SUCCESS)
            continue;

        int x = origin_x + pad;

        while (ghostty_render_state_row_cells_next(cells)) {
            int sel_row = (y - (origin_y + pad)) / cell_height;
            int sel_col = (x - (origin_x + pad)) / cell_width;
            bool cell_selected = sel_contains(sel_row, sel_col);

            // How many codepoints make up the grapheme? 0 = empty cell.
            uint32_t grapheme_len = 0;
            ghostty_render_state_row_cells_get(cells,
                GHOSTTY_RENDER_STATE_ROW_CELLS_DATA_GRAPHEMES_LEN, &grapheme_len);

            if (grapheme_len == 0) {
                // The cell has no text, but it might have a background
                // color (e.g. from an erase with a color set).  The
                // BG_COLOR data query resolves content-tag bg colors
                // and palette indices for us, returning INVALID_VALUE
                // when the cell has no background.
                GhosttyColorRgb bg = {0};
                if (ghostty_render_state_row_cells_get(cells,
                        GHOSTTY_RENDER_STATE_ROW_CELLS_DATA_BG_COLOR, &bg) == GHOSTTY_SUCCESS) {
                    DrawRectangle(x, y, cell_width, cell_height,
                                  (Color){ bg.r, bg.g, bg.b, 255 });
                }

                if (cell_selected) {
                    DrawRectangle(x, y, cell_width, cell_height, UI2RAY(g_ui_theme.selection));
                    sel_capture(sel_row, " ", 1);
                }
                row_capture(sel_row, sel_col, " ", 1);

                x += cell_width;
                continue;
            }

            // Read the grapheme codepoints.
            uint32_t codepoints[16];
            uint32_t len = grapheme_len < 16 ? grapheme_len : 16;
            ghostty_render_state_row_cells_get(cells,
                GHOSTTY_RENDER_STATE_ROW_CELLS_DATA_GRAPHEMES_BUF, codepoints);

            // Build a UTF-8 string from the grapheme codepoints.
            char text[64];
            int pos = 0;
            for (uint32_t i = 0; i < len && pos < 60; i++) {
                char u8[4];
                int n = utf8_encode(codepoints[i], u8);
                memcpy(&text[pos], u8, n);
                pos += n;
            }
            text[pos] = '\0';

            // Resolve foreground and background colors using the new
            // per-cell color queries.  These flatten style colors,
            // content-tag colors, and palette lookups into a single RGB
            // value, returning INVALID_VALUE when the cell has no
            // explicit color (in which case we use the terminal default).
            GhosttyColorRgb fg = colors.foreground;
            ghostty_render_state_row_cells_get(cells,
                GHOSTTY_RENDER_STATE_ROW_CELLS_DATA_FG_COLOR, &fg);

            GhosttyColorRgb bg_rgb = colors.background;
            bool has_bg = ghostty_render_state_row_cells_get(cells,
                GHOSTTY_RENDER_STATE_ROW_CELLS_DATA_BG_COLOR, &bg_rgb) == GHOSTTY_SUCCESS;

            // Read the style for flags (inverse, bold, italic) — color
            // resolution is handled above via the new API.
            GhosttyStyle style = GHOSTTY_INIT_SIZED(GhosttyStyle);
            ghostty_render_state_row_cells_get(cells,
                GHOSTTY_RENDER_STATE_ROW_CELLS_DATA_STYLE, &style);

            // Inverse (reverse video): swap foreground and background colors.
            if (style.inverse) {
                GhosttyColorRgb tmp = fg;
                fg = bg_rgb;
                bg_rgb = tmp;
                has_bg = true;
            }

            Color ray_fg = { fg.r, fg.g, fg.b, 255 };

            // Draw a background rectangle if the cell has a non-default bg
            // or if inverse mode forced a swap.
            if (has_bg) {
                DrawRectangle(x, y, cell_width, cell_height, (Color){ bg_rgb.r, bg_rgb.g, bg_rgb.b, 255 });
            }

            if (cell_selected) {
                DrawRectangle(x, y, cell_width, cell_height, UI2RAY(g_ui_theme.selection));
                sel_capture(sel_row, text, pos);
            }
            row_capture(sel_row, sel_col, text, pos);

            // Italic: apply a simple shear by shifting the top of the glyph
            // to the right.  The offset is proportional to font size so it
            // looks reasonable at any scale.
            int italic_offset = style.italic ? (font_size / 6) : 0;

            // Bold: use the real bold font face when style.bold is set.
            // JetBrains Mono Bold shares the regular advance width, so the
            // grid does not shift. (§E4)
            Font draw_font = (style.bold && bold_font.texture.id != 0) ? bold_font : font;
            DrawTextEx(draw_font, text, (Vector2){x + italic_offset, y}, font_size, 0, ray_fg);

            // Faint: overlay a semi-transparent bg-colored rectangle to
            // desaturate the glyph toward the background.
            if (style.faint) {
                Color fade_color = (Color){ bg_rgb.r, bg_rgb.g, bg_rgb.b, 140 };
                DrawRectangle(x, y, cell_width, cell_height, fade_color);
            }

            // Invisible: hide the text by drawing the background over it.
            if (style.invisible) {
                Color hide = (Color){ bg_rgb.r, bg_rgb.g, bg_rgb.b, 255 };
                DrawRectangle(x, y, cell_width, cell_height, hide);
            }

            // Underline: pick underline_color if set, else fg.
            Color deco_color = ray_fg;
            if (style.underline != GHOSTTY_SGR_UNDERLINE_NONE) {
                if (style.underline_color.tag == GHOSTTY_STYLE_COLOR_RGB)
                    deco_color = (Color){ style.underline_color.value.rgb.r,
                                          style.underline_color.value.rgb.g,
                                          style.underline_color.value.rgb.b, 255 };
                int uy = y + cell_height - 2;
                if (style.underline == GHOSTTY_SGR_UNDERLINE_DOUBLE)
                    DrawRectangle(x, uy - 2, cell_width, 1, deco_color);
                DrawRectangle(x, uy, cell_width, 1, deco_color);
            }

            // Strikethrough: a horizontal line through the middle of the cell.
            if (style.strikethrough)
                DrawRectangle(x, y + cell_height / 2, cell_width, 1, ray_fg);

            // Overline: a line at the top of the cell.
            if (style.overline)
                DrawRectangle(x, y, cell_width, 1, ray_fg);

            x += cell_width;
        }

        // Clear per-row dirty flag after rendering it.
        bool clean = false;
        ghostty_render_state_row_set(row_iter,
            GHOSTTY_RENDER_STATE_ROW_OPTION_DIRTY, &clean);

        y += cell_height;
    }

    // --- Layer 2: images below text (INT32_MIN/2 <= z < 0) ---
    // Drawn after cell backgrounds but before the cursor and any
    // above-text images.  In our single-pass renderer the cell text
    // has already been drawn, but this still achieves the correct
    // visual for the common case where images sit behind text.
    if (has_kitty && placement_iter) {
        render_kitty_images(terminal, kitty_gfx, placement_iter,
                            cell_width, cell_height, pad,
                            GHOSTTY_KITTY_PLACEMENT_LAYER_BELOW_TEXT);
    }

    // Draw the cursor with proper visual style, blink, and focus-loss
    // hollow rendering (§E4).  Reads CURSOR_VISUAL_STYLE, CURSOR_BLINKING,
    // and CURSOR_PASSWORD_INPUT from the engine (ghostty/vt/render.h).
    {
        bool cursor_visible = false;
        ghostty_render_state_get(render_state,
            GHOSTTY_RENDER_STATE_DATA_CURSOR_VISIBLE, &cursor_visible);
        bool cursor_in_viewport = false;
        ghostty_render_state_get(render_state,
            GHOSTTY_RENDER_STATE_DATA_CURSOR_VIEWPORT_HAS_VALUE, &cursor_in_viewport);

        if (cursor_in_viewport) {
            uint16_t cx = 0, cy = 0;
            ghostty_render_state_get(render_state,
                GHOSTTY_RENDER_STATE_DATA_CURSOR_VIEWPORT_X, &cx);
            ghostty_render_state_get(render_state,
                GHOSTTY_RENDER_STATE_DATA_CURSOR_VIEWPORT_Y, &cy);

            GhosttyColorRgb cur_rgb = colors.foreground;
            if (colors.cursor_has_value)
                cur_rgb = colors.cursor;

            int cur_x = origin_x + pad + cx * cell_width;
            int cur_y = origin_y + pad + cy * cell_height;
            bool focused = IsWindowFocused();

            // Read visual style and blink from the engine.
            int  vstyle = (int)GHOSTTY_RENDER_STATE_CURSOR_VISUAL_STYLE_BLOCK;
            uint32_t vstyle_u32 = (uint32_t)GHOSTTY_RENDER_STATE_CURSOR_VISUAL_STYLE_BLOCK;
            if (ghostty_render_state_get(render_state,
                    GHOSTTY_RENDER_STATE_DATA_CURSOR_VISUAL_STYLE, &vstyle_u32) == GHOSTTY_SUCCESS)
                vstyle = (int)vstyle_u32;
            bool blinking = false;
            uint32_t blink_u32 = 0;
            if (ghostty_render_state_get(render_state,
                    GHOSTTY_RENDER_STATE_DATA_CURSOR_BLINKING, &blink_u32) == GHOSTTY_SUCCESS)
                blinking = (blink_u32 != 0);

            // Determine blink phase (~500 ms on/off at 60 fps).
            bool blink_on = true;
            if (blinking && cfg->cursor_blink && focused)
                blink_on = (frame_count / 30) % 2 == 0;

            // Window unfocused → always hollow outline.
            if (!focused) {
                Color hollow = { cur_rgb.r, cur_rgb.g, cur_rgb.b, 200 };
                DrawRectangleLines(cur_x, cur_y, cell_width, cell_height, hollow);
            } else if (cursor_visible && blink_on) {
                Color cur_color = { cur_rgb.r, cur_rgb.g, cur_rgb.b, g_ui_theme.cursor_alpha };

                switch (vstyle) {
                    case GHOSTTY_RENDER_STATE_CURSOR_VISUAL_STYLE_BLOCK:
                    default:
                        DrawRectangle(cur_x, cur_y, cell_width, cell_height, cur_color);
                        break;
                    case GHOSTTY_RENDER_STATE_CURSOR_VISUAL_STYLE_BAR:
                        DrawRectangle(cur_x, cur_y, 2, cell_height, cur_color);
                        break;
                    case GHOSTTY_RENDER_STATE_CURSOR_VISUAL_STYLE_UNDERLINE:
                        DrawRectangle(cur_x, cur_y + cell_height - 3, cell_width, 3, cur_color);
                        break;
                    case GHOSTTY_RENDER_STATE_CURSOR_VISUAL_STYLE_BLOCK_HOLLOW:
                        DrawRectangleLines(cur_x, cur_y, cell_width, cell_height, cur_color);
                        break;
                }
            }
        }
    }

    // --- Layer 3: images above text (z >= 0) ---
    if (has_kitty && placement_iter) {
        render_kitty_images(terminal, kitty_gfx, placement_iter,
                            cell_width, cell_height, pad,
                            GHOSTTY_KITTY_PLACEMENT_LAYER_ABOVE_TEXT);
    }

    // Draw the scrollbar when there is scrollback content to scroll through.
    // The scrollbar thumb spans the pane height (or the full screen height
    // for a single-pane layout).
    if (scrollbar && scrollbar->total > scrollbar->len) {
        // Use the pane rect's height; fall back to GetScreenHeight() for
        // backwards compatibility (the caller may pass origin_y=0 for the
        // focused leaf in a classic single-pane layout).
        int scr_h = GetScreenHeight() - origin_y;

        const int bar_width = 6;
        const int bar_margin = 2;
        int bar_x = origin_x + term_area_w - bar_width - bar_margin;

        double visible_frac = (double)scrollbar->len / (double)scrollbar->total;
        int thumb_height = (int)(scr_h * visible_frac);
        if (thumb_height < 10) thumb_height = 10;

        double scroll_frac = (scrollbar->total > scrollbar->len)
            ? (double)scrollbar->offset / (double)(scrollbar->total - scrollbar->len)
            : 1.0;
        int thumb_y = origin_y + (int)(scroll_frac * (scr_h - thumb_height));

        DrawRectangle(bar_x, thumb_y, bar_width, thumb_height,
                      UI2RAY(g_ui_theme.scrollbar));
    }

    // Reset global dirty state so the next update reports changes accurately.
    GhosttyRenderStateDirty clean_state = GHOSTTY_RENDER_STATE_DIRTY_FALSE;
    ghostty_render_state_set(render_state,
        GHOSTTY_RENDER_STATE_OPTION_DIRTY, &clean_state);
}

// ---------------------------------------------------------------------------
// Build info
// ---------------------------------------------------------------------------

// Log compile-time build info from libghostty-vt so we can quickly tell
// whether the library was built with SIMD, and in which optimization mode.
static void log_build_info(void)
{
    bool simd = false;
    ghostty_build_info(GHOSTTY_BUILD_INFO_SIMD, &simd);

    GhosttyOptimizeMode opt = GHOSTTY_OPTIMIZE_DEBUG;
    ghostty_build_info(GHOSTTY_BUILD_INFO_OPTIMIZE, &opt);

    const char *opt_str;
    switch (opt) {
    case GHOSTTY_OPTIMIZE_DEBUG:        opt_str = "Debug";        break;
    case GHOSTTY_OPTIMIZE_RELEASE_SAFE: opt_str = "ReleaseSafe";  break;
    case GHOSTTY_OPTIMIZE_RELEASE_SMALL: opt_str = "ReleaseSmall"; break;
    case GHOSTTY_OPTIMIZE_RELEASE_FAST: opt_str = "ReleaseFast";  break;
    default:                            opt_str = "Unknown";       break;
    }

    TraceLog(LOG_INFO, "ghostty-vt: simd:     %s", simd ? "enabled" : "disabled");
    TraceLog(LOG_INFO, "ghostty-vt: optimize: %s", opt_str);
}

// ---------------------------------------------------------------------------
// System callbacks (process-global, set once at startup)
// ---------------------------------------------------------------------------

// decode_png — decodes raw PNG data into RGBA pixels using Raylib's
// stb_image-based decoder.  The output buffer is allocated through the
// provided GhosttyAllocator so the library can free it later.
static bool decode_png(void *userdata,
                       const GhosttyAllocator *allocator,
                       const uint8_t *data,
                       size_t data_len,
                       GhosttySysImage *out)
{
    (void)userdata;

    // Raylib's LoadImageFromMemory decodes the PNG via stb_image.
    Image img = LoadImageFromMemory(".png", data, (int)data_len);
    if (img.data == NULL) return false;

    // Convert to uncompressed R8G8B8A8 so we have a known pixel layout.
    ImageFormat(&img, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8);

    const size_t pixel_len = (size_t)img.width * (size_t)img.height * 4;
    uint8_t *pixels = ghostty_alloc(allocator, pixel_len);
    if (!pixels) {
        UnloadImage(img);
        return false;
    }
    memcpy(pixels, img.data, pixel_len);
    UnloadImage(img);

    out->width    = (uint32_t)img.width;
    out->height   = (uint32_t)img.height;
    out->data     = pixels;
    out->data_len = pixel_len;
    return true;
}

// ---------------------------------------------------------------------------
// Effects callbacks
// ---------------------------------------------------------------------------

// (EffectsContext, effect callbacks, register_session_effects, and
// update_session_effects moved to ui_effects.c — included above.)

// Reload the terminal font at the given *logical* size (applying the current
// content scale via load_terminal_font), then recompute cell metrics, the grid,
// and the pty winsize. Shared by font-size changes (settings/zoom) and
// content-scale changes (window dragged to a differently-scaled monitor).
static bool rebuild_terminal_font(Font *font, Font *bold_font, int font_size,
                                  int *cell_width, int *cell_height,
                                  uint16_t *term_cols, uint16_t *term_rows,
                                  int term_area_w, int pad,
                                  TermEngine *te, int pty_fd)
{
    int new_cell_width = 0;
    int new_cell_height = 0;
    Font new_font = load_terminal_font(font_size, &new_cell_width, &new_cell_height);
    if (new_font.texture.id == 0)
        return false;

    UnloadFont(*font);
    *font = new_font;
    GuiSetFont(*font);              // keep RayGUI on the freshly reloaded font

    // Reload bold variant at the same logical size.
    if (bold_font && bold_font->texture.id != 0)
        UnloadFont(*bold_font);
    if (bold_font) {
        Vector2 dpi_scale = fangs_content_scale();
        int font_size_px = (int)(font_size * dpi_scale.y);
        if (font_size_px < 1) font_size_px = 1;
        *bold_font = LoadFontFromMemory(".ttf", font_jetbrains_mono_bold,
                           (int)sizeof(font_jetbrains_mono_bold), font_size_px, NULL, 0);
        if (bold_font->texture.id != 0)
            SetTextureFilter(bold_font->texture, TEXTURE_FILTER_BILINEAR);
    }

    *cell_width = new_cell_width;
    *cell_height = new_cell_height;

    compute_terminal_grid(term_area_w, pad, *cell_width, *cell_height,
                          term_cols, term_rows);
    term_engine_resize(te, *term_cols, *term_rows, *cell_width, *cell_height);
    if (pty_fd >= 0)
        pty_set_winsize(pty_fd, *term_cols, *term_rows, *cell_width, *cell_height);
    return true;
}

// (declared in ui_effects.h)

static bool apply_config(const AppConfig *cfg,
                         Font *font, Font *bold_font,
                         int *font_size,
                         int *cell_width,
                         int *cell_height,
                         uint16_t *term_cols,
                         uint16_t *term_rows,
                         int term_area_w,
                         int pad,
                         Session *s)
{
    TermEngine *te = (TermEngine *)session_engine(s);
    int pty_fd = session_pty_fd(s);
    if (!te || pty_fd < 0) return false;

    if (cfg->font_size != *font_size) {
        if (!rebuild_terminal_font(font, bold_font, cfg->font_size, cell_width, cell_height,
                                   term_cols, term_rows, term_area_w, pad, te, pty_fd))
            return false;
        *font_size = cfg->font_size;
    }

    update_session_effects(s, *term_cols, *term_rows, *cell_width, *cell_height);
    return true;
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------

// (declared in ui_effects.h)

// Sink for pty_read: feed drained PTY bytes through the cmdblocks OSC-133
// observer, which forwards every byte to the VT engine unchanged while tracking
// command boundaries for the block overlay.
static void feed_engine(void *userdata, const uint8_t *data, size_t len)
{
    TermEngine *te = (TermEngine *)userdata;
    if (g_cmdblocks)
        cmdblocks_feed(g_cmdblocks, te, data, len);
    else
        term_engine_write(te, data, len);
}

// Resolve the AI key: FANGS_API_KEY env wins over the config file value.
static const char *resolve_api_key(const AppConfig *cfg)
{
    const char *env = getenv("FANGS_API_KEY");
    if (env && env[0])
        return env;
    return cfg->api_key;
}

// Build and launch a streaming AI request for `prompt`, using redacted recent
// terminal output as context. Returns NULL if there's no key or setup fails.
// block_context (§15): when non-NULL, it is the already-redacted command-block
// context and REPLACES the scrollback dump for this send. NULL → normal chat,
// which captures the last ~120 redacted lines.
static AiStream *start_ai_request(TermEngine *te, const AppConfig *cfg,
                                  const char *prompt, const char *block_context)
{
    char *ctx = block_context ? NULL : context_build(te, 120, 8192);  // redacted
    const char *context_text = block_context ? block_context
                             : (ctx ? ctx : "(none)");
    const char *sys =
        "You are a terminal assistant embedded in the user's terminal. Recent "
        "terminal output is provided as context. Answer concisely. If you "
        "recommend a command, put ONLY that command on a single line inside one "
        "```sh fenced block.";

    size_t ulen = strlen(context_text) + strlen(prompt) + 64;
    char *user = malloc(ulen);
    snprintf(user, ulen, "Recent terminal output:\n%s\n\nQuestion: %s",
             context_text, prompt);

    // system + recent conversation history + the augmented current question.
    enum { MAX_TURNS = 10 };
    AiMessage msgs[MAX_TURNS + 2];
    int n = 0;
    msgs[n++] = (AiMessage){ "system", sys };

    // History = prior sidebar messages, excluding the current question (the
    // last one, just pushed) and any MSG_SYSTEM notices. Capped to MAX_TURNS.
    int count = ui_sidebar_count();
    int hist_end = count > 0 ? count - 1 : 0;
    int begin = hist_end - MAX_TURNS;
    if (begin < 0) begin = 0;
    for (int i = begin; i < hist_end; i++) {
        MsgRole r = ui_sidebar_role(i);
        if (r == MSG_SYSTEM) continue;
        const char *text = ui_sidebar_text(i);
        if (!text[0]) continue;
        msgs[n++] = (AiMessage){ r == MSG_USER ? "user" : "assistant", text };
    }
    msgs[n++] = (AiMessage){ "user", user };

    AiConfig aic = {
        .provider = cfg->provider,
        .endpoint = cfg->endpoint,
        .model = cfg->model,
        .api_key = resolve_api_key(cfg),
        .max_tokens = cfg->max_tokens,
        .stream = true,
    };
    AiStream *s = ai_stream_start(&aic, msgs, n);
    free(ctx);
    free(user);
    return s;
}

// Inline (Ctrl+Space) request: a strict "one bare command" prompt with a small
// context budget. Returns NULL if there's no key or setup fails.
static AiStream *start_inline_request(TermEngine *te, const AppConfig *cfg,
                                      const char *prompt)
{
    char *ctx = context_build(te, 40, 4096);
    const char *sys =
        "You translate the user's request into a single shell command for their "
        "shell. Output ONLY the command on one line. No explanation, no markdown, "
        "no backticks. Recent terminal output is provided for context. If unsure, "
        "output the closest single command.";

    size_t ulen = (ctx ? strlen(ctx) : 0) + strlen(prompt) + 64;
    char *user = malloc(ulen);
    snprintf(user, ulen, "Recent terminal output:\n%s\n\nRequest: %s",
             ctx ? ctx : "(none)", prompt);

    AiMessage msgs[2] = {
        { "system", sys },
        { "user", user },
    };
    AiConfig aic = {
        .provider = cfg->provider,
        .endpoint = cfg->endpoint,
        .model = cfg->model,
        .api_key = resolve_api_key(cfg),
        .max_tokens = cfg->max_tokens,
        .stream = true,
    };
    AiStream *s = ai_stream_start(&aic, msgs, 2);
    free(ctx);
    free(user);
    return s;
}

// Pixel position just below the terminal cursor, for anchoring the inline prompt.
static void cursor_pixel(GhosttyRenderState rs, int cell_width, int cell_height,
                         int pad, int *px, int *py)
{
    uint16_t cx = 0, cy = 0;
    ghostty_render_state_get(rs, GHOSTTY_RENDER_STATE_DATA_CURSOR_VIEWPORT_X, &cx);
    ghostty_render_state_get(rs, GHOSTTY_RENDER_STATE_DATA_CURSOR_VIEWPORT_Y, &cy);
    *px = pad + (int)cx * cell_width;
    *py = pad + ((int)cy + 1) * cell_height;
}

// Mix two colors (t in 0..1), for deriving widget shades from a theme.
static Color color_mix(Color a, Color b, float t)
{
    return (Color){
        (unsigned char)(a.r + (b.r - a.r) * t),
        (unsigned char)(a.g + (b.g - a.g) * t),
        (unsigned char)(a.b + (b.b - a.b) * t),
        255,
    };
}

// Style RayGUI (settings modal + sidebar/inline widgets) to match the active
// terminal theme, so the in-app UI isn't a light panel over a dark terminal.
// Derives shades from the theme's bg/fg/blue, so it works for dark and light.
static void apply_gui_style(const Theme *t)
{
    Color bg     = (Color){t->bg.r, t->bg.g, t->bg.b, 255};
    Color fg     = (Color){t->fg.r, t->fg.g, t->fg.b, 255};
    Color accent = (Color){t->ansi[4].r, t->ansi[4].g, t->ansi[4].b, 255};  // blue
    Color border = color_mix(bg, fg, 0.30f);
    Color base   = color_mix(bg, fg, 0.08f);
    Color base_f = color_mix(bg, accent, 0.14f);   // hover: faint tint
    Color base_p = color_mix(bg, accent, 0.18f);   // focused/editing: mostly dark — focus is shown by the accent border, not a heavy fill
    Color dim    = color_mix(bg, fg, 0.45f);

    GuiSetStyle(DEFAULT, BACKGROUND_COLOR, ColorToInt(bg));
    GuiSetStyle(DEFAULT, LINE_COLOR,       ColorToInt(border));

    GuiSetStyle(DEFAULT, BORDER_COLOR_NORMAL, ColorToInt(border));
    GuiSetStyle(DEFAULT, BASE_COLOR_NORMAL,   ColorToInt(base));
    GuiSetStyle(DEFAULT, TEXT_COLOR_NORMAL,   ColorToInt(fg));

    GuiSetStyle(DEFAULT, BORDER_COLOR_FOCUSED, ColorToInt(accent));
    GuiSetStyle(DEFAULT, BASE_COLOR_FOCUSED,   ColorToInt(base_f));
    GuiSetStyle(DEFAULT, TEXT_COLOR_FOCUSED,   ColorToInt(fg));

    GuiSetStyle(DEFAULT, BORDER_COLOR_PRESSED, ColorToInt(accent));
    GuiSetStyle(DEFAULT, BASE_COLOR_PRESSED,   ColorToInt(base_p));
    GuiSetStyle(DEFAULT, TEXT_COLOR_PRESSED,   ColorToInt(fg));   // readable while typing (was accent-on-accent)

    GuiSetStyle(DEFAULT, BORDER_COLOR_DISABLED, ColorToInt(border));
    GuiSetStyle(DEFAULT, BASE_COLOR_DISABLED,   ColorToInt(base));
    GuiSetStyle(DEFAULT, TEXT_COLOR_DISABLED,   ColorToInt(dim));
}

// Apply config to all sessions in the active tab. Used by end-of-frame font
// zoom and settings-save so every pane picks up theme/font changes.
// Returns true if all sessions applied successfully.
static bool apply_config_all_sessions(const AppConfig *cfg,
                                       Font *font, Font *bold_font,
                                       int *font_size,
                                       int *cell_width, int *cell_height,
                                       uint16_t *term_cols, uint16_t *term_rows,
                                       int term_area_w, int pad)
{
    if (app.n_tabs <= 0 || app.active < 0) return false;
    Tab *tab = &app.tabs[app.active];
    if (!tab || !tab->root) return false;
    PaneNode *leaves[64];
    int n = 0;
    pane_collect_leaves(tab->root, leaves, 64, &n);
    bool ok = true;
    for (int i = 0; i < n; i++) {
        Session *s = leaves[i]->leaf.session;
        if (!apply_config(cfg, font, bold_font, font_size, cell_width, cell_height,
                          term_cols, term_rows, term_area_w, pad, s))
            ok = false;
    }
    return ok;
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------

// (declared in ui_effects.h; no forward declaration needed.)

int main(void)
{
    log_build_info();

    AppConfig cfg;
    const char *config_path = config_default_path();
    if (!config_load(&cfg, config_path)) {
        fprintf(stderr, "warning: failed to load config at %s; using defaults\n", config_path);
        toast_push(TOAST_WARN, "Failed to load config; using defaults.");
    }

    int font_size = cfg.font_size;

    // Enable HiDPI *before* creating the window so raylib can set up the
    // framebuffer at the native display resolution.
    SetConfigFlags(FLAG_WINDOW_HIGHDPI);

    // Initialize window
    InitWindow(800, 600, "Fangs");
    // raylib's default exit key is ESC. A terminal must pass ESC straight
    // through to the child (vim normal mode, cancelling prompts, every TUI),
    // and the settings modal needs ESC to dismiss itself — not kill the app.
    // Disable the exit key so the loop only ends on the window close button.
    SetExitKey(KEY_NULL);
    SetWindowState(FLAG_WINDOW_RESIZABLE);
    SetTargetFPS(60);

    // Process-global curl init (paired with ai_global_cleanup at exit). Done
    // here, before any worker thread exists, since it isn't thread-safe.
    ai_global_init();

    // App handles: sessions via tabs+panes, AI streams, accumulated data.
    int exit_code = 0;
    AiStream *active_stream = NULL;   // in-flight sidebar AI request
    AiStream *inline_stream = NULL;   // in-flight inline (Ctrl+Space) request
    char inline_answer[8192] = "";    // accumulates the inline command reply

    // g_cmdblocks is set by sync_active_session() per-frame; prime it NULL.
    g_cmdblocks = NULL;

    int cell_width = 0;
    int cell_height = 0;
    Font mono_font = load_terminal_font(font_size, &cell_width, &cell_height);
    if (mono_font.texture.id == 0) {
        fprintf(stderr, "LoadFontFromMemory failed\n");
        toast_push(TOAST_ERROR, "Failed to load terminal font.");
        exit_code = 1;
        goto cleanup;
    }

    // Load the bold variant for SGR bold rendering (§E4).
    Font bold_font = {0};
    {
        Vector2 dpi_scale = fangs_content_scale();
        int font_size_px = (int)(font_size * dpi_scale.y);
        if (font_size_px < 1) font_size_px = 1;
        bold_font = LoadFontFromMemory(".ttf", font_jetbrains_mono_bold,
                           (int)sizeof(font_jetbrains_mono_bold), font_size_px, NULL, 0);
        if (bold_font.texture.id != 0)
            SetTextureFilter(bold_font.texture, TEXTURE_FILTER_BILINEAR);
    }

    // Use the terminal font for all RayGUI widgets + AI panels, so the AI
    // features match the rest of the terminal instead of raylib's bitmap font.
    GuiSetFont(mono_font);

    // Content scale (HiDPI). Tracked so the font + UI rescale live when the
    // window is dragged to a differently-scaled monitor.
    float applied_scale = fangs_content_scale().y;

    // Small padding from window edges, in pixels.  Passed to render_terminal()
    // and handle_mouse() so all layout uses a single value.
    const int pad = 4;
    const int sidebar_width = 380;   // logical px; the UI renders in logical space
    const int min_terminal_w = 320;  // (crispness comes from the 2x font texture)

    Layout lo = layout_compute(GetScreenWidth(), GetScreenHeight(),
                               ui_sidebar_visible(),
                               sidebar_width, pad,
                               min_terminal_w);
    int term_area_w = lo.terminal.w;

    uint16_t term_cols = 0;
    uint16_t term_rows = 0;
    compute_terminal_grid(term_area_w, pad, cell_width, cell_height,
                          &term_cols, &term_rows);

    // Install the PNG decoder (process-global, before any terminal exists)
    // so the Kitty graphics protocol can accept PNG images.
    ghostty_sys_set(GHOSTTY_SYS_OPT_DECODE_PNG, (const void *)decode_png);

    // Initialise the first tab/session via the App (§16.5).
    Session *s = app_init_first_tab(term_cols, term_rows, cell_width, cell_height,
                                     cfg.scrollback);
    if (!s) {
        fprintf(stderr, "failed to create initial session\n");
        toast_push(TOAST_ERROR, "Failed to create terminal session.");
        exit_code = 1;
        goto cleanup;
    }
    register_session_effects(s);

    // Borrow handles for the per-frame input/render code via the active session.
    // sync_active_session() is called each frame to keep these current across
    // tab switches and split changes.
    TermEngine *te = NULL;
    int pty_fd = -1;
    pid_t child = -1;
    bool child_exited = true;
    sync_active_session(&te, &pty_fd, &child, &child_exited);
    GhosttyTerminal terminal = term_engine_terminal(te);
    GhosttyRenderState render_state = term_engine_render_state(te);
    GhosttyRenderStateRowIterator row_iter = term_engine_row_iter(te);
    GhosttyRenderStateRowCells row_cells = term_engine_row_cells(te);
    GhosttyKittyGraphicsPlacementIterator placement_iter = term_engine_placement_iter(te);
    GhosttyKeyEncoder key_encoder = term_engine_key_encoder(te);
    GhosttyKeyEvent key_event = term_engine_key_event(te);
    GhosttyMouseEncoder mouse_encoder = term_engine_mouse_encoder(te);
    GhosttyMouseEvent mouse_event = term_engine_mouse_event(te);

    bool child_reaped = false;
    int child_exit_status = -1;

    // Track window size so we only recalculate the grid on actual changes.
    int prev_width = GetScreenWidth();
    int prev_height = GetScreenHeight();
    int prev_term_area_w = term_area_w;
    char applied_theme[32] = "";   // engine-applied theme name; re-apply on change

    // Track focus state so we only send focus events on transitions.
    // Initialize from the actual window state to avoid a spurious
    // focus-lost event on startup.
    bool prev_focused = IsWindowFocused();

    // Scrollbar drag state — when the user clicks and drags the
    // scrollbar thumb we continuously reposition the viewport.
    bool scrollbar_dragging = false;

    const char *phase3_smoke_report_path = getenv("FANGS_PHASE3_SMOKE_REPORT");
    const char *phase3_smoke_screenshot_path = getenv("FANGS_PHASE3_SMOKE_SCREENSHOT");
    bool phase3_smoke = (phase3_smoke_report_path && phase3_smoke_report_path[0] != '\0')
                     || (phase3_smoke_screenshot_path && phase3_smoke_screenshot_path[0] != '\0');
    bool phase3_smoke_started = false;
    int phase3_smoke_frames = 0;

    // Command-block visual smoke: feed canned OSC-133 marks + output so the
    // block overlay can be screenshotted deterministically without a shell
    // that has the integration snippet. Dev/CI only; gated by the env var.
    const char *blocks_smoke_path = getenv("FANGS_BLOCKS_SMOKE_SCREENSHOT");
    bool blocks_smoke = (blocks_smoke_path && blocks_smoke_path[0] != '\0');
    bool blocks_smoke_started = false;
    int  blocks_smoke_frames = 0;
    int frame_count = 0;

    // Each frame: handle resize → read pty → process input → render.
    while (!WindowShouldClose()) {
        frame_count++;
        if (phase3_smoke && !phase3_smoke_started) {
            if (!ui_sidebar_visible())
                ui_sidebar_toggle();
            ui_sidebar_push(MSG_USER, "phase3 smoke prompt");
            ui_sidebar_push(MSG_SYSTEM, "(AI not wired yet - Phase 4)");
            // FANGS_SMOKE_FOCUS opens the input in edit mode so the headless
            // smoke can exercise GuiTextBox's edit path — a regression guard for
            // the narrow-sidebar SIGBUS. Default stays unfocused (PTY passthrough).
            ui_sidebar_focus(getenv("FANGS_SMOKE_FOCUS") != NULL);
            phase3_smoke_started = true;
        }

        if (blocks_smoke && !blocks_smoke_started) {
            // Full A/B/C/D protocol per command, exactly as the shell snippet
            // emits (docs/shell-integration.md): A=prompt start, B=prompt end,
            // C=exec (output begins), D=done. C is what lets the engine's
            // select_output identify the output region for "copy output".
            static const char canned[] =
                "\x1b[2J\x1b[3J\x1b[H"
                "\x1b]133;A\x1b\\$ \x1b]133;B\x1b\\npm run build\r\n\x1b]133;C\x1b\\  vite v5.4 building for production...\r\n  built in 1.21s\r\n\x1b]133;D;0\x1b\\"
                "\x1b]133;A\x1b\\$ \x1b]133;B\x1b\\cargo test\r\n\x1b]133;C\x1b\\  error[E0382]: borrow of moved value: `cfg`\r\n  test result: FAILED. 1 passed; 1 failed\r\n\x1b]133;D;101\x1b\\"
                "\x1b]133;A\x1b\\$ \x1b]133;B\x1b\\git status\r\n\x1b]133;C\x1b\\  On branch main\r\n  nothing to commit, working tree clean\r\n\x1b]133;D;0\x1b\\"
                "\x1b]133;A\x1b\\$ \x1b]133;B\x1b\\";
            cmdblocks_feed(g_cmdblocks, te, (const uint8_t *)canned, sizeof(canned) - 1);
            blocks_smoke_started = true;
        }

        // Config changes from the settings modal or the font-zoom chord are
        // applied once at the end of the frame via apply_config(); declared here
        // so the zoom chord (below, before handle_input) can request it.
        bool apply_saved_config = false;

        // Intercept settings shortcut before handle_input() can forward the
        // comma key to the PTY. Accept Super+, on macOS and Ctrl+, elsewhere.
        bool settings_shortcut_consumed = false;
        if (IsKeyPressed(KEY_COMMA)
            && (IsKeyDown(KEY_LEFT_SUPER) || IsKeyDown(KEY_RIGHT_SUPER)
                || IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL))) {
            ui_settings_toggle();
            settings_shortcut_consumed = true;
            while (GetCharPressed() != 0) {
                // Drain any printable comma event from the shortcut frame.
            }
        }

        bool sidebar_chord_consumed = false;
        bool sidebar_chord = IsKeyPressed(KEY_B)
            && ((IsKeyDown(KEY_LEFT_SUPER) || IsKeyDown(KEY_RIGHT_SUPER))
                || ((IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL))
                    && (IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT))));
        if (sidebar_chord && !ui_settings_open()) {
            ui_sidebar_toggle();
            ui_sidebar_focus(ui_sidebar_visible());
            sidebar_chord_consumed = true;
            while (GetCharPressed() != 0) {
                // Drain any printable event from the shortcut frame.
            }
        }

        // Inline AI: Ctrl+Space opens a floating prompt anchored at the cursor.
        bool inline_chord = IsKeyPressed(KEY_SPACE)
            && (IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL));
        if (inline_chord && !ui_settings_open() && !ui_inline_active()) {
            int icx = 0, icy = 0;
            cursor_pixel(render_state, cell_width, cell_height, pad, &icx, &icy);
            ui_inline_open(icx, icy);
            while (GetCharPressed() != 0) {
                // Drain the triggering keystroke so it doesn't seed the input.
            }
        }

        bool ctrl_down  = IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL);
        bool shift_down = IsKeyDown(KEY_LEFT_SHIFT)   || IsKeyDown(KEY_RIGHT_SHIFT);
        bool cmd_down   = IsKeyDown(KEY_LEFT_SUPER)   || IsKeyDown(KEY_RIGHT_SUPER);
        bool alt_down   = IsKeyDown(KEY_LEFT_ALT)     || IsKeyDown(KEY_RIGHT_ALT);
        // Tab/split/focus chords: Cmd on macOS, Ctrl+Shift on Linux. Plain
        // Ctrl+<key> must reach the shell (Ctrl+D EOF, Ctrl+W word-erase,
        // Ctrl+T transpose), so these never trigger on bare Ctrl — matching the
        // copy/paste chords above.
        bool tab_chord  = cmd_down || (ctrl_down && shift_down);

        // Command-block navigation: Cmd/Ctrl + Up/Down jumps between command
        // prompts (Warp-style). Intercepted before handle_input so the arrow
        // keys aren't also forwarded to the child shell.
        bool block_nav_consumed = false;
        if ((cmd_down || ctrl_down) && !ui_settings_open() && !ui_inline_active()
            && !g_search_active && !ui_sidebar_focused()) {
            if (IsKeyPressed(KEY_UP))   block_nav_consumed = cmdblocks_navigate(g_cmdblocks, te, -1);
            if (IsKeyPressed(KEY_DOWN)) block_nav_consumed = cmdblocks_navigate(g_cmdblocks, te, +1);
        }

        // Clipboard: Ctrl+Shift+C/V (Linux) or Cmd+C/V (macOS); Shift+Insert pastes.
        // Intercept BEFORE handle_input so Ctrl+Shift+C never reaches the pty as ^C.
        bool clipboard_consumed = false;
        if (((ctrl_down && shift_down) || cmd_down) && IsKeyPressed(KEY_C)) {
            sel_copy_to_clipboard();
            clipboard_consumed = true;
        }
        // Only paste into the PTY when no on-screen text field is capturing
        // input — otherwise the settings field, AI sidebar input, and inline
        // prompt (all raygui GuiTextBox, which now handles paste itself) would
        // be bypassed and the paste would land in the terminal behind them.
        bool text_field_capturing = ui_settings_open() || ui_inline_active()
            || ui_sidebar_focused() || g_search_active;
        if ((((ctrl_down && shift_down) || cmd_down) && IsKeyPressed(KEY_V))
            || (shift_down && IsKeyPressed(KEY_INSERT))) {
            if (!text_field_capturing) {
                do_paste(pty_fd, terminal);
                clipboard_consumed = true;
                while (GetCharPressed() != 0) { }
            }
        }

        // Find overlay: Ctrl+F / Cmd+F toggles; while open it captures typing.
        bool search_consumed = false;
        if ((ctrl_down || cmd_down) && IsKeyPressed(KEY_F)) {
            g_search_active = !g_search_active;
            if (!g_search_active) g_search_query[0] = '\0';
            search_consumed = true;
            while (GetCharPressed() != 0) { }
        }
        if (g_search_active) {
            if (IsKeyPressed(KEY_ESCAPE)) { g_search_active = false; g_search_query[0] = '\0'; }
            else search_input();
        }

        // Font zoom: Ctrl/Cmd + '='/'+' grows, '-' shrinks, '0' resets to the
        // config default. Intercepted before handle_input so '='/'-' aren't also
        // forwarded to the shell. Mutates cfg.font_size, persists, and lets the
        // end-of-frame apply_config() reload the font + reflow the grid + pty.
        bool zoom_consumed = false;
        if ((ctrl_down || cmd_down) && !ui_settings_open() && !ui_inline_active()
            && !g_search_active && !ui_sidebar_focused()) {
            int new_size = cfg.font_size;
            if (IsKeyPressed(KEY_EQUAL) || IsKeyPressed(KEY_KP_ADD))
                new_size += 1;
            else if (IsKeyPressed(KEY_MINUS) || IsKeyPressed(KEY_KP_SUBTRACT))
                new_size -= 1;
            else if (IsKeyPressed(KEY_ZERO) || IsKeyPressed(KEY_KP_0))
                new_size = FANGS_DEFAULT_FONT_SIZE;
            if (new_size < FANGS_MIN_FONT_SIZE) new_size = FANGS_MIN_FONT_SIZE;
            if (new_size > FANGS_MAX_FONT_SIZE) new_size = FANGS_MAX_FONT_SIZE;
            if (new_size != cfg.font_size) {
                cfg.font_size = new_size;
                if (!config_save(&cfg, config_path)) {
                    fprintf(stderr, "warning: failed to save config at %s\n", config_path);
                    toast_push(TOAST_WARN, "Failed to save config (font zoom).");
                }
                apply_saved_config = true;
                zoom_consumed = true;
                while (GetCharPressed() != 0) { }  // drain '='/'-'/'+' text events
            }
        }

        // Tab and split operations: Cmd+… (macOS) / Ctrl+Shift+… (Linux).
        // Intercepted before handle_input so these key events never reach the
        // shell — and gated on tab_chord so bare Ctrl+<key> still does.
        if (tab_chord && !ui_settings_open() && !ui_inline_active()
            && !g_search_active && !ui_sidebar_focused()) {

            // --- New tab: Cmd/Ctrl+T ---
            if (IsKeyPressed(KEY_T)) {
                Session *cur = sync_active_session(&te, &pty_fd, &child, &child_exited);
                app_add_tab(term_cols, term_rows, cell_width, cell_height,
                            cfg.scrollback, session_cwd(cur),
                            &te, &pty_fd, &child, &child_exited);
                prev_term_area_w = -1;
                while (GetCharPressed() != 0) { }
            }

            // --- Close focused pane / active tab: Cmd/Ctrl+W ---
            if (IsKeyPressed(KEY_W)) {
                app_close_active();
                sync_active_session(&te, &pty_fd, &child, &child_exited);
                prev_term_area_w = -1;
                while (GetCharPressed() != 0) { }
            }

            // --- Tab switch: Cmd/Ctrl+1..9 ---
            int tab_idx = -1;
                 if (IsKeyPressed(KEY_ONE))   tab_idx = 0;
            else if (IsKeyPressed(KEY_TWO))   tab_idx = 1;
            else if (IsKeyPressed(KEY_THREE)) tab_idx = 2;
            else if (IsKeyPressed(KEY_FOUR))  tab_idx = 3;
            else if (IsKeyPressed(KEY_FIVE))  tab_idx = 4;
            else if (IsKeyPressed(KEY_SIX))   tab_idx = 5;
            else if (IsKeyPressed(KEY_SEVEN)) tab_idx = 6;
            else if (IsKeyPressed(KEY_EIGHT)) tab_idx = 7;
            else if (IsKeyPressed(KEY_NINE))  tab_idx = 8;
            if (tab_idx >= 0) {
                app_switch_tab(tab_idx, &te, &pty_fd, &child, &child_exited);
                prev_term_area_w = -1;
                while (GetCharPressed() != 0) { }
            }

            // --- Split D. Direction picker: on macOS Shift = vertical
            // (Cmd+D horizontal, Cmd+Shift+D vertical, per spec §16.8); on
            // Linux Shift is already part of the chord, so Alt = vertical
            // (Ctrl+Shift+D horizontal, Ctrl+Shift+Alt+D vertical). ---
            if (IsKeyPressed(KEY_D)) {
                bool vertical = cmd_down ? shift_down : alt_down;
                Session *cur = sync_active_session(&te, &pty_fd, &child, &child_exited);
                app_split_focused(vertical ? PANE_VSPLIT : PANE_HSPLIT,
                                  term_cols, term_rows, cell_width, cell_height,
                                  cfg.scrollback, session_cwd(cur),
                                  &te, &pty_fd, &child, &child_exited);
                prev_term_area_w = -1;
                while (GetCharPressed() != 0) { }
            }
        }

        // --- Pane focus move: Cmd+Opt+Arrow (macOS) / Ctrl+Shift+Arrow (Linux).
        // Only when the active tab has more than one pane, so single-pane users
        // keep Ctrl+Shift+Arrow / modified arrows for their TUIs (§16.8). ---
        bool focus_chord = (cmd_down && alt_down) || (ctrl_down && shift_down);
        if (focus_chord && app.n_tabs > 0
            && pane_count_leaves(app.tabs[app.active].root) > 1
            && !ui_settings_open() && !ui_inline_active()
            && !g_search_active && !ui_sidebar_focused()) {
            int dx = 0, dy = 0;
            if (IsKeyPressed(KEY_LEFT))       dx = -1;
            else if (IsKeyPressed(KEY_RIGHT)) dx = 1;
            else if (IsKeyPressed(KEY_UP))    dy = -1;
            else if (IsKeyPressed(KEY_DOWN))  dy = 1;
            if (dx != 0 || dy != 0) {
                Tab *t = &app.tabs[app.active];
                PaneNode *nf = pane_focus_move(t->root, t->focused, dx, dy);
                if (nf) {
                    t->focused = nf;
                    sync_active_session(&te, &pty_fd, &child, &child_exited);
                }
                while (GetCharPressed() != 0) { }
            }
        }

        // Recalculate grid dimensions when the window or split layout changes.
        // We update both the ghostty terminal (so it reflows text) and the
        // pty's winsize (so the child shell knows about the new size and
        // can send SIGWINCH to its foreground process group).
        int w = GetScreenWidth();
        int h = GetScreenHeight();

        // Re-detect content scale: if the window moved to a monitor with a
        // different scale, rebuild the font (and reflow) so font_size stays a
        // consistent on-screen size. Scale only drives font-texture resolution;
        // the UI and layout are sized in logical px (see the 1.0f passed below).
        float ui_scale = fangs_content_scale().y;
        if (ui_scale != applied_scale) {
            if (rebuild_terminal_font(&mono_font, &bold_font, font_size, &cell_width, &cell_height,
                                      &term_cols, &term_rows, term_area_w, pad, te, pty_fd)) {
                applied_scale = ui_scale;
                prev_term_area_w = -1;   // force the grid/winsize resync below
            } else {
                ui_scale = applied_scale;   // reload failed; keep the current font
            }
        }

        lo = layout_compute(w, h, ui_sidebar_visible(),
                            sidebar_width, pad,
                            min_terminal_w);
        term_area_w = lo.terminal.w;
        if (w != prev_width || h != prev_height || term_area_w != prev_term_area_w) {
            compute_terminal_grid(term_area_w, pad, cell_width, cell_height,
                                  &term_cols, &term_rows);
            // Resize all sessions in the active tab.
            Tab *tab = &app.tabs[app.active];
            PaneNode *leaves[64];
            int n_leaves = 0;
            pane_collect_leaves(tab->root, leaves, 64, &n_leaves);
            for (int i = 0; i < n_leaves; i++) {
                Session *ss = leaves[i]->leaf.session;
                TermEngine *ste = (TermEngine *)session_engine(ss);
                int spfd = session_pty_fd(ss);
                term_engine_resize(ste, term_cols, term_rows, cell_width, cell_height);
                update_session_effects(ss, term_cols, term_rows, cell_width, cell_height);
                pty_set_winsize(spfd, term_cols, term_rows, cell_width, cell_height);
            }
            prev_width = w;
            prev_height = h;
            prev_term_area_w = term_area_w;
        }

        // Drain PTY output from ALL sessions in the active tab and feed their
        // VT engines. In blocks-smoke mode we ignore the real shell so the
        // canned content renders without interleaving.
        Session *active_session = sync_active_session(&te, &pty_fd, &child, &child_exited);
        if (!active_session)
            break;   // no sessions left — exit the app

        if (!blocks_smoke) {
            if (app.n_tabs > 0 && app.active >= 0) {
                Tab *tab = &app.tabs[app.active];
                PaneNode *leaves[64];
                int n_leaves = 0;
                pane_collect_leaves(tab->root, leaves, 64, &n_leaves);
                for (int i = 0; i < n_leaves; i++) {
                    Session *ss = leaves[i]->leaf.session;
                    session_feed_pty(ss);
                }
            }
        }

        // Send focus in/out events when the window focus state changes,
        // but only if the application has enabled focus reporting
        // (DECSET 1004).  Sending CSI I / CSI O unconditionally would
        // inject unexpected escape sequences into shells that never
        // asked for them.
        bool focused = IsWindowFocused();
        if (focused != prev_focused) {
            bool focus_mode = false;
            if (!child_exited
                && ghostty_terminal_mode_get(terminal,
                       GHOSTTY_MODE_FOCUS_EVENT, &focus_mode) == GHOSTTY_SUCCESS
                && focus_mode) {
                GhosttyFocusEvent focus_event = focused
                    ? GHOSTTY_FOCUS_GAINED : GHOSTTY_FOCUS_LOST;
                char focus_buf[8];
                size_t focus_written = 0;
                GhosttyResult focus_res = ghostty_focus_encode(
                    focus_event, focus_buf, sizeof(focus_buf), &focus_written);
                if (focus_res == GHOSTTY_SUCCESS && focus_written > 0)
                    pty_write(pty_fd, focus_buf, focus_written);
            }
            prev_focused = focused;
        }

        // Reap the child if exited so the exit status is available for the
        // banner and the auto-close check below.
        if (!session_child_alive(active_session))
            session_reap(active_session);
        child_exit_status = session_exit_status(active_session);

        // A clean shell exit (the user typed `exit` or pressed Ctrl-D) should
        // close the window, just like any other terminal. Only an abnormal
        // exit — a non-zero status or a signal — keeps the window open with
        // the banner below so the user can scroll back and inspect output.
        if (!session_child_alive(active_session) && child_exit_status == 0)
            break;

        if (ui_sidebar_visible() && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)
            && GetMouseX() < term_area_w) {
            ui_sidebar_focus(false);
        }

        // Handle scrollbar drag-to-scroll before mouse forwarding so
        // clicks on the scrollbar region don't leak into terminal apps
        // (e.g. vim, tmux) as spurious mouse events.
        bool scrollbar_consumed = false;
        if (!ui_settings_open()) {
            scrollbar_consumed = handle_scrollbar(terminal, render_state,
                                                   &scrollbar_dragging,
                                                   term_area_w);
        }

        // Host text selection (click-drag) when the app isn't grabbing the mouse
        // (or Shift is held to force it). Consumes the drag so it isn't also
        // forwarded to the pty as mouse events.
        bool mouse_tracking = false;
        ghostty_terminal_get(terminal, GHOSTTY_TERMINAL_DATA_MOUSE_TRACKING, &mouse_tracking);
        bool can_select = (!mouse_tracking || shift_down)
                          && !ui_settings_open() && !ui_inline_active();
        bool selection_consumed = false;
        // Ctrl/Cmd+click on a URL opens it (handled before starting a selection).
        if ((ctrl_down || cmd_down) && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)
            && GetMouseX() < term_area_w) {
            int ucc = (GetMouseX() - pad) / cell_width;
            int ucr = (GetMouseY() - pad) / cell_height;
            char url[2048];
            if (url_at(ucr, ucc, url, (int)sizeof(url))) {
                open_url(url);
                selection_consumed = true;
            }
        }
        if (!selection_consumed && can_select && !scrollbar_consumed && GetMouseX() < term_area_w) {
            int cc = (GetMouseX() - pad) / cell_width;
            int cr = (GetMouseY() - pad) / cell_height;
            if (cc < 0) cc = 0;
            if (cr < 0) cr = 0;
            if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
                g_sel.sr = g_sel.er = cr; g_sel.sc = g_sel.ec = cc;
                g_sel.dragging = true; g_sel.active = true;
                selection_consumed = true;
            } else if (g_sel.dragging && IsMouseButtonDown(MOUSE_BUTTON_LEFT)) {
                g_sel.er = cr; g_sel.ec = cc;
                selection_consumed = true;
            }
        }
        if (g_sel.dragging && IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) {
            g_sel.dragging = false;
            if (g_sel.sr == g_sel.er && g_sel.sc == g_sel.ec)
                g_sel.active = false;   // a plain click clears the selection
            selection_consumed = true;
        }

        // Forward keyboard/mouse input only while the child is alive and no UI
        // element is capturing keys. Sidebar visibility alone does not block.
        if (!ui_inline_active() && !inline_chord && !clipboard_consumed
            && !g_search_active && !search_consumed && !block_nav_consumed
            && !zoom_consumed
            && ui_sidebar_allows_pty_input(child_exited, ui_settings_open(),
                lo.sidebar_visible, ui_sidebar_focused(),
                settings_shortcut_consumed, sidebar_chord_consumed)) {
            handle_input(pty_fd, key_encoder, key_event, terminal);
            if (!scrollbar_consumed && !selection_consumed && GetMouseX() < term_area_w)
                handle_mouse(pty_fd, mouse_encoder, mouse_event, terminal,
                             cell_width, cell_height, pad, term_area_w);
        }

        // Apply the color theme when it changes (e.g. on Save). Setting the
        // palette/default colors is a mutating call, so do it once per change.
        // Apply to EVERY session in EVERY tab — not just the active pane — so
        // background panes/tabs don't keep a stale palette (§16 × E3).
        if (strcmp(cfg.theme, applied_theme) != 0) {
            Theme th = theme_resolve(cfg.theme);
            for (int ti = 0; ti < app.n_tabs; ti++) {
                PaneNode *tl[64];
                int tln = 0;
                pane_collect_leaves(app.tabs[ti].root, tl, 64, &tln);
                for (int li = 0; li < tln; li++) {
                    TermEngine *lte = (TermEngine *)session_engine(tl[li]->leaf.session);
                    if (lte) term_engine_apply_theme(lte, &th);
                }
            }
            apply_gui_style(&th);
            ui_theme_derive(&th);      // -> also updates g_ui_theme
            snprintf(applied_theme, sizeof(applied_theme), "%s", cfg.theme);
        }

        // Snapshot the terminal state into the render state for every session in
        // the active tab.  Each leaf needs its own snapshots before we draw,
        // otherwise stale render states produce visual flicker / blank panes (§16.4).
        {
            Tab *rtab = &app.tabs[app.active];
            PaneNode *rleaves[64];
            int rn_leaves = 0;
            pane_collect_leaves(rtab->root, rleaves, 64, &rn_leaves);
            for (int li = 0; li < rn_leaves; li++) {
                Session *ls = rleaves[li]->leaf.session;
                if (ls) term_engine_update((TermEngine *)session_engine(ls));
            }
        }

        // Drain any AI tokens the worker thread produced since last frame and
        // append them to the streaming assistant message. All on the main
        // thread — the worker only ever touches its own mutex-guarded buffer.
        if (active_stream) {
            char delta[2048];
            bool is_reason = false, stream_done = false, stream_ok = false;
            while (ai_stream_poll(active_stream, delta, (int)sizeof(delta),
                                  &is_reason, &stream_done, &stream_ok) > 0)
                ui_sidebar_append_assistant(delta, is_reason);
            if (stream_done) {
                ui_sidebar_end_assistant();
                if (!stream_ok)
                    ui_sidebar_push(MSG_SYSTEM, ai_stream_error(active_stream));
                ai_stream_free(active_stream);
                active_stream = NULL;
            }
        }

        // Drain the inline (Ctrl+Space) request: accumulate the answer, then on
        // completion stage the sanitised single command at the prompt.
        if (inline_stream) {
            char delta[1024];
            bool ir = false, idone = false, iok = false;
            while (ai_stream_poll(inline_stream, delta, (int)sizeof(delta),
                                  &ir, &idone, &iok) > 0) {
                if (!ir) {   // answer only; inline ignores reasoning
                    size_t cur = strlen(inline_answer);
                    snprintf(inline_answer + cur, sizeof(inline_answer) - cur, "%s", delta);
                }
            }
            if (idone) {
                if (iok) {
                    char cmd[1024];
                    if (inline_sanitize_command(inline_answer, cmd, (int)sizeof(cmd)))
                        pty_write(pty_fd, cmd, strlen(cmd));   // staged, NO newline
                    ui_inline_cancel();
                } else {
                    ui_inline_set_error(ai_stream_error(inline_stream));
                }
                ai_stream_free(inline_stream);
                inline_stream = NULL;
            }
        }

        Theme theme = theme_resolve(cfg.theme);
        Color win_bg = { theme.bg.r, theme.bg.g, theme.bg.b, 255 };

        // Query scrollbar state for the renderer.
        GhosttyTerminalScrollbar scrollbar = {0};
        GhosttyTerminalScrollbar *scrollbar_ptr = NULL;
        if (ghostty_terminal_get(terminal, GHOSTTY_TERMINAL_DATA_SCROLLBAR,
                                 &scrollbar) == GHOSTTY_SUCCESS)
            scrollbar_ptr = &scrollbar;

        // Draw every pane in the active tab. For single-pane layouts this
        // renders once at (lo.terminal.x, lo.terminal.y, lo.terminal.w, lo.terminal.h).
        BeginDrawing();
        ClearBackground(win_bg);

        Tab *tab = &app.tabs[app.active];
        PaneNode *leaves[64];
        int n_leaves = 0;
        pane_collect_leaves(tab->root, leaves, 64, &n_leaves);

        // Allocate enough space for all leaf rects.
        PaneRectEntry pane_rects[64];
        PaneRectCollector collector = {
            .entries = pane_rects,
            .count = 0,
            .capacity = 64,
        };
        layout_compute_panes(tab->root,
                             lo.terminal.x, lo.terminal.y,
                             lo.terminal.w, lo.terminal.h,
                             pane_rect_collect_cb, &collector);

        for (int i = 0; i < collector.count; i++) {
            PaneNode *leaf = collector.entries[i].leaf;
            int px = collector.entries[i].x;
            int py = collector.entries[i].y;
            int pw = collector.entries[i].w;
            int ph = collector.entries[i].h;

            if (pw < 1 || ph < 1) continue;

            Session *ss = leaf->leaf.session;
            TermEngine *lte = (TermEngine *)session_engine(ss);
            if (!lte) continue;

            // Borrow render handles from this session's term_engine, exactly
            // as the main loop does for the active session (§16.4).
            GhosttyTerminal lterm = term_engine_terminal(lte);
            GhosttyRenderState lrs = term_engine_render_state(lte);
            GhosttyRenderStateRowIterator lri = term_engine_row_iter(lte);
            GhosttyRenderStateRowCells lrc = term_engine_row_cells(lte);
            GhosttyKittyGraphicsPlacementIterator lpi = term_engine_placement_iter(lte);

            // Query scrollbar state.
            GhosttyTerminalScrollbar lsb = {0};
            GhosttyTerminalScrollbar *lsb_ptr = NULL;
            if (ghostty_terminal_get(lterm, GHOSTTY_TERMINAL_DATA_SCROLLBAR,
                                     &lsb) == GHOSTTY_SUCCESS)
                lsb_ptr = &lsb;

            // Compute the pane's grid in cells (for scrollbar bounds).
            int lterm_cols = (pw - 2 * pad) / cell_width;
            if (lterm_cols < 1) lterm_cols = 1;
            int lterm_rows = (ph - 2 * pad) / cell_height;
            if (lterm_rows < 1) lterm_rows = 1;
            int lpane_term_area_w = pw;

            BeginScissorMode(px, py, pw, ph);
            render_terminal(lrs, lri, lrc, mono_font, bold_font,
                            cell_width, cell_height, font_size, pad,
                            lpane_term_area_w, lsb_ptr, lterm, lpi,
                            px, py, &cfg, frame_count);

            // Focused-pane highlight (a 1-pixel bright border).
            if (leaf == tab->focused) {
                Color focus_border = UI2RAY(g_ui_theme.focus_border);
                DrawRectangle(px, py, pw, 1, focus_border);                    // top
                DrawRectangle(px, py, 1, ph, focus_border);                    // left
                DrawRectangle(px, py + ph - 1, pw, 1, focus_border);          // bottom
                DrawRectangle(px + pw - 1, py, 1, ph, focus_border);          // right
            }

            // Command-block overlay for the focused pane only.
            if (leaf == tab->focused && g_cmdblocks) {
                CmdBlockAction cb_action = {0};
                bool block_click = IsMouseButtonPressed(MOUSE_BUTTON_LEFT)
                    && GetMouseX() >= px && GetMouseX() < px + pw
                    && GetMouseY() >= py && GetMouseY() < py + ph
                    && !ui_settings_open() && !ui_inline_active() && !ui_sidebar_focused();
                if (cmdblocks_draw(g_cmdblocks, te, mono_font, &theme,
                                   cell_width, cell_height, font_size,
                                   pad, lpane_term_area_w, lterm_rows,
                                   GetMouseX(), GetMouseY(), block_click,
                                   &cb_action)) {
                    g_sel.active = false;
                    g_sel.dragging = false;

                    if (cb_action.action == CB_ACTION_ASK_AI) {
                        char ctx_buf[16384];
                        ai_block_build_context(cb_action.command,
                                               cb_action.output ? cb_action.output : "",
                                               cb_action.exit_code,
                                               ctx_buf, (int)sizeof(ctx_buf));
                        char *redacted = redact_secrets(ctx_buf);
                        if (redacted) {
                            ui_sidebar_set_oneshot_context(redacted);
                        } else {
                            ui_sidebar_set_oneshot_context(strdup(ctx_buf));
                        }
                        ui_sidebar_prefill(ai_block_default_question(cb_action.exit_code));
                        ui_sidebar_open_focused();
                        if (cb_action.output) {
                            free(cb_action.output);
                            cb_action.output = NULL;
                        }
                    }
                }
            }
            EndScissorMode();
        }

        // Draw gutter lines between panes (the gap from layout_compute_panes
        // creates a 2-pixel gap; fill it with the background color or a
        // subtle separator).
        for (int i = 0; i < collector.count; i++) {
            PaneNode *leaf = collector.entries[i].leaf;
            // Only draw a gutter for internal (non-leaf) children — visual
            // gaps are already handled by compute_panes_rec.  Draw a thin
            // line along the right edge of each left-child region to make
            // the split visually distinct.
            (void)leaf;
        }

        if (g_search_active) {
            int matches = draw_search_highlights(pad, cell_width, cell_height);
            draw_search_box(mono_font, term_area_w, matches);
        }

        if (ui_sidebar_visible() && lo.sidebar_visible) {
            DrawLine(lo.sidebar.x, 0, lo.sidebar.x, lo.sidebar.h,
                     UI2RAY(g_ui_theme.sidebar_separator));
            // E5: tell the sidebar whether a key is configured so it can show
            // the first-run setup card instead of a dead input.
            const char *cur_key = resolve_api_key(&cfg);
            ui_sidebar_set_has_key(cur_key && cur_key[0]);
            // out_prompt carries just the question now; any §15 block context
            // is taken separately below, so this buffer stays small.
            char submitted[1024] = "";
            char run_cmd[1024] = "";
            // The UI renders in logical space (crispness from the 2x font
            // texture), so widgets size at scale 1.0 to match the terminal.
            if (ui_sidebar_draw(mono_font, lo.sidebar, submitted, (int)sizeof(submitted),
                                run_cmd, (int)sizeof(run_cmd), 1.0f)) {
                // A new question interrupts any in-flight stream.
                if (active_stream) {
                    ai_stream_cancel(active_stream);
                    ai_stream_free(active_stream);
                    active_stream = NULL;
                    ui_sidebar_end_assistant();
                }
                ui_sidebar_push(MSG_USER, submitted);
                // §15: redacted command-block context (NULL for a normal turn).
                // Sent to the model in place of the scrollback dump, not shown
                // in the chat bubble. We own it and must free it.
                char *block_ctx = ui_sidebar_take_oneshot_context();
                const char *key = resolve_api_key(&cfg);
                if (!key || !key[0]) {
                    ui_sidebar_push(MSG_SYSTEM,
                        "No API key. Set FANGS_API_KEY or add one in Ctrl+, settings.");
                } else {
                    active_stream = start_ai_request(te, &cfg, submitted, block_ctx);
                    if (active_stream)
                        ui_sidebar_begin_assistant();
                    else
                        ui_sidebar_push(MSG_SYSTEM, "Failed to start the AI request.");
                }
                free(block_ctx);
            }
            // Run button: stage the command at the prompt — NO trailing newline,
            // the user reviews and presses Enter themselves.
            if (run_cmd[0])
                pty_write(pty_fd, run_cmd, strlen(run_cmd));
        }

        // Inline AI prompt: floating over the terminal, below the settings modal.
        ui_inline_draw(mono_font, 1.0f);
        const char *inline_prompt = ui_inline_take_prompt();
        if (inline_prompt) {
            const char *ikey = resolve_api_key(&cfg);
            if (!ikey || !ikey[0]) {
                ui_inline_set_error("No API key (set FANGS_API_KEY)");
            } else {
                inline_answer[0] = '\0';
                inline_stream = start_inline_request(te, &cfg, inline_prompt);
                if (inline_stream)
                    ui_inline_set_waiting("thinking…");
                else
                    ui_inline_set_error("failed to start request");
            }
        }

        // Show a banner when the child process has exited so the user
        // knows the shell is gone (they can still scroll / inspect output).
        if (child_exited) {
            char exit_msg[128];
            if (child_exit_status >= 0)
                snprintf(exit_msg, sizeof(exit_msg),
                         "[process exited with status %d]", child_exit_status);
            else
                snprintf(exit_msg, sizeof(exit_msg), "[process exited]");

            Vector2 msg_size = MeasureTextEx(mono_font, exit_msg, font_size, 0);
            int screen_w = GetScreenWidth();
            int screen_h = GetScreenHeight();
            int banner_h = (int)msg_size.y + 8;
            DrawRectangle(0, screen_h - banner_h, screen_w, banner_h,
                          UI2RAY(g_ui_theme.exit_banner_bg));
            DrawTextEx(mono_font, exit_msg,
                       (Vector2){(screen_w - msg_size.x) / 2,
                                 screen_h - banner_h + 4},
                       font_size, 0, UI2RAY(g_ui_theme.exit_banner_text));
        }

        // Advance toast timers each frame.
        toast_tick(GetFrameTime());

        if (ui_settings_open()) {
            bool saved = false;
            ui_settings_draw(&cfg, &saved, 1.0f);
            if (saved) {
                if (!config_save(&cfg, config_path)) {
                    fprintf(stderr, "warning: failed to save config at %s\n", config_path);
                    toast_push(TOAST_WARN, "Failed to save config.");
                } else {
                    apply_saved_config = true;
                    toast_push(TOAST_INFO, "Settings saved.");
                }
            }
        }

        // Draw toast notifications (fading pills, bottom-right).
        {
            int n_toasts = toast_count();
            int toast_w = 360;
            int toast_x = GetScreenWidth() - toast_w - 12;
            int toast_y = GetScreenHeight() - 12;
            for (int i = 0; i < n_toasts; i++) {
                ToastLevel tl;
                const char *tmsg;
                float talpha;
                if (!toast_get(i, &tl, &tmsg, &talpha)) break;
                if (talpha <= 0.0f) continue;
                Color tbg;
                switch (tl) {
                    case TOAST_ERROR: tbg = (Color){ 180, 50, 50, (unsigned char)(220 * talpha) }; break;
                    case TOAST_WARN:  tbg = (Color){ 200, 150, 30, (unsigned char)(220 * talpha) }; break;
                    default:          tbg = (Color){ 60, 60, 60, (unsigned char)(220 * talpha) }; break;
                }
                Vector2 tsz = MeasureTextEx(mono_font, tmsg, font_size, 0);
                int th = (int)tsz.y + 8;
                toast_y -= th + 4;
                DrawRectangle(toast_x, toast_y, toast_w, th, tbg);
                Color tc = (Color){ 220, 220, 220, (unsigned char)(255 * talpha) };
                DrawTextEx(mono_font, tmsg,
                           (Vector2){ (float)toast_x + 6, (float)toast_y + 4 },
                           font_size, 0, tc);
            }
        }

        EndDrawing();

        // Free any textures that were uploaded during this frame's
        // kitty image rendering.  Safe now that EndDrawing() has
        // flushed all draw commands to the GPU.
        flush_deferred_textures();

        if (blocks_smoke && blocks_smoke_started) {
            blocks_smoke_frames++;
            if (blocks_smoke_frames >= 3) {
                if (!export_screen_image(blocks_smoke_path))
                    exit_code = 1;
                break;
            }
        }

        if (phase3_smoke && phase3_smoke_started) {
            phase3_smoke_frames++;
            if (phase3_smoke_frames >= 2) {
                bool screenshot_written = false;
                if (phase3_smoke_screenshot_path && phase3_smoke_screenshot_path[0] != '\0')
                    screenshot_written = export_screen_image(phase3_smoke_screenshot_path);

                bool report_written = write_phase3_smoke_report(
                    phase3_smoke_report_path, lo, term_area_w,
                    term_cols, term_rows, screenshot_written);

                if ((phase3_smoke_report_path && phase3_smoke_report_path[0] != '\0' && !report_written)
                    || (phase3_smoke_screenshot_path && phase3_smoke_screenshot_path[0] != '\0' && !screenshot_written)) {
                    exit_code = 1;
                }
                break;
            }
        }

        if (apply_saved_config) {
            if (!apply_config_all_sessions(&cfg, &mono_font, &bold_font, &font_size,
                                            &cell_width, &cell_height,
                                            &term_cols, &term_rows, term_area_w, pad)) {
                fprintf(stderr, "warning: failed to apply config\n");
                toast_push(TOAST_WARN, "Failed to apply config.");
            } else {
                prev_width = GetScreenWidth();
                prev_height = GetScreenHeight();
                prev_term_area_w = term_area_w;
            }
        }
    }

cleanup:
    // Join any in-flight AI worker before tearing down (and before curl's global
    // cleanup). Safe even if active_stream is NULL.
    if (active_stream) {
        ai_stream_cancel(active_stream);
        ai_stream_free(active_stream);
        active_stream = NULL;
    }
    if (inline_stream) {
        ai_stream_cancel(inline_stream);
        ai_stream_free(inline_stream);
        inline_stream = NULL;
    }
    if (mono_font.texture.id != 0)
        UnloadFont(mono_font);
    CloseWindow();
    // Destroy all tabs/sessions — this closes PTY fds, kills children,
    // destroys term engines, and frees userdata via session_destroy().
    app_destroy_all();
    if (g_cmdblocks) cmdblocks_destroy(g_cmdblocks);
    ai_global_cleanup();
    return exit_code;
}
