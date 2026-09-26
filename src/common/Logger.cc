#include "common/Logger.hpp"

#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <sys/time.h>
#include <time.h>

#include <algorithm>
#include <atomic>
#include <mutex>

static std::atomic<int> g_level(LOG_LEVEL_INFO);
static std::mutex g_log_mtx;

void setLogLevel(LogLevel level) { g_level = level; }

LogLevel logLevelFromString(const std::string &s)
{
    std::string v = s;
    std::transform(v.begin(), v.end(), v.begin(), ::tolower);
    if (v == "debug")
        return LOG_LEVEL_DEBUG;
    if (v == "warn" || v == "warning")
        return LOG_LEVEL_WARN;
    if (v == "error")
        return LOG_LEVEL_ERROR;
    return LOG_LEVEL_INFO;
}

void setThreadName(const std::string &name)
{
    // Linux 限制线程名最长 15 字节
    pthread_setname_np(pthread_self(), name.substr(0, 15).c_str());
}

void logPrint(LogLevel level, const char *fmt, ...)
{
    if (level < g_level.load())
        return;

    static const char tags[] = {'D', 'I', 'W', 'E'};
    // 先按栈缓冲格式化, 超长(如性能报告)再按实际长度分配
    char stack_buf[1024];
    std::string heap_buf;
    const char *msg = stack_buf;
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(stack_buf, sizeof(stack_buf), fmt, ap);
    va_end(ap);
    if (n >= (int)sizeof(stack_buf))
    {
        heap_buf.resize(n + 1);
        va_start(ap, fmt);
        vsnprintf(&heap_buf[0], heap_buf.size(), fmt, ap);
        va_end(ap);
        msg = heap_buf.c_str();
    }

    struct timeval tv;
    gettimeofday(&tv, nullptr);
    struct tm tm_now;
    localtime_r(&tv.tv_sec, &tm_now);
    char ts[32];
    strftime(ts, sizeof(ts), "%H:%M:%S", &tm_now);

    char tname[16] = {0};
    pthread_getname_np(pthread_self(), tname, sizeof(tname));

    std::lock_guard<std::mutex> lock(g_log_mtx);
    FILE *out = level >= LOG_LEVEL_WARN ? stderr : stdout;
    fprintf(out, "%s.%03d [%c] [%s] %s\n", ts, (int)(tv.tv_usec / 1000), tags[level], tname, msg);
    fflush(out);
}
