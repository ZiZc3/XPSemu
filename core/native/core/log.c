/*
 * hakuX native core — logging to stderr and the Android log.
 */
#include "native/core/nc.h"

#include <stdio.h>
#include <unistd.h>

#ifdef __ANDROID__
#include <android/log.h>
#endif

void nc_log(const char *tag, const char *fmt, ...)
{
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
#if defined(__PROSPERO__)
    /* one write by the logging thread, no stdio lock: a starved thread holding
     * stderr's lock froze everything that logged in the Chrome port */
    extern void rx_log_line(const char *text, size_t len);
    char line[1200];
    const int n = snprintf(line, sizeof(line), "%s: %s\n", tag, buf);
    rx_log_line(line, n < (int)sizeof(line) ? (size_t)n : sizeof(line) - 1);
#else
    fprintf(stderr, "%s: %s\n", tag, buf);
#endif
#ifdef __ANDROID__
    __android_log_write(ANDROID_LOG_INFO, tag, buf);
#endif
}

void nc_dump_line(int fd, const char *line, size_t len)
{
    if (fd >= 0) {
        write(fd, line, len);
        return;
    }
    char tag[64] = "hakuX-native";
    const char *colon = memchr(line, ':', len);
    if (colon && (size_t)(colon - line) < sizeof(tag)) {
        memcpy(tag, line, (size_t)(colon - line));
        tag[colon - line] = '\0';
        len -= (size_t)(colon - line) + 1;
        line = colon + 1;
    }
    while (len && *line == ' ') {
        line++;
        len--;
    }
    while (len && line[len - 1] == '\n') {
        len--;
    }
    nc_log(tag, "%.*s", (int)len, line);
}
