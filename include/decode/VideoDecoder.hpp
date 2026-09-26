#ifndef DECODE_VIDEO_DECODER_HPP
#define DECODE_VIDEO_DECODER_HPP

#include <memory>
#include <string>

#include "opencv2/core/core.hpp"
#include "stream/StreamTypes.hpp"

// 解码输出的一帧. MPP 输出 NV12(带 stride, 可能有 DMA-BUF fd), FFmpeg 软解直接输出 BGR
struct DecodedFrame
{
    enum Format
    {
        NV12,
        BGR,
    };
    Format format = NV12;
    int width = 0;
    int height = 0;
    int hor_stride = 0;
    int ver_stride = 0;
    int fd = -1;                   // DMA-BUF fd, RGA 可直接使用
    const uint8_t *data = nullptr; // NV12 虚拟地址
    cv::Mat bgr;                   // format == BGR 时有效
    int64_t pts = 0;
};

// C 风格回调(与 MPP 示例保持一致): userdata 为注册者自身指针
typedef void (*DecoderFrameCallback)(void *userdata, const DecodedFrame &frame);

class VideoDecoder
{
public:
    virtual ~VideoDecoder() = default;
    virtual int init(const StreamParams &params) = 0;
    // 送入一个 Annex-B 包; 解码出的帧通过回调同步输出. 返回 <0 表示解码器异常(需要重建)
    virtual int decode(const VideoPacket &pkt) = 0;
    virtual const char *name() const = 0;

    void setCallback(DecoderFrameCallback cb, void *userdata)
    {
        callback_ = cb;
        userdata_ = userdata;
    }

protected:
    void emit(const DecodedFrame &f)
    {
        if (callback_)
            callback_(userdata_, f);
    }

    DecoderFrameCallback callback_ = nullptr;
    void *userdata_ = nullptr;
};

// backend: auto(优先 MPP, 失败回退 FFmpeg) | mpp | ffmpeg
std::unique_ptr<VideoDecoder> createDecoder(const std::string &backend, const StreamParams &params, int mpp_split_parse);

#endif
