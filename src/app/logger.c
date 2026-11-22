#include "logger.h"
#include "version.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

static const char *log_mode = "unknown";
static const char *log_transport = "unknown";
static LogLevel minimum_level = LOG_LEVEL_INFO;

static const char *level_name(LogLevel level)
{
    switch (level) {
    case LOG_LEVEL_DEBUG: return "DEBUG";
    case LOG_LEVEL_INFO: return "INFO";
    case LOG_LEVEL_WARN: return "WARN";
    case LOG_LEVEL_ERROR: return "ERROR";
    default: return "ERROR";
    }
}

static void write_quoted(FILE *out, const char *text)
{
    const unsigned char *p = (const unsigned char *)(text ? text : "");
    fputc('"', out);
    for (; *p; ++p) {
        switch (*p) {
        case '\\': fputs("\\\\", out); break;
        case '"': fputs("\\\"", out); break;
        case '\n': fputs("\\n", out); break;
        case '\r': fputs("\\r", out); break;
        case '\t': fputs("\\t", out); break;
        default:
            if (*p < 0x20U || *p == 0x7fU)
                fprintf(out, "\\x%02x", (unsigned)*p);
            else
                fputc(*p, out);
        }
    }
    fputc('"', out);
}

void logger_set_context(const char *mode, const char *transport, int verbose)
{
    log_mode = mode ? mode : "unknown";
    log_transport = transport ? transport : "unknown";
    minimum_level = verbose ? LOG_LEVEL_DEBUG : LOG_LEVEL_INFO;
}

void logger_log(LogLevel level, const char *component, const char *event,
                const char *format, ...)
{
    char message[1024];
    char timestamp[32] = "1970-01-01T00:00:00Z";
    time_t now = time(NULL);
    struct tm utc;
    va_list args;

    if (level < minimum_level) return;
    if (now != (time_t)-1) {
#ifdef _WIN32
        if (gmtime_s(&utc, &now) == 0)
#else
        if (gmtime_r(&now, &utc) != NULL)
#endif
            (void)strftime(timestamp, sizeof(timestamp), "%Y-%m-%dT%H:%M:%SZ", &utc);
    }

    va_start(args, format);
    (void)vsnprintf(message, sizeof(message), format ? format : "", args);
    va_end(args);

    fprintf(stderr, "ts=%s build=%s level=%s mode=%s transport=%s component=%s event=%s message=",
            timestamp, QNETLINK_BUILD_ID, level_name(level), log_mode, log_transport,
            component ? component : "app", event ? event : "event");
    write_quoted(stderr, message);
    fputc('\n', stderr);
}
