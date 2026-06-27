#include "ai_block.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *fmt_boilerplate =
    "The user ran this command in their terminal:\n"
    "\n"
    "$ %s\n"
    "\n"
    "It exited with status %d. Its output was:\n"
    "\n"
    "```\n"
    "%s\n"
    "```\n";

int ai_block_build_context(const char *command, const char *output, int exit_code,
                           char *out, int cap)
{
    if (!out || cap <= 0)
        return -1;

    const char *cmd = command ? command : "";
    const char *out_str = output ? output : "";

    // First try: how much do we need for the full thing?
    int needed = snprintf(NULL, 0, fmt_boilerplate, cmd, exit_code, out_str);
    if (needed < 0)
        return -1;

    if (needed < cap) {
        return snprintf(out, (size_t)cap, fmt_boilerplate, cmd, exit_code, out_str);
    }

    // Doesn't fit — trim output to make room.  We need the boilerplate with
    // an empty output string to know the irreducible minimum.
    int base = snprintf(NULL, 0, fmt_boilerplate, cmd, exit_code, "");
    if (base < 0)
        return -1;

    if (base >= cap) {
        // Even the boilerplate alone won't fit — nothing we can do.
        out[0] = '\0';
        return -1;
    }

    // Trim output so the whole thing fits.
    int max_out = cap - base - 1;   // -1 for NUL
    if (max_out < 0)
        max_out = 0;

    // Make a truncated copy of output sized to exactly what fits, so a large
    // `cap` actually carries a large output (no fixed stack-buffer cap).
    size_t out_len = strlen(out_str);
    size_t trim = (size_t)max_out < out_len ? (size_t)max_out : out_len;

    char *trimmed = (char *)malloc(trim + 1);
    if (!trimmed) {
        out[0] = '\0';
        return -1;
    }
    memcpy(trimmed, out_str, trim);
    trimmed[trim] = '\0';

    int written = snprintf(out, (size_t)cap, fmt_boilerplate, cmd, exit_code, trimmed);
    free(trimmed);
    return written;
}

const char *ai_block_default_question(int exit_code)
{
    if (exit_code > 0)
        return "Why did this command fail?";
    if (exit_code == 0)
        return "Explain this output.";
    return "Explain this command.";
}
