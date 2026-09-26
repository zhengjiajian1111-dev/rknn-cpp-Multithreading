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

// 一次推理的工作区: 模型输入 + 3 个输出 + letterbox 参数, 由调用方(每路推理线程)持有.
// 前处理/后处理只读写工作区和模型参数, 不需要 NPU 上下文, 因此可以在借实例之前/归还之后执行.
struct InferBuffers
{
    cv::Mat input;                  // 模型输入: RGB, model_h x model_w, NHWC uint8
    std::vector<int8_t> outputs[3]; // 3 个输出头: int8, NCHW, 由 rknn_outputs_get 直接写入
    LetterBox lb;
    bool use_rga = true;            // 前处理用 RGA; 失败后置 false, 该工作区改走 OpenCV
};

// rknn_lite: 一个 RKNN 上下文实例(YOLOv5, 1 输入 3 输出)
//  * 第一个实例 rknn_init 加载模型, 其余实例 rknn_dup_context 复用权重, 只增加少量内存;
//  * 每个实例通过 rknn_set_core_mask 绑定到指定 NPU 核心;
//  * 一次推理拆成三步, 只有 run() 需要独占上下文(由 ModelManager 的实例池保证):
//      prepare(): 前处理 BGR -> 模型输入(RGA/OpenCV)
//      run()    : rknn_inputs_set / rknn_run / rknn_outputs_get, 输出直接写入工作区
//      decode() : 后处理 解码 + NMS
//    prepare/decode 只读初始化后不再变化的模型参数(各实例相同), 可多线程同时调用.
class RknnLite
{
public:
    RknnLite(const ModelConfig &cfg, int instance_id, std::shared_ptr<const std::vector<std::string>> labels);
    ~RknnLite();

    RknnLite(const RknnLite &) = delete;
    RknnLite &operator=(const RknnLite &) = delete;

    // master 为空: 从文件加载; 否则复用 master 的权重
    int init(RknnLite *master, rknn_core_mask core_mask);

    bool prepare(const cv::Mat &ori_img, InferBuffers &buf) const;
    int run(InferBuffers &buf);
    void decode(InferBuffers &buf, detect_result_group_t &result) const;

    // 在 ori_img 上画模型框(调试用; 多模型并行时由推理线程在全部完成后统一绘制, 避免并发写同一图像)
    static void drawResults(cv::Mat &img, const detect_result_group_t &group, const cv::Scalar &color);

    int instanceId() const { return instance_id_; }
    const std::string &modelName() const { return cfg_.name; }

private:
    ModelConfig cfg_;
    int instance_id_;
    std::shared_ptr<const std::vector<std::string>> labels_;
    std::mutex mtx_;

    rknn_context ctx_ = 0;
    bool ctx_ready_ = false;
    rknn_input_output_num io_num_{};
    std::vector<rknn_tensor_attr> input_attrs_;
    std::vector<rknn_tensor_attr> output_attrs_;
    std::vector<int32_t> out_zps_;
    std::vector<float> out_scales_;
    size_t out_sizes_[3] = {0, 0, 0}; // 各输出头 int8 字节数(= 元素个数)
    int grid_hw_[3][2] = {{0, 0}, {0, 0}, {0, 0}};
    int num_classes_ = 80;
    int model_w_ = 0, model_h_ = 0, model_c_ = 0;

    PerfStat *st_pre_ = nullptr;
    PerfStat *st_npu_ = nullptr;
    PerfStat *st_post_ = nullptr;
};

#endif
