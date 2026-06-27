// ui_toast — Non-blocking notification toast overlay.
#include "ui_toast.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#define TOAST_RING 16

typedef struct {
    ToastLevel level;
    char msg[TOAST_MAX_MSG];
    double ttl;       // remaining seconds
    double max_ttl;   // initial TTL (for fade calc)
} ToastEntry;

static struct {
    ToastEntry ring[TOAST_RING];
    int head;         // oldest index
    int count;        // active entries (wrap after head)
} g_toast = {0};

void toast_push(ToastLevel level, const char *msg)
{
    double ttl = 0.0;
    switch (level) {
        case TOAST_INFO:  ttl = TOAST_TTL_INFO;  break;
        case TOAST_WARN:  ttl = TOAST_TTL_WARN;  break;
        case TOAST_ERROR: ttl = TOAST_TTL_ERROR; break;
    }

    int idx = (g_toast.head + g_toast.count) % TOAST_RING;
    if (g_toast.count == TOAST_RING) {
        // Ring full: overwrite oldest, advance head.
        idx = g_toast.head;
        g_toast.head = (g_toast.head + 1) % TOAST_RING;
    } else {
        g_toast.count++;
    }

    g_toast.ring[idx].level   = level;
    g_toast.ring[idx].ttl     = ttl;
    g_toast.ring[idx].max_ttl = ttl;
    snprintf(g_toast.ring[idx].msg, TOAST_MAX_MSG, "%s", msg ? msg : "");
}

void toast_tick(double dt)
{
    // Walk active entries backwards so removal is O(n).
    int n = g_toast.count;
    for (int i = n - 1; i >= 0; i--) {
        int idx = (g_toast.head + i) % TOAST_RING;
        g_toast.ring[idx].ttl -= dt;
        if (g_toast.ring[idx].ttl <= 0.0) {
            // Remove this entry: shift remaining forwards.
            for (int j = i; j < g_toast.count - 1; j++) {
                int src = (g_toast.head + j + 1) % TOAST_RING;
                int dst = (g_toast.head + j) % TOAST_RING;
                g_toast.ring[dst] = g_toast.ring[src];
            }
            g_toast.count--;
        }
    }

    // If count dropped to zero, reset head to keep it clean.
    if (g_toast.count == 0) g_toast.head = 0;
}

int toast_count(void)
{
    return g_toast.count;
}

bool toast_get(int i, ToastLevel *level, const char **msg, float *alpha)
{
    if (i < 0 || i >= g_toast.count) return false;
    int idx = (g_toast.head + g_toast.count - 1 - i) % TOAST_RING;
    if (level) *level = g_toast.ring[idx].level;
    if (msg)   *msg   = g_toast.ring[idx].msg;
    if (alpha) {
        double remain = g_toast.ring[idx].ttl;
        double max    = g_toast.ring[idx].max_ttl;
        *alpha = (max > 0.0) ? (float)(remain / max) : 0.0f;
        if (*alpha < 0.0f) *alpha = 0.0f;
        if (*alpha > 1.0f) *alpha = 1.0f;
    }
    return true;
}

void toast_clear(void)
{
    g_toast.head  = 0;
    g_toast.count = 0;
}
