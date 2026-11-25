// Log lines go to the journal (`journalctl -t c1-tune`) however the app was started, and to
// stderr as well when it has somewhere to go.
#pragma once

#include <cstdarg>
#include <cstdio>
#include <ctime>
#include <syslog.h>
#include <unistd.h>

inline void logLine(const char *format, ...) __attribute__((format(printf, 1, 2)));
inline void logLine(const char *format, ...) {
    char line[2048];
    va_list args;
    va_start(args, format);
    vsnprintf(line, sizeof line, format, args);
    va_end(args);
    static const bool terminal = [] { openlog("c1-tune", LOG_PID, LOG_USER); return isatty(STDERR_FILENO) != 0; }();
    static const bool file = !terminal && lseek(STDERR_FILENO, 0, SEEK_CUR) >= 0;   // redirected to a file
    // The camera's own chatter (only logged with --verbose) stays out of the journal.
    if (__builtin_strncmp(line, "device: ", 8) != 0) syslog(LOG_INFO, "%s", line);
    if (terminal || file) {
        timespec ts{};
        clock_gettime(CLOCK_MONOTONIC, &ts);
        fprintf(stderr, "[%5ld.%03ld] %s\n", long(ts.tv_sec % 100000), ts.tv_nsec / 1000000, line);
    }
}
