// Copyright (c) 2021 by Rockchip Electronics Co., Ltd. All Rights Reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//
// 修改说明:
//  1. 去掉全局 labels 与静态初始化标志(多线程并发调用时存在竞争), 改为由模型实例传入;
//  2. 修正 NMS 的类别判断: 原实现用排序后的位置 i 去索引 classIds, 且内层循环没有检查
//     候选框 m 的类别, 会出现跨类别误抑制、同类重复框漏抑制的问题;
//  3. 类别数由输出张量形状推导, 支持自定义类别数的 YOLOv5 模型;
//  4. 坐标映射支持 letterbox(等比缩放 + 填充), 并裁剪到原图范围.

#include "infer/postprocess.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <algorithm>
#include <fstream>
#include <numeric>
#include <set>
#include <vector>

static const int anchors[3][6] = {
    {10, 13, 16, 30, 33, 23},
    {30, 61, 62, 45, 59, 119},
    {116, 90, 156, 198, 373, 326},
};

inline static int clamp(float val, int min, int max) { return val > min ? (val < max ? val : max) : min; }

std::vector<std::string> loadLabels(const std::string &path)
{
    std::vector<std::string> labels;
    std::ifstream f(path);
    std::string line;
    while (std::getline(f, line))
    {
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n' || line.back() == ' '))
            line.pop_back();
        labels.push_back(line);
    }
    return labels;
}

static float CalculateOverlap(float xmin0, float ymin0, float xmax0, float ymax0, float xmin1, float ymin1, float xmax1,
                              float ymax1)
{
    float w = fmax(0.f, fmin(xmax0, xmax1) - fmax(xmin0, xmin1) + 1.0);
    float h = fmax(0.f, fmin(ymax0, ymax1) - fmax(ymin0, ymin1) + 1.0);
    float i = w * h;
    float u = (xmax0 - xmin0 + 1.0) * (ymax0 - ymin0 + 1.0) + (xmax1 - xmin1 + 1.0) * (ymax1 - ymin1 + 1.0) - i;
    return u <= 0.f ? 0.f : (i / u);
}

// order 已按置信度降序排列, 存放的是候选框的原始下标; 被抑制的置为 -1
static int nms(int validCount, const std::vector<float> &outputLocations, const std::vector<int> &classIds,
               std::vector<int> &order, int filterId, float threshold)
{
    for (int i = 0; i < validCount; ++i)
    {
        int n = order[i];
        if (n == -1 || classIds[n] != filterId)
            continue;
        for (int j = i + 1; j < validCount; ++j)
        {
            int m = order[j];
            if (m == -1 || classIds[m] != filterId)
                continue;
            float xmin0 = outputLocations[n * 4 + 0];
            float ymin0 = outputLocations[n * 4 + 1];
            float xmax0 = outputLocations[n * 4 + 0] + outputLocations[n * 4 + 2];
            float ymax0 = outputLocations[n * 4 + 1] + outputLocations[n * 4 + 3];

            float xmin1 = outputLocations[m * 4 + 0];
            float ymin1 = outputLocations[m * 4 + 1];
            float xmax1 = outputLocations[m * 4 + 0] + outputLocations[m * 4 + 2];
            float ymax1 = outputLocations[m * 4 + 1] + outputLocations[m * 4 + 3];

            float iou = CalculateOverlap(xmin0, ymin0, xmax0, ymax0, xmin1, ymin1, xmax1, ymax1);
            if (iou > threshold)
                order[j] = -1;
        }
    }
    return 0;
}

inline static int32_t __clip(float val, float min, float max)
{
    float f = val <= min ? min : (val >= max ? max : val);
    return f;
}

static int8_t qnt_f32_to_affine(float f32, int32_t zp, float scale)
{
    float dst_val = (f32 / scale) + zp;
    int8_t res = (int8_t)__clip(dst_val, -128, 127);
    return res;
}

static float deqnt_affine_to_f32(int8_t qnt, int32_t zp, float scale) { return ((float)qnt - (float)zp) * scale; }

