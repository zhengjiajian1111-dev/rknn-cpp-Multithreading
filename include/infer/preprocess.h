#ifndef _RKNN_YOLOV5_DEMO_PREPROCESS_H_
#define _RKNN_YOLOV5_DEMO_PREPROCESS_H_

#include "infer/postprocess.h"
#include "opencv2/core/core.hpp"

// 预处理: BGR 原图 -> 模型输入(RGB, model_w x model_h, NHWC uint8)
//  * letterbox=true 时等比缩放并用 114 灰边填充(与 YOLOv5 训练时一致, 精度更好);
//  * use_rga=true 时由 RGA 一次完成 缩放 + BGR->RGB, 失败自动回退 OpenCV 并把 use_rga 置 false;
//  * input 需预先分配为 model_h x model_w 的 CV_8UC3, 预处理不再产生额外内存分配.
bool preprocess(const cv::Mat &bgr, cv::Mat &input, bool letterbox, bool &use_rga, LetterBox &lb);

#endif //_RKNN_YOLOV5_DEMO_PREPROCESS_H_
