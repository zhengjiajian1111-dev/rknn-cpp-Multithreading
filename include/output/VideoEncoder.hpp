#ifndef OUTPUT_VIDEO_ENCODER_HPP
#define OUTPUT_VIDEO_ENCODER_HPP

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "opencv2/core/core.hpp"

// 编码输出: Annex-B H.264 一帧
struct EncodedPacket
{
    std::vector<uint8_t> data;
    int64_t pts = 0; // 毫秒
    bool key = false;
};

class VideoEncoder
{
public:
    virtual ~VideoEncoder() = default;
    virtual bool init(int width, int height, int fps, int bitrate_kbps, int gop) = 0;
    virtual bool encode(const cv::Mat &bgr, int64_t pts_ms, std::vector<EncodedPacket> &out) = 0;
    virtual void requestKeyframe() = 0;
    virtual const char *name() const = 0;

    // SPS/PPS(Annex-B), 用于 FLV/RTSP 的 extradata
    const std::vector<uint8_t> &header() const { return header_; }
    int width() const { return width_; }
    int height() const { return height_; }

protected:
    // 统一关键帧判定, 并保证每个关键帧都带内携带 SPS/PPS:
    // GB28181 的 PS 流、RTSP 客户端中途接入都依赖带内参数集才能解码
    void finalizePacket(EncodedPacket &pkt);

    std::vector<uint8_t> header_;
    int width_ = 0;
    int height_ = 0;
};

namespace h264
{
    bool hasNalType(const uint8_t *data, size_t size, int type);
    std::vector<uint8_t> extractParamSets(const uint8_t *data, size_t size);
}

// pref: auto(优先 MPP 硬编, 失败回退 FFmpeg) | mpp | ffmpeg | ffmpeg:<encoder name>
std::unique_ptr<VideoEncoder> createEncoder(const std::string &pref, bool use_rga, int width, int height, int fps,
                                            int bitrate_kbps, int gop);

#endif
