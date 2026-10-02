#ifndef HIDWAY_CLIENT_CONFIG_H
#define HIDWAY_CLIENT_CONFIG_H

typedef struct {
    char target[128]; /* Tailscale IP / hostname of the Raspberry Pi relay */
    int port;         /* UDP port of hidwayd */
} hidway_config_t;

/* Load from `path` (INI: target=..., port=...). Missing file/keys keep the
 * defaults. Always leaves `cfg` fully populated. Returns true if a file was
 * read. */
int hidway_config_load(const char *path, hidway_config_t *cfg);

#endif /* HIDWAY_CLIENT_CONFIG_H */
