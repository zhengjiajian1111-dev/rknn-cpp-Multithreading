#ifndef _RKNN_YOLOV5_DEMO_POSTPROCESS_H_
#define _RKNN_YOLOV5_DEMO_POSTPROCESS_H_

#include <stdint.h>

#include <string>
#include <vector>

#define OBJ_NAME_MAX_SIZE 32
#define OBJ_NUMB_MAX_SIZE 64
#define NMS_THRESH 0.45
#define BOX_THRESH 0.25

typedef struct _BOX_RECT
{
    int left;
    int right;
    int top;
    int bottom;
} BOX_RECT;

typedef struct __detect_result_t
{
    char name[OBJ_NAME_MAX_SIZE];
    int cls_id;
    BOX_RECT box;
    float prop;
} detect_result_t;

typedef struct _detect_result_group_t
{
    int id;
    int count;
    detect_result_t results[OBJ_NUMB_MAX_SIZE];
} detect_result_group_t;

// 预处理的几何变换参数, 后处理据此把模型坐标映射回原图
struct LetterBox
{
    int src_w = 0;
    int src_h = 0;
    float scale_x = 1.f; // 原图 -> 模型输入 的缩放
    float scale_y = 1.f;
    int pad_left = 0;
    int pad_top = 0;
};

// 读取标签文件, 每行一个类别名
std::vector<std::string> loadLabels(const std::string &path);

// YOLOv5 三个输出头(int8 量化, NCHW)的解码 + 按类别 NMS.
// 线程安全: 不使用任何全局/静态可变状态, 可被多个模型实例并发调用.
int post_process(int8_t *input0, int8_t *input1, int8_t *input2, int model_in_h, int model_in_w,
                 const int grid_hw[3][2], int num_classes, float conf_threshold, float nms_threshold,
                 const LetterBox &lb, const std::vector<int32_t> &qnt_zps, const std::vector<float> &qnt_scales,
                 const std::vector<std::string> &labels, detect_result_group_t *group);

#endif //_RKNN_YOLOV5_DEMO_POSTPROCESS_H_
