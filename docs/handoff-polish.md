# Polish Round Handoff — E3–E7 (UI theming, crisp text, errors, sidebar, macOS)

> **Audience:** whoever executes the polish round (you, now).
> **Prereq:** v1 + E1/E2 foundations are in — builds/tests/run clean; 11 ctest suites green.
> This round is **refinement only** (no new surfaces; tabs/splits §16 are out of scope).
> **Spec:** `spec.md` §17–§21.  **Plan steps:** `plan.md` §7.2 (E3–E7).  **Date:** 2026-06-26.

---

## 0. The thesis (why this round exists)

Three gaps, found by reading the code, unify the whole round:

1. **The engine is themed; the app chrome is not.** `Theme` (`src/theme.h`: `bg`, `fg`, `cursor`,
   `ansi[16]`, `is_light`) only colors terminal output. The sidebar, search bar, selection, scrollbar,
   and cursor alpha are **hardcoded dark literals** in `main.c`/`ui_sidebar.c`, so every *light* theme is
   unreadable. → **E3** fixes this and is the keystone; E4–E7 draw their colors from it.
2. **Text styling is faked while the engine hands us the truth.** The cell loop (`src/main.c`
   ~1250–1263) reads only `bold/italic/inverse` and fakes them (bold = draw-twice; italic = shear; no
   underline). `GhosttyStyle` actually carries `bold, italic, faint, blink, inverse, invisible,
   strikethrough, overline, underline (+ underline_color)`, and the render state carries full cursor
   style/blink. → **E4**.
3. **Failures only reach stderr** — invisible in `Fangs.app`. → **E5** (toasts + first-run card).

E6 (sidebar delight) and E7 (macOS distribution) are independent quality wins on top.

**Build & iterate:** `cmake --build build`; `ctest --test-dir build --output-on-failure`;
`bash scripts/macos-build.sh` for a clean build. Run: `./build/fangs`.

---

## 1. Sequencing & what you reuse

**Do E3 first.** Everything else colors its chrome from `UiTheme`. After that, E4–E7 in any order.

```
src/
  theme.{c,h}        # Theme{bg,fg,cursor,ansi[16],is_light} + theme-apply path  (E3 derives from this)
  config.{c,h}       # AppConfig (tiny today); resolve_api_key lives in main.c    (E4/E5/E7 extend)
  main.c             # the render loop (~1180–1340), input router, the 5 stderr   (E3/E4/E5/E6/E7)
                     #   failure sites, LoadFontFromMemory(~109), 800x600(~1757)
  ui_sidebar.{c,h}   # panel bg(~287) + role tints(~61-63); prefill/oneshot ctx   (E3/E5/E6)
  ai_block.{c,h}     # ai_block_build_context() (from E1)                          (E6 REUSE as-is)
  cmdblocks.{c,h}    # tracks OSC-133 blocks; latest-block lookup                  (E6 REUSE)
  redact.{c,h}       # secret scrub before anything leaves the machine (§7.3)      (E6 REUSE)
CMakeLists.txt       # single-font bin2header block (~46–57)                       (E4 generalizes)
packaging/macos/fangs.rb, scripts/macos-bundle.sh, .github/workflows/release.yml  (E7)
```

**Engine facts you'll rely on (verified against the pinned headers):**
- Cell style — `build/_deps/ghostty-src/include/ghostty/vt/style.h`: `GhosttyStyle{ bold, italic, faint,
  blink, inverse, invisible, strikethrough, overline; int underline /*GHOSTTY_SGR_UNDERLINE_**/;
  GhosttyStyleColor underline_color; }`.
- Cursor — `build/_deps/ghostty-src/include/ghostty/vt/render.h`:
  `GHOSTTY_RENDER_STATE_DATA_CURSOR_VISUAL_STYLE` → `…CURSOR_VISUAL_STYLE_{BAR,BLOCK,UNDERLINE,BLOCK_HOLLOW}`;
  `…CURSOR_BLINKING`; `…CURSOR_PASSWORD_INPUT`; `…CURSOR_VISIBLE`; `…CURSOR_VIEWPORT_{HAS_VALUE,X,Y}`.
  (The current renderer at `main.c` ~1293–1308 ignores all of `VISUAL_STYLE`/`BLINKING`.)

---

## 2. E3 — UI theming (`src/ui_theme.{c,h}` — new, pure)  *keystone*

**Mirror `theme.h`: keep the header raylib/ghostty-free** so it unit-tests without a window. Return a
small struct of chrome colors derived from `Theme`; convert to raylib `Color` at the draw site.

