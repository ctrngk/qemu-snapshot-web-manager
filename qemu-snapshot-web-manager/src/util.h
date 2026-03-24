#ifndef UTIL_H
#define UTIL_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <time.h>

typedef enum { LOG_DEBUG, LOG_INFO, LOG_WARN, LOG_ERROR } log_level_t;

void log_msg(log_level_t level, const char *fmt, ...);
char *str_dup(const char *s);
char *str_fmt(const char *fmt, ...);           /* returns malloc'd formatted string */
int   str_starts_with(const char *str, const char *prefix);
int   str_eq(const char *a, const char *b);
char *url_decode(const char *src);             /* returns malloc'd decoded string */
char *path_segment(const char *url, int index); /* extract segment: "/api/vms/foo" idx=2 → "foo" */

#endif
