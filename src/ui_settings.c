#include "ui_settings.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "raylib.h"
#define RAYGUI_IMPLEMENTATION
#include "raygui.h"

#include "theme.h"
#include "ui_theme.h"

typedef enum {
    EDIT_FONT_FAMILY,
    EDIT_FONT_SIZE,
    EDIT_SCROLLBACK,
    EDIT_ENDPOINT,
    EDIT_MODEL,
    EDIT_API_KEY,
    EDIT_MAX_TOKENS,
    EDIT_COUNT
} EditField;

static bool settings_open = false;
static bool draft_valid = false;
static AppConfig draft;
static bool editing[EDIT_COUNT] = {0};

static void clear_editing(void)
{
    for (int i = 0; i < EDIT_COUNT; i++)
        editing[i] = false;
}

static int provider_index(const char *provider)
{
    if (strcmp(provider, "anthropic") == 0) return 1;
    if (strcmp(provider, "ollama") == 0) return 2;
    if (strcmp(provider, "custom") == 0) return 3;
    return 0;
}

static const char *provider_from_index(int index)
{
    switch (index) {
    case 1: return "anthropic";
    case 2: return "ollama";
    case 3: return "custom";
    default: return "openai";
    }
}

// When the user switches provider, prefill endpoint + model with that
// provider's defaults so the request shape and URL stay consistent. "custom"
// keeps whatever the user already typed.
static void apply_provider_defaults(AppConfig *c, const char *provider)
{
    if (strcmp(provider, "anthropic") == 0) {
        snprintf(c->endpoint, sizeof(c->endpoint),
                 "https://api.anthropic.com/v1/messages");
        snprintf(c->model, sizeof(c->model), "claude-opus-4-8");
    } else if (strcmp(provider, "ollama") == 0) {
        snprintf(c->endpoint, sizeof(c->endpoint),
                 "http://localhost:11434/v1/chat/completions");
        snprintf(c->model, sizeof(c->model), "llama3.1");
    } else if (strcmp(provider, "openai") == 0) {
        snprintf(c->endpoint, sizeof(c->endpoint),
                 "https://api.openai.com/v1/chat/completions");
        snprintf(c->model, sizeof(c->model), "gpt-4o-mini");
    }
}

// Build a ';'-joined list of theme display names for GuiComboBox.
static const char *theme_combo_list(void)
{
    static char buf[256];
    buf[0] = '\0';
    for (int i = 0; i < theme_count(); i++) {
        if (i > 0)
            strncat(buf, ";", sizeof(buf) - strlen(buf) - 1);
        strncat(buf, theme_name(i), sizeof(buf) - strlen(buf) - 1);
    }
    return buf;
}

static void begin_edit(EditField field)
{
    bool was_editing = editing[field];
    clear_editing();
    editing[field] = !was_editing;
}

static void draw_labeled_text_box(const char *label, Rectangle bounds,
                                  char *text, int text_size, EditField field,
                                  float s)
{
    GuiLabel((Rectangle){bounds.x, bounds.y - 22*s, bounds.width, 18*s}, label);
    if (GuiTextBox(bounds, text, text_size, editing[field]))
        begin_edit(field);
}

static void draw_labeled_spinner(const char *label, Rectangle bounds,
                                 int *value, int min_value, int max_value,
                                 EditField field, float s)
{
    GuiLabel((Rectangle){bounds.x, bounds.y - 22*s, bounds.width, 18*s}, label);
    if (GuiSpinner(bounds, NULL, value, min_value, max_value, editing[field]))
        begin_edit(field);
}

bool ui_settings_open(void)
{
    return settings_open;
}

void ui_settings_toggle(void)
{
    settings_open = !settings_open;
    draft_valid = false;
    clear_editing();
}

