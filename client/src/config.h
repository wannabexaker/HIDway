#ifndef HIDWAY_CLIENT_CONFIG_H
#define HIDWAY_CLIENT_CONFIG_H

#include <stddef.h>

/* A hotkey as Win32 RegisterHotKey() inputs, plus its text form for display. */
typedef struct {
    unsigned mods; /* MOD_CONTROL | MOD_ALT | MOD_SHIFT | MOD_WIN */
    unsigned vk;   /* virtual-key code */
    char text[40];
} hidway_hotkey_t;

typedef struct {
    char target[128];       /* Tailscale IP / hostname of the relay */
    int port;               /* UDP port of hidwayd */
    int send_rate_hz;       /* state send rate while armed */
    int auto_disarm_ms;     /* auto-disarm if the link is silent this long (0 = off) */
    int relay_keyboard;     /* relay keyboard input */
    int relay_mouse;        /* relay mouse input */
    int history_enabled;    /* record recent-input history in the UI */
    hidway_hotkey_t toggle; /* arm/disarm hotkey */
    hidway_hotkey_t panic;  /* release everything + disarm */
} hidway_config_t;

/* Load from `path`. Missing file/keys keep defaults; always fully populated.
 * Returns 1 if a file was read, else 0. */
int hidway_config_load(const char *path, hidway_config_t *cfg);

/* Write the config back to `path`. Returns 1 on success. */
int hidway_config_save(const char *path, const hidway_config_t *cfg);

/* Parse "ctrl+shift+f12" into a hotkey. Returns 1 on success. */
int hidway_hotkey_parse(const char *s, hidway_hotkey_t *hk);

#endif /* HIDWAY_CLIENT_CONFIG_H */
