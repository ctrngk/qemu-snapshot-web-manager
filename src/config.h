#ifndef CONFIG_H
#define CONFIG_H

typedef struct {
    int         port;
    int         idle_timeout;   /* seconds, 0 = disabled */
    char        uri[256];
    char        static_dir[256];
    char        dirty_state_path[256];
} qswm_config_t;

/* Fill cfg with compiled-in defaults */
void config_defaults(qswm_config_t *cfg);

/* Parse a single key=value pair into cfg */
void config_parse_line(qswm_config_t *cfg, const char *key, const char *value);

/* Parse an INI-style buffer (one [general] section) into cfg */
void config_parse_buffer(qswm_config_t *cfg, const char *buf);

/* Scan conf_dir for *.conf files in alphabetical order, merge into cfg */
int  config_load_dropins(qswm_config_t *cfg, const char *conf_dir);

#endif
