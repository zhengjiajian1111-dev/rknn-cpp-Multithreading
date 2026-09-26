#ifndef COMMON_RGA_UTILS_HPP
#define COMMON_RGA_UTILS_HPP

#include "opencv2/core/core.hpp"

// RGA 2D 加速封装. 所有函数失败时返回 false, 调用方负责回退到 OpenCV(CPU) 实现.
// 编译时未启用 RGA(-DENABLE_RGA=OFF) 时全部返回 false.
namespace rga
{
    bool available();

    // 把 src 缩放(并做 BGR<->RGB 转换)写入 dst 的 dst_rect 区域, src/dst 均为 CV_8UC3
    bool resizeInto(const cv::Mat &src, bool src_bgr, cv::Mat &dst, bool dst_bgr, const cv::Rect &dst_rect);

    // MPP 解码输出(NV12, 带 stride) -> BGR. fd >= 0 时走 DMA-BUF 零拷贝, 否则使用虚拟地址
    bool nv12ToBgr(int fd, const void *vaddr, int width, int height, int hor_stride, int ver_stride, cv::Mat &bgr);

    // BGR -> NV12(带 stride), 写入编码器输入 buffer
    bool bgrToNv12(const cv::Mat &bgr, int dst_fd, void *dst_vaddr, int hor_stride, int ver_stride);
}

#endif
