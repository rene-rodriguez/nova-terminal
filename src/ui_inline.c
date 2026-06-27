#include "ui_inline.h"

#include <stdio.h>
#include <string.h>

#include "raylib.h"
#include "raygui.h"
#include "ui_theme.h"

#define INLINE_INPUT_SIZE 512
#define INLINE_STATUS_SIZE 160

static InlineState state = INLINE_IDLE;
static int  anchor_x = 0, anchor_y = 0;
static char input_text[INLINE_INPUT_SIZE] = "";
static char status_text[INLINE_STATUS_SIZE] = "";
static char pending_prompt[INLINE_INPUT_SIZE] = "";
static bool prompt_ready = false;
static bool is_error = false;

void ui_inline_open(int ax, int ay)
{
    state = INLINE_INPUT;
    anchor_x = ax;
    anchor_y = ay;
    input_text[0] = '\0';
    status_text[0] = '\0';
    pending_prompt[0] = '\0';
    prompt_ready = false;
    is_error = false;
}

bool ui_inline_active(void) { return state != INLINE_IDLE; }
InlineState ui_inline_state(void) { return state; }

const char *ui_inline_take_prompt(void)
{
    if (!prompt_ready)
        return NULL;
    prompt_ready = false;
    return pending_prompt;
}

void ui_inline_set_waiting(const char *status)
{
    state = INLINE_WAITING;
    is_error = false;
    snprintf(status_text, sizeof(status_text), "%s", status ? status : "thinking…");
}

void ui_inline_set_error(const char *msg)
{
    state = INLINE_WAITING;
    is_error = true;
    snprintf(status_text, sizeof(status_text), "%s", msg ? msg : "request failed");
}

void ui_inline_cancel(void)
{
    state = INLINE_IDLE;
    input_text[0] = '\0';
    status_text[0] = '\0';
    pending_prompt[0] = '\0';
    prompt_ready = false;
    is_error = false;
}

void ui_inline_draw(Font font, float scale)
{
    if (state == INLINE_IDLE)
        return;

    if (IsKeyPressed(KEY_ESCAPE)) {   // ESC cancels from any state
        ui_inline_cancel();
        return;
    }

    float s = (scale > 0.1f) ? scale : 1.0f;   // HiDPI widget scale

    int sw = GetScreenWidth();
    int sh = GetScreenHeight();
    int w = (int)(480 * s), h = (int)(74 * s);
    int x = anchor_x, y = anchor_y;
    if (x + w > sw - 8) x = sw - 8 - w;
    if (x < 8) x = 8;
    if (y + h > sh - 8) y = sh - 8 - h;
    if (y < 8) y = 8;

    Rectangle panel = {(float)x, (float)y, (float)w, (float)h};
    DrawRectangleRec(panel, UI2RAY(g_ui_theme.inline_bg));
    DrawRectangleLinesEx(panel, 1.0f, UI2RAY(g_ui_theme.inline_border));

    DrawTextEx(font, "Ctrl+Space — ask for a command",
               (Vector2){panel.x + 12*s, panel.y + 8*s}, 14.0f*s, 0,
               UI2RAY(g_ui_theme.subtitle));

    Rectangle row = {panel.x + 12*s, panel.y + 32*s, panel.width - 24*s, 28*s};

    if (state == INLINE_INPUT) {
        bool enter = IsKeyPressed(KEY_ENTER);
        int prev_ts = GuiGetStyle(DEFAULT, TEXT_SIZE);
        GuiSetStyle(DEFAULT, TEXT_SIZE, (int)(16 * s));
        GuiTextBox(row, input_text, INLINE_INPUT_SIZE, true);
        GuiSetStyle(DEFAULT, TEXT_SIZE, prev_ts);
        DrawTextEx(font, "Enter to stage · Esc to cancel",
                   (Vector2){panel.x + 12*s, panel.y + h - 16*s}, 11.0f*s, 0,
                   UI2RAY(g_ui_theme.subtitle));
        if (enter && input_text[0] != '\0') {
            snprintf(pending_prompt, sizeof(pending_prompt), "%s", input_text);
            prompt_ready = true;
            state = INLINE_WAITING;
            snprintf(status_text, sizeof(status_text), "thinking…");
        }
    } else {  // INLINE_WAITING (possibly showing an error)
        Color c = is_error ? UI2RAY(g_ui_theme.inline_error)
                           : UI2RAY(g_ui_theme.text);
        DrawTextEx(font, status_text[0] ? status_text : "thinking…",
                   (Vector2){row.x, row.y + 4*s}, 16.0f*s, 0, c);
        DrawTextEx(font, "Esc to dismiss",
                   (Vector2){panel.x + 12*s, panel.y + h - 16*s}, 11.0f*s, 0,
                   UI2RAY(g_ui_theme.subtitle));
    }
}
