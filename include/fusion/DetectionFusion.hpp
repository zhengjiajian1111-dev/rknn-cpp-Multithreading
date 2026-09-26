#ifndef FUSION_DETECTION_FUSION_HPP
#define FUSION_DETECTION_FUSION_HPP

#include <cstdint>
#include <string>
#include <vector>

#include "common/Config.hpp"
#include "infer/postprocess.h"
#include "opencv2/core/core.hpp"

struct FusedDetection
{
    cv::Rect2f box;
    int cls_id = 0;
    std::string name;
    float score = 0.f;
    int votes = 0;           // 有几个模型检出了该目标
    uint32_t model_mask = 0; // 检出该目标的模型集合(bit i = 模型 i)
};

// s5. 多模型检测结果融合
//
// 流程: fuseDetections(g1, g2, ..., gN)
//   1. 展平所有模型的检测框, 过滤低分框, 按 分数*模型权重 降序排序;
//   2. 关联(association): 依次为每个框寻找 IoU 最大且 > iou_thresh 的同类簇,
//      同一个簇中每个模型最多贡献一个框(模型内部已做过 NMS, 同模型的两个框必然是不同目标);
//   3. 簇内融合: weighted(加权框融合 WBF) / confidence(取最高分框, 置信度 noisy-OR 提升) / nms(只保留最高分框);
//   4. 投票过滤: votes < min_votes 的簇丢弃 —— 误检/漏检权衡的核心旋钮:
//        min_votes=1  : 任意模型检出即保留(并集), 召回高, 误检多;
//        min_votes=N  : 所有模型都检出才保留(交集), 精度高, 漏检多;
//   5. 对融合结果再做一次 applyNMS, 去掉相邻簇之间残留的重叠框.
class DetectionFusion
{
public:
    enum class Method
    {
        NMS,
        WEIGHTED,
        CONFIDENCE,
    };

    DetectionFusion() = default;
    DetectionFusion(const FusionConfig &cfg, const std::vector<float> &model_weights);

    std::vector<FusedDetection> fuseDetections(const std::vector<detect_result_group_t> &groups) const;
    std::vector<FusedDetection> fuseDetections(const detect_result_group_t &g1, const detect_result_group_t &g2,
                                               const detect_result_group_t &g3, const detect_result_group_t &g4) const;

    static float calculateIoU(const cv::Rect2f &a, const cv::Rect2f &b);
    // 标准 NMS: 按分数降序, 高分框只抑制与其 IoU > thresh 的"同类"低分框
    static std::vector<FusedDetection> applyNMS(std::vector<FusedDetection> dets, float iou_thresh,
                                                bool class_agnostic);
    static void drawFusedDetections(cv::Mat &img, const std::vector<FusedDetection> &dets, int num_models);

private:
    struct Candidate
    {
        cv::Rect2f box;
        int cls_id;
        float score;  // 原始分数
        float weight; // 模型权重
        int model;
        const char *name;
    };

    FusedDetection weightedFusion(const std::vector<Candidate> &cluster, int num_models) const;
    FusedDetection confidenceFusion(const std::vector<Candidate> &cluster) const;

    Method method_ = Method::WEIGHTED;
    float iou_thresh_ = 0.55f;
    float nms_thresh_ = 0.45f;
    float score_thresh_ = 0.25f;
    int min_votes_ = 1;
    bool class_agnostic_ = false;
    std::vector<float> weights_;
};

#endif
