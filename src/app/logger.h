#ifndef QNETLINK_LOGGER_H
#define QNETLINK_LOGGER_H

typedef enum LogLevel {
    LOG_LEVEL_DEBUG,
    LOG_LEVEL_INFO,
    LOG_LEVEL_WARN,
    LOG_LEVEL_ERROR
} LogLevel;

void logger_set_context(const char *mode, const char *transport, int verbose);
void logger_log(LogLevel level, const char *component, const char *event,
                const char *format, ...);

#endif
