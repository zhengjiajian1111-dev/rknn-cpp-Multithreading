#ifndef DECODE_FFMPEG_DECODER_HPP
#define DECODE_FFMPEG_DECODER_HPP

#include "decode/VideoDecoder.hpp"

struct AVCodecContext;
struct AVFrame;
struct AVPacket;
struct SwsContext;

// FFmpeg 软解码: 用于 PC 调试, 或 MPP 不支持的编码格式(MJPEG/rawvideo 等)的兜底
class FfmpegDecoder : public VideoDecoder
{
public:
    ~FfmpegDecoder() override;
    int init(const StreamParams &params) override;
    int decode(const VideoPacket &pkt) override;
    const char *name() const override { return "ffmpeg"; }

private:
    int receiveFrames();

    AVCodecContext *ctx_ = nullptr;
    AVFrame *frame_ = nullptr;
    AVPacket *pkt_ = nullptr;
    SwsContext *sws_ = nullptr;
};

#endif
