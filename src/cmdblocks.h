// cmdblocks — Warp-style command blocks on top of OSC-133 semantic marks.
//
// The shell (see docs/shell-integration.md) emits OSC 133 marks: A at prompt
// start, D;<code> when a command finishes. We anchor a *tracked* grid ref at
// each prompt row (so it survives scrollback/reflow) and remember the exit code
// from the preceding D. Each frame those anchors become viewport rows that
// drive the overlay: status-colored separators, a left gutter strip, ✓/✗
// badges, a hover "copy output" button, a hover "Ask AI" button (§15), and
// Ctrl+Up/Down navigation.
//
// Everything is observation-only: bytes are forwarded to the VT engine
// unmodified (the engine does its own OSC-133 tracking, which powers the
// select_output API we use for copy). Without shell integration there are no
// marks and the overlay simply never draws.
//
// Per-session (§16.7): each Session owns its own CmdBlocks instance, so
// every pane tracks its own command blocks independently.
#ifndef FANGS_CMDBLOCKS_H
#define FANGS_CMDBLOCKS_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#include "raylib.h"
#include "term_engine.h"
#include "theme.h"

// Opaque handle — one per Session (§16.7).
typedef struct CmdBlocks CmdBlocks;

// Action reported when a hover button is clicked. The host checks `action`
// after cmdblocks_draw returns true, reads the block data, and takes the
// appropriate action (copy to clipboard, or open AI sidebar with context).
// The caller must free `output` (malloc'd) when action is CB_ACTION_ASK_AI.
#define CB_CMD_MAX   256
#define CB_OUTPUT_MAX 65536

typedef enum {
    CB_ACTION_NONE = 0,
    CB_ACTION_COPY_OUTPUT,
    CB_ACTION_ASK_AI,
} CbActionType;

typedef struct {
    CbActionType action;
    int   exit_code;        // exit code of the block
    char  command[CB_CMD_MAX]; // extracted command text (prompt row)
    char *output;           // malloc'd output text (caller must free), NULL if none
    int   output_len;       // byte length of output (0 if none)
} CmdBlockAction;

// Create/destroy a CmdBlocks instance (one per Session).
CmdBlocks *cmdblocks_create(void);
void       cmdblocks_destroy(CmdBlocks *cb);

// PTY sink: forward a chunk to the engine while tracking command boundaries.
// Drop-in replacement for a bare term_engine_write() in the read loop.
void cmdblocks_feed(CmdBlocks *cb, TermEngine *te, const uint8_t *data, size_t len);

// Draw the block overlay over the already-rendered grid. Call inside the
// terminal scissor each frame. When a hover button is clicked, fills *action
// and returns true (consumed). The caller must free action->output when
// action->action == CB_ACTION_ASK_AI.
bool cmdblocks_draw(CmdBlocks *cb, TermEngine *te, Font font, const Theme *theme,
                    int cell_w, int cell_h, int font_size,
                    int pad, int term_area_w, int rows,
                    int mouse_x, int mouse_y, bool click,
                    CmdBlockAction *action);

// Scroll the viewport to the previous (dir < 0) or next (dir > 0) command.
// Returns true only if there was a target to scroll to, so the caller can let
// the key fall through to the child (e.g. a TUI) when there's nothing to do.
bool cmdblocks_navigate(CmdBlocks *cb, TermEngine *te, int dir);

#endif // FANGS_CMDBLOCKS_H
