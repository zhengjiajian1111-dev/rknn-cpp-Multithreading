#ifndef INFER_RKNN_LITE_HPP
#define INFER_RKNN_LITE_HPP

#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "common/Config.hpp"
#include "infer/postprocess.h"
#include "opencv2/core/core.hpp"
#include "rknn_api.h"

class PerfStat;

// rknn_lite: 一个 RKNN 上下文实例(YOLOv5, 1 输入 3 输出)
//  * 第一个实例 rknn_init 加载模型, 其余实例 rknn_dup_context 复用权重, 只增加少量内存;
//  * 每个实例通过 rknn_set_core_mask 绑定到指定 NPU 核心;
//  * 一个上下文同一时刻只能被一个线程使用, 由 ModelManager 的实例池保证独占.
class RknnLite
{
public:
    RknnLite(const ModelConfig &cfg, int instance_id, std::shared_ptr<const std::vector<std::string>> labels,
             bool use_rga);
    ~RknnLite();

    RknnLite(const RknnLite &) = delete;
    RknnLite &operator=(const RknnLite &) = delete;

    // master 为空: 从文件加载; 否则复用 master 的权重
    int init(RknnLite *master, rknn_core_mask core_mask);

    // 前处理(BGR->RGB + 缩放) -> rknn_inputs_set / rknn_run / rknn_outputs_get -> post_process 解码 + NMS
    int interf(const cv::Mat &ori_img, detect_result_group_t &result);

    // 在 ori_img 上画模型框(调试用; 多模型并行时由推理线程在全部完成后统一绘制, 避免并发写同一图像)
    static void drawResults(cv::Mat &img, const detect_result_group_t &group, const cv::Scalar &color);

    int instanceId() const { return instance_id_; }
    const std::string &modelName() const { return cfg_.name; }

private:
    ModelConfig cfg_;
    int instance_id_;
    std::shared_ptr<const std::vector<std::string>> labels_;
    bool use_rga_;
    std::mutex mtx_;

    rknn_context ctx_ = 0;
    bool ctx_ready_ = false;
    rknn_input_output_num io_num_{};
    std::vector<rknn_tensor_attr> input_attrs_;
    std::vector<rknn_tensor_attr> output_attrs_;
    std::vector<int32_t> out_zps_;
    std::vector<float> out_scales_;
    int grid_hw_[3][2] = {{0, 0}, {0, 0}, {0, 0}};
    int num_classes_ = 80;
    int model_w_ = 0, model_h_ = 0, model_c_ = 0;
    cv::Mat input_; // 复用的模型输入缓冲

    PerfStat *st_pre_ = nullptr;
    PerfStat *st_npu_ = nullptr;
    PerfStat *st_post_ = nullptr;
};

#endif
