// UI chrome theming — derive chrome colors from the terminal Theme.
// Pure + testable (no raylib/ghostty types). Mirror of theme.h.
#ifndef FANGS_UI_THEME_H
#define FANGS_UI_THEME_H

#include "theme.h"

typedef struct { unsigned char r, g, b, a; } UiColor;

typedef struct {
    UiColor panel_bg, panel_border;      // sidebar / settings / inline overlay
    UiColor selection;                    // terminal text selection wash
    UiColor search_bg, search_border, search_hit;
    UiColor search_text, search_count;    // search box label / match-count
    UiColor scrollbar;                    // thumb
    unsigned char cursor_alpha;           // block-cursor fill alpha
    UiColor msg_user, msg_assistant, msg_system;  // sidebar role tints
    UiColor text;                         // sidebar / inline normal text
    UiColor subtitle;                     // sidebar subtitle ("streaming…")
    UiColor reasoning;                    // sidebar "thinking" text
    UiColor run_button, run_button_hover; // sidebar Run button
    UiColor accent;                       // buttons, focus, ⚡ Explain error
    UiColor inline_bg, inline_border;     // Ctrl+Space floating prompt

    // Chrome overlays (derived from theme, never hardcoded).
    UiColor modal_overlay;                // settings-modal semi-transparent backdrop
    UiColor inline_error;                 // inline-prompt error text (red-ish)
    UiColor focus_border;                 // focused-pane highlight border
    UiColor sidebar_separator;            // vertical divider between grid and sidebar
    UiColor exit_banner_bg;               // process-exit banner background
    UiColor exit_banner_text;             // process-exit banner text
} UiTheme;

// Convenience: convert UiColor (our struct) to raylib Color.
#define UI2RAY(uc) ((Color){ (uc).r, (uc).g, (uc).b, (uc).a })

// Derive chrome colors from a terminal Theme. Pure function.
UiTheme ui_theme_derive(const Theme *t);

// Global derived UI theme — updated by main.c on theme change.
extern UiTheme g_ui_theme;

#endif // FANGS_UI_THEME_H
