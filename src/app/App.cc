#include "app/App.hpp"

#include <stdlib.h>

#include <chrono>
#include <thread>

#include "common/Logger.hpp"
#include "common/PerfMonitor.hpp"
#include "common/RgaUtils.hpp"
#include "opencv2/highgui/highgui.hpp"

static std::string loaderStateText(StreamLoader::State s)
{
    switch (s)
    {
    case StreamLoader::kConnecting:
        return "CONNECTING...";
    case StreamLoader::kReconnecting:
        return "RECONNECTING...";
    case StreamLoader::kFinished:
        return "FINISHED";
    default:
        return "NO SIGNAL";
    }
}

int App::init(const AppConfig &cfg)
{
    cfg_ = cfg;
    setLogLevel(logLevelFromString(cfg_.general.log_level));
    cfg_.dump();

    bool use_rga = cfg_.general.use_rga && rga::available();
    if (cfg_.general.use_rga && !rga::available())
        LOGW("built without RGA, image ops fall back to OpenCV");

    // 1. 模型(NPU 上下文)最先初始化: 失败立即返回, 此时还没有任何线程和网络连接
    //    前处理的 RGA 开关独立于其它环节: 普通内存上的 RGA 不一定比 OpenCV 快, 以 <model>.pre 实测为准
    bool pre_rga = cfg_.infer.use_rga && rga::available();
    if (models_.init(cfg_.models, pre_rga, cfg_.infer.threads) != 0)
    {
        LOGE("model init failed");
        return -1;
    }
    std::vector<float> weights;
    for (const auto &m : cfg_.models)
        weights.push_back(m.weight);
    fusion_.reset(new DetectionFusion(cfg_.fusion, weights));

    // 2. 队列与缓冲
    const size_t n = cfg_.sources.size();
    std::vector<Mbuffer *> outputs;
    std::vector<std::string> names;
    for (size_t i = 0; i < n; i++)
    {
        pkt_queues_.emplace_back(new BlockingQueue<VideoPacketPtr>(cfg_.decoder.packet_queue));
        decoded_.emplace_back(new Mbuffer());
        images_.emplace_back(new Mbuffer());
        outputs.push_back(images_.back().get());
        names.push_back(cfg_.sources[i].name);
    }

    // 3. 各级工作对象
    for (size_t i = 0; i < n; i++)
    {
        int ch = (int)i;
        loaders_.emplace_back(new StreamLoader(ch, cfg_.sources[i], pkt_queues_[i].get()));
        decoders_.emplace_back(new DecodeWorker(ch, cfg_.decoder, use_rga, pkt_queues_[i].get(), decoded_[i].get()));
        infers_.emplace_back(
            new InferWorker(ch, cfg_.infer, decoded_[i].get(), images_[i].get(), &models_, fusion_.get()));
    }
    compositor_.reset(new Compositor(cfg_.mosaic, outputs, names, &mosaic_, use_rga,
                                     [this](int ch) { return loaderStateText(loaders_[ch]->state()); }));
    if (!cfg_.pushes.empty())
    {
        streaming_.reset(new StreamingMgr(cfg_.pushes, use_rga));
        if (streaming_->init(&mosaic_, outputs) != 0)
            return -1;
    }

    // 4. 启动: 消费者 -> 生产者
    PerfMonitor::instance().start(cfg_.general.perf_interval);
    if (streaming_)
        streaming_->start();
    compositor_->start();
    for (auto &w : infers_)
        w->start();
    for (auto &d : decoders_)
        d->start();
    for (auto &l : loaders_)
        l->start();
    started_ = true;
    LOGI("pipeline started: %zu channel(s)", n);
    return 0;
}

int App::run(const std::atomic<bool> &quit)
{
    bool display = cfg_.general.display;
    if (display && !getenv("DISPLAY") && !getenv("WAYLAND_DISPLAY"))
    {
        LOGW("no DISPLAY found, run headless");
        display = false;
    }
    const char *win = "rknn multi-stream";
    if (display)
    {
        try
        {
            cv::namedWindow(win, cv::WINDOW_NORMAL);
            cv::resizeWindow(win, 1280, 720);
        }
        catch (const cv::Exception &e)
        {
            LOGW("cannot create window (%s), run headless", e.what());
            display = false;
        }
    }

    uint64_t seq = 0;
    int64_t all_finished_at = 0;
    while (!quit)
    {
        // 所有输入都是不循环的文件且已播完: 留 1 秒让下游处理完最后几帧后退出
        bool all_finished = true;
        for (auto &l : loaders_)
            all_finished = all_finished && l->finished();
        if (all_finished)
        {
            if (all_finished_at == 0)
                all_finished_at = nowMs();
            else if (nowMs() - all_finished_at > 1000)
            {
                LOGI("all sources finished");
                break;
            }
        }

        if (display)
        {
            // cv::imshow 只在主线程调用(GUI 后端普遍要求)
            FrameData fd;
            if (mosaic_.waitNew(fd, seq, 100))
            {
                seq = fd.seq;
                cv::imshow(win, fd.img);
            }
            int key = cv::waitKey(1);
            if (key == 'q' || key == 27)
                break;
        }
        else
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }
    if (display)
        cv::destroyAllWindows();
    return 0;
}

void App::stop()
{
    if (stopped_)
        return;
    stopped_ = true;
    if (!started_)
    {
        models_.shutdown();
        return;
    }
    LOGI("stopping pipeline...");

    // 1. 通知所有线程退出, 关闭队列/缓冲唤醒阻塞中的线程(避免 join 卡死)
    for (auto &l : loaders_)
        l->requestStop();
    for (auto &q : pkt_queues_)
        q->close();
    for (auto &d : decoders_)
        d->requestStop();
    for (auto &m : decoded_)
        m->close();
    for (auto &w : infers_)
        w->requestStop();
    for (auto &m : images_)
        m->close();
    compositor_->requestStop();
    mosaic_.close();
    if (streaming_)
        streaming_->requestStop();

    // 2. 按 生产者 -> 消费者 的顺序 join
    for (auto &l : loaders_)
        l->join();
    for (auto &d : decoders_)
        d->join();
    for (auto &w : infers_)
        w->join();
    compositor_->join();
    if (streaming_)
        streaming_->join(); // 内部完成 RTMP trailer / RTSP TEARDOWN / GB28181 BYE + 注销

    // 3. 所有使用 NPU 的线程都已退出, 最后销毁上下文(先 dup 实例, 后 master)
    models_.shutdown();
    PerfMonitor::instance().stop();
    LOGI("pipeline stopped cleanly");
}