```c
// ui_theme.h
typedef struct { unsigned char r, g, b, a; } UiColor;
typedef struct {
    UiColor panel_bg, panel_border, selection, search_bg, search_border, search_hit, scrollbar;
    unsigned char cursor_alpha;
    UiColor msg_user, msg_assistant, msg_system, accent;
} UiTheme;
UiTheme ui_theme_derive(const Theme *t);   // pure: blend toward fg over bg; flip on is_light
```

1. Write `tests/ui_theme_tests.c` **first** (register in `CMakeLists.txt` + `ctest`): for a known light
   and dark theme, assert minimum contrast vs `bg`, `selection != bg`, and that `is_light` flips polarity.
2. Implement `ui_theme_derive`. Don't over-engineer the blend — a couple of fixed ratios toward `fg`
   plus an `is_light` branch is enough.
3. Replace every hardcoded literal (spec §17.3) with a `UiTheme` lookup: `main.c` selection
   `(120,145,205,90)`, search hit `(235,200,90,120)`, search box bg/border, scrollbar
   `(200,200,200,128)`, cursor alpha `128`; `ui_sidebar.c` panel bg `(28,30,34,255)` + role tints;
   `ui_settings.c` modal bg; `ui_inline.c` overlay/text; `cmdblocks.c` gutter/badge/button/accent.
4. **Recompute the `UiTheme` on the same path that already calls `term_engine_apply_theme`** — both
   initial load *and* `Ctrl+,` hot-reload. (Easy to forget the hot-reload case → light theme half-applies.)

---

## 3. E4 — crisp text & cursor (`src/main.c` render path)

1. **Bold face.** Add `assets/JetBrainsMono-Bold.ttf`. Generalize the single-font `bin2header` block in
   `CMakeLists.txt` (~46–57) to also emit `font_jetbrains_mono_bold.h`. Load `Font bold_font` beside the
   regular load (~109); use it when `style.bold`; **delete the draw-twice hack** (~1259–1261); `UnloadFont`
   it in cleanup (beside `mono_font` ~2532). **Verify `cell_width` is unchanged** — JBM Bold shares the
   regular advance width, but confirm the grid doesn't shift.
2. **Decorations** in the cell loop, theme-`fg` colored (or `underline_color` when set): `underline`
   (single/double/curly per `GHOSTTY_SGR_UNDERLINE_*`), `strikethrough` (mid), `overline` (top), `faint`
   (blend fg ~50% → bg), `invisible` (skip the `DrawTextEx`). Keep the existing italic shear or replace
   it — your call; bold is the one that must become real.
3. **Cursor** (replace ~1293–1308): read `…CURSOR_VISUAL_STYLE` → draw BAR (thin left bar) / BLOCK
   (filled, `cursor_alpha`) / UNDERLINE (bottom bar) / hollow (outline). Blink off a frame timer **only
   when `…CURSOR_BLINKING`**. Draw `BLOCK_HOLLOW` whenever `!IsWindowFocused()`.
4. **Config:** add `cursor_style_default` + `cursor_blink` to `AppConfig` (`config.{h,c}` + defaults that
   preserve today). Verification is visual — no new test module needed.

---

## 4. E5 — error surfacing & first-run (`src/toast.{c,h}` — new + sidebar card)

1. Write `tests/toast_tests.c` **first** (register in CMake/ctest): over-capacity drops oldest, TTL
   expiry removes, `toast_get` returns newest-first with decreasing alpha.
2. `toast.{c,h}`: `toast_push(level,msg)` (copies; bounded ring), `toast_tick(dt)`, `toast_count()`,
   `toast_get(i,&level,&msg,&alpha)`. **Logic pure** (testable); **drawing lives in `main.c`** (fading
   pills, bottom-right, colored from `UiTheme`).
3. Route the five stderr-only sites (`main.c` ~1748 config-load, ~1815 font-load, ~1826 engine-create,
   ~2049 config-save, ~2481 config-apply) and AI errors (`ai_http.c` / `ui_sidebar.c`) through
   `toast_push` **in addition to** the existing `fprintf(stderr…)`.
4. **First-run card:** when `resolve_api_key(&cfg)` finds nothing (env or file) and the sidebar opens,
   render a card in `ui_sidebar.c` — a line on `FANGS_API_KEY` + a button that opens settings (`Ctrl+,`)
   — instead of the dead input. **Never writes a key.**
5. Add `docs/config.example` (every `AppConfig` field, default, one-line note) and link it from README.

---

## 5. E6 — AI sidebar polish (`src/ui_sidebar.{c,h}`)

