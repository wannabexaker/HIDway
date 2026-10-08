#include "config.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <ctype.h>
#include <limits.h>
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

/* --------------------------------------------------------------- hotkeys */

static unsigned key_name_to_vk(const char *k)
{
    if (strlen(k) == 1) {
        char c = (char)toupper((unsigned char)k[0]);
        if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))
            return (unsigned)c;
    }
    if ((k[0] == 'f' || k[0] == 'F') && isdigit((unsigned char)k[1])) {
        int n = atoi(k + 1);
        if (n >= 1 && n <= 24)
            return VK_F1 + (unsigned)(n - 1);
    }
    if (!strcmp(k, "end")) return VK_END;
    if (!strcmp(k, "home")) return VK_HOME;
    if (!strcmp(k, "ins") || !strcmp(k, "insert")) return VK_INSERT;
    if (!strcmp(k, "del") || !strcmp(k, "delete")) return VK_DELETE;
    if (!strcmp(k, "space")) return VK_SPACE;
    if (!strcmp(k, "esc") || !strcmp(k, "escape")) return VK_ESCAPE;
    if (!strcmp(k, "tab")) return VK_TAB;
    if (!strcmp(k, "enter") || !strcmp(k, "return")) return VK_RETURN;
    if (!strcmp(k, "pgup")) return VK_PRIOR;
    if (!strcmp(k, "pgdn")) return VK_NEXT;
    if (!strcmp(k, "up")) return VK_UP;
    if (!strcmp(k, "down")) return VK_DOWN;
    if (!strcmp(k, "left")) return VK_LEFT;
    if (!strcmp(k, "right")) return VK_RIGHT;
    return 0;
}

static void vk_to_name(unsigned vk, char *out, size_t cap)
{
    if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9')) {
        snprintf(out, cap, "%c", (char)tolower((int)vk));
        return;
    }
    if (vk >= VK_F1 && vk <= VK_F24) {
        snprintf(out, cap, "f%u", vk - VK_F1 + 1);
        return;
    }
    switch (vk) {
    case VK_END: strncpy(out, "end", cap); break;
    case VK_HOME: strncpy(out, "home", cap); break;
    case VK_INSERT: strncpy(out, "ins", cap); break;
    case VK_DELETE: strncpy(out, "del", cap); break;
    case VK_SPACE: strncpy(out, "space", cap); break;
    case VK_ESCAPE: strncpy(out, "esc", cap); break;
    case VK_TAB: strncpy(out, "tab", cap); break;
    case VK_RETURN: strncpy(out, "enter", cap); break;
    case VK_PRIOR: strncpy(out, "pgup", cap); break;
    case VK_NEXT: strncpy(out, "pgdn", cap); break;
    case VK_UP: strncpy(out, "up", cap); break;
    case VK_DOWN: strncpy(out, "down", cap); break;
    case VK_LEFT: strncpy(out, "left", cap); break;
    case VK_RIGHT: strncpy(out, "right", cap); break;
    default: snprintf(out, cap, "vk0x%02X", vk); break;
    }
    out[cap - 1] = 0;
}

static void hotkey_format(const hidway_hotkey_t *hk, char *out, size_t cap)
{
    char key[16];
    vk_to_name(hk->vk, key, sizeof key);
    snprintf(out, cap, "%s%s%s%s%s",
             (hk->mods & MOD_CONTROL) ? "ctrl+" : "",
             (hk->mods & MOD_ALT) ? "alt+" : "",
             (hk->mods & MOD_SHIFT) ? "shift+" : "",
             (hk->mods & MOD_WIN) ? "win+" : "",
             key);
    out[cap - 1] = 0;
}

int hidway_hotkey_parse(const char *s, hidway_hotkey_t *hk)
{
    char buf[64];
    strncpy(buf, s, sizeof buf - 1);
    buf[sizeof buf - 1] = 0;
    for (char *p = buf; *p; p++)
        *p = (char)tolower((unsigned char)*p);

    unsigned mods = 0, vk = 0;
    char *save = NULL;
    for (char *tok = strtok_s(buf, "+", &save); tok; tok = strtok_s(NULL, "+", &save)) {
        trim(tok);
        if (!*tok)
            continue;
        if (!strcmp(tok, "ctrl") || !strcmp(tok, "control")) mods |= MOD_CONTROL;
        else if (!strcmp(tok, "alt")) mods |= MOD_ALT;
        else if (!strcmp(tok, "shift")) mods |= MOD_SHIFT;
        else if (!strcmp(tok, "win") || !strcmp(tok, "cmd")) mods |= MOD_WIN;
        else {
            unsigned k = key_name_to_vk(tok);
            if (k)
                vk = k;
        }
    }
    if (!vk)
        return 0;
    hk->mods = mods;
    hk->vk = vk;
    hotkey_format(hk, hk->text, sizeof hk->text);
    return 1;
}

