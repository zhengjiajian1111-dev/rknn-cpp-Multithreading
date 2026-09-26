#ifndef OUTPUT_COMPOSITOR_HPP
#define OUTPUT_COMPOSITOR_HPP

#include <atomic>
#include <functional>
#include <string>
#include <vector>

#include "common/Config.hpp"
#include "common/Mbuffer.hpp"
#include "common/WorkerThread.hpp"

class PerfStat;

// 拼接线程: 按固定帧率读取各路最新结果 images[i], combineImage 拼成多路画面写入 mosaic Mbuffer.
// 以固定节拍"拉取"而不是被各路"推动", 某一路卡顿/断流不会拖慢整体输出, 该格显示状态提示.
class Compositor
{
public:
    // status(ch) 返回该路状态文字(如 "RECONNECTING"), 没有画面时显示
    using StatusFn = std::function<std::string(int)>;

    Compositor(const MosaicConfig &cfg, std::vector<Mbuffer *> inputs, std::vector<std::string> names, Mbuffer *out,
               bool use_rga, StatusFn status);
    ~Compositor();

    void start();
    void requestStop();
    void join();

    static cv::Mat combineImage(const std::vector<cv::Mat> &images, const std::vector<std::string> &labels, int cols,
                                const cv::Size &canvas_size, bool &use_rga);

private:
    void run();

    MosaicConfig cfg_;
    std::vector<Mbuffer *> inputs_;
    std::vector<std::string> names_;
    Mbuffer *out_;
    bool use_rga_;
    StatusFn status_;
    WorkerThread thread_;
    std::atomic<bool> running_{false};
    PerfStat *st_compose_ = nullptr;
};

#endif
