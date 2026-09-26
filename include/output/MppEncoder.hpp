#ifndef OUTPUT_MPP_ENCODER_HPP
#define OUTPUT_MPP_ENCODER_HPP

#include "output/VideoEncoder.hpp"

#ifdef HAVE_MPP
#include <rockchip/rk_mpi.h>

// MPP 硬件 H.264 编码: BGR -(RGA)-> NV12(MPP DRM buffer) -> encode_put_frame / encode_get_packet
class MppEncoder : public VideoEncoder
{
public:
    explicit MppEncoder(bool use_rga) : use_rga_(use_rga) {}
    ~MppEncoder() override;

    bool init(int width, int height, int fps, int bitrate_kbps, int gop) override;
    bool encode(const cv::Mat &bgr, int64_t pts_ms, std::vector<EncodedPacket> &out) override;
    void requestKeyframe() override { force_idr_ = true; }
    const char *name() const override { return "mpp_h264"; }

private:
    void release();

    bool use_rga_;
    bool force_idr_ = false;
    int hor_stride_ = 0;
    int ver_stride_ = 0;
    size_t frame_size_ = 0;
    MppCtx ctx_ = nullptr;
    MppApi *mpi_ = nullptr;
    MppBufferGroup grp_ = nullptr;
    MppBuffer frm_buf_ = nullptr;
    MppBuffer hdr_buf_ = nullptr;
};
#endif

#endif
