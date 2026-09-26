#include "infer/InferWorker.hpp"

#include <chrono>

#include "common/FpsController.hpp"
#include "common/Logger.hpp"
#include "common/PerfMonitor.hpp"

InferWorker::InferWorker(int channel, const InferConfig &cfg, Mbuffer *in, Mbuffer *out, ModelManager *models,
                         const DetectionFusion *fusion)
    : ch_(channel), cfg_(cfg), in_(in), out_(out), models_(models), fusion_(fusion)
{
    std::string p = "ch" + std::to_string(ch_);
    auto &pm = PerfMonitor::instance();
    st_wait_frame_ = pm.stat(p + ".wait_frame");
    st_infer_ = pm.stat(p + ".infer");
    st_fusion_ = pm.stat(p + ".fusion");
    st_out_ = pm.stat(p + ".out");
    st_drop_frame_ = pm.stat(p + ".drop.frame");
    st_drop_fps_ = pm.stat(p + ".drop.fps");
    st_reuse_ = pm.stat(p + ".reuse");
}

InferWorker::~InferWorker()
{
    requestStop();
    join();
}

void InferWorker::start()
{
    running_ = true;
    thread_.start("ch" + std::to_string(ch_) + "-infer", [this] { run(); });
}

void InferWorker::requestStop() { running_ = false; }

void InferWorker::join() { thread_.join(); }

void InferWorker::run()
{
    static const cv::Scalar kModelColors[] = {{0, 255, 255}, {255, 0, 255}, {255, 255, 0}, {0, 128, 255}};
    FpsController fps(cfg_.target_fps);
    uint64_t last_seq = 0;
    uint64_t processed = 0;
    std::vector<detect_result_group_t> groups;
    ModelManager::Workspace ws; // 本路专用的推理工作区(模型输入/输出缓冲), 复用避免每帧分配
    std::vector<FusedDetection> last_dets;
    int64_t last_infer_ms = 0;
    bool have_result = false;

    while (running_)
    {
        FrameData frame;
        auto wait_start = std::chrono::steady_clock::now();
        if (!in_->waitNew(frame, last_seq, 200))
        {
            if (in_->closed())
                break;
            continue;
        }
        // 等新帧的时间: 推理计时之外的主要空闲, 偏大说明解码出帧不均匀或推理比出帧快
        st_wait_frame_->add(
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - wait_start).count());
        // 序号不连续 = 推理线程跟不上解码, 中间帧被 Mbuffer 覆盖丢弃
        if (last_seq != 0 && frame.seq > last_seq + 1)
            st_drop_frame_->inc(frame.seq - last_seq - 1);
        last_seq = frame.seq;

        // 目标帧率控制: 超出 target_fps 的帧直接丢弃
        if (!fps.accept())
        {
            st_drop_fps_->inc();
            continue;
        }

        int64_t now = nowMs();
        bool stale = !have_result || now - last_infer_ms > cfg_.reuse_max_ms;
        bool do_infer = (processed % cfg_.infer_interval == 0) || stale;
        processed++;

        if (do_infer)
        {
            {
                ScopedTimer t(st_infer_);
                // 线程池并发提交所有模型并等待: 得到 g1..gN
                if (models_->inferAll(frame.img, groups, ws) != 0)
                    LOGW("ch%d: inference failed", ch_);
            }
            {
                ScopedTimer t(st_fusion_);
                last_dets = fusion_->fuseDetections(groups);
            }
            last_infer_ms = nowMs();
            have_result = true;
            if (cfg_.draw_model_boxes)
            {
                for (size_t k = 0; k < groups.size(); k++)
                    RknnLite::drawResults(frame.img, groups[k], kModelColors[k % 4]);
            }
        }
        else
        {
            st_reuse_->inc();
        }

        // 结果复用: 未推理的帧也绘制最近一次的融合结果(未过期时)
        if (have_result && nowMs() - last_infer_ms <= cfg_.reuse_max_ms)
            DetectionFusion::drawFusedDetections(frame.img, last_dets, (int)models_->size());

        // frame.img 只有本线程持有(解码线程每帧写入新的 Mat), 可以直接在上面画框
        out_->write(frame.img, frame.pts);
        st_out_->inc();
    }
}
