// session — A single terminal session (one shell, one PTY, one VT engine).
// A leaf pane owns exactly one Session (§16.2).
#ifndef FANGS_SESSION_H
#define FANGS_SESSION_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <sys/types.h>   // pid_t

typedef struct Session Session;

// Create a new session: spawn a shell, create the VT engine, and wire them
// together. `cols`/`rows` are the initial terminal grid dimensions; `cell_w`
// and `cell_h` are the pixel size of a cell. `cwd` is the initial working
// directory (NULL → $HOME). Returns NULL on failure.
Session *session_create(uint16_t cols, uint16_t rows, int cell_w, int cell_h,
                        int max_scrollback, const char *cwd);

// Destroy a session: join the child, free the engine, close the PTY.
void session_destroy(Session *s);

// Drain the PTY: read available bytes → feed cmdblocks → feed VT engine.
void session_feed_pty(Session *s);

// Resize the terminal grid and propagate to the child via TIOCSWINSZ.
void session_resize(Session *s, uint16_t cols, uint16_t rows,
                    int cell_w, int cell_h);

// Accessors.
int         session_pty_fd(const Session *s);
pid_t       session_child_pid(const Session *s);    // child PID for signal/cleanup
void       *session_engine(Session *s);             // returns TermEngine* (opaque here)
void       *session_cmdblocks(Session *s);          // returns CmdBlocks* (opaque here)
bool        session_child_alive(const Session *s);
const char *session_cwd(const Session *s);          // last OSC-7 cwd, or $HOME

// Spawn a new child in the same session (after the original exited).
// Returns true on success.
bool session_reap(Session *s);
bool session_respawn(Session *s, const char *cwd);
int  session_exit_status(const Session *s);        // -1 if alive / unknown

// Opaque userdata slot for the host (main.c stores EffectsContext here).
void  session_set_userdata(Session *s, void *userdata);
void *session_userdata(const Session *s);

#endif // FANGS_SESSION_H
