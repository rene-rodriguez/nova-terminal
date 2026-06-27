// ui_toast — Non-blocking notification toast overlay.
// Pure (no raylib/ghostty types); draw lives in main.c.
#ifndef FANGS_UI_TOAST_H
#define FANGS_UI_TOAST_H

#include <stdbool.h>

#define TOAST_MAX_MSG 256

typedef enum {
    TOAST_INFO,
    TOAST_WARN,
    TOAST_ERROR,
} ToastLevel;

// Push a notification. Copies `msg` internally (truncated).
// Drops oldest when the ring is full.
void toast_push(ToastLevel level, const char *msg);

// Advance the timer by `dt` seconds. Expired entries are removed.
void toast_tick(double dt);

// Current number of active (non-expired) toasts.
int  toast_count(void);

// Read the i-th newest toast (0 = newest). Returns false if out of range.
// *alpha is [1.0 .. 0.0] — fade as the toast approaches expiry.
bool toast_get(int i, ToastLevel *level, const char **msg, float *alpha);

// Clear all toasts.
void toast_clear(void);

// Default TTL for each level (seconds).
#define TOAST_TTL_INFO  4.0
#define TOAST_TTL_WARN  6.0
#define TOAST_TTL_ERROR 8.0

#endif // FANGS_UI_TOAST_H
