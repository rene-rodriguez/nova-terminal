// Tests for UI chrome theming (E3 / spec §17).
// Pure no-window tests — UiTheme is derived from Theme, not from the screen.
#include "theme.h"
#include "ui_theme.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

static int failures = 0;

#define EXPECT(expr) do { \
    if (!(expr)) { \
        fprintf(stderr, "FAIL %s:%d: expected true: %s\n", __FILE__, __LINE__, #expr); \
        failures++; \
    } \
} while (0)

#define EXPECT_RGB(c, R, G, B) do { \
    UiColor c__ = (c); \
    if (c__.r != (R) || c__.g != (G) || c__.b != (B)) { \
        fprintf(stderr, "FAIL %s:%d: expected {%d,%d,%d}, got {%d,%d,%d}\n", \
                __FILE__, __LINE__, (R), (G), (B), c__.r, c__.g, c__.b); \
        failures++; \
    } \
} while (0)

// Helper: normalized Euclidean distance in RGB space, 0-1.
static float rgb_dist(const UiColor *a, const UiColor *b) {
    int dr = (int)a->r - (int)b->r;
    int dg = (int)a->g - (int)b->g;
    int db = (int)a->b - (int)b->b;
    float d = sqrtf((float)(dr*dr + dg*dg + db*db)) / 441.67f;  // max dist ≈ 441.67
    return d < 0.001f ? 0.001f : d;
}

// ---------- One Dark (dark theme) ----------
static void test_dark_theme_contrast(void)
{
    Theme dark = theme_resolve("onedark");
    UiTheme ui = ui_theme_derive(&dark);

    // selection must be visibly different from bg
    EXPECT(rgb_dist(&ui.selection, &ui.panel_bg) > 0.08f);

    // panel_border must differ from panel_bg
    EXPECT(rgb_dist(&ui.panel_border, &ui.panel_bg) > 0.02f);

    // search_hit must be visible
    EXPECT(rgb_dist(&ui.search_hit, &ui.panel_bg) > 0.25f);

    // accent must be visible
    EXPECT(rgb_dist(&ui.accent, &ui.panel_bg) > 0.25f);

    // search_bg different from border
    EXPECT(memcmp(&ui.search_bg, &ui.search_border, sizeof(UiColor)) != 0);

    // role tints should all be different from each other
    EXPECT(memcmp(&ui.msg_user, &ui.msg_assistant, sizeof(UiColor)) != 0);
    EXPECT(memcmp(&ui.msg_system, &ui.msg_user, sizeof(UiColor)) != 0);
}

// ---------- One Light (light theme) ----------
static void test_light_theme_contrast(void)
{
    Theme light = theme_resolve("onelight");
    UiTheme ui = ui_theme_derive(&light);

    EXPECT(rgb_dist(&ui.selection, &ui.panel_bg) > 0.08f);
    EXPECT(rgb_dist(&ui.panel_border, &ui.panel_bg) > 0.02f);
    EXPECT(rgb_dist(&ui.search_hit, &ui.panel_bg) > 0.25f);
    EXPECT(rgb_dist(&ui.accent, &ui.panel_bg) > 0.25f);
}

// ---------- Light flips polarity ----------
static void test_light_flips_polarity(void)
{
    Theme dark = theme_resolve("onedark");
    Theme light = theme_resolve("onelight");
    UiTheme u_dark = ui_theme_derive(&dark);
    UiTheme u_light = ui_theme_derive(&light);

    int dark_lum = (int)u_dark.panel_bg.r + (int)u_dark.panel_bg.g + (int)u_dark.panel_bg.b;
    int light_lum = (int)u_light.panel_bg.r + (int)u_light.panel_bg.g + (int)u_light.panel_bg.b;
    EXPECT(light_lum > dark_lum);

    EXPECT(rgb_dist(&u_dark.selection, &u_dark.panel_bg) > 0.05f);
    EXPECT(rgb_dist(&u_light.selection, &u_light.panel_bg) > 0.05f);
}

// ---------- Gruvbox (dark) sanity ----------
static void test_gruvbox_dark(void)
{
    Theme t = theme_resolve("gruvbox");
    UiTheme ui = ui_theme_derive(&t);
    EXPECT(rgb_dist(&ui.selection, &ui.panel_bg) > 0.08f);
    EXPECT(rgb_dist(&ui.accent, &ui.panel_bg) > 0.20f);
}

int main(void)
{
    test_dark_theme_contrast();
    test_light_theme_contrast();
    test_light_flips_polarity();
    test_gruvbox_dark();

    if (failures) {
        fprintf(stderr, "%d ui_theme test failure(s)\n", failures);
        return 1;
    }
    return 0;
}
