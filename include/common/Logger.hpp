#ifndef COMMON_LOGGER_HPP
#define COMMON_LOGGER_HPP

#include <string>

// 简单的线程安全日志: 带时间戳、级别、线程名, 便于多线程排查问题
enum LogLevel
{
    LOG_LEVEL_DEBUG = 0,
    LOG_LEVEL_INFO = 1,
    LOG_LEVEL_WARN = 2,
    LOG_LEVEL_ERROR = 3,
};

void setLogLevel(LogLevel level);
LogLevel logLevelFromString(const std::string &s);
void logPrint(LogLevel level, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

// 设置当前线程名(top -H / htop 中可见, 用于定位哪个线程占用 CPU)
void setThreadName(const std::string &name);

#define LOGD(...) logPrint(LOG_LEVEL_DEBUG, __VA_ARGS__)
#define LOGI(...) logPrint(LOG_LEVEL_INFO, __VA_ARGS__)
#define LOGW(...) logPrint(LOG_LEVEL_WARN, __VA_ARGS__)
#define LOGE(...) logPrint(LOG_LEVEL_ERROR, __VA_ARGS__)

#endif
