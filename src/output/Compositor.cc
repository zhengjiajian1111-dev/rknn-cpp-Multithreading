#include "output/Compositor.hpp"

#include <stdio.h>

#include <algorithm>
#include <cmath>

#include "common/FpsController.hpp"
#include "common/Logger.hpp"
#include "common/PerfMonitor.hpp"
#include "common/RgaUtils.hpp"
#include "opencv2/imgproc/imgproc.hpp"

Compositor::Compositor(const MosaicConfig &cfg, std::vector<Mbuffer *> inputs, std::vector<std::string> names,
                       Mbuffer *out, bool use_rga, StatusFn status)
    : cfg_(cfg), inputs_(std::move(inputs)), names_(std::move(names)), out_(out), use_rga_(use_rga && rga::available()),
      status_(std::move(status))
{
    st_compose_ = PerfMonitor::instance().stat("mosaic.compose");
}

Compositor::~Compositor()
{
    requestStop();
    join();
}

void Compositor::start()
{
    running_ = true;
    thread_.start("mosaic", [this] { run(); });
}

void Compositor::requestStop() { running_ = false; }

void Compositor::join() { thread_.join(); }

static void drawLabel(cv::Mat &img, const std::string &text, cv::Point org, double scale)
{
    int baseline = 0;
    cv::Size ts = cv::getTextSize(text, cv::FONT_HERSHEY_SIMPLEX, scale, 1, &baseline);
    cv::rectangle(img, cv::Rect(org.x, org.y - ts.height - 6, ts.width + 8, ts.height + 10), cv::Scalar(0, 0, 0),
                  cv::FILLED);
    cv::putText(img, text, cv::Point(org.x + 4, org.y), cv::FONT_HERSHEY_SIMPLEX, scale, cv::Scalar(0, 255, 0), 1);
}

cv::Mat Compositor::combineImage(const std::vector<cv::Mat> &images, const std::vector<std::string> &labels, int cols,
                                 const cv::Size &canvas_size, bool &use_rga)
{
    const int n = std::max<int>(1, (int)images.size());
    if (cols <= 0)
        cols = (int)std::ceil(std::sqrt((double)n));
    int rows = (n + cols - 1) / cols;
    int cell_w = (canvas_size.width / cols) & ~1;
    int cell_h = (canvas_size.height / rows) & ~1;

    cv::Mat canvas(canvas_size, CV_8UC3, cv::Scalar(0, 0, 0));
    for (int i = 0; i < (int)images.size(); i++)
    {
        cv::Rect cell((i % cols) * cell_w, (i / cols) * cell_h, cell_w, cell_h);
        const cv::Mat &img = images[i];
        if (!img.empty())
        {
            // 等比缩放后居中放入格子
            double s = std::min(cell_w / (double)img.cols, cell_h / (double)img.rows);
            int w = std::max(2, (int)(img.cols * s) & ~1);
            int h = std::max(2, (int)(img.rows * s) & ~1);
            cv::Rect dst(cell.x + ((cell_w - w) / 2 & ~1), cell.y + ((cell_h - h) / 2 & ~1), w, h);
            bool done = false;
            if (use_rga)
            {
                done = rga::resizeInto(img, true, canvas, true, dst);
                if (!done)
                {
                    LOGW("mosaic: rga resize failed, fallback to opencv");
                    use_rga = false;
                }
            }
            if (!done)
            {
                cv::Mat roi = canvas(dst);
                cv::resize(img, roi, dst.size(), 0, 0, cv::INTER_LINEAR);
            }
        }
        if (i < (int)labels.size() && !labels[i].empty())
            drawLabel(canvas, labels[i], cv::Point(cell.x + 6, cell.y + 24), 0.6);
        cv::rectangle(canvas, cell, cv::Scalar(64, 64, 64), 1);
    }
    return canvas;
}

void Compositor::run()
{
    FpsController fps(cfg_.fps);
    const cv::Size canvas(cfg_.width & ~1, cfg_.height & ~1);
    std::vector<PerfStat *> out_stats, infer_stats;
    for (size_t i = 0; i < inputs_.size(); i++)
    {
        out_stats.push_back(PerfMonitor::instance().stat("ch" + std::to_string(i) + ".out"));
        infer_stats.push_back(PerfMonitor::instance().stat("ch" + std::to_string(i) + ".infer"));
    }

    while (running_)
    {
        fps.wait(&running_);
        if (!running_)
            break;
        std::vector<cv::Mat> images(inputs_.size());
        std::vector<std::string> labels(inputs_.size());
        auto now = std::chrono::steady_clock::now();
        for (size_t i = 0; i < inputs_.size(); i++)
        {
            FrameData fd;
            bool fresh = inputs_[i]->peek(fd) && now - fd.ts < std::chrono::seconds(3);
            if (fresh)
                images[i] = fd.img;
            if (!cfg_.show_label)
                continue;
            char buf[160];
            if (fresh)
                snprintf(buf, sizeof(buf), "CH%zu %s | %.1f fps | infer %.1f fps %.0f ms", i, names_[i].c_str(),
                         out_stats[i]->rate(), infer_stats[i]->rate(), infer_stats[i]->avgMs());
            else
                snprintf(buf, sizeof(buf), "CH%zu %s | %s", i, names_[i].c_str(),
                         status_ ? status_((int)i).c_str() : "NO SIGNAL");
            labels[i] = buf;
        }
        cv::Mat mosaic;
        {
            ScopedTimer t(st_compose_);
            mosaic = combineImage(images, labels, cfg_.cols, canvas, use_rga_);
        }
        out_->write(mosaic, nowMs());
    }
}