void ui_settings_draw(AppConfig *cfg, bool *out_saved, float scale)
{
    if (out_saved)
        *out_saved = false;
    if (!settings_open)
        return;

    float s = (scale > 0.1f) ? scale : 1.0f;   // HiDPI widget scale

    if (!draft_valid) {
        draft = *cfg;
        draft_valid = true;
    }

    if (IsKeyPressed(KEY_ESCAPE)) {
        settings_open = false;
        draft_valid = false;
        clear_editing();
        return;
    }

    int screen_w = GetScreenWidth();
    int screen_h = GetScreenHeight();
    DrawRectangle(0, 0, screen_w, screen_h, UI2RAY(g_ui_theme.modal_overlay));

    float panel_w = 620.0f * s;
    float panel_h = 560.0f * s;
    if (panel_w > screen_w - 32.0f*s) panel_w = (float)screen_w - 32.0f*s;
    if (panel_h > screen_h - 32.0f*s) panel_h = (float)screen_h - 32.0f*s;
    Rectangle panel = {
        ((float)screen_w - panel_w) / 2.0f,
        ((float)screen_h - panel_h) / 2.0f,
        panel_w,
        panel_h
    };

    GuiPanel(panel, "Settings");

    const float margin = 24.0f * s;
    const float row_h = 30.0f * s;
    const float gap = 18.0f * s;
    const float col_gap = 22.0f * s;
    float x = panel.x + margin;
    float y = panel.y + 48.0f*s;
    float full_w = panel.width - margin * 2.0f;
    float half_w = (full_w - col_gap) / 2.0f;

    GuiLabel((Rectangle){x, y, full_w, 20*s}, "Terminal");
    y += 32.0f*s;

    draw_labeled_text_box("Font family", (Rectangle){x, y, half_w, row_h},
                          draft.font_family, (int)sizeof(draft.font_family),
                          EDIT_FONT_FAMILY, s);
    draw_labeled_spinner("Font size", (Rectangle){x + half_w + col_gap, y, half_w, row_h},
                         &draft.font_size, 8, 72, EDIT_FONT_SIZE, s);
    y += row_h + gap + 18.0f*s;

    GuiLabel((Rectangle){x, y - 22.0f*s, half_w, 18*s}, "Theme");
    int active_theme = theme_index_of(draft.theme);
    GuiComboBox((Rectangle){x, y, half_w, row_h}, theme_combo_list(), &active_theme);
    snprintf(draft.theme, sizeof(draft.theme), "%s", theme_slug(active_theme));

    draw_labeled_spinner("Scrollback", (Rectangle){x + half_w + col_gap, y, half_w, row_h},
                         &draft.scrollback, 100, 100000, EDIT_SCROLLBACK, s);
    y += row_h + gap + 28.0f*s;

    GuiLabel((Rectangle){x, y, full_w, 20*s}, "AI");
    y += 32.0f*s;

    GuiLabel((Rectangle){x, y - 22.0f*s, full_w, 18*s}, "Provider");
    int active_provider = provider_index(draft.provider);
    int prev_provider = active_provider;
    GuiToggleGroup((Rectangle){x, y, 110*s, row_h}, "openai;anthropic;ollama;custom", &active_provider);
    snprintf(draft.provider, sizeof(draft.provider), "%s", provider_from_index(active_provider));
    if (active_provider != prev_provider)
        apply_provider_defaults(&draft, draft.provider);
    y += row_h + gap + 18.0f*s;

    draw_labeled_text_box("Endpoint", (Rectangle){x, y, full_w, row_h},
                          draft.endpoint, (int)sizeof(draft.endpoint),
                          EDIT_ENDPOINT, s);
    y += row_h + gap + 18.0f*s;

    draw_labeled_text_box("Model", (Rectangle){x, y, half_w, row_h},
                          draft.model, (int)sizeof(draft.model),
                          EDIT_MODEL, s);
    draw_labeled_spinner("Max tokens", (Rectangle){x + half_w + col_gap, y, half_w, row_h},
                         &draft.max_tokens, 1, 200000, EDIT_MAX_TOKENS, s);
    y += row_h + gap + 18.0f*s;

    const char *env_key = getenv("FANGS_API_KEY");
    bool key_from_env = env_key && env_key[0] != '\0';
    GuiLabel((Rectangle){x, y - 22.0f*s, half_w, 18*s}, "API key");
    if (key_from_env)
        GuiDisable();
    if (GuiTextBox((Rectangle){x, y, half_w, row_h}, draft.api_key,
                   (int)sizeof(draft.api_key), editing[EDIT_API_KEY])) {
        begin_edit(EDIT_API_KEY);
    }
    if (key_from_env) {
        GuiEnable();
        GuiLabel((Rectangle){x + half_w + 12.0f*s, y + 6.0f*s, 120.0f*s, 18.0f*s}, "(from env)");
    }

    bool stream_checked = draft.stream;
    GuiCheckBox((Rectangle){x + half_w + col_gap, y + 6.0f*s, 18.0f*s, 18.0f*s},
                "Stream responses", &stream_checked);
    draft.stream = stream_checked;

    Rectangle cancel = {panel.x + panel.width - 202.0f*s, panel.y + panel.height - 54.0f*s, 82.0f*s, 30.0f*s};
    Rectangle save = {panel.x + panel.width - 106.0f*s, panel.y + panel.height - 54.0f*s, 82.0f*s, 30.0f*s};

    if (GuiButton(cancel, "Cancel")) {
        settings_open = false;
        draft_valid = false;
        clear_editing();
    }

    if (GuiButton(save, "Save")) {
        *cfg = draft;
        settings_open = false;
        draft_valid = false;
        clear_editing();
        if (out_saved)
            *out_saved = true;
    }
}
