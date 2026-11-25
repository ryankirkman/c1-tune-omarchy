// Timestamped lines on stderr (the journal, when started by the session).
#pragma once

#include <cstdarg>
#include <cstdio>
#include <ctime>

inline void logLine(const char *format, ...) __attribute__((format(printf, 1, 2)));
inline void logLine(const char *format, ...) {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    char line[1024];
    va_list args;
    va_start(args, format);
    vsnprintf(line, sizeof line, format, args);
    va_end(args);
    fprintf(stderr, "[%5ld.%03ld] %s\n", long(ts.tv_sec % 100000), ts.tv_nsec / 1000000, line);
}
