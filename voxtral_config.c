/*
 * voxtral_config.c - Configuration file parser
 *
 * Simple key = value format. Lines starting with # are comments.
 * Whitespace around keys and values is trimmed.
 */

#include "voxtral_config.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static void set_defaults(vox_config_t *cfg) {
    cfg->model_dir[0] = '\0';
    cfg->silence_threshold = 0.01f;
    cfg->silence_duration_ms = 2000;
    cfg->processing_interval = 2.0f;
    cfg->sound_enabled = 1;
}

static char *trim(char *s) {
    while (*s == ' ' || *s == '\t') s++;
    char *end = s + strlen(s) - 1;
    while (end > s && (*end == ' ' || *end == '\t' || *end == '\n' || *end == '\r'))
        *end-- = '\0';
    return s;
}

void vox_config_load(vox_config_t *cfg) {
    set_defaults(cfg);

    const char *home = getenv("HOME");
    if (!home) return;

    char dir[512], path[512];
    snprintf(dir, sizeof(dir), "%s/.config/voxtral", home);
    snprintf(path, sizeof(path), "%s/.config/voxtral/config", home);

    /* Create config dir if needed */
    mkdir(dir, 0755);

    FILE *f = fopen(path, "r");
    if (!f) return;

    char line[512];
    while (fgets(line, sizeof(line), f)) {
        char *s = trim(line);
        if (*s == '\0' || *s == '#') continue;

        char *eq = strchr(s, '=');
        if (!eq) continue;

        *eq = '\0';
        char *key = trim(s);
        char *val = trim(eq + 1);

        if (strcmp(key, "model_dir") == 0) {
            snprintf(cfg->model_dir, sizeof(cfg->model_dir), "%s", val);
        } else if (strcmp(key, "silence_threshold") == 0) {
            float v = (float)atof(val);
            if (v > 0) cfg->silence_threshold = v;
        } else if (strcmp(key, "silence_duration_ms") == 0) {
            int v = atoi(val);
            if (v > 0) cfg->silence_duration_ms = v;
        } else if (strcmp(key, "processing_interval") == 0) {
            float v = (float)atof(val);
            if (v > 0) cfg->processing_interval = v;
        } else if (strcmp(key, "sound") == 0) {
            cfg->sound_enabled = (strcmp(val, "off") != 0);
        }
    }
    fclose(f);
}
