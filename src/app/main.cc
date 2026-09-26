#include <signal.h>
#include <stdio.h>
#include <stdlib.h>

#include <atomic>
#include <exception>
#include <string>
#include <vector>

extern "C"
{
#include <libavformat/avformat.h>
#ifdef HAVE_AVDEVICE
#include <libavdevice/avdevice.h>
#endif
}

#include "app/App.hpp"
#include "common/Config.hpp"
#include "common/Logger.hpp"

static std::atomic<bool> g_quit(false);

static void onSignal(int) { g_quit = true; }

static void usage(const char *prog)
{
    printf("Usage:\n"
           "  %s -c <config.ini>                         按配置文件运行(多路/多模型/推流)\n"
           "  %s <model.rknn> <source0> [source1 ...]    快速运行: 单模型, 本地窗口显示\n"
           "source: MP4 文件 / rtsp:// / rtmp:// / 摄像头序号(0 -> /dev/video0)\n",
           prog, prog);
}

int main(int argc, char **argv)
{
    setThreadName("main");

    // 推流/GB28181 的 TCP 连接被对端关闭后继续写会触发 SIGPIPE(默认直接杀死进程)
    signal(SIGPIPE, SIG_IGN);
    struct sigaction sa;
    sa.sa_handler = onSignal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);
    std::set_terminate([] {
        LOGE("std::terminate called (joinable std::thread destroyed or uncaught exception)");
        abort();
    });

    AppConfig cfg;
    std::string err;
    if (argc == 3 && std::string(argv[1]) == "-c")
    {
        if (!AppConfig::loadFromFile(argv[2], cfg, err))
        {
            LOGE("config error: %s", err.c_str());
            return -1;
        }
    }
    else if (argc >= 3)
    {
        std::vector<std::string> sources;
        for (int i = 2; i < argc; i++)
        {
            std::string s = argv[i];
            if (s.size() == 1 && isdigit((unsigned char)s[0]))
                s = "/dev/video" + s; // 兼容旧版本: 摄像头序号
            sources.push_back(s);
        }
        cfg = AppConfig::makeDefault(argv[1], sources);
        if (!cfg.validate(err))
        {
            LOGE("config error: %s", err.c_str());
            return -1;
        }
    }
    else
    {
        usage(argv[0]);
        return -1;
    }

    avformat_network_init();
#ifdef HAVE_AVDEVICE
    avdevice_register_all();
#endif
    av_log_set_level(AV_LOG_ERROR);

    int ret = 0;
    {
        App app;
        if (app.init(cfg) != 0)
        {
            LOGE("init failed");
            ret = -1;
        }
        else
        {
            app.run(g_quit);
        }
        app.stop();
    }
    avformat_network_deinit();
    return ret;
}
