#include "util.h"
#include <ctype.h>

static const char *level_str(log_level_t level)
{
    switch (level) {
    case LOG_DEBUG: return "DEBUG";
    case LOG_INFO:  return "INFO";
    case LOG_WARN:  return "WARN";
    case LOG_ERROR: return "ERROR";
    default:        return "???";
    }
}

void log_msg(log_level_t level, const char *fmt, ...)
{
#ifndef DEBUG
    if (level == LOG_DEBUG)
        return;
#endif

    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);

    char ts[20];
    strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &tm);

    fprintf(stderr, "[%s] [%s] ", ts, level_str(level));

    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);

    fputc('\n', stderr);
}

char *str_dup(const char *s)
{
    if (!s)
        return NULL;
    size_t len = strlen(s) + 1;
    char *copy = malloc(len);
    if (copy)
        memcpy(copy, s, len);
    return copy;
}

char *str_fmt(const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    int len = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);

    if (len < 0)
        return NULL;

    char *buf = malloc((size_t)len + 1);
    if (!buf)
        return NULL;

    va_start(ap, fmt);
    vsnprintf(buf, (size_t)len + 1, fmt, ap);
    va_end(ap);

    return buf;
}

int str_starts_with(const char *str, const char *prefix)
{
    if (!str || !prefix)
        return 0;
    return strncmp(str, prefix, strlen(prefix)) == 0;
}

int str_eq(const char *a, const char *b)
{
    if (a == b)
        return 1;
    if (!a || !b)
        return 0;
    return strcmp(a, b) == 0;
}

static int hex_digit(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

char *url_decode(const char *src)
{
    if (!src)
        return NULL;

    size_t len = strlen(src);
    char *out = malloc(len + 1);
    if (!out)
        return NULL;

    size_t j = 0;
    for (size_t i = 0; i < len; i++) {
        if (src[i] == '%' && i + 2 < len) {
            int hi = hex_digit(src[i + 1]);
            int lo = hex_digit(src[i + 2]);
            if (hi >= 0 && lo >= 0) {
                out[j++] = (char)(hi * 16 + lo);
                i += 2;
                continue;
            }
        }
        if (src[i] == '+') {
            out[j++] = ' ';
        } else {
            out[j++] = src[i];
        }
    }
    out[j] = '\0';
    return out;
}

char *path_segment(const char *url, int index)
{
    if (!url || index < 0)
        return NULL;

    const char *p = url;
    /* skip leading slash */
    if (*p == '/')
        p++;

    int seg = 0;
    while (*p) {
        const char *end = strchr(p, '/');
        if (!end)
            end = p + strlen(p);

        if (seg == index) {
            size_t slen = (size_t)(end - p);
            if (slen == 0)
                return NULL;
            char *result = malloc(slen + 1);
            if (result) {
                memcpy(result, p, slen);
                result[slen] = '\0';
            }
            return result;
        }

        seg++;
        p = (*end == '/') ? end + 1 : end;
    }

    return NULL;
}

/* Base64 decode table */
static const unsigned char b64_table[256] = {
    ['A']=0,['B']=1,['C']=2,['D']=3,['E']=4,['F']=5,['G']=6,['H']=7,
    ['I']=8,['J']=9,['K']=10,['L']=11,['M']=12,['N']=13,['O']=14,['P']=15,
    ['Q']=16,['R']=17,['S']=18,['T']=19,['U']=20,['V']=21,['W']=22,['X']=23,
    ['Y']=24,['Z']=25,['a']=26,['b']=27,['c']=28,['d']=29,['e']=30,['f']=31,
    ['g']=32,['h']=33,['i']=34,['j']=35,['k']=36,['l']=37,['m']=38,['n']=39,
    ['o']=40,['p']=41,['q']=42,['r']=43,['s']=44,['t']=45,['u']=46,['v']=47,
    ['w']=48,['x']=49,['y']=50,['z']=51,['0']=52,['1']=53,['2']=54,['3']=55,
    ['4']=56,['5']=57,['6']=58,['7']=59,['8']=60,['9']=61,['+']=62,['/']=63,
};

char *base64_decode(const char *src, size_t *out_len)
{
    if (!src) return NULL;
    size_t slen = strlen(src);
    size_t alloc = (slen / 4) * 3 + 4;
    unsigned char *out = malloc(alloc);
    if (!out) return NULL;

    size_t j = 0;
    unsigned int buf = 0;
    int bits = 0;
    for (size_t i = 0; i < slen; i++) {
        unsigned char c = (unsigned char)src[i];
        if (c == '=' || c == '\n' || c == '\r' || c == ' ') continue;
        buf = (buf << 6) | b64_table[c];
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out[j++] = (unsigned char)((buf >> bits) & 0xFF);
        }
    }
    out[j] = '\0';
    if (out_len) *out_len = j;
    return (char *)out;
}

/* Escape a string for safe embedding inside a JSON string value.
 * Handles: " → \", \ → \\, newline → \n, tab → \t, CR → \r
 * Returns malloc'd string. Caller frees. */
char *json_escape(const char *src)
{
    if (!src) return str_dup("");

    size_t len = strlen(src);
    /* Worst case: every char needs escaping → 2x */
    char *out = malloc(len * 2 + 1);
    if (!out) return NULL;

    size_t j = 0;
    for (size_t i = 0; i < len; i++) {
        switch (src[i]) {
        case '"':  out[j++] = '\\'; out[j++] = '"';  break;
        case '\\': out[j++] = '\\'; out[j++] = '\\'; break;
        case '\n': out[j++] = '\\'; out[j++] = 'n';  break;
        case '\r': out[j++] = '\\'; out[j++] = 'r';  break;
        case '\t': out[j++] = '\\'; out[j++] = 't';  break;
        default:   out[j++] = src[i]; break;
        }
    }
    out[j] = '\0';
    return out;
}
