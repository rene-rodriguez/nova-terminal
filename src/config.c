#include "config.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static void copy_string(char *dst, size_t dst_size, const char *src)
{
    if (dst_size == 0)
        return;
    snprintf(dst, dst_size, "%s", src ? src : "");
}

static char *trim(char *s)
{
    while (*s && isspace((unsigned char)*s))
        s++;

    char *end = s + strlen(s);
    while (end > s && isspace((unsigned char)end[-1]))
        end--;
    *end = '\0';

    return s;
}

static void strip_inline_comment(char *s)
{
    for (char *p = s; *p; p++) {
        if ((*p == ';' || *p == '#') && (p == s || isspace((unsigned char)p[-1]))) {
            *p = '\0';
            return;
        }
    }
}

static bool parse_int_value(const char *value, int *out)
{
    char *end = NULL;
    errno = 0;
    long n = strtol(value, &end, 10);
    if (errno != 0 || end == value)
        return false;
    while (*end && isspace((unsigned char)*end))
        end++;
    if (*end != '\0')
        return false;
    *out = (int)n;
    return true;
}

static bool parse_bool_value(const char *value, bool *out)
{
    if (strcmp(value, "true") == 0 || strcmp(value, "1") == 0
        || strcmp(value, "yes") == 0 || strcmp(value, "on") == 0) {
        *out = true;
        return true;
    }
    if (strcmp(value, "false") == 0 || strcmp(value, "0") == 0
        || strcmp(value, "no") == 0 || strcmp(value, "off") == 0) {
        *out = false;
        return true;
    }
    return false;
}

static void apply_key_value(AppConfig *c, const char *section,
                            const char *key, const char *value)
{
    if (strcmp(section, "terminal") == 0) {
        if (strcmp(key, "font_family") == 0) {
            copy_string(c->font_family, sizeof(c->font_family), value);
        } else if (strcmp(key, "font_size") == 0) {
            int parsed = 0;
            if (parse_int_value(value, &parsed) && parsed > 0)
                c->font_size = parsed;
        } else if (strcmp(key, "theme") == 0) {
            copy_string(c->theme, sizeof(c->theme), value);
        } else if (strcmp(key, "scrollback") == 0) {
            int parsed = 0;
            if (parse_int_value(value, &parsed) && parsed > 0)
                c->scrollback = parsed;
        } else if (strcmp(key, "cursor_style") == 0) {
            int parsed = 0;
            if (parse_int_value(value, &parsed) && parsed >= 0 && parsed <= 2)
                c->cursor_style = parsed;
        } else if (strcmp(key, "cursor_blink") == 0) {
            bool parsed = false;
            if (parse_bool_value(value, &parsed))
                c->cursor_blink = parsed;
        }
    } else if (strcmp(section, "ai") == 0) {
        if (strcmp(key, "provider") == 0) {
            copy_string(c->provider, sizeof(c->provider), value);
        } else if (strcmp(key, "endpoint") == 0) {
            copy_string(c->endpoint, sizeof(c->endpoint), value);
        } else if (strcmp(key, "model") == 0) {
            copy_string(c->model, sizeof(c->model), value);
        } else if (strcmp(key, "api_key") == 0) {
            copy_string(c->api_key, sizeof(c->api_key), value);
        } else if (strcmp(key, "stream") == 0) {
            bool parsed = false;
            if (parse_bool_value(value, &parsed))
                c->stream = parsed;
        } else if (strcmp(key, "max_tokens") == 0) {
            int parsed = 0;
            if (parse_int_value(value, &parsed) && parsed > 0)
                c->max_tokens = parsed;
        }
    }
}

static void mkdir_if_missing(const char *path)
{
    if (mkdir(path, 0700) != 0 && errno != EEXIST)
        perror(path);
}

void config_defaults(AppConfig *c)
{
    memset(c, 0, sizeof(*c));

    copy_string(c->font_family, sizeof(c->font_family), "JetBrainsMono Nerd Font");
    c->font_size = 16;
    copy_string(c->theme, sizeof(c->theme), "dark");
    c->scrollback = 1000;

    copy_string(c->provider, sizeof(c->provider), "openai");
    copy_string(c->endpoint, sizeof(c->endpoint),
                "https://api.openai.com/v1/chat/completions");
    copy_string(c->model, sizeof(c->model), "gpt-4o-mini");
    copy_string(c->api_key, sizeof(c->api_key), "");
    c->stream = true;
    c->max_tokens = 1024;

    c->cursor_style = 0;    // block
    c->cursor_blink  = true;
}

const char *config_default_path(void)
{
    static char path[4096];
    const char *home = getenv("HOME");
    if (!home || home[0] == '\0')
        home = ".";

    char config_dir[4096];
    char app_dir[4096];
    snprintf(config_dir, sizeof(config_dir), "%s/.config", home);
    snprintf(app_dir, sizeof(app_dir), "%s/fangs", config_dir);
    mkdir_if_missing(config_dir);
    mkdir_if_missing(app_dir);
    snprintf(path, sizeof(path), "%s/config", app_dir);
    return path;
}

bool config_load(AppConfig *c, const char *path)
{
    config_defaults(c);

    FILE *f = fopen(path, "r");
    if (!f) {
        if (errno == ENOENT)
            return config_save(c, path);
        return false;
    }

    char section[32] = "";
    char line[1024];
    while (fgets(line, sizeof(line), f)) {
        char *s = trim(line);
        if (*s == '\0' || *s == ';' || *s == '#')
            continue;

        if (*s == '[') {
            char *close = strchr(s, ']');
            if (!close)
                continue;
            *close = '\0';
            copy_string(section, sizeof(section), trim(s + 1));
            continue;
        }

        char *eq = strchr(s, '=');
        if (!eq)
            continue;

        *eq = '\0';
        char *key = trim(s);
        char *value = trim(eq + 1);
        strip_inline_comment(value);
        value = trim(value);

        apply_key_value(c, section, key, value);
    }

    bool ok = !ferror(f);
    fclose(f);
    return ok;
}

bool config_save(const AppConfig *c, const char *path)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0)
        return false;

    if (fchmod(fd, 0600) != 0) {
        close(fd);
        return false;
    }

    FILE *f = fdopen(fd, "w");
    if (!f) {
        close(fd);
        return false;
    }

    int written = fprintf(f,
        "[terminal]\n"
        "font_family = %s\n"
        "font_size = %d\n"
        "theme = %s\n"
        "scrollback = %d\n"
        "cursor_style = %d\n"
        "cursor_blink = %s\n"
        "\n"
        "[ai]\n"
        "provider = %s\n"
        "endpoint = %s\n"
        "model = %s\n"
        "api_key = %s\n"
        "stream = %s\n"
        "max_tokens = %d\n",
        c->font_family,
        c->font_size,
        c->theme,
        c->scrollback,
        c->cursor_style,
        c->cursor_blink ? "true" : "false",
        c->provider,
        c->endpoint,
        c->model,
        c->api_key,
        c->stream ? "true" : "false",
        c->max_tokens);

    bool ok = written >= 0 && fclose(f) == 0;
    return ok;
}
