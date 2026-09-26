// Copyright (c) 2023 by Rockchip Electronics Co., Ltd. All Rights Reserved.
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

#include "infer/preprocess.h"

#include <algorithm>
#include <cmath>

#include "common/Logger.hpp"
#include "common/RgaUtils.hpp"
#include "opencv2/imgproc/imgproc.hpp"

bool preprocess(const cv::Mat &bgr, cv::Mat &input, bool letterbox, bool &use_rga, LetterBox &lb)
{
    if (bgr.empty() || input.empty())
        return false;
    const int model_w = input.cols;
    const int model_h = input.rows;
    lb.src_w = bgr.cols;
    lb.src_h = bgr.rows;

    cv::Rect roi(0, 0, model_w, model_h);
    if (letterbox)
    {
        float s = std::min(model_w / (float)bgr.cols, model_h / (float)bgr.rows);
        // 宽高/偏移取偶数, 满足 RGA 的对齐要求
        int nw = std::min(model_w, ((int)std::lround(bgr.cols * s)) & ~1);
        int nh = std::min(model_h, ((int)std::lround(bgr.rows * s)) & ~1);
        int px = ((model_w - nw) / 2) & ~1;
        int py = ((model_h - nh) / 2) & ~1;
        roi = cv::Rect(px, py, nw, nh);
        if (nw != model_w || nh != model_h)
            input.setTo(cv::Scalar(114, 114, 114));
    }
    lb.scale_x = roi.width / (float)bgr.cols;
    lb.scale_y = roi.height / (float)bgr.rows;
    lb.pad_left = roi.x;
    lb.pad_top = roi.y;

    if (use_rga)
    {
        if (rga::resizeInto(bgr, true, input, false, roi))
            return true;
        LOGW("rga preprocess failed (src %dx%d), fallback to opencv", bgr.cols, bgr.rows);
        use_rga = false;
    }

    // CPU: 先缩放再转颜色(处理的像素更少); 直接写入输入缓冲的 ROI 并原地转换, 不产生临时图像
    cv::Mat dst_roi = input(roi);
    if (bgr.cols == roi.width && bgr.rows == roi.height)
    {
        cv::cvtColor(bgr, dst_roi, cv::COLOR_BGR2RGB);
    }
    else
    {
        cv::resize(bgr, dst_roi, roi.size(), 0, 0, cv::INTER_LINEAR);
        cv::cvtColor(dst_roi, dst_roi, cv::COLOR_BGR2RGB);
    }
    return true;
}
