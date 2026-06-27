# Project Plan: Fangs — The Native Anti-Warp Terminal

> **Status:** Planning · **Last updated:** 2026-06-20
> **Engine decision:** Path A — `libghostty-vt` + Raylib (C). See [§3](#3-architecture-decision).
> **Companion document:** [`docs/spec.md`](./spec.md) — the detailed technical specification.

A native, hardware-accelerated terminal emulator built on top of `libghostty-vt`. It
replicates the most-requested AI features of Warp (inline command generation, an AI chat
sidebar, terminal context awareness) while eliminating Warp's pain points: forced cloud
logins, telemetry, subscriptions, and non-standard TTY block rendering.

> **Naming note:** Earlier drafts called this "Ghostling-BYOK," then "Ghosttly"; the project is
> now **Fangs** (GitHub: `rene-rodriguez/fangs`). The binary, config dir
> (`~/.config/fangs`), and API-key env var (`FANGS_API_KEY`) all use the `fangs`
> name. Not to be confused with the upstream project we build on, `ghostty-org/ghostling`.

---

## 1. Project Overview

Fangs is a single-binary, local-first terminal for **CachyOS/Linux and macOS** (both first-class targets).
It pairs a best-in-class VT engine (Ghostty's, via `libghostty-vt`) with a Raylib-rendered
UI and a thin **BYOK** AI layer that talks directly to the model provider of your choice.

The whole product is three AI features grafted onto a real terminal:

1. **AI context sidebar** — chat that can read your own scrollback.
2. **Inline command generation** (`Ctrl+Space`) — natural language → a shell command staged at your prompt.
3. **Dual-sync configuration** — an `ini` dotfile that is the source of truth, plus a GUI modal.

## 2. The Anti-Warp Philosophy

These tenets exist to solve the community's actual grievances with Warp:

- **Zero telemetry & no logins.** A pure local executable. You never create an account to use your own terminal.
- **True BYOK.** The client connects directly to your chosen provider (Anthropic, OpenAI, or any OpenAI-compatible endpoint, including local models via Ollama / llama.cpp). Keys live on your disk, nowhere else.
- **Open, audited core engine.** By building on `libghostty-vt`, the VT layer is standard, inspectable, and community-maintained.
- **Standard TTY behavior.** We do **not** intercept stdin to fabricate visual "blocks." The PTY loop stays pure; AI features are overlays in the Raylib render pipeline, never in the byte stream.

---

## 3. Architecture Decision

We evaluated three engines. **Path A was chosen.**

| Path | Engine | Verdict |
|---|---|---|
| **A (chosen)** | `libghostty-vt` + Raylib, C | Ghostty-grade VT; `ghostty-org/ghostling` already wires `forkpty` + render loop + input encoding in this exact stack; ships a **formatter API** purpose-built for dumping scrollback to an LLM. |
| B | `libvterm` + Raylib, C | Stable and Zig-free, but a weaker VT (no kitty graphics / ligatures) and you hand-roll all context extraction. |
| C | `alacritty_terminal`, Rust | The most battle-tested engine and a far nicer async networking story — but a full stack pivot away from C/Raylib, *and* it still has no formatter, so it loses A's key advantage. |

### Why A

- **It already exists, working.** `ghostty-org/ghostling` is an official minimal reference terminal built on the libghostty C API. It is single-file C, CMake 3.19+ / Ninja, and **already uses Raylib**. It hands us `forkpty()`, a non-blocking PTY read loop, `TIOCSWINSZ` resize, key/mouse encoding via Ghostty's encoders, and a render-state cell-iteration draw loop — i.e. most of Phases 1–3 for free.
- **The formatter API is the deciding factor.** `libghostty-vt` ships `<ghostty/vt/formatter.h>` ("format terminal content as plain text, VT sequences, or HTML"). That is almost exactly the AI-context-extraction primitive Feature B needs. Neither B nor C gives us this.
- **It matches the philosophy.** Ghostty is the gold standard for a standard, audited VT.

### The two known risks — and how we contain them

1. **`libghostty-vt`'s C API is unstable** (no tagged release; breaking changes expected).
   → **Mitigation:** vendor it pinned to a known-good commit SHA, documented in the spec. Upgrades are deliberate, tested events — never tracking `main`.
2. **Streaming AI in C is the annoying part** (libcurl + SSE parsing + JSON), versus Rust's `reqwest`/`serde`.
   → **Mitigation:** it is *bounded to one module* (`ai_http.c`): a libcurl worker thread using `CURLOPT_WRITEFUNCTION` for streaming, a ~40-line SSE line splitter, and vendored single-header **cJSON**. Immediate-mode UI is a natural fit for streaming — we just redraw the growing buffer each frame.

### The architectural move that keeps a pivot cheap

Two narrow seams isolate both risks so neither is ever a rewrite:

- **`term_engine.h`** — wraps `libghostty-vt` behind `spawn / write / resize / read_render_state / dump_text`. If the API breaks badly, or we later want libvterm (B) or a Rust core (C), we swap *one* implementation.
- **`ai_provider.h`** — wraps `ai_http.c` behind `send(messages, on_token)`. Swapping providers or the HTTP layer touches one file.

This is the concrete form of "fail early with A, then pivot rather than start from zero."

---

## 4. Tech Stack

- **Terminal engine:** `libghostty-vt` (VT parsing, terminal state, render-state + formatter APIs). Pinned commit; built via **Zig 0.15.x**.
- **Bootstrap reference:** `ghostty-org/ghostling` (`main.c`) — starting skeleton, then refactored into modules.
- **Graphics / GUI:** C + Raylib (GPU-accelerated 2D) + RayGUI (immediate-mode widgets).
- **Networking:** `libcurl` (streaming via write-callback on a worker thread).
- **JSON:** vendored single-header **cJSON**.
- **Concurrency:** POSIX threads (one AI worker thread + mutex-guarded token buffer).
- **Build:** CMake + Ninja. **Requires Zig 0.15.x on PATH.** Target: Arch Linux / CachyOS.

## 5. Module Structure

Ghostling's single `main.c` is the bootstrap, but we grow into modules from day one:

```
src/
  main.c          # event loop, frame orchestration, input routing
  term_engine.h/.c# SEAM: wraps libghostty-vt (spawn/write/resize/render/dump_text)
  pty.c           # forkpty, non-blocking read, TIOCSWINSZ (lifted from ghostling)
  render.c        # Raylib draw: split layout, cell grid, font atlas
  config.c        # ini parse/write -> AppConfig struct; hot reload
  ai_provider.h   # SEAM: send(messages, on_token) contract
  ai_http.c       # libcurl worker thread, SSE parser, cJSON (implements ai_provider)
  context.c       # scrollback -> text via formatter API (with render-state fallback)
  ui_sidebar.c    # RayGUI chat panel (history + input + Run buttons)
  ui_settings.c   # RayGUI Ctrl+, settings modal
  ui_inline.c     # Ctrl+Space floating prompt + PTY injection
```

---

## 6. Feature Architecture

### A. Dual-Sync Configuration (Dotfile + GUI)

- **The dotfile** is the single source of truth: an `ini` at `~/.config/fangs/config`, holding both terminal settings (font, colors) and AI settings (provider, endpoint, model, system prompts). The API key is read from an **env var by default** (`FANGS_API_KEY`), with the file as a fallback — secrets should not be the first thing committed by accident.
- **The GUI overlay:** `Ctrl+,` draws a RayGUI modal over the terminal to edit provider, endpoint, model, and key visually.
- **Hot reload:** "Save" writes the `AppConfig` struct back to the `ini` and re-applies live (font/colors/AI settings) without restart.

### B. The AI Context Sidebar

- **Layout:** the Raylib window splits ~75% terminal grid / ~25% chat panel.
- **Context extraction:** `context.c` uses the `libghostty-vt` **formatter API** to dump the visible screen + recent scrollback as plain text to feed the model (render-state row/cell iteration is the proven fallback). Default capture: last N lines (configurable), redaction pass before send.
- **Execution hook:** model responses containing a fenced command render a RayGUI **Run** button that injects the bytes straight into the PTY fd — staged at the prompt, never auto-executed.

### C. Inline Command Generation (`Ctrl+Space`)

A small state machine, no stdin interception of the real shell:

1. **Intercept:** `Ctrl+Space` sets `inline_mode = true` and opens a floating RayGUI input near the cursor. Normal keys route to that box, not the PTY.
2. **Prompt:** user types naturally (e.g. *"undo last git commit"*).
3. **Process:** sent via `ai_provider` with a strict system prompt: return *only* the raw command, no prose, no fences.
4. **Inject:** the response is `write()`-en to the PTY fd as if typed. It sits at the prompt for the user to review and press **Enter**. We never press Enter for them.

---

## 7. Development Roadmap

> Sequencing principle: **front-load the two real risks, then build terminal-first.** AI is additive and must never block a working base.

### Phase 0 — Risk Spike (days, not weeks)

The two things we're actually unsure about, proven before committing to the full build:

- **0a. Engine builds & pins.** Clone `ghostling`, pin `libghostty-vt` to a specific commit, and get a clean build + run with Zig 0.15.2. **Exit:** a working terminal window from the pinned source.
  - **STATUS (2026-06-20): ✅ DONE on macOS.** Pins resolved (ghostling `f9034e4`, ghostty/libghostty-vt `ae52f97`, raylib `5.5`, Zig `0.15.2`); cmake/ninja/zig set up; full build succeeds → `vendor/ghostling/build/ghostling` (1.2 MB Mach-O arm64), reproducible via `scripts/macos-build.sh`. A macOS-26.5/Xcode-26 toolchain quirk (Zig 0.15.2 can't parse the new SDK's `libSystem.tbd`; initially looked like a hard block but was a poisoned Zig cache) is handled by that script (hybrid SDK + `xcrun` shim); see `spec.md` §4.1. **Runtime smoke test passed** — window opens, libghostty-vt + raylib(GLFW/Cocoa) + OpenGL/Metal on Apple M2 init clean. **Still to do:** verify on real CachyOS/Linux hardware (expected clean); run Phase 0b (streaming-AI-in-C spike).
- **0b. Streaming AI in C works end-to-end.** A throwaway `ai_http.c` that streams tokens from one hardcoded prompt into a Raylib window via the worker-thread + ring-buffer model. **Exit:** tokens visibly stream in.
  - **STATUS (2026-06-20): ✅ PROVEN.** `spike/ai_stream/stream_test.c` streamed live from a hosted OpenAI-compatible reasoning model: HTTP 200, 68 deltas, exit 0 — libcurl `CURLOPT_WRITEFUNCTION` + SSE line-split + cJSON, plain C. The Raylib + worker-thread + mutex-buffer variant (`stream_window.c`) renders thinking→answer live, proving the concurrency/UI model. **Design finding:** reasoning models stream `delta.reasoning_content` (thinking) separately from `delta.content` (answer) — the sidebar must treat them as distinct regions.

If either spike fails, we pivot here — having spent days, not weeks. **Both 0a and 0b passed → architecture de-risked.**

### Phase 1 — Engine & Baseline ✅ DONE (2026-06-20)

- Adopt ghostling as the bootstrap; introduce `term_engine.h` seam around it.
- Verify the PTY loop, resize, input encoding, and cell rendering on the target OS.
- **Exit:** a terminal you can daily-drive (no AI yet).
- **STATUS: complete.** Real `src/` project: `main.c` (window/input/render/effects/loop), `pty.{c,h}` (forkpty plumbing, engine-agnostic via a sink callback), `term_engine.{c,h}` (THE SEAM — owns all libghostty-vt handles + lifecycle + `term_engine_dump_text()` formatter for Phase 4). Root `CMakeLists.txt` (portable; FetchContent raylib 5.5 + ghostty `ae52f97`, bin2header font from `assets/`). Builds via `scripts/macos-build.sh` → `build/fangs` (1.2 MB Mach-O arm64) and **runs clean** (ghostty-vt + raylib/Cocoa/OpenGL init, embedded JetBrains Mono). Ghostling stays in `vendor/` as reference only. Next: **Phase 2 handoff** in `docs/handoff-phase2.md`.

### Phase 2 — Configuration & GUI Modal ✅ DONE (2026-06-21)

- `config.c`: parse `~/.config/fangs/config` into `AppConfig`; env-var key precedence.
- RayGUI settings modal on `Ctrl+,`; Save writes back to disk + hot-reloads.
- **Exit:** change font/provider/model from the GUI and from the file, both take effect live.
- **STATUS: complete.** `config.{c,h}` (hand-rolled INI parser, `0600` perms, env-key precedence, defaults-on-missing), `ui_settings.{c,h}` (RayGUI modal: font/theme/scrollback + AI provider/endpoint/model/key/stream/max-tokens, draft-then-Save, ESC/Cancel to discard, greys out the key field when `FANGS_API_KEY` is set), wired into `main.c` (intercept `Ctrl+,`/`Cmd+,` before PTY forwarding, gate input while open, hot-reload `font_size` → font reload + grid resize + `pty_set_winsize`, theme→window-bg live). `tests/config_tests.c` (4 cases: defaults, missing-file-creates, INI parse, save round-trip) wired into `ctest` — all pass. Builds warning-clean.
- **Bug fixed during review (2026-06-21):** `SetExitKey(KEY_NULL)` was missing — raylib's default exit key is **ESC**, so pressing ESC closed the whole terminal (fatal for vim/TUIs and for dismissing the modal). Now ESC passes through to the child and only dismisses the modal; the loop ends solely on the window close button.

### Phase 3 — Split Layout & Sidebar UI ✅ DONE (2026-06-21) → see `docs/handoff-phase3.md`

- **The layout seam:** replace the terminal path's direct `GetScreenWidth/Height` reads with a single `Layout` (terminal rect + sidebar rect, derived from window size + sidebar state). Grid math, the scrollbar, and mouse mapping all key off `layout.terminal`, not the window — this is the spine of the phase.
- Constrain the terminal draw to the left (`BeginScissorMode`); build the right-hand RayGUI chat panel (scrollable history + input box + Send), toggled by a chord (`Cmd+B` / `Ctrl+Shift+B`).
- **Focus model (the subtle part):** the sidebar can be *visible-but-unfocused* (terminal still types) vs *focused* (input box captures keys). Unlike the Phase 2 modal, visibility ≠ blocking — only sidebar **focus** gates PTY input.
- **Exit:** a (not-yet-wired) chat panel renders alongside a working terminal; typing in it and submitting echoes into the history (labelled "AI not wired — Phase 4"); the terminal stays fully usable while the panel is visible.
- **STATUS: complete.** `layout.{c,h}` (pure `layout_compute`), `ui_sidebar.{c,h}` (panel: scrollable wrapped history, input + Send, mouse-wheel scroll, auto-scroll-on-submit, scissor-clipped), `ui_sidebar_model.{c,h}` (pure predicates `ui_sidebar_should_submit` + `ui_sidebar_allows_pty_input`), wired into `main.c` (per-frame layout, `Cmd+B`/`Ctrl+Shift+B` chord, reflow-only-on-change, terminal scissor, click-to-(un)focus). Terminal path now threads `term_area_w` through `compute_terminal_grid`/`handle_mouse`/`handle_scrollbar`/`render_terminal`. `tests/layout_tests.c` + `tests/ui_sidebar_model_tests.c` added to `ctest` (3 suites, all pass). A gated headless smoke harness (`FANGS_PHASE3_SMOKE_REPORT`/`_SCREENSHOT`) verifies the split/grid/focus without a display. Builds warning-clean; reviewed 2026-06-21 — no bugs found, ESC fix intact.

### Phase 4 — Context Extraction & Networking ✅ DONE (2026-06-21) → see `docs/handoff-phase4.md`

- **The threading spine:** promote `spike/ai_stream/stream_window.c` (already proven) into `ai_http.c` behind `ai_provider.h`. The worker pthread fills a **mutex-guarded buffer**; the raylib main loop **drains it each frame** via a poll API and appends to the sidebar — *no raylib/UI call ever happens off the main thread.* Reasoning models split `delta.reasoning_content` (thinking) from `delta.content` (answer) — keep them as distinct regions.
- `context.c`: capture recent terminal text via `term_engine_dump_text()` (already built), trim to a byte budget, **mandatory redaction pass** (keys/tokens/passwords) before send; assemble system+context+user messages.
- **Run buttons:** parse fenced commands out of the answer → a **Run** button injects the bytes via `pty_write()` *staged at the prompt, never with a newline* — the user presses Enter.
- Key resolution: `FANGS_API_KEY` env wins over `cfg.api_key`; never logged.
- **Exit:** ask the sidebar about an on-screen error and get a streamed, context-aware answer.
- **STATUS: complete.** `ai_provider.h` (seam) + `ai_http.c` (worker pthread + mutex buffer + libcurl, poll API `ai_stream_start/poll/error/cancel/free`), `sse.{c,h}` (pure SSE/JSON delta parser, partial-line-safe, reasoning+content), `context.{c,h}` + `redact.{c,h}` (`term_engine_dump_text` → `text_tail` → `redact_secrets`), `cmdextract.{c,h}` (single-line fenced command only — multi-line refused for safety), vendored `cJSON.c`. Sidebar gained streaming append (`begin/append/end_assistant`, dimmed reasoning region) + **Run** buttons; `main.c` drains each frame, interrupts in-flight on a new question, frees/joins on close, `FANGS_API_KEY` wins. Build links libcurl + pthreads; **6 ctest suites pass** (added sse/redact/cmdextract). Worker lifecycle (spawn → poll → graceful error → clean join → no-key guard) verified offline against a dead endpoint; live streaming uses the same curl/SSE code proven in Phase 0b. Needs the user's `FANGS_API_KEY` to exercise a real answer end-to-end.

### Phase 5 — Inline AI Generation ✅ DONE (2026-06-21) → see `docs/handoff-phase5.md`

- `ui_inline.c`: `Ctrl+Space` interceptor, floating input near the cursor, focus capture, a strict "return only the raw command, no prose, no fences" system prompt, staged PTY injection.
- **Reuses Phase 4 wholesale:** the `ai_provider` seam, `resolve_api_key`, the worker/poll drain pattern, and `pty_write`-staged-no-newline injection all already exist — Phase 5 is mostly a second, smaller input surface + a one-line-command system prompt.
- **Exit:** type "undo last git commit", review the staged command, press Enter yourself.
- **STATUS: complete.** `ui_inline.{c,h}` (floating prompt, `INLINE_IDLE→INPUT→WAITING` state machine, anchored at the cursor via a new `cursor_pixel` helper over `CURSOR_VIEWPORT_X/Y`, ESC cancels from any state) + `inline_cmd.{c,h}` (pure `inline_sanitize_command` — strips fences/backticks/`$ ` prompt markers, single line only). `main.c` intercepts `Ctrl+Space` before PTY forwarding, gates input while active, runs a **separate** `inline_stream` (drained each frame, answer-only), and on completion stages the sanitised command via `pty_write` with **no newline**; both workers joined on close. **7 ctest suites pass** (added `inline_cmd_tests`); builds warning-clean; GUI smoke test green. Live generation needs the user's `FANGS_API_KEY`.

---

> **🎉 v1 FEATURE-COMPLETE (2026-06-21):** all five feature phases done. Fangs is a native,
> GPU-accelerated, BYOK terminal with a context-aware AI sidebar and inline `Ctrl+Space` command
> generation — no logins, no telemetry, no subscription.
>
> **v1 polish — done (2026-06-21):**
> - **Theming** (`theme.{c,h}` + `term_engine_apply_theme`): full **engine-backed** theming.
>   `cfg.theme` pushes the default fg/bg/cursor **and the complete 256-color palette** into
>   libghostty-vt via `ghostty_terminal_set(GHOSTTY_TERMINAL_OPT_COLOR_*)`, so *all* output is themed
>   — `ls --color`, vim, prompts, 256-color apps — applied live on Save. A theme **selector**
>   (`Ctrl+,` → Theme combo box) offers **One Dark, Dark Modern, GitHub Dark, Gruvbox, Monokai** and
>   light themes **One Light, Light Modern, GitHub Light, Gruvbox Light**, backed by a `theme.c`
>   registry (add a theme = one `static const Theme` + a registry row). The RayGUI in-app UI
>   (settings modal, sidebar/inline widgets) restyles itself to match the active theme via
>   `apply_gui_style()`, so it's never a light panel floating over a dark terminal.
>   *(Correction: the engine **does** expose color setters — `OPT_COLOR_PALETTE`/`_FOREGROUND`/… in
>   `terminal.h`. An earlier note here said it didn't; a first-pass header grep was too narrow. The
>   initial render-layer override was replaced with this proper engine path.)*
> - **Multi-turn chat:** the sidebar now sends recent history (last ~10 turns, via
>   `ui_sidebar_count/role/text`) as `user`/`assistant` messages, with terminal context attached to
>   the current question only.
> - **Sidebar font sizing:** RayGUI widgets sized to match the message body (16px) now that they're
>   on JetBrains Mono; roomier input row.
>
> - **Terminal essentials (2026-06-21):** mouse text selection + copy/paste (bracketed-paste safe),
>   `Ctrl`/`Cmd`+click to open URLs, and `Ctrl+F` find-in-view. OSC 133 shell integration documented
>   in `docs/shell-integration.md` — the engine tracks command/output boundaries once the shell emits
>   marks (also improves AI-context scoping).
>
> **CachyOS/Linux verification (2026-06-22):** ✅ builds clean from the pinned source on CachyOS
> (x86-64, Zig 0.15.2 — note `pacman` ships 0.16.0, which won't build the pin; fetch 0.15.2 from
> ziglang.org) → 1.8 MB ELF; **all 8 ctest suites pass.** One Linux-only bug found & fixed: the
> project's `src/pty.h` shadowed glibc's `<pty.h>` on the `-Isrc` angle-bracket path, so `forkpty`
> was undeclared (macOS uses `<util.h>`, never hit it) — now the Linux branch declares the prototype
> directly. Engine + raylib init clean; the GUI window itself still needs a graphical session to open
> (the dev box is headless), so on-screen feature confirmation is the one remaining check.
>
> **Anthropic-native provider (2026-06-22):** ✅ done. The `ai_provider`/`ai_http` seam branches by
> `cfg.provider`: selecting **anthropic** builds the native `/v1/messages` body (system hoisted to
> the top-level `system` field, `max_tokens` always sent), authenticates with `x-api-key` +
> `anthropic-version: 2023-06-01`, and `sse.c` auto-detects Anthropic `content_block_delta` events
> (`text_delta`/`thinking_delta`) vs OpenAI `choices[].delta` by JSON shape. The settings provider
> toggle prefills the Anthropic endpoint + `claude-opus-4-8` on switch. Unit-tested (new SSE case);
> live end-to-end needs the user's `FANGS_API_KEY`.
>
> **Packaging — AUR (2026-06-22):** ✅ `packaging/aur/` ships a verified VCS `PKGBUILD`
> (`fangs-git`) + `.desktop` + `.SRCINFO`. It fetches the pinned Zig 0.15.2 (build-time only,
> since `pacman` ships the wrong 0.16.0), builds via CMake, runs the 8 ctest suites in `check()`, and
> installs the binary + the bundled `libghostty-vt.so` (in `/usr/lib`, no RPATH/`$srcdir` leak) + the
> launcher + font license. Verified end-to-end with `makepkg` on CachyOS. Caveats: x86-64 only,
> network-at-build (FetchContent), and the GitHub repo must be public before AUR consumers can clone.
>
> **Command-block UI (2026-06-22):** ✅ done. Warp-style blocks on top of OSC 133:
> per-command **separator + colored left gutter**, a **✓/✗ exit-status badge**, a
> hover **copy-output** button, and **Cmd/Ctrl+↑/↓** navigation between prompts. Built
> as `cmdblocks.c` (model + raylib overlay) over a pure, unit-tested OSC-133 scanner
> (`cmdblocks_osc.c`); a tracked grid ref is anchored at each prompt (`A`) and the exit
> code comes from the `D` mark, so positions survive scrollback/reflow. "Copy output"
> uses the engine's `ghostty_terminal_select_output()` + `selection_format_alloc()`.
> The byte stream is observed, never modified (philosophy intact). *Correction to the
> earlier note: the engine does **not** expose `GHOSTTY_ROW_SEMANTIC_*` /
> `GHOSTTY_CELL_DATA_SEMANTIC_CONTENT` in the pinned commit — the real surface is
> `ghostty_terminal_grid_ref` + `select_output`.* Verified on macOS: 9 ctest suites pass
> (added `cmdblocks_osc_tests`); a gated `FANGS_BLOCKS_SMOKE_SCREENSHOT` smoke screenshots
> the overlay; copy-to-clipboard and scroll-navigation confirmed; a real zsh running the
> documented snippet emits exactly the `A/B/C/D` marks the parser consumes.
>
> **Packaging — macOS (2026-06-22):** ✅ done. `scripts/macos-bundle.sh` turns the built
> binary into a relocatable, ad-hoc-signed **`Fangs.app`** + a distributable `.zip`:
> it bundles the one non-system dep (`libghostty-vt.dylib`) into `Contents/Frameworks/`,
> rewrites load commands to `@executable_path/../Frameworks` (no build-path leak), writes
> `Info.plist`, installs a generated `assets/fangs.icns`, signs inner-out, and prints the zip
> `sha256`. A Homebrew **cask** (`packaging/macos/fangs.rb`) installs the release
> `.app`; see `packaging/macos/README.md`. Verified on macOS: the bundled app launches and
> resolves its dylib from inside the bundle; `otool -L` shows only `/usr/lib` + `/System` +
> `@rpath`. Caveats: arm64-only and ad-hoc-signed (not notarized — Gatekeeper needs a
> right-click→Open or `xattr` clear on download); both documented.
>
> **Linux on-screen verification (2026-06-22):** ✅ **done — closes the last v1 item.**
> Ran the full `docs/linux-verification.md` runbook on a real CachyOS laptop with a graphical
> session (Hyprland 0.55.4 / Wayland), AMD Ryzen 7 5800H. Clean `Release` build with Zig 0.15.2
> → **9/9 ctest suites pass**; both gated screenshot smokes render. On-screen, every feature
> confirmed: window opens with an interactive zsh (truecolor fastfetch + git prompt); **theming**
> recolors live on Save (One Dark → Monokai); **AI sidebar** toggles (`Ctrl+Shift+B`) with the
> terminal still usable; **command blocks** draw separators + colored gutters + green ✓ / red ✗
> badges by exit status, the hover **copy** button puts a block's output on the Wayland clipboard,
> and **`Ctrl+↑/↓`** jumps the viewport between prompts. The two AI round-trips were exercised
> against a **local Ollama** model (`qwen2.5:7b` via the OpenAI-compatible
> `http://localhost:11434/v1/chat/completions` endpoint, selected through the **ollama** provider
> in `Ctrl+,`): the sidebar returned a **streamed, context-aware** answer ("CachyOS x86_64 with
> Linux 7.0.12-1-cachyos", read from the on-screen `uname`), and **inline generation** (`Ctrl+Space`)
> staged `ls -lA` at the prompt with no trailing newline. No cloud key required.
>
> **HiDPI / font zoom (2026-06-22):** ✅ surfaced while verifying on a 1.5× Wayland laptop —
> the font looked tiny. Fangs scales `font_size` by the display content scale (right on macOS
> Retina), but GLFW's **Wayland** backend reports ~1.0 under fractional scaling, so the auto-scale
> never applied. `fangs_content_scale()` in `main.c` now resolves the scale in three tiers:
> (1) **`FANGS_SCALE`** env override (explicit, always wins); (2) `GetWindowScaleDPI()` when it
> reports a real value (macOS Retina, X11); (3) **auto-detect from monitor physical DPI** — a
> probe confirmed raylib reports `scaleDPI=1.0` *and* `render==screen` on this GLFW/Wayland build
> (no usable content-scale or framebuffer-ratio signal), so the fallback derives `scale ≈ DPI/96`
> from `GetMonitorPhysicalWidth/Height` (eDP-1: 1920×1080 / 340×190mm → ~144 DPI → snaps to 1.5,
> matching the compositor exactly), snapped to a 0.25 step and only applied above ~125 DPI so
> normal ~96-DPI displays are untouched. Net result: **`font_size` stays logical (16) and a synced
> dotfile renders correctly across machines with no per-machine tuning.** Also added **live font
> zoom** — `Ctrl +/-` resize and `Ctrl 0` resets, intercepted before `handle_input` (so `=`/`-`
> aren't forwarded to the shell), reusing the settings-modal path (`config_save` + end-of-frame
> `apply_config()` → font reload + grid reflow + `pty_set_winsize`), clamped 6–96 px. Documented
> in `docs/linux-verification.md`. *Remaining nice-to-haves: re-detect scale when the window is
> dragged to a differently-scaled monitor (font is loaded once at startup today), and scale the
> RayGUI widgets to match.*

---

## 7.1 Post-v1 Enhancements

> Enhancements designed after the v1 feature set shipped — the feature work E1–E2 (`spec.md` §15–§16)
> and a polish round E3–E7 (`spec.md` §17–§21). **Sequencing: E1 first** (small, self-contained, high
> value), **then E2** (large). The polish round is independent of E2 (tabs/splits are *not* part of it);
> within it, **E3 first** — it's the keystone every other piece draws its colors from. All keep every
> §2 invariant (pure PTY stream, no auto-exec) and leave the `term_engine` / `ai_provider` seams
> untouched. Note: E2's per-`Session` refactor later makes E1's affordance pane-aware — a minor
> follow-up called out in E2, not a blocker for E1.

### E1 — Command Blocks → AI Context (spec §15)

One-click "Ask AI" / "⚡ Explain error" on a command block → opens the global sidebar pre-loaded
with that block's command + output and an editable prefilled question. Reuses the Phase 4 streaming
path entirely; this adds an entry point, not transport.

1. **`ai_block.{c,h}` (new, pure).** `ai_block_build_context(cmd, output, exit_code, out, cap)` +
   `ai_block_default_question(exit_code)`. Bounded, human-readable context body (spec §15.3).
2. **`tests/ai_block_tests.c`** → `ctest`: context format, byte-budget trim, question-by-exit-code.
   (TDD: write these first.)
3. **`cmdblocks.{c,h}`:** add the per-block hover button (label/emphasis by exit code) beside "copy
   output"; report the click via a `CmdBlockAction` out-param on `cmdblocks_draw` (spec §15.2).
4. **`ui_sidebar.{c,h}`:** add `ui_sidebar_prefill()` and `ui_sidebar_set_oneshot_context()` (next
   send prepends the block context instead of the scrollback dump, then reverts).
5. **`main.c` wiring:** on the action, copy the borrowed strings, run output through the existing
   redaction pass, build context → set one-shot context + prefill question → open/focus the sidebar.
6. **Exit:** hover a ✗ block → `⚡ Explain error`; click → sidebar opens focused with that block as
   context + an editable "Why did this command fail?"; Enter streams a context-aware answer; the next
   turn reverts to scrollback context. `ctest` green; build warning-clean; byte stream untouched.

### E2 — Tabs + Splits (spec §16)

Multiple terminal sessions per window, each tab subdividable into panes (tmux/Ghostty model). The
global sidebar + inline `Ctrl+Space` follow the focused pane. A single tab/pane looks exactly like
today.

1. **`session.{c,h}` (new):** extract the per-terminal state `main()` holds inline — `TermEngine *`,
   `pty_fd`, child pid, grid/cell dims, scroll offset — into one opaque `Session` (spec §16.2).
2. **Per-session state moves (required refactor, spec §16.7):** convert `cmdblocks`'s module-global
   `static struct` to an opaque per-`Session` `CmdBlocks *` (`feed/draw/navigate/reset` take the
   handle); move `main.c`'s `g_sel*` / `g_search*` / `g_row*` globals into `Session`. Relieves the
   2,481-line `main.c`.
3. **`pane.{c,h}` (new, pure):** binary split tree (leaf=Session, internal=H/V split+ratio) with
   `pane_split` / `pane_close` (parent-collapse) / `pane_focus_move` / `pane_set_ratio`. Plus
   `tests/pane_tests.c` (TDD-first): split, close-collapse, directional focus, ratio math.
4. **`layout.{c,h}`:** add recursive `layout_compute_panes(root, content, divider_px, on_leaf)`;
   reserve the tab-bar strip off the top before computing `content`. Extend `tests/layout_tests.c`.
5. **`main.c` — App/Tab + per-leaf render & input:** `App{Tab tabs[]; active}`,
   `Tab{PaneNode*root; *focused}`; render each visible leaf in its own scissor (parameterize the
   existing draw path by `Session*` + `Rect`); accent border on the focused leaf; route input to
   `tabs[active]->focused`; resize every leaf on layout change.
6. **Tab bar + keybindings:** RayGUI strip shown only when `n_tabs ≥ 2`; keys per spec §16.8
   (`Cmd/Ctrl+T`/`W`/`1–9`, `Cmd+D` / `Cmd+Shift+D` split, `Cmd+Opt+Arrows` focus); new tab/pane
   inherits the focused pane's cwd (OSC-7 → else `$HOME`).
7. **Follow-up:** point E1's `CmdBlockAction` at the focused `Session`'s `CmdBlocks` handle.
8. **Exit (spec §16.9):** `Cmd+T` adds a tab (bar appears only at ≥2); `Cmd+D` splits into
   independent shells that render/resize correctly; sidebar + inline target the focused pane;
   single tab/pane is pixel-identical to today; `session_tests` + `pane_tests` + extended
   `layout_tests` green; build warning-clean; all §2 invariants intact.

---

## 7.2 Polish Round (E3–E7)

> Refinement of the shipped feature set — no new surfaces (tabs/splits are out). One thesis: the engine
> is themed but the app chrome is not, text styling is faked while the engine already exposes the real
> data, and failures only reach stderr. Spec: `spec.md` §17–§21. Handoff: `docs/handoff-polish.md`.
> **Sequencing: E3 first** — it's the keystone every later piece draws its colors from — then E4–E7 in
> any order by appetite. Each step adds its tests before host wiring (TDD), matching the v1 phases.

### E3 — UI Theming (spec §17) — *keystone, do first*

Every app-chrome pixel derives from the active `Theme` instead of hardcoded dark RGB, so light themes
stop breaking and the app reads as one surface.

1. **`ui_theme.{c,h}` (new, pure):** `UiTheme ui_theme_derive(const Theme *t)` → chrome colors blended
   from `bg`/`fg` with `is_light` polarity (spec §17.2). No raylib/ghostty types in the header.
2. **`tests/ui_theme_tests.c`** → `ctest` (TDD-first): legible contrast vs `bg`, selection ≠ bg, light
   flips polarity, for a known light and dark theme.
3. **Replace literals (spec §17.3):** `main.c` (selection, search box/hit, scrollbar, cursor alpha),
   `ui_sidebar.c` (panel bg, role tints), `ui_settings.c` (modal bg), `ui_inline.c` (overlay/text),
   `cmdblocks.c` (gutter/badge/button/accent) — convert `UiColor` → raylib `Color` at the draw site.
4. **Recompute** the `UiTheme` on the existing theme-change path (initial load + `Ctrl+,` hot-reload).
5. **Exit:** a light theme leaves sidebar/search/selection/scrollbar/cursor all readable; dark unchanged;
   `ui_theme_tests` green; build warning-clean.

### E4 — Crisp Text & Cursor (spec §18)

Render the full `GhosttyStyle` + cursor state the engine already exposes; drop the faked bold/italic.

1. **Real bold face:** add `assets/JetBrainsMono-Bold.ttf`; generalize the `bin2header` block in
   `CMakeLists.txt` (~46–57) to emit `font_jetbrains_mono_bold.h`; load `bold_font` in `main.c` (~109)
   and use it when `style.bold`; drop the draw-twice hack; `UnloadFont` in cleanup.
2. **Decorations** in the cell loop: `underline` (single/double/curly via `GHOSTTY_SGR_UNDERLINE_*`,
   honoring `underline_color`), `strikethrough`, `overline`, `faint` (blend toward bg), `invisible`
   (skip glyph). All theme-fg colored.
3. **Cursor state** (replace the flat block): render `BAR`/`BLOCK`/`UNDERLINE`/hollow from
   `CURSOR_VISUAL_STYLE`; blink off a frame timer when `CURSOR_BLINKING`; `BLOCK_HOLLOW` when
   `!IsWindowFocused()`.
4. **Config:** add opt-in `cursor_style_default` + `cursor_blink` to `AppConfig` (defaults = today).
5. **Exit:** SGR samples + `vim` render every decoration with real bold glyphs; cursor switches
   style/blinks/hollows on focus loss; build warning-clean; suites green. (Visual verification; no new
   test module.)

### E5 — Error Surfacing & First-Run (spec §19)

1. **`toast.{c,h}` (new):** bounded queue (`toast_push/tick/count/get`); enqueue/expire/cap logic pure,
   drawing in `main.c` (fading pills, bottom-right, `UiTheme`-colored).
2. **`tests/toast_tests.c`** → `ctest` (TDD-first): over-capacity drops oldest, TTL expiry, newest-first
   with decreasing alpha.
3. **Route failures:** the five stderr-only sites (`main.c` ~1748/1815/1826/2049/2481) + AI errors
   (`ai_http.c`/`ui_sidebar.c`) also `toast_push`.
4. **First-run card:** when no key resolves (reuse `resolve_api_key`), `ui_sidebar.c` shows a
   connect-key card with a button into the settings modal instead of a dead input.
5. **`docs/config.example`** (commented, every field + default) referenced from `README.md`.
6. **Exit:** bad endpoint → error toast; corrupt config → load toast + defaults; no key → connect-key
   card; `toast_tests` green; build warning-clean.

### E6 — AI Sidebar Polish (spec §20)

1. **Syntax-highlight fenced code** in replies (heuristic token tint, `UiTheme`-colored) during the
   existing word-wrap pass.
2. **Smooth streaming auto-scroll** (lerp toward bottom; yield to manual scroll-up).
3. **Copy-whole-reply** button per assistant message.
4. **"Ask AI about the last command"** keybinding (`Cmd+Shift+/`): find the latest `cmdblocks` block →
   `ai_block_build_context()` (E1) → `ui_sidebar_set_oneshot_context()` + `ui_sidebar_prefill()` →
   open/focus the sidebar. Redaction still applies; no auto-send.
5. **Exit:** fenced replies render highlighted; streaming auto-scrolls and yields to scroll-up;
   copy-reply works; the shortcut prefills the last block's context (no-op/toast when no blocks);
   invariants intact; build warning-clean.

### E7 — macOS Distribution & Window Polish (spec §21)

1. **Notarization (gated on Apple Developer ID):** add Developer ID signing + hardened-runtime
   entitlements + `notarytool submit --wait` + `stapler staple` to `scripts/macos-bundle.sh` /
   `.github/workflows/release.yml`; drop the `xattr` caveat from `packaging/macos/fangs.rb`.
   **Prereq:** paid Developer ID + app-specific password/API key + entitlements plist as CI secrets —
   if unavailable, ship step 2 and leave a documented stub.
2. **Window polish (no account):** persist + restore window size/position (replaces hardcoded `800×600`,
   `main.c` ~1757); `SetMouseCursor` I-beam over text, pointer over buttons/links.
3. **Exit:** relaunch restores window geometry; cursor changes on hover; if notarized,
   `spctl -a -vvv build/Fangs.app` = accepted and a fresh download opens without the right-click dance.

---

## 8. Risk Register (summary)

| Risk | Likelihood | Impact | Mitigation |
|---|---|---|---|
| `libghostty-vt` API breaks on upgrade | High | Med | Pin commit SHA; `term_engine` seam; deliberate upgrades only |
| Streaming/SSE/JSON in C is painful (**retired** — proven in 0b) | — | — | Validated end-to-end vs an OpenAI-compatible endpoint; isolated in `ai_http.c`; cJSON; worker-thread + mutex buffer |
| Zig 0.15.2 ↔ macOS 26.5 SDK linker quirk (**realized → worked around**) | Low (resid.) | Low | `scripts/macos-build.sh` (hybrid SDK + `xcrun` shim); verified building on macOS 26.5. Linux unaffected |
| Raylib kitty-keyboard limitation (upstream-known) | Med | Low | Accept for v1; document; revisit input layer later |
| Formatter API signatures differ from assumption | Med | Low | Confirm against pinned header in Phase 0; render-state fallback exists |

## 9. Open Questions

- ~~Exact pinned `libghostty-vt` commit SHA.~~ **Resolved:** ghostty `ae52f97dcac558735cfa916ea3965f247e5c6e9e`.
- ~~macOS-native build block.~~ **Resolved:** `scripts/macos-build.sh` (hybrid SDK + `xcrun` shim), verified building.
- ~~Verify CachyOS/Linux on real hardware.~~ **Resolved (2026-06-22):** builds clean + all 8 ctest suites pass on CachyOS x86-64 (Zig 0.15.2); fixed a Linux-only `forkpty` header-shadow bug. GUI window open still pending a graphical session (dev box is headless).
- Confirmed `ghostty_formatter_*` signatures vs. falling back to render-state iteration.
- ~~Provider list for v1 (OpenAI-compatible is the baseline; Anthropic-native messages format TBD).~~ **Resolved (2026-06-22):** OpenAI-compatible **and** Anthropic-native `/v1/messages` both ship behind the `ai_provider` seam (provider toggle selects).
- ~~Packaging: AUR `PKGBUILD` vs. plain `cmake --install`.~~ **Resolved (2026-06-22):** AUR `PKGBUILD` shipped in `packaging/aur/` (verified via `makepkg`); macOS `.app` + Homebrew cask shipped in `packaging/macos/` via `scripts/macos-bundle.sh` (verified — bundled app launches, deps self-contained). Notarization (Developer ID) is the one open packaging item.
