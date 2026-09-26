#include "common/RgaUtils.hpp"

#include "common/Logger.hpp"

#ifdef HAVE_RGA
#include <string.h>

#include "im2d.hpp"
#include "rga.h"

namespace rga
{
    bool available() { return true; }

    static bool ok(IM_STATUS s) { return s == IM_STATUS_SUCCESS || s == IM_STATUS_NOERROR; }

    bool resizeInto(const cv::Mat &src, bool src_bgr, cv::Mat &dst, bool dst_bgr, const cv::Rect &dst_rect)
    {
        if (src.type() != CV_8UC3 || dst.type() != CV_8UC3 || !src.isContinuous() || !dst.isContinuous())
            return false;
        rga_buffer_t s = wrapbuffer_virtualaddr((void *)src.data, src.cols, src.rows,
                                                src_bgr ? RK_FORMAT_BGR_888 : RK_FORMAT_RGB_888);
        rga_buffer_t d = wrapbuffer_virtualaddr((void *)dst.data, dst.cols, dst.rows,
                                                dst_bgr ? RK_FORMAT_BGR_888 : RK_FORMAT_RGB_888);
        rga_buffer_t pat;
        memset(&pat, 0, sizeof(pat));
        im_rect srect = {0, 0, src.cols, src.rows};
        im_rect drect = {dst_rect.x, dst_rect.y, dst_rect.width, dst_rect.height};
        im_rect prect;
        memset(&prect, 0, sizeof(prect));
        IM_STATUS st = improcess(s, d, pat, srect, drect, prect, -1, nullptr, nullptr, IM_SYNC);
        if (!ok(st))
        {
            LOGD("rga resize failed: %s", imStrError(st));
            return false;
        }
        return true;
    }

    bool nv12ToBgr(int fd, const void *vaddr, int width, int height, int hor_stride, int ver_stride, cv::Mat &bgr)
    {
        bgr.create(height, width, CV_8UC3);
        rga_buffer_t s = fd >= 0 ? wrapbuffer_fd(fd, width, height, RK_FORMAT_YCbCr_420_SP, hor_stride, ver_stride)
                                 : wrapbuffer_virtualaddr((void *)vaddr, width, height, RK_FORMAT_YCbCr_420_SP,
                                                          hor_stride, ver_stride);
        rga_buffer_t d = wrapbuffer_virtualaddr((void *)bgr.data, width, height, RK_FORMAT_BGR_888);
        IM_STATUS st = imcvtcolor(s, d, RK_FORMAT_YCbCr_420_SP, RK_FORMAT_BGR_888);
        if (!ok(st))
        {
            LOGD("rga nv12->bgr failed: %s", imStrError(st));
            return false;
        }
        return true;
    }

    bool bgrToNv12(const cv::Mat &bgr, int dst_fd, void *dst_vaddr, int hor_stride, int ver_stride)
    {
        if (bgr.type() != CV_8UC3 || !bgr.isContinuous())
            return false;
        rga_buffer_t s = wrapbuffer_virtualaddr((void *)bgr.data, bgr.cols, bgr.rows, RK_FORMAT_BGR_888);
        rga_buffer_t d = dst_fd >= 0 ? wrapbuffer_fd(dst_fd, bgr.cols, bgr.rows, RK_FORMAT_YCbCr_420_SP, hor_stride,
                                                     ver_stride)
                                     : wrapbuffer_virtualaddr(dst_vaddr, bgr.cols, bgr.rows, RK_FORMAT_YCbCr_420_SP,
                                                              hor_stride, ver_stride);
        IM_STATUS st = imcvtcolor(s, d, RK_FORMAT_BGR_888, RK_FORMAT_YCbCr_420_SP);
        if (!ok(st))
        {
            LOGD("rga bgr->nv12 failed: %s", imStrError(st));
            return false;
        }
        return true;
    }
}

#else

namespace rga
{
    bool available() { return false; }
    bool resizeInto(const cv::Mat &, bool, cv::Mat &, bool, const cv::Rect &) { return false; }
    bool nv12ToBgr(int, const void *, int, int, int, int, cv::Mat &) { return false; }
    bool bgrToNv12(const cv::Mat &, int, void *, int, int) { return false; }
}

#endif
