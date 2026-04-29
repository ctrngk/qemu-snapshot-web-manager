#include "config.h"
#include "dirty_state.h"
#include "util.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <ctype.h>

void config_defaults(qswm_config_t *cfg)
{
    cfg->port = 9091;
    cfg->idle_timeout = 600;
    snprintf(cfg->uri, sizeof(cfg->uri), "qemu:///system");
    snprintf(cfg->static_dir, sizeof(cfg->static_dir),
             "/usr/local/share/qswm/static");
    snprintf(cfg->dirty_state_path, sizeof(cfg->dirty_state_path),
             DIRTY_STATE_DEFAULT_PATH);
}

void config_parse_line(qswm_config_t *cfg, const char *key, const char *value)
{
    if (strcmp(key, "port") == 0)
        cfg->port = atoi(value);
    else if (strcmp(key, "idle_timeout") == 0)
        cfg->idle_timeout = atoi(value);
    else if (strcmp(key, "uri") == 0)
        snprintf(cfg->uri, sizeof(cfg->uri), "%s", value);
    else if (strcmp(key, "static_dir") == 0)
        snprintf(cfg->static_dir, sizeof(cfg->static_dir), "%s", value);
    else if (strcmp(key, "dirty_state_path") == 0)
        snprintf(cfg->dirty_state_path, sizeof(cfg->dirty_state_path),
                 "%s", value);
    /* unknown keys silently ignored */
}

void config_parse_buffer(qswm_config_t *cfg, const char *buf)
{
    const char *p = buf;
    char line[512];

    while (*p) {
        /* read one line */
        const char *eol = strchr(p, '\n');
        size_t len = eol ? (size_t)(eol - p) : strlen(p);
        if (len >= sizeof(line)) len = sizeof(line) - 1;
        memcpy(line, p, len);
        line[len] = '\0';
        p = eol ? eol + 1 : p + len;

        /* trim leading whitespace */
        char *s = line;
        while (isspace((unsigned char)*s)) s++;

        /* skip empty, comments, section headers */
        if (*s == '\0' || *s == '#' || *s == ';' || *s == '[')
            continue;

        /* split on '=' */
        char *eq = strchr(s, '=');
        if (!eq) continue;

        *eq = '\0';
        char *key = s;
        char *val = eq + 1;

        /* trim trailing whitespace from key */
        char *end = key + strlen(key) - 1;
        while (end > key && isspace((unsigned char)*end)) *end-- = '\0';

        /* trim leading whitespace from value */
        while (isspace((unsigned char)*val)) val++;
        /* trim trailing whitespace from value */
        end = val + strlen(val) - 1;
        while (end > val && isspace((unsigned char)*end)) *end-- = '\0';

        config_parse_line(cfg, key, val);
    }
}

static int cmp_str(const void *a, const void *b)
{
    return strcmp(*(const char **)a, *(const char **)b);
}

int config_load_dropins(qswm_config_t *cfg, const char *conf_dir)
{
    DIR *d = opendir(conf_dir);
    if (!d) return 0; /* no dir = use defaults, not an error */

    /* collect .conf filenames */
    char *files[128];
    int count = 0;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL && count < 128) {
        size_t nlen = strlen(ent->d_name);
        if (nlen > 5 && strcmp(ent->d_name + nlen - 5, ".conf") == 0)
            files[count++] = str_dup(ent->d_name);
    }
    closedir(d);

    /* sort alphabetically */
    qsort(files, (size_t)count, sizeof(char *), cmp_str);

    /* parse each file */
    for (int i = 0; i < count; i++) {
        char path[512];
        snprintf(path, sizeof(path), "%s/%s", conf_dir, files[i]);

        FILE *f = fopen(path, "r");
        if (!f) {
            log_msg(LOG_WARN, "Cannot open config: %s", path);
            free(files[i]);
            continue;
        }

        fseek(f, 0, SEEK_END);
        long sz = ftell(f);
        rewind(f);

        if (sz > 0 && sz < 65536) {
            char *buf = malloc((size_t)sz + 1);
            if (buf) {
                size_t rd = fread(buf, 1, (size_t)sz, f);
                buf[rd] = '\0';
                config_parse_buffer(cfg, buf);
                log_msg(LOG_INFO, "Loaded config: %s", path);
                free(buf);
            }
        }
        fclose(f);
        free(files[i]);
    }

    return count;
}
