<div align="center">

# Nova Terminal

**A native, GPU-accelerated terminal with the AI you actually want — and none of the lock-in.**

Inline command generation, an AI chat sidebar that reads your screen, and Warp-style command
blocks. **Local-first and bring-your-own-key** — one binary, no account, no Nova cloud; point it at
your own API key or run a fully local model.

[![License: MIT](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)
[![Latest release](https://img.shields.io/github/v/release/rene-rodriguez/nova-terminal?sort=semver)](https://github.com/rene-rodriguez/nova-terminal/releases/latest)
![Platforms](https://img.shields.io/badge/platforms-macOS%20·%20Linux-lightgrey)
![Built on libghostty-vt](https://img.shields.io/badge/VT%20engine-libghostty--vt-44aa55.svg)

</div>

Nova is a single local binary for **macOS and Linux**, built on Ghostty's
[`libghostty-vt`](https://libghostty.tip.ghostty.org/) terminal engine and rendered with
[Raylib](https://www.raylib.com/). No account, no telemetry, no subscription.

## Why Nova

Warp put genuinely useful AI in the terminal — then locked it behind an account, cloud telemetry,
and a subscription, and rendered its UI with non-standard TTY blocks. Nova keeps the features and
drops the rest:

- **No account, no telemetry.** One local binary. You never sign in to use your own terminal.
- **Local-first & BYOK.** Nova has no backend of its own — it connects straight from your machine
  to the endpoint *you* choose: Anthropic's native API, any OpenAI-compatible endpoint, or a
  **fully local** model via Ollama or llama.cpp. Your key lives on your disk, and scrollback is
  redacted on-device before anything is sent.
- **A real, standard terminal.** The VT layer is Ghostty's `libghostty-vt` — inspectable and
  community-maintained. Nova never intercepts your shell's stdin to fake visual "blocks"; every AI
  feature is an overlay in the render pipeline, so the PTY byte stream stays pure.

## Features

- **AI chat sidebar** &nbsp;`Cmd+B` / `Ctrl+Shift+B` — ask about what's on your screen. Nova
  captures recent scrollback (redacted for keys, tokens, and passwords *before* it leaves your
  machine), streams the answer live, and keeps multi-turn context. Any command in the reply gets a
  **Run** button that stages it at your prompt — never auto-executed.
- **Inline command generation** &nbsp;`Ctrl+Space` — describe what you want in plain language
  (*"undo last git commit"*) and get the command staged at your prompt, no trailing newline. You
  review and press Enter yourself, always.
- **Command blocks** &nbsp;Warp-style, via OSC 133 — once your shell emits the marks, each command
  gets a separator, a colored gutter, and a **✓ / ✗ exit-status badge**; hover to copy its output,
  and jump between commands with `Cmd+↑` / `Cmd+↓`. A pure render overlay — the byte stream stays
  untouched. (See [`docs/shell-integration.md`](docs/shell-integration.md).)
- **Live configuration** — an INI dotfile is the source of truth, with an in-app settings modal
  (`Ctrl+,`) that round-trips to it and hot-reloads instantly. No restarts.
- **First-class theming** — One Dark, Dark Modern, GitHub Dark, Gruvbox, Monokai, and light
  variants. Each themes the full 256-color palette in the engine, so *all* output is colored
  (`ls --color`, vim, prompts, 256-color apps), and the UI restyles to match.
- **Terminal essentials** — mouse selection with copy/paste (bracketed-paste safe),
  `Ctrl`/`Cmd`+click to open URLs, and `Ctrl+F` find-in-view.

## Install

### Quick install (prebuilt binary)

```sh
curl -fsSL https://raw.githubusercontent.com/rene-rodriguez/nova-terminal/main/install.sh | sh
```

Detects your OS and CPU, downloads the matching tarball from the
[latest release](https://github.com/rene-rodriguez/nova-terminal/releases/latest), and installs it
under `~/.local` (`bin/nova-terminal` plus the bundled `libghostty-vt`, resolved via a relative
RPATH). Make sure `~/.local/bin` is on your `PATH`, then run `nova-terminal`.

| Option | How |
|---|---|
| Install to a custom prefix | `curl … \| NOVA_PREFIX=/usr/local sh` |
| Pin a specific version | `curl … \| NOVA_VERSION=v0.1.1 sh` |
| Authenticate a private mirror | `curl … \| NOVA_GITHUB_TOKEN=… sh` (also reads `GH_TOKEN` / `GITHUB_TOKEN`) |

Prebuilt targets: **Linux x86_64** and **macOS arm64** (Apple Silicon). **Intel Macs** and any other
target build from [source](#build-from-source) — the installer prints exactly how if no prebuilt
binary matches. Releases are built by CI (`.github/workflows/release.yml`) on every `v*` tag.

### Build from source

Requires **CMake 3.19+**, **Ninja**, a C compiler, **libcurl**, and **Zig 0.15.2** on `PATH` (Zig
compiles `libghostty-vt` at build time — the one non-obvious dependency).

> **Zig version matters.** Nova pins `libghostty-vt` to a commit that builds with Zig **0.15.2**.
> Arch/CachyOS `pacman` currently ships 0.16.0, which won't build the pin — grab 0.15.2 from
> [ziglang.org/download](https://ziglang.org/download/) and put it first on `PATH`.

```bash
# Linux / CachyOS
sudo pacman -S --needed cmake ninja base-devel git curl   # plus Zig 0.15.2 on PATH
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release && cmake --build build
./build/nova-terminal

# macOS (handles the Zig 0.15.2 ↔ macOS SDK workaround automatically)
bash scripts/macos-build.sh
./build/nova-terminal
```

Rebuilds are incremental and fast: `cmake --build build`.

- **Arch / CachyOS:** build a package straight from `packaging/aur/` (`makepkg -si`) — it fetches the
  pinned Zig for you. See [`packaging/aur/README.md`](packaging/aur/README.md).
- **macOS:** `scripts/macos-bundle.sh` produces a relocatable, self-contained `Nova Terminal.app`
  (plus a distributable zip); a Homebrew **cask** lives in `packaging/macos/`. See
  [`packaging/macos/README.md`](packaging/macos/README.md).

## Keybindings

| Keys | Action |
|---|---|
| `Ctrl+,` / `Cmd+,` | Settings modal (font, theme, AI provider / model / key) |
| `Cmd+B` / `Ctrl+Shift+B` | Toggle the AI chat sidebar |
| `Ctrl+Space` | Inline command generation — describe a command, get it staged |
| `Cmd+↑` / `Cmd+↓` (or `Ctrl+↑` / `Ctrl+↓`) | Jump to previous / next command block |
| Drag · `Cmd+C` / `Ctrl+Shift+C` | Select text · copy the selection |
| `Cmd+V` / `Ctrl+Shift+V` / `Shift+Insert` | Paste (bracketed-paste safe) |
| `Ctrl` / `Cmd` + click a URL | Open it (`open` / `xdg-open`) |
| `Ctrl+F` / `Cmd+F` | Find — highlights matches in view (Esc closes) |

## Configuration

Settings live in `~/.config/nova-terminal/config` (INI), editable in-app via `Ctrl+,` (`Cmd+,` on
macOS). **Save** writes the file and hot-reloads live — font size resizes the grid, theme changes
apply instantly, no restart.

The AI features are **bring-your-own-key**: set the **`NOVA_API_KEY`** environment variable
(preferred), or store a key in the settings modal (written `0600`). When the env var is set, the
modal greys out the key field and shows *(from env)*. The key is never logged.

Pick your provider in the settings toggle:

- **anthropic** — the native Claude API (`x-api-key`, `/v1/messages`).
- **openai** / **ollama** / **custom** — any OpenAI-compatible endpoint (hosted or local).

Switching the provider prefills a sensible default endpoint and model. Scrollback sent to the model
is run through a redaction pass first, terminal context is attached only to the current question,
and both the sidebar's Run buttons and inline generation stage commands without a newline — so you
always press Enter yourself.

## Project layout

```
src/            main.c · pty · term_engine (libghostty-vt seam) · config (INI)
                ui_settings (Ctrl+, modal) · layout · ui_sidebar (chat panel)
                ai_provider + ai_http (AI seam) · sse · context + redact
                cmdextract (Run buttons) · ui_inline (Ctrl+Space)
                cmdblocks + cmdblocks_osc (OSC 133) · theme · raygui.h / cJSON.c (vendored)
tests/          config · layout · ui_sidebar_model · sse · redact · cmdextract · inline_cmd · theme · cmdblocks_osc  (ctest)
assets/         embedded font (JetBrains Mono, OFL)
scripts/        build, bundle, and release packaging
packaging/      aur/ (Arch · CachyOS) · macos/ (Homebrew cask)
docs/           plan.md · spec.md · shell-integration.md · handoff notes
```

Two narrow seams keep the project honest: `term_engine.h` wraps the VT engine
(`spawn / write / resize / render / dump_text`), and `ai_provider.h` wraps the AI transport
(`send(messages, on_token)`). Swapping the engine or a provider touches a single file.

## Documentation

- [`docs/plan.md`](docs/plan.md) — roadmap and architecture decisions.
- [`docs/spec.md`](docs/spec.md) — technical spec (modules, build, config, AI provider).
- [`docs/shell-integration.md`](docs/shell-integration.md) — OSC 133 shell snippets (zsh / bash) for command blocks.

## License

Nova Terminal is released under the **MIT License** — see [`LICENSE`](LICENSE). The same license as
upstream Ghostty, it lets anyone use, modify, and redistribute Nova (including commercially) with
attribution and no warranty.

The only bundled third-party asset is the **JetBrains Mono** font (embedded for the terminal/UI),
under the **SIL Open Font License 1.1** — shipped verbatim as `LICENSE-OFL-JetBrainsMono.txt` in
every release. The OFL places no restriction on Nova's own source license. Everything else Nova
links (libcurl, OpenGL, X11/Wayland) is a system library, and `libghostty-vt` is built from pinned
upstream Ghostty source.
