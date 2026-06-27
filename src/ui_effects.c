// ui_effects — Ghostty terminal effects (VT query response callbacks).
// Each callback handles a terminal query that needs application-level
// input (PTY fd, geometry).  Extracted from main.c so the callback set
// can be reused per-session without cluttering the host module.
#include "ui_effects.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "raylib.h"
#include "pty.h"
#include "term_engine.h"

// Context passed through the terminal's userdata pointer to all effect
// callbacks so they can reach the pty fd (and anything else they need)
// without global state.
typedef struct {
    int pty_fd;
    int cell_width;
    int cell_height;
    uint16_t cols;
    uint16_t rows;
} EffectsContext;

// write_pty effect — the terminal calls this whenever a VT sequence
// requires a response back to the application (device status reports,
// mode queries, device attributes, etc.).  Without this, programs like
// vim and tmux that probe terminal capabilities would hang.
static void effect_write_pty(GhosttyTerminal terminal, void *userdata,
                             const uint8_t *data, size_t len)
{
    (void)terminal;
    EffectsContext *ctx = (EffectsContext *)userdata;
    pty_write(ctx->pty_fd, (const char *)data, len);
}

// size effect — responds to XTWINOPS size queries (CSI 14/16/18 t)
// so programs can discover the terminal geometry in cells and pixels.
static bool effect_size(GhosttyTerminal terminal, void *userdata,
                        GhosttySizeReportSize *out_size)
{
    (void)terminal;
    EffectsContext *ctx = (EffectsContext *)userdata;
    out_size->rows = ctx->rows;
    out_size->columns = ctx->cols;
    out_size->cell_width = (uint32_t)ctx->cell_width;
    out_size->cell_height = (uint32_t)ctx->cell_height;
    return true;
}

// device_attributes effect — responds to DA1/DA2/DA3 queries so
// terminal applications can identify the terminal's capabilities.
// We report VT220-level conformance with a modest feature set.
static bool effect_device_attributes(GhosttyTerminal terminal, void *userdata,
                                     GhosttyDeviceAttributes *out_attrs)
{
    (void)terminal;
    (void)userdata;

    // DA1: VT220-level with a few common features.
    out_attrs->primary.conformance_level = GHOSTTY_DA_CONFORMANCE_VT220;
    out_attrs->primary.features[0] = GHOSTTY_DA_FEATURE_COLUMNS_132;
    out_attrs->primary.features[1] = GHOSTTY_DA_FEATURE_SELECTIVE_ERASE;
    out_attrs->primary.features[2] = GHOSTTY_DA_FEATURE_ANSI_COLOR;
    out_attrs->primary.num_features = 3;

    // DA2: VT220-type, version 1, no ROM cartridge.
    out_attrs->secondary.device_type = GHOSTTY_DA_DEVICE_TYPE_VT220;
    out_attrs->secondary.firmware_version = 1;
    out_attrs->secondary.rom_cartridge = 0;

    // DA3: arbitrary unit id.
    out_attrs->tertiary.unit_id = 0;

    return true;
}

// xtversion effect — responds to CSI > q with our application name.
static GhosttyString effect_xtversion(GhosttyTerminal terminal, void *userdata)
{
    (void)terminal;
    (void)userdata;
    return (GhosttyString){ .ptr = (const uint8_t *)"fangs", .len = 5 };
}

// title_changed effect — updates the raylib window title whenever the
// terminal receives an OSC 0 or OSC 2 title-setting sequence.
static void effect_title_changed(GhosttyTerminal terminal, void *userdata)
{
    (void)userdata;
    GhosttyString title = {0};
    if (ghostty_terminal_get(terminal, GHOSTTY_TERMINAL_DATA_TITLE, &title) != GHOSTTY_SUCCESS)
        return;

    // SetWindowTitle expects a NUL-terminated string, so copy into a
    // stack buffer.  Truncate quietly if the title is absurdly long.
    char buf[256];
    size_t len = title.len < sizeof(buf) - 1 ? title.len : sizeof(buf) - 1;
    memcpy(buf, title.ptr, len);
    buf[len] = '\0';
    SetWindowTitle(buf);
}

// color_scheme effect — responds to CSI ? 996 n.  Raylib has no API to
// query the OS color scheme, so we return false to silently ignore the
// query rather than guessing.
static bool effect_color_scheme(GhosttyTerminal terminal, void *userdata,
                                GhosttyColorScheme *out_scheme)
{
    (void)terminal;
    (void)userdata;
    (void)out_scheme;
    return false;
}

// Register ghostty effects (VT query responses) for the given session on its
// terminal.  Allocates/returns a heap EffectsContext stored as the session's
// opaque userdata — freed by session_destroy().
void register_session_effects(Session *s)
{
    int pty_fd = session_pty_fd(s);
    if (pty_fd < 0) return;

    EffectsContext *ctx = (EffectsContext *)calloc(1, sizeof(EffectsContext));
    ctx->pty_fd = pty_fd;
    // cell/col/row are updated on resize via update_session_effects().
    session_set_userdata(s, ctx);

    GhosttyTerminal term = term_engine_terminal((TermEngine *)session_engine(s));
    ghostty_terminal_set(term, GHOSTTY_TERMINAL_OPT_USERDATA, ctx);
    ghostty_terminal_set(term, GHOSTTY_TERMINAL_OPT_WRITE_PTY,
        (const void *)effect_write_pty);
    ghostty_terminal_set(term, GHOSTTY_TERMINAL_OPT_SIZE,
        (const void *)effect_size);
    ghostty_terminal_set(term, GHOSTTY_TERMINAL_OPT_DEVICE_ATTRIBUTES,
        (const void *)effect_device_attributes);
    ghostty_terminal_set(term, GHOSTTY_TERMINAL_OPT_XTVERSION,
        (const void *)effect_xtversion);
    ghostty_terminal_set(term, GHOSTTY_TERMINAL_OPT_TITLE_CHANGED,
        (const void *)effect_title_changed);
    ghostty_terminal_set(term, GHOSTTY_TERMINAL_OPT_COLOR_SCHEME,
        (const void *)effect_color_scheme);
}

// Update the terminal geometry stored in the session's EffectsContext after
// a resize or font change.
void update_session_effects(Session *s, uint16_t cols, uint16_t rows,
                            int cell_w, int cell_h)
{
    EffectsContext *ctx = (EffectsContext *)session_userdata(s);
    if (ctx) {
        ctx->cols = cols;
        ctx->rows = rows;
        ctx->cell_width = cell_w;
        ctx->cell_height = cell_h;
    }
}
