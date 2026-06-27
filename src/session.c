#include "session.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>
#include <errno.h>
#include <sys/wait.h>

#include "cmdblocks.h"
#include "pty.h"
#include "term_engine.h"

struct Session {
    TermEngine *te;
    CmdBlocks  *cmdblocks;       // per-session command block tracking (§16.7)
    int  pty_fd;
    pid_t child;
    bool child_alive;
    int  child_exit_status;      // -1 = alive/unknown
    int  cell_w;
    int  cell_h;
    uint16_t cols;
    uint16_t rows;
    char cwd[1024];
    int  max_scrollback;
    void *userdata;              // opaque slot for host (EffectsContext)
};

Session *session_create(uint16_t cols, uint16_t rows, int cell_w, int cell_h,
                        int max_scrollback, const char *cwd)
{
    Session *s = (Session *)calloc(1, sizeof(Session));
    if (!s)
        return NULL;

    s->cols = cols;
    s->rows = rows;
    s->cell_w = cell_w;
    s->cell_h = cell_h;
    s->max_scrollback = max_scrollback;

    if (cwd && cwd[0])
        snprintf(s->cwd, sizeof(s->cwd), "%s", cwd);
    else
        snprintf(s->cwd, sizeof(s->cwd), "%s", getenv("HOME") ? getenv("HOME") : "/");

    s->te = term_engine_create(cols, rows, cell_w, cell_h, max_scrollback);
    if (!s->te) {
        free(s);
        return NULL;
    }

    // Pass the requested cwd straight through (may be NULL → inherit launch
    // dir); s->cwd above is only the display/fallback value.
    s->pty_fd = pty_spawn(&s->child, cols, rows, cell_w, cell_h, cwd);
    if (s->pty_fd < 0) {
        term_engine_destroy(s->te);
        free(s);
        return NULL;
    }
    s->child_alive = true;
    s->child_exit_status = -1;

    s->cmdblocks = cmdblocks_create();
    if (!s->cmdblocks) {
        term_engine_destroy(s->te);
        close(s->pty_fd);
        free(s);
        return NULL;
    }

    return s;
}

void session_destroy(Session *s)
{
    if (!s)
        return;

    // Close PTY to signal the child.
    if (s->pty_fd >= 0) {
        close(s->pty_fd);
        s->pty_fd = -1;
    }

    // Reap child.
    if (s->child > 0) {
        int status = 0;
        waitpid(s->child, &status, WNOHANG);
        s->child = -1;
        s->child_alive = false;
    }

    cmdblocks_destroy(s->cmdblocks);
    s->cmdblocks = NULL;
    term_engine_destroy(s->te);
    free(s->userdata);   // EffectsContext (if any)
    free(s);
}

void session_feed_pty(Session *s)
{
    if (!s || s->pty_fd < 0 || !s->child_alive)
        return;

    uint8_t buf[65536];
    ssize_t n = read(s->pty_fd, buf, sizeof(buf));
    if (n > 0)
        cmdblocks_feed(s->cmdblocks, s->te, buf, (size_t)n);
    else if (n == 0)
        s->child_alive = false;
    // n < 0: EAGAIN is fine; other errors mark child dead.
    else if (errno != EAGAIN && errno != EINTR)
        s->child_alive = false;
}

void session_resize(Session *s, uint16_t cols, uint16_t rows,
                    int cell_w, int cell_h)
{
    if (!s)
        return;

    s->cols = cols;
    s->rows = rows;
    s->cell_w = cell_w;
    s->cell_h = cell_h;

    term_engine_resize(s->te, cols, rows, cell_w, cell_h);
    pty_set_winsize(s->pty_fd, cols, rows, cell_w, cell_h);
}

int session_pty_fd(const Session *s)
{
    return s ? s->pty_fd : -1;
}

pid_t session_child_pid(const Session *s)
{
    return s ? s->child : -1;
}

void *session_engine(Session *s)
{
    return s ? (void *)s->te : NULL;
}

void *session_cmdblocks(Session *s)
{
    return s ? (void *)s->cmdblocks : NULL;
}

bool session_child_alive(const Session *s)
{
    return s && s->child_alive;
}

static int hex_val(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// Return the session's live working directory. The shell reports it via OSC 7
// (if configured), which the engine tracks; we read that and cache it. When no
// OSC-7 pwd is available, fall back to the stored initial cwd ($HOME). This is
// what a new tab/pane inherits (spec §16.6).
const char *session_cwd(const Session *s)
{
    if (!s)
        return "";

    GhosttyTerminal term = s->te ? term_engine_terminal(s->te) : NULL;
    GhosttyString pwd = {0};
    if (term && ghostty_terminal_get(term, GHOSTTY_TERMINAL_DATA_PWD, &pwd)
            == GHOSTTY_SUCCESS && pwd.ptr && pwd.len > 0) {
        char raw[1024];
        size_t rn = pwd.len < sizeof(raw) - 1 ? pwd.len : sizeof(raw) - 1;
        memcpy(raw, pwd.ptr, rn);
        raw[rn] = '\0';

        // OSC 7 is usually "file://host/path"; reduce to the local path.
        const char *path = raw;
        if (strncmp(raw, "file://", 7) == 0) {
            const char *slash = strchr(raw + 7, '/');
            path = slash ? slash : raw + 7;
        }

        // Percent-decode into the cached buffer (handles spaces etc.).
        Session *m = (Session *)s;
        size_t o = 0;
        for (size_t i = 0; path[i] && o < sizeof(m->cwd) - 1; i++) {
            int hi, lo;
            if (path[i] == '%' && (hi = hex_val((unsigned char)path[i + 1])) >= 0
                                && (lo = hex_val((unsigned char)path[i + 2])) >= 0) {
                m->cwd[o++] = (char)((hi << 4) | lo);
                i += 2;
            } else {
                m->cwd[o++] = path[i];
            }
        }
        m->cwd[o] = '\0';
    }

    return s->cwd;
}

void session_set_userdata(Session *s, void *userdata)
{
    if (s) s->userdata = userdata;
}

void *session_userdata(const Session *s)
{
    return s ? s->userdata : NULL;
}

bool session_reap(Session *s)
{
    if (!s || s->child <= 0)
        return false;
    int status = 0;
    if (waitpid(s->child, &status, WNOHANG) == s->child) {
        s->child_alive = false;
        s->child = -1;
        if (WIFEXITED(status))
            s->child_exit_status = WEXITSTATUS(status);
        else if (WIFSIGNALED(status))
            s->child_exit_status = 128 + WTERMSIG(status);
        else
            s->child_exit_status = 0;
        return true;
    }
    return false;
}

int session_exit_status(const Session *s)
{
    return s ? s->child_exit_status : -1;
}

bool session_respawn(Session *s, const char *cwd)
{
    if (!s)
        return false;

    if (s->child > 0) {
        close(s->pty_fd);
        int status = 0;
        waitpid(s->child, &status, WNOHANG);
        s->child = -1;
    }

    const char *dir = cwd ? cwd : s->cwd;
    s->pty_fd = -1;
    s->child_alive = false;

    s->pty_fd = pty_spawn(&s->child, s->cols, s->rows, s->cell_w, s->cell_h, dir);
    if (s->pty_fd < 0)
        return false;

    s->child_alive = true;
    if (dir && dir[0])
        snprintf(s->cwd, sizeof(s->cwd), "%s", dir);
    return true;
}
