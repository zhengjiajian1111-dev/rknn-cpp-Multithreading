#ifndef DECODE_MPP_DECODER_HPP
#define DECODE_MPP_DECODER_HPP

#include "decode/VideoDecoder.hpp"

#ifdef HAVE_MPP
#include <rockchip/rk_mpi.h>

// s2. MppDecoder: Rockchip MPP 硬件解码 H.264/H.265 -> NV12
//   mpp_create/mpp_init -> decode_put_packet/decode_get_frame -> mpp_frame_get_xxx -> 回调
class MppDecoder : public VideoDecoder
{
public:
    explicit MppDecoder(int split_parse = 1) : split_parse_(split_parse) {}
    ~MppDecoder() override;

    int init(const StreamParams &params) override;
    int decode(const VideoPacket &pkt) override;
    const char *name() const override { return "mpp"; }

private:
    // 取出所有已解码帧, 返回取到的帧数; 出错返回 <0
    int drainFrames();
    void release();

    int split_parse_;
    MppCtx ctx_ = nullptr;
    MppApi *mpi_ = nullptr;
    MppBufferGroup frm_grp_ = nullptr;
    uint64_t frame_count_ = 0;
    int error_count_ = 0;
};
#endif

#endif
