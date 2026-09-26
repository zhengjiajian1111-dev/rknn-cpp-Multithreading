#include "fusion/DetectionFusion.hpp"

#include <stdio.h>

#include <algorithm>
#include <cmath>

#include "opencv2/imgproc/imgproc.hpp"

DetectionFusion::DetectionFusion(const FusionConfig &cfg, const std::vector<float> &model_weights)
    : iou_thresh_(cfg.iou_thresh), nms_thresh_(cfg.nms_thresh), score_thresh_(cfg.score_thresh),
      min_votes_(std::max(1, cfg.min_votes)), class_agnostic_(cfg.class_agnostic), weights_(model_weights)
{
    if (cfg.method == "nms")
        method_ = Method::NMS;
    else if (cfg.method == "confidence")
        method_ = Method::CONFIDENCE;
    else
        method_ = Method::WEIGHTED;
}

float DetectionFusion::calculateIoU(const cv::Rect2f &a, const cv::Rect2f &b)
{
    float inter = (a & b).area();
    float uni = a.area() + b.area() - inter;
    return uni <= 0.f ? 0.f : inter / uni;
}

std::vector<FusedDetection> DetectionFusion::fuseDetections(const detect_result_group_t &g1,
                                                            const detect_result_group_t &g2,
                                                            const detect_result_group_t &g3,
                                                            const detect_result_group_t &g4) const
{
    return fuseDetections(std::vector<detect_result_group_t>{g1, g2, g3, g4});
}

FusedDetection DetectionFusion::weightedFusion(const std::vector<Candidate> &cluster, int num_models) const
{
    // WBF: 以 分数*权重 为权, 对框坐标加权平均; 置信度按"参与模型数/总模型数"折算,
    // 只有部分模型检出的目标置信度会被降低
    float wsum = 0.f, ssum = 0.f, x1 = 0.f, y1 = 0.f, x2 = 0.f, y2 = 0.f;
    for (const auto &c : cluster)
    {
        float w = c.score * c.weight;
        wsum += w;
        ssum += c.weight;
        x1 += w * c.box.x;
        y1 += w * c.box.y;
        x2 += w * (c.box.x + c.box.width);
        y2 += w * (c.box.y + c.box.height);
    }
    FusedDetection d;
    if (wsum > 0)
    {
        x1 /= wsum;
        y1 /= wsum;
        x2 /= wsum;
        y2 /= wsum;
    }
    d.box = cv::Rect2f(x1, y1, x2 - x1, y2 - y1);
    float mean = ssum > 0 ? wsum / ssum : 0.f;
    d.score = mean * std::min<int>((int)cluster.size(), num_models) / (float)num_models;
    d.cls_id = cluster[0].cls_id;
    d.name = cluster[0].name;
    return d;
}

FusedDetection DetectionFusion::confidenceFusion(const std::vector<Candidate> &cluster) const
{
    // 取最高分框的位置; 置信度 noisy-OR: 1 - Π(1 - s_i), 多个模型一致时置信度提升
    float miss = 1.f;
    for (const auto &c : cluster)
        miss *= 1.f - std::min(1.f, std::max(0.f, c.score * c.weight));
    FusedDetection d;
    d.box = cluster[0].box;
    d.score = 1.f - miss;
    d.cls_id = cluster[0].cls_id;
    d.name = cluster[0].name;
    return d;
}

