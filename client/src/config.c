#include "config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void trim(char *s)
{
    char *p = s;
    while (*p == ' ' || *p == '\t')
        p++;
    if (p != s)
        memmove(s, p, strlen(p) + 1);
    size_t n = strlen(s);
    while (n && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\r' || s[n - 1] == '\n'))
        s[--n] = 0;
}

int hidway_config_load(const char *path, hidway_config_t *cfg)
{
    /* Defaults: a placeholder Tailscale CGNAT address; the user sets the real
     * one in hidway.ini. */
    strcpy(cfg->target, "100.100.100.100");
    cfg->port = 47800;

    FILE *f = fopen(path, "r");
    if (!f)
        return 0;

    char line[256];
    while (fgets(line, sizeof line, f)) {
        char *hash = strpbrk(line, "#;");
        if (hash)
            *hash = 0;
        char *eq = strchr(line, '=');
        if (!eq)
            continue;
        *eq = 0;
        char *key = line, *val = eq + 1;
        trim(key);
        trim(val);
        if (strcmp(key, "target") == 0 && *val) {
            strncpy(cfg->target, val, sizeof cfg->target - 1);
            cfg->target[sizeof cfg->target - 1] = 0;
        } else if (strcmp(key, "port") == 0) {
            int p = atoi(val);
            if (p > 0 && p < 65536)
                cfg->port = p;
        }
    }
    fclose(f);
    return 1;
}