static int process(int8_t *input, const int *anchor, int grid_h, int grid_w, int stride, int num_classes,
                   std::vector<float> &boxes, std::vector<float> &objProbs, std::vector<int> &classId, float threshold,
                   int32_t zp, float scale)
{
    int validCount = 0;
    int grid_len = grid_h * grid_w;
    int prop_box_size = 5 + num_classes;
    // 在量化域比较阈值, 省去大量反量化计算
    int8_t thres_i8 = qnt_f32_to_affine(threshold, zp, scale);
    for (int a = 0; a < 3; a++)
    {
        for (int i = 0; i < grid_h; i++)
        {
            for (int j = 0; j < grid_w; j++)
            {
                int8_t box_confidence = input[(prop_box_size * a + 4) * grid_len + i * grid_w + j];
                if (box_confidence < thres_i8)
                    continue;
                int offset = (prop_box_size * a) * grid_len + i * grid_w + j;
                int8_t *in_ptr = input + offset;

                int8_t maxClassProbs = in_ptr[5 * grid_len];
                int maxClassId = 0;
                for (int k = 1; k < num_classes; ++k)
                {
                    int8_t prob = in_ptr[(5 + k) * grid_len];
                    if (prob > maxClassProbs)
                    {
                        maxClassId = k;
                        maxClassProbs = prob;
                    }
                }
                if (maxClassProbs <= thres_i8)
                    continue;
                // 最终得分 = 目标置信度 * 类别置信度, 低于阈值的直接过滤(减少误检和 NMS 计算量)
                float score = deqnt_affine_to_f32(maxClassProbs, zp, scale) * deqnt_affine_to_f32(box_confidence, zp, scale);
                if (score < threshold)
                    continue;

                float box_x = (deqnt_affine_to_f32(*in_ptr, zp, scale)) * 2.0 - 0.5;
                float box_y = (deqnt_affine_to_f32(in_ptr[grid_len], zp, scale)) * 2.0 - 0.5;
                float box_w = (deqnt_affine_to_f32(in_ptr[2 * grid_len], zp, scale)) * 2.0;
                float box_h = (deqnt_affine_to_f32(in_ptr[3 * grid_len], zp, scale)) * 2.0;
                box_x = (box_x + j) * (float)stride;
                box_y = (box_y + i) * (float)stride;
                box_w = box_w * box_w * (float)anchor[a * 2];
                box_h = box_h * box_h * (float)anchor[a * 2 + 1];
                box_x -= (box_w / 2.0);
                box_y -= (box_h / 2.0);

                objProbs.push_back(score);
                classId.push_back(maxClassId);
                validCount++;
                boxes.push_back(box_x);
                boxes.push_back(box_y);
                boxes.push_back(box_w);
                boxes.push_back(box_h);
            }
        }
    }
    return validCount;
}

int post_process(int8_t *input0, int8_t *input1, int8_t *input2, int model_in_h, int model_in_w,
                 const int grid_hw[3][2], int num_classes, float conf_threshold, float nms_threshold,
                 const LetterBox &lb, const std::vector<int32_t> &qnt_zps, const std::vector<float> &qnt_scales,
                 const std::vector<std::string> &labels, detect_result_group_t *group)
{
    memset(group, 0, sizeof(detect_result_group_t));
    (void)model_in_w;

    std::vector<float> filterBoxes;
    std::vector<float> objProbs;
    std::vector<int> classId;

    int8_t *inputs[3] = {input0, input1, input2};
    int validCount = 0;
    for (int k = 0; k < 3; k++)
    {
        int grid_h = grid_hw[k][0];
        int grid_w = grid_hw[k][1];
        int stride = model_in_h / grid_h; // 8 / 16 / 32
        validCount += process(inputs[k], anchors[k], grid_h, grid_w, stride, num_classes, filterBoxes, objProbs,
                              classId, conf_threshold, qnt_zps[k], qnt_scales[k]);
    }
    if (validCount <= 0)
        return 0;

    // 先按置信度降序排序, 再按类别做 NMS: 高分框抑制与之重叠的低分同类框
    std::vector<int> indexArray(validCount);
    std::iota(indexArray.begin(), indexArray.end(), 0);
    std::stable_sort(indexArray.begin(), indexArray.end(), [&](int a, int b) { return objProbs[a] > objProbs[b]; });

    std::set<int> class_set(classId.begin(), classId.end());
    for (auto c : class_set)
        nms(validCount, filterBoxes, classId, indexArray, c, nms_threshold);

    int last_count = 0;
    for (int i = 0; i < validCount; ++i)
    {
        if (indexArray[i] == -1 || last_count >= OBJ_NUMB_MAX_SIZE)
            continue;
        int n = indexArray[i];

        // 模型坐标 -> 去掉 letterbox 填充 -> 除以缩放比例 -> 原图坐标
        float x1 = (filterBoxes[n * 4 + 0] - lb.pad_left) / lb.scale_x;
        float y1 = (filterBoxes[n * 4 + 1] - lb.pad_top) / lb.scale_y;
        float x2 = x1 + filterBoxes[n * 4 + 2] / lb.scale_x;
        float y2 = y1 + filterBoxes[n * 4 + 3] / lb.scale_y;
        int id = classId[n];

        detect_result_t &r = group->results[last_count];
        r.box.left = clamp(x1, 0, lb.src_w - 1);
        r.box.top = clamp(y1, 0, lb.src_h - 1);
        r.box.right = clamp(x2, 0, lb.src_w - 1);
        r.box.bottom = clamp(y2, 0, lb.src_h - 1);
        r.prop = objProbs[n];
        r.cls_id = id;
        if (id < (int)labels.size())
            snprintf(r.name, OBJ_NAME_MAX_SIZE, "%s", labels[id].c_str());
        else
            snprintf(r.name, OBJ_NAME_MAX_SIZE, "cls%d", id);
        last_count++;
    }
    group->count = last_count;
    return 0;
}
