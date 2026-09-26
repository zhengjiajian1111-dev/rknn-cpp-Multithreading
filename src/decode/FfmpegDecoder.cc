#include "decode/FfmpegDecoder.hpp"

#include <string.h>

extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
}

#include "common/Logger.hpp"

FfmpegDecoder::~FfmpegDecoder()
{
    if (sws_)
        sws_freeContext(sws_);
    av_packet_free(&pkt_);
    av_frame_free(&frame_);
    avcodec_free_context(&ctx_);
}

int FfmpegDecoder::init(const StreamParams &params)
{
    if (!params.par)
        return -1;
    const AVCodec *codec = avcodec_find_decoder(params.par->codec_id);
    if (!codec)
    {
        LOGE("ffmpeg: no decoder for %s", avcodec_get_name(params.par->codec_id));
        return -1;
    }
    ctx_ = avcodec_alloc_context3(codec);
    if (!ctx_ || avcodec_parameters_to_context(ctx_, params.par.get()) < 0)
        return -1;
    ctx_->thread_count = 2;
    if (avcodec_open2(ctx_, codec, nullptr) < 0)
    {
        LOGE("ffmpeg: open decoder %s failed", codec->name);
        return -1;
    }
    frame_ = av_frame_alloc();
    pkt_ = av_packet_alloc();
    LOGI("ffmpeg software decoder ready: %s", codec->name);
    return 0;
}

int FfmpegDecoder::receiveFrames()
{
    while (true)
    {
        int ret = avcodec_receive_frame(ctx_, frame_);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
            return 0;
        if (ret < 0)
            return ret;

        sws_ = sws_getCachedContext(sws_, frame_->width, frame_->height, (AVPixelFormat)frame_->format,
                                    frame_->width, frame_->height, AV_PIX_FMT_BGR24, SWS_BILINEAR, nullptr,
                                    nullptr, nullptr);
        if (!sws_)
        {
            av_frame_unref(frame_);
            return -1;
        }
        DecodedFrame f;
        f.format = DecodedFrame::BGR;
        f.width = frame_->width;
        f.height = frame_->height;
        f.bgr.create(frame_->height, frame_->width, CV_8UC3);
        uint8_t *dst[4] = {f.bgr.data, nullptr, nullptr, nullptr};
        int dst_stride[4] = {(int)f.bgr.step[0], 0, 0, 0};
        sws_scale(sws_, frame_->data, frame_->linesize, 0, frame_->height, dst, dst_stride);
        f.pts = frame_->pts;
        av_frame_unref(frame_);
        emit(f);
    }
}

int FfmpegDecoder::decode(const VideoPacket &pkt)
{
    if (!ctx_)
        return -1;
    int ret;
    if (pkt.eos || pkt.data.empty())
    {
        ret = avcodec_send_packet(ctx_, nullptr);
    }
    else
    {
        if (av_new_packet(pkt_, (int)pkt.data.size()) < 0)
            return -1;
        memcpy(pkt_->data, pkt.data.data(), pkt.data.size());
        pkt_->pts = pkt.pts;
        pkt_->dts = AV_NOPTS_VALUE;
        if (pkt.key)
            pkt_->flags |= AV_PKT_FLAG_KEY;
        ret = avcodec_send_packet(ctx_, pkt_);
        if (ret == AVERROR(EAGAIN))
        {
            // 输出缓冲满: 先取帧再重新送包
            if (receiveFrames() < 0)
            {
                av_packet_unref(pkt_);
                return -1;
            }
            ret = avcodec_send_packet(ctx_, pkt_);
        }
        av_packet_unref(pkt_);
    }
    if (ret < 0 && ret != AVERROR_EOF)
    {
        // 单个坏包不致命, 继续解码后续包
        LOGD("ffmpeg send packet failed: %d", ret);
    }
    return receiveFrames() < 0 ? -1 : 0;
}