/* ---------------------------------------------------------------- config */

static void set_defaults(hidway_config_t *cfg)
{
    strcpy(cfg->target, "100.100.100.100");
    cfg->port = 47800;
    cfg->send_rate_hz = 250;
    cfg->auto_disarm_ms = 3000;
    cfg->relay_keyboard = 1;
    cfg->relay_mouse = 1;
    cfg->history_enabled = 1;
    hidway_hotkey_parse("ctrl+shift+f12", &cfg->toggle);
    hidway_hotkey_parse("ctrl+alt+shift+end", &cfg->panic);
    cfg->window_x = cfg->window_y = INT_MIN;
}

static int clampi(int v, int lo, int hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

int hidway_config_load(const char *path, hidway_config_t *cfg)
{
    set_defaults(cfg);

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
        if (!*key)
            continue;

        if (!strcmp(key, "target") && *val) {
            strncpy(cfg->target, val, sizeof cfg->target - 1);
            cfg->target[sizeof cfg->target - 1] = 0;
        } else if (!strcmp(key, "port")) {
            cfg->port = clampi(atoi(val), 1, 65535);
        } else if (!strcmp(key, "send_rate_hz")) {
            cfg->send_rate_hz = clampi(atoi(val), 10, 1000);
        } else if (!strcmp(key, "auto_disarm_ms")) {
            cfg->auto_disarm_ms = clampi(atoi(val), 0, 60000);
        } else if (!strcmp(key, "relay_keyboard")) {
            cfg->relay_keyboard = atoi(val) ? 1 : 0;
        } else if (!strcmp(key, "relay_mouse")) {
            cfg->relay_mouse = atoi(val) ? 1 : 0;
        } else if (!strcmp(key, "history_enabled")) {
            cfg->history_enabled = atoi(val) ? 1 : 0;
        } else if (!strcmp(key, "toggle_hotkey")) {
            hidway_hotkey_parse(val, &cfg->toggle);
        } else if (!strcmp(key, "panic_hotkey")) {
            hidway_hotkey_parse(val, &cfg->panic);
        } else if (!strcmp(key, "window_x")) {
            cfg->window_x = (int)strtol(val, NULL, 10);
        } else if (!strcmp(key, "window_y")) {
            cfg->window_y = (int)strtol(val, NULL, 10);
        } else if (!strcmp(key, "key")) {
            strncpy(cfg->key_hex, val, sizeof cfg->key_hex - 1);
            cfg->key_hex[sizeof cfg->key_hex - 1] = 0;
        }
    }
    fclose(f);
    return 1;
}

int hidway_config_save(const char *path, const hidway_config_t *cfg)
{
    FILE *f = fopen(path, "w");
    if (!f)
        return 0;
    fprintf(f, "# HIDway client configuration.\n\n");
    fprintf(f, "target = %s\n", cfg->target);
    fprintf(f, "port = %d\n\n", cfg->port);
    fprintf(f, "send_rate_hz = %d\n", cfg->send_rate_hz);
    fprintf(f, "auto_disarm_ms = %d   # 0 = disabled\n\n", cfg->auto_disarm_ms);
    fprintf(f, "relay_keyboard = %d\n", cfg->relay_keyboard);
    fprintf(f, "relay_mouse = %d\n", cfg->relay_mouse);
    fprintf(f, "history_enabled = %d\n\n", cfg->history_enabled);
    fprintf(f, "toggle_hotkey = %s\n", cfg->toggle.text);
    fprintf(f, "panic_hotkey = %s\n\n", cfg->panic.text);
    if (cfg->window_x != INT_MIN && cfg->window_y != INT_MIN) {
        fprintf(f, "window_x = %d\n", cfg->window_x);
        fprintf(f, "window_y = %d\n", cfg->window_y);
    }
    if (cfg->key_hex[0]) /* keep the end-to-end key across saves */
        fprintf(f, "\n# End-to-end key, same as the relay's key file. Keep secret.\nkey = %s\n",
                cfg->key_hex);
    fclose(f);
    return 1;
}
