#ifndef OUTPUT_FFMPEG_ENCODER_HPP
#define OUTPUT_FFMPEG_ENCODER_HPP

#include <deque>
#include <string>

#include "output/VideoEncoder.hpp"

struct AVCodecContext;
struct AVFrame;
struct AVPacket;
struct SwsContext;

// FFmpeg 编码(h264_rkmpp / libx264 / ...), 用于没有 MPP 开发包或 PC 调试的场景
class FfmpegEncoder : public VideoEncoder
{
public:
    explicit FfmpegEncoder(const std::string &names) : names_(names) {}
    ~FfmpegEncoder() override;

    bool init(int width, int height, int fps, int bitrate_kbps, int gop) override;
    bool encode(const cv::Mat &bgr, int64_t pts_ms, std::vector<EncodedPacket> &out) override;
    void requestKeyframe() override { force_key_ = true; }
    const char *name() const override { return codec_name_.c_str(); }

private:
    bool tryOpen(const std::string &name, int fps, int bitrate_kbps, int gop);
    void release();

    std::string names_;
    std::string codec_name_;
    AVCodecContext *ctx_ = nullptr;
    AVFrame *frame_ = nullptr;
    AVPacket *pkt_ = nullptr;
    SwsContext *sws_ = nullptr;
    int64_t frame_index_ = 0;
    std::deque<std::pair<int64_t, int64_t>> pts_map_; // 帧序号 -> 毫秒时间戳
    bool force_key_ = false;
};

#endif
