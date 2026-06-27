#include "pty.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <pwd.h>

#if defined(__APPLE__)
#include <util.h>
#else
// glibc declares forkpty() in <pty.h>, but the project ships its own "pty.h"
// which shadows the system header on the -Isrc angle-bracket search path, so
// `#include <pty.h>` would pull in our header (and never declare forkpty).
// Declare the prototype directly — the BSD/glibc signature is stable.
#include <termios.h>
extern int forkpty(int *__amaster, char *__name,
                   const struct termios *__termp, const struct winsize *__winp);
#endif

int pty_spawn(pid_t *child_out, uint16_t cols, uint16_t rows,
              int cell_width, int cell_height, const char *cwd)
{
    int pty_fd;
    struct winsize ws = {
        .ws_row = rows,
        .ws_col = cols,
        .ws_xpixel = (unsigned short)(cols * cell_width),
        .ws_ypixel = (unsigned short)(rows * cell_height),
    };

    // forkpty() = openpty + fork + login_tty.
    pid_t child = forkpty(&pty_fd, NULL, NULL, &ws);
    if (child < 0) {
        perror("forkpty");
        return -1;
    }
    if (child == 0) {
        const char *shell = getenv("SHELL");
        if (!shell || shell[0] == '\0') {
            struct passwd *pw = getpwuid(getuid());
            if (pw && pw->pw_shell && pw->pw_shell[0] != '\0')
                shell = pw->pw_shell;
            else
                shell = "/bin/sh";
        }
        const char *shell_name = strrchr(shell, '/');
        shell_name = shell_name ? shell_name + 1 : shell;

        // Start a LOGIN shell: argv[0] prefixed with '-' is the POSIX convention
        // that tells the shell to source the user's login profile
        // (~/.zprofile/.zshrc, ~/.bash_profile, …). This is how Terminal.app,
        // iTerm and Ghostty recover the user's full PATH: a GUI app launched from
        // Finder/Dock inherits only launchd's minimal PATH (/usr/bin:/bin:…), so
        // without a login shell, Homebrew (/opt/homebrew/bin), ~/.local/bin and
        // other profile-added entries would be missing.
        char login_argv0[256];
        snprintf(login_argv0, sizeof(login_argv0), "-%s", shell_name);

        // Open in the requested directory (a new tab/pane inherits the focused
        // pane's cwd). Ignore failure — fall back to the inherited cwd.
        if (cwd && cwd[0])
            (void)(chdir(cwd) == 0);

        setenv("TERM", "xterm-256color", 1);
        execl(shell, login_argv0, (char *)NULL);
        _exit(127); // execl only returns on error
    }

    // Parent: non-blocking master so reads return EAGAIN instead of stalling.
    int flags = fcntl(pty_fd, F_GETFL);
    if (flags < 0 || fcntl(pty_fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        perror("fcntl O_NONBLOCK");
        close(pty_fd);
        return -1;
    }

    *child_out = child;
    return pty_fd;
}

void pty_write(int pty_fd, const char *buf, size_t len)
{
    while (len > 0) {
        ssize_t n = write(pty_fd, buf, len);
        if (n > 0) {
            buf += n;
            len -= (size_t)n;
        } else if (n < 0) {
            if (errno == EINTR)
                continue;
            break; // EAGAIN or real error — drop the remainder
        }
    }
}

PtyReadResult pty_read(int pty_fd, PtySink sink, void *userdata)
{
    uint8_t buf[4096];
    for (;;) {
        ssize_t n = read(pty_fd, buf, sizeof(buf));
        if (n > 0) {
            sink(userdata, buf, (size_t)n);
        } else if (n == 0) {
            return PTY_READ_EOF;
        } else {
            if (errno == EAGAIN)
                return PTY_READ_OK;
            if (errno == EINTR)
                continue;
            if (errno == EIO) // Linux: slave close often reports EIO not EOF
                return PTY_READ_EOF;
            perror("pty read");
            return PTY_READ_ERROR;
        }
    }
}

void pty_set_winsize(int pty_fd, uint16_t cols, uint16_t rows,
                     int cell_width, int cell_height)
{
    struct winsize ws = {
        .ws_row = rows,
        .ws_col = cols,
        .ws_xpixel = (unsigned short)(cols * cell_width),
        .ws_ypixel = (unsigned short)(rows * cell_height),
    };
    ioctl(pty_fd, TIOCSWINSZ, &ws);
}
