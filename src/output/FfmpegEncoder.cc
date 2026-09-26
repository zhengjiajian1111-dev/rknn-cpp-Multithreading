#include "output/FfmpegEncoder.hpp"

#include <sstream>

extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>
}

#include "common/Logger.hpp"
#include "opencv2/imgproc/imgproc.hpp"

FfmpegEncoder::~FfmpegEncoder() { release(); }

void FfmpegEncoder::release()
{
    if (sws_)
    {
        sws_freeContext(sws_);
        sws_ = nullptr;
    }
    av_packet_free(&pkt_);
    av_frame_free(&frame_);
    avcodec_free_context(&ctx_);
}

static AVPixelFormat pickPixFmt(const AVCodec *codec)
{
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#endif
    const AVPixelFormat *fmts = codec->pix_fmts;
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif
    if (!fmts)
        return AV_PIX_FMT_YUV420P;
    for (const AVPixelFormat *p = fmts; *p != AV_PIX_FMT_NONE; p++)
    {
        if (*p == AV_PIX_FMT_NV12)
            return AV_PIX_FMT_NV12;
    }
    for (const AVPixelFormat *p = fmts; *p != AV_PIX_FMT_NONE; p++)
    {
        if (*p == AV_PIX_FMT_YUV420P)
            return AV_PIX_FMT_YUV420P;
    }
    return fmts[0];
}

bool FfmpegEncoder::tryOpen(const std::string &name, int fps, int bitrate_kbps, int gop)
{
    const AVCodec *codec = avcodec_find_encoder_by_name(name.c_str());
    if (!codec)
        return false;
    ctx_ = avcodec_alloc_context3(codec);
    ctx_->width = width_;
    ctx_->height = height_;
    ctx_->pix_fmt = pickPixFmt(codec);
    ctx_->time_base = AVRational{1, fps};
    ctx_->framerate = AVRational{fps, 1};
    ctx_->gop_size = gop > 0 ? gop : fps * 2;
    ctx_->max_b_frames = 0; // 实时推流不用 B 帧, 降低延迟且 dts == pts
    ctx_->bit_rate = (int64_t)bitrate_kbps * 1000;
    if (name == "libx264")
    {
        av_opt_set(ctx_->priv_data, "preset", "ultrafast", 0);
        av_opt_set(ctx_->priv_data, "tune", "zerolatency", 0);
        av_opt_set(ctx_->priv_data, "forced-idr", "1", 0);
    }
    if (avcodec_open2(ctx_, codec, nullptr) < 0)
    {
        avcodec_free_context(&ctx_);
        return false;
    }
    codec_name_ = name;
    return true;
}

bool FfmpegEncoder::init(int width, int height, int fps, int bitrate_kbps, int gop)
{
    width_ = width & ~1;
    height_ = height & ~1;
    if (fps <= 0)
        fps = 25;
    std::stringstream ss(names_);
    std::string name;
    while (std::getline(ss, name, ','))
    {
        if (!name.empty() && tryOpen(name, fps, bitrate_kbps, gop))
            break;
    }
    if (!ctx_)
        return false;

    frame_ = av_frame_alloc();
    frame_->format = ctx_->pix_fmt;
    frame_->width = width_;
    frame_->height = height_;
    if (av_frame_get_buffer(frame_, 0) < 0)
    {
        release();
        return false;
    }
    pkt_ = av_packet_alloc();
    sws_ = sws_getContext(width_, height_, AV_PIX_FMT_BGR24, width_, height_, ctx_->pix_fmt, SWS_BILINEAR, nullptr,
                          nullptr, nullptr);
    if (!sws_)
    {
        release();
        return false;
    }
    LOGI("ffmpeg encoder '%s' ready: %dx%d %dfps %dkbps", codec_name_.c_str(), width_, height_, fps, bitrate_kbps);
    return true;
}

bool FfmpegEncoder::encode(const cv::Mat &bgr_in, int64_t pts_ms, std::vector<EncodedPacket> &out)
{
    if (!ctx_)
        return false;
    cv::Mat bgr = bgr_in;
    if (bgr.cols != width_ || bgr.rows != height_)
        cv::resize(bgr_in, bgr, cv::Size(width_, height_));

    if (av_frame_make_writable(frame_) < 0)
        return false;
    const uint8_t *src[4] = {bgr.data, nullptr, nullptr, nullptr};
    int src_stride[4] = {(int)bgr.step[0], 0, 0, 0};
    sws_scale(sws_, src, src_stride, 0, height_, frame_->data, frame_->linesize);

    frame_->pts = frame_index_;
    pts_map_.emplace_back(frame_index_, pts_ms);
    if (pts_map_.size() > 64)
        pts_map_.pop_front();
    frame_index_++;
    frame_->pict_type = force_key_ ? AV_PICTURE_TYPE_I : AV_PICTURE_TYPE_NONE;
    force_key_ = false;

    if (avcodec_send_frame(ctx_, frame_) < 0)
        return false;
    while (avcodec_receive_packet(ctx_, pkt_) == 0)
    {
        EncodedPacket pkt;
        pkt.data.assign(pkt_->data, pkt_->data + pkt_->size);
        pkt.pts = pts_ms;
        for (auto &kv : pts_map_)
        {
            if (kv.first == pkt_->pts)
            {
                pkt.pts = kv.second;
                break;
            }
        }
        av_packet_unref(pkt_);
        finalizePacket(pkt);
        out.push_back(std::move(pkt));
    }
    return true;
}
