// ui_effects — Ghostty terminal effects (VT query response callbacks).
// Extracted from main.c so the callback set can be reused per-session.
#ifndef FANGS_UI_EFFECTS_H
#define FANGS_UI_EFFECTS_H

#include <stdint.h>
#include <ghostty/vt.h>

#include "session.h"

// Register ghostty effects (VT query responses) for the given session on its
// terminal.  Allocates/returns a heap EffectsContext stored as the session's
// opaque userdata — freed by session_destroy().
void register_session_effects(Session *s);

// Update the terminal geometry stored in the session's EffectsContext after
// a resize or font change.
void update_session_effects(Session *s, uint16_t cols, uint16_t rows,
                            int cell_w, int cell_h);

#endif // FANGS_UI_EFFECTS_H
