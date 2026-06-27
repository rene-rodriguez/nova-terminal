// UI chrome theming — derive chrome colors from the terminal Theme.
#include "ui_theme.h"

#include <stdlib.h>   // abs()

// ---------------------------------------------------------------------------
// Internal helpers (all channel-integer ops, no raylib dependency)
// ---------------------------------------------------------------------------

// Linear blend: a * (1 - t/255) + b * (t/255).  t in [0, 255].
static unsigned char blend_ch(unsigned char a, unsigned char b, int t) {
    int v = (int)a + ((int)b - (int)a) * t / 255;
    return (unsigned char)(v < 0 ? 0 : v > 255 ? 255 : v);
}

static UiColor blend_cc(UiColor a, UiColor b, int t) {
    UiColor r;
    r.r = blend_ch(a.r, b.r, t);
    r.g = blend_ch(a.g, b.g, t);
    r.b = blend_ch(a.b, b.b, t);
    r.a = blend_ch(a.a, b.a, t);
    return r;
}

// Convert a ThemeColor to a fully-opaque UiColor (alpha stored separately).
static UiColor tc2uic(ThemeColor c, unsigned char a) {
    UiColor u = { c.r, c.g, c.b, a };
    return u;
}

// Saturation proxy: sum of absolute channel differences.
static int sat_tc(ThemeColor c) {
    return abs((int)c.r - (int)c.g) + abs((int)c.g - (int)c.b) + abs((int)c.b - (int)c.r);
}

// Global derived UI theme — updated by main.c on theme change.
UiTheme g_ui_theme;

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