1. **Syntax-highlight fenced code:** detect ```lang fences during the existing word-wrap pass (~98) and
   tint a heuristic token set (keywords/strings/comments/numbers) in a mono box. `UiTheme`-colored.
   Heuristic only — no real parser.
2. **Smooth auto-scroll:** while tokens stream, lerp the scroll offset toward the bottom each frame; stop
   if the user scrolled up. Replaces any hard snap.
3. **Copy-whole-reply** button per assistant message (beside the per-fence Run/Copy).
4. **"Ask AI about the last command"** — new keybinding (`Cmd+Shift+/`) in `main.c` input handling,
   beside the existing `Ctrl+,` / `Cmd+B` / `Ctrl+Space` intercepts. Find the latest `cmdblocks` block,
   run its output through `redact` (§7.3), call `ai_block_build_context()` →
   `ui_sidebar_set_oneshot_context()` + `ui_sidebar_prefill(ai_block_default_question(exit_code))`, then
   open/focus the sidebar. **This is literally the hover "Ask AI" path (§15.5), keyboard-driven.** No
   auto-send. If there's no block, no-op (or a toast).

---

## 6. E7 — macOS distribution & window polish

1. **Notarization — gated on a paid Apple Developer ID.** Today `packaging/macos/fangs.rb` ships an
   ad-hoc-signed zip with an `xattr`/right-click→Open caveat; `scripts/macos-bundle.sh` and
   `.github/workflows/release.yml` do no real signing. Add Developer ID `codesign` + hardened-runtime
   entitlements plist + `notarytool submit --wait` + `stapler staple`; then drop the `xattr` caveat from
   the cask. **Needs CI secrets (Developer ID, app-specific password / App Store Connect API key,
   entitlements).** If you don't have the account yet, ship step 2 and leave a stub + TODO — don't block
   the round on it.
2. **Window polish (no account):** persist window size/position on resize/move and restore on startup
   (replace hardcoded `800×600`, `main.c` ~1757) — a small state file or new `AppConfig` fields.
   `SetMouseCursor` → I-beam over terminal text, pointer over buttons/links, default elsewhere (key off
   the hover regions already computed each frame).

---

## 7. Gotchas (read before coding)

- **Keep `ui_theme.h` and `toast.h` raylib-free** (mirror `theme.h`) so their tests stay pure; convert
  `UiColor` → raylib `Color` at the draw site. This is the whole reason they're testable.
- **Don't add ~20 color knobs to config — derive.** The only new config in this round is the two cursor
  fields (E4) and optional window-geometry (E7).
- **Recompute `UiTheme` on hot-reload, not just startup** (E3 step 4) — the easy miss.
- **Bold must not shift the grid** (E4): confirm `cell_width` is identical with the bold face loaded.
- **Cursor: blink only when the engine says so** (`CURSOR_BLINKING`); hollow on `!IsWindowFocused()`.
- **All `ui_*`/`toast_*` calls run on the main thread.** The AI worker thread only fills its mutex
  buffer — unchanged. Don't push toasts from the worker.
- **Redaction still applies** on the E6 "ask about last command" path before anything leaves the machine.
- **Invariants hold throughout:** PTY byte stream is observed only, nothing auto-executes, secrets go
  only to the configured endpoint. None of E3–E7 touches the `term_engine` / `ai_provider` seams.

---

## 8. Acceptance checklist (whole round)

- [ ] **E3:** switch to a light theme via `Ctrl+,` → sidebar, search bar, selection, scrollbar, cursor all
      readable; dark unchanged; `ui_theme_tests` green.
- [ ] **E4:** `printf` SGR samples + `vim` render real bold glyphs + underline (single/double/curly) +
      strikethrough + overline + faint + invisible; cursor switches block/bar/underline, blinks when
      asked, goes hollow on focus loss.
- [ ] **E5:** bad AI endpoint → error toast (not silence); corrupt config → load toast + defaults;
      no key → connect-key card; `toast_tests` green; `docs/config.example` exists + linked.
- [ ] **E6:** fenced replies render highlighted; streaming auto-scrolls and yields to manual scroll-up;
      copy-reply works; `Cmd+Shift+/` prefills the last block's context (no-op when no blocks).
- [ ] **E7:** relaunch restores window geometry; cursor changes on hover; if notarized,
      `spctl -a -vvv build/Fangs.app` = accepted and a fresh download opens without the right-click dance.
- [ ] `ctest --test-dir build` green (existing 11 + `ui_theme_tests` + `toast_tests`);
      `cmake --build build` warning-clean; `vim`/`htop` still fine.
