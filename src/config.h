#ifndef FANGS_CONFIG_H
#define FANGS_CONFIG_H

#include <stdbool.h>

typedef struct {
    char font_family[128];
    int font_size;
    char theme[32];
    int scrollback;

    char provider[32];
    char endpoint[256];
    char model[128];
    char api_key[256];
    bool stream;
    int max_tokens;

    // E4: cursor configuration
    int cursor_style;    // 0=block, 1=bar, 2=underline (default 0)
    bool cursor_blink;   // default true
} AppConfig;

void config_defaults(AppConfig *c);
bool config_load(AppConfig *c, const char *path);
bool config_save(const AppConfig *c, const char *path);
const char *config_default_path(void);

#endif // FANGS_CONFIG_H