UiTheme ui_theme_derive(const Theme *t)
{
    UiTheme u;
    UiColor bg = tc2uic(t->bg, 255);
    UiColor fg = tc2uic(t->fg, 255);

    if (!t->is_light) {
        // ---- Dark theme ----
        // Panel bg: lifted from terminal bg toward fg.
        u.panel_bg     = blend_cc(bg, fg, 36);
        u.panel_border = blend_cc(bg, fg, 72);

        // Selection: blue-ish wash on panel bg (ansi[4] = blue).
        // Blend hard from panel_bg so even muted teals produce visible contrast.
        UiColor sel_tint = tc2uic(t->ansi[4], 90);
        u.selection = blend_cc(u.panel_bg, sel_tint, 140);

        // Search box background / border: lifted from panel bg toward fg.
        u.search_bg    = blend_cc(u.panel_bg, fg, 40);
        u.search_border = blend_cc(u.panel_bg, fg, 80);

        // Search hit: yellow-ish highlight on panel bg (ansi[3] = yellow).
        UiColor hit_tint = tc2uic(t->ansi[3], 120);
        u.search_hit = blend_cc(u.panel_bg, hit_tint, 180);

        // Search text colors: derived from panel bg / fg.
        u.search_text  = blend_cc(fg, bg, 20);   // near-fg
        u.search_count = blend_cc(fg, bg, 40);   // slightly dimmer

        // Scrollbar: semi-transparent light gray.
        u.scrollbar    = (UiColor){200, 200, 200, 128};

        // Cursor alpha for selection underlay.
        u.cursor_alpha = 128;

        // Message role tints on panel bg (blended hard for contrast).
        UiColor blue_tint  = tc2uic(t->ansi[4], 60);
        UiColor green_tint = tc2uic(t->ansi[2], 60);
        u.msg_user      = blend_cc(u.panel_bg, blue_tint,  200);
        u.msg_assistant = blend_cc(u.panel_bg, green_tint, 180);
        u.msg_system    = blend_cc(u.panel_bg, fg,         140);

        // Accent: pick the most saturated of ansi[4] (blue) or cursor color.
        ThemeColor accent_c = (sat_tc(t->cursor) > sat_tc(t->ansi[4]))
                             ? t->cursor : t->ansi[4];
        u.accent = tc2uic(accent_c, 220);

        // Sidebar normal text: near-fg on panel_bg.
        u.text        = blend_cc(fg, bg, 40);
        u.subtitle    = blend_cc(fg, bg, 100);
        u.reasoning   = blend_cc(fg, bg, 80);

        // Run button: green-ish derived from ansi[2].
        UiColor run_base = tc2uic(t->ansi[2], 200);
        u.run_button      = blend_cc(u.panel_bg, run_base, 160);
        u.run_button_hover = blend_cc(u.panel_bg, run_base, 210);

        // Inline (Ctrl+Space) overlay.
        u.inline_bg    = blend_cc(bg, fg, 20);
        u.inline_border = blend_cc(bg, fg, 55);

        // Chrome overlays.
        u.modal_overlay   = (UiColor){ bg.r, bg.g, bg.b, 160 };
        u.inline_error    = tc2uic(t->ansi[1], 220);     // red from palette
        u.focus_border    = blend_cc(bg, fg, 180);       // distinct but not loud
        u.focus_border.a  = 192;
        u.sidebar_separator = blend_cc(bg, fg, 90);
        u.exit_banner_bg  = (UiColor){ bg.r, bg.g, bg.b, 200 };
        u.exit_banner_text = fg;
    } else {
        // ---- Light theme ----
        // Panel bg: pulled slightly darker from terminal bg toward fg.
        u.panel_bg     = blend_cc(bg, fg, 48);
        u.panel_border = blend_cc(bg, fg, 96);

        // Selection: blue-ish wash on panel bg.
        UiColor sel_tint = tc2uic(t->ansi[4], 70);
        u.selection = blend_cc(u.panel_bg, sel_tint, 180);

        // Search box.
        u.search_bg    = blend_cc(u.panel_bg, fg, 40);
        u.search_border = blend_cc(u.panel_bg, fg, 100);

        // Search hit: yellow-ish on panel bg for guaranteed contrast.
        UiColor hit_tint = tc2uic(t->ansi[3], 100);
        u.search_hit = blend_cc(u.panel_bg, hit_tint, 220);

        // Search text colors: derived from panel bg / fg.
        u.search_text  = blend_cc(fg, bg, 40);   // near-fg
        u.search_count = blend_cc(fg, bg, 60);   // slightly dimmer

        u.scrollbar    = (UiColor){140, 140, 140, 180};
        u.cursor_alpha = 160;

        // Message role tints on panel bg.
        UiColor blue_tint  = tc2uic(t->ansi[4], 80);
        UiColor green_tint = tc2uic(t->ansi[2], 80);
        u.msg_user      = blend_cc(u.panel_bg, blue_tint,   180);
        u.msg_assistant = blend_cc(u.panel_bg, green_tint,  160);
        u.msg_system    = blend_cc(u.panel_bg, fg,          120);

        ThemeColor accent_c = (sat_tc(t->cursor) > sat_tc(t->ansi[4]))
                             ? t->cursor : t->ansi[4];
        u.accent = tc2uic(accent_c, 240);

        // Sidebar normal text: near-fg on panel_bg.
        u.text        = blend_cc(fg, bg, 60);
        u.subtitle    = blend_cc(fg, bg, 120);
        u.reasoning   = blend_cc(fg, bg, 100);

        // Run button.
        UiColor run_base = tc2uic(t->ansi[2], 230);
        u.run_button      = blend_cc(u.panel_bg, run_base, 180);
        u.run_button_hover = blend_cc(u.panel_bg, run_base, 230);

        // Inline (Ctrl+Space) overlay.
        u.inline_bg    = blend_cc(bg, fg, 30);
        u.inline_border = blend_cc(bg, fg, 65);

        // Chrome overlays.
        u.modal_overlay   = (UiColor){ 0, 0, 0, 120 };
        u.inline_error    = tc2uic(t->ansi[1], 240);
        u.focus_border    = blend_cc(bg, fg, 120);
        u.focus_border.a  = 192;
        u.sidebar_separator = blend_cc(bg, fg, 160);
        u.exit_banner_bg  = (UiColor){ 0, 0, 0, 160 };
        u.exit_banner_text = fg;
    }

    // Update the global.
    g_ui_theme = u;
    return u;
}