std::vector<FusedDetection> DetectionFusion::fuseDetections(const std::vector<detect_result_group_t> &groups) const
{
    const int num_models = (int)groups.size();
    std::vector<Candidate> cands;
    for (int m = 0; m < num_models; m++)
    {
        float w = m < (int)weights_.size() ? weights_[m] : 1.f;
        for (int i = 0; i < groups[m].count; i++)
        {
            const detect_result_t &r = groups[m].results[i];
            if (r.prop < score_thresh_)
                continue;
            Candidate c;
            c.box = cv::Rect2f((float)r.box.left, (float)r.box.top, (float)(r.box.right - r.box.left),
                               (float)(r.box.bottom - r.box.top));
            c.cls_id = r.cls_id;
            c.score = r.prop;
            c.weight = w;
            c.model = m;
            c.name = r.name;
            cands.push_back(c);
        }
    }
    std::stable_sort(cands.begin(), cands.end(),
                     [](const Candidate &a, const Candidate &b) { return a.score * a.weight > b.score * b.weight; });

    // 关联: 贪心地把每个框分配给 IoU 最大的簇
    std::vector<std::vector<Candidate>> clusters;
    std::vector<cv::Rect2f> cluster_box; // 簇当前的代表框(按分数加权平均), 用于后续匹配
    std::vector<uint32_t> cluster_mask;
    for (const auto &c : cands)
    {
        int best = -1;
        float best_iou = iou_thresh_;
        for (size_t k = 0; k < clusters.size(); k++)
        {
            if (!class_agnostic_ && clusters[k][0].cls_id != c.cls_id)
                continue;
            if (cluster_mask[k] & (1u << c.model))
                continue;
            float iou = calculateIoU(cluster_box[k], c.box);
            if (iou > best_iou)
            {
                best_iou = iou;
                best = (int)k;
            }
        }
        if (best < 0)
        {
            clusters.push_back({c});
            cluster_box.push_back(c.box);
            cluster_mask.push_back(1u << c.model);
        }
        else
        {
            clusters[best].push_back(c);
            cluster_mask[best] |= 1u << c.model;
            cluster_box[best] = weightedFusion(clusters[best], num_models).box;
        }
    }

    std::vector<FusedDetection> out;
    for (size_t k = 0; k < clusters.size(); k++)
    {
        const auto &cl = clusters[k];
        if ((int)cl.size() < min_votes_)
            continue;
        FusedDetection d;
        switch (method_)
        {
        case Method::WEIGHTED:
            d = weightedFusion(cl, num_models);
            break;
        case Method::CONFIDENCE:
            d = confidenceFusion(cl);
            break;
        case Method::NMS:
        default:
            d.box = cl[0].box;
            d.score = cl[0].score;
            d.cls_id = cl[0].cls_id;
            d.name = cl[0].name;
            break;
        }
        d.votes = (int)cl.size();
        d.model_mask = cluster_mask[k];
        out.push_back(d);
    }
    return applyNMS(std::move(out), nms_thresh_, class_agnostic_);
}

std::vector<FusedDetection> DetectionFusion::applyNMS(std::vector<FusedDetection> dets, float iou_thresh,
                                                      bool class_agnostic)
{
    std::stable_sort(dets.begin(), dets.end(),
                     [](const FusedDetection &a, const FusedDetection &b) { return a.score > b.score; });
    std::vector<bool> suppressed(dets.size(), false);
    std::vector<FusedDetection> keep;
    for (size_t i = 0; i < dets.size(); i++)
    {
        if (suppressed[i])
            continue;
        keep.push_back(dets[i]);
        for (size_t j = i + 1; j < dets.size(); j++)
        {
            if (suppressed[j])
                continue;
            if (!class_agnostic && dets[j].cls_id != dets[i].cls_id)
                continue;
            if (calculateIoU(dets[i].box, dets[j].box) > iou_thresh)
                suppressed[j] = true;
        }
    }
    return keep;
}

static cv::Scalar classColor(int cls)
{
    static const cv::Scalar palette[] = {
        {56, 56, 255}, {151, 157, 255}, {31, 112, 255}, {29, 178, 255}, {49, 210, 207}, {10, 249, 72},
        {23, 204, 146}, {134, 219, 61}, {52, 147, 26}, {187, 212, 0}, {168, 153, 44}, {255, 194, 0},
        {147, 69, 52}, {255, 115, 100}, {236, 24, 0}, {255, 56, 132}, {133, 0, 82}, {255, 56, 203},
    };
    return palette[(cls < 0 ? -cls : cls) % (int)(sizeof(palette) / sizeof(palette[0]))];
}

void DetectionFusion::drawFusedDetections(cv::Mat &img, const std::vector<FusedDetection> &dets, int num_models)
{
    char text[96];
    for (const auto &d : dets)
    {
        cv::Scalar color = classColor(d.cls_id);
        cv::Rect r(cv::Point((int)d.box.x, (int)d.box.y),
                   cv::Point((int)(d.box.x + d.box.width), (int)(d.box.y + d.box.height)));
        cv::rectangle(img, r, color, 2);
        if (num_models > 1)
            snprintf(text, sizeof(text), "%s %.0f%% %d/%d", d.name.c_str(), d.score * 100, d.votes, num_models);
        else
            snprintf(text, sizeof(text), "%s %.0f%%", d.name.c_str(), d.score * 100);
        int baseline = 0;
        cv::Size ts = cv::getTextSize(text, cv::FONT_HERSHEY_SIMPLEX, 0.5, 1, &baseline);
        int ty = std::max(r.y, ts.height + 4);
        cv::rectangle(img, cv::Rect(r.x, ty - ts.height - 4, ts.width + 4, ts.height + 4), color, cv::FILLED);
        cv::putText(img, text, cv::Point(r.x + 2, ty - 3), cv::FONT_HERSHEY_SIMPLEX, 0.5, cv::Scalar(255, 255, 255),
                    1);
    }
}
