#include "output/MppEncoder.hpp"

#ifdef HAVE_MPP
#include <string.h>

#include "common/Logger.hpp"
#include "common/RgaUtils.hpp"
#include "opencv2/imgproc/imgproc.hpp"

#ifndef MODULE_TAG
#define MODULE_TAG "rknn_ms"
#endif

#define ALIGN_UP(x, a) (((x) + (a)-1) & ~((a)-1))

MppEncoder::~MppEncoder() { release(); }

void MppEncoder::release()
{
    if (ctx_)
    {
        mpi_->reset(ctx_);
        mpp_destroy(ctx_);
        ctx_ = nullptr;
    }
    if (frm_buf_)
    {
        mpp_buffer_put(frm_buf_);
        frm_buf_ = nullptr;
    }
    if (hdr_buf_)
    {
        mpp_buffer_put(hdr_buf_);
        hdr_buf_ = nullptr;
    }
    if (grp_)
    {
        mpp_buffer_group_put(grp_);
        grp_ = nullptr;
    }
}

bool MppEncoder::init(int width, int height, int fps, int bitrate_kbps, int gop)
{
    width_ = width & ~1;
    height_ = height & ~1;
    hor_stride_ = ALIGN_UP(width_, 16);
    ver_stride_ = ALIGN_UP(height_, 16);
    frame_size_ = (size_t)hor_stride_ * ver_stride_ * 3 / 2;
    if (fps <= 0)
        fps = 25;

    if (mpp_create(&ctx_, &mpi_) != MPP_OK)
    {
        ctx_ = nullptr;
        return false;
    }
    // 编码为异步硬件流程: encode_get_packet 阻塞直到本帧编码完成
    RK_S64 timeout = MPP_POLL_BLOCK;
    mpi_->control(ctx_, MPP_SET_OUTPUT_TIMEOUT, &timeout);
    if (mpp_init(ctx_, MPP_CTX_ENC, MPP_VIDEO_CodingAVC) != MPP_OK)
    {
        LOGE("mpp enc init failed");
        release();
        return false;
    }

    MppEncCfg cfg = nullptr;
    mpp_enc_cfg_init(&cfg);
    mpi_->control(ctx_, MPP_ENC_GET_CFG, cfg);
    mpp_enc_cfg_set_s32(cfg, "prep:width", width_);
    mpp_enc_cfg_set_s32(cfg, "prep:height", height_);
    mpp_enc_cfg_set_s32(cfg, "prep:hor_stride", hor_stride_);
    mpp_enc_cfg_set_s32(cfg, "prep:ver_stride", ver_stride_);
    mpp_enc_cfg_set_s32(cfg, "prep:format", MPP_FMT_YUV420SP);

    int bps = bitrate_kbps * 1000;
    mpp_enc_cfg_set_s32(cfg, "rc:mode", MPP_ENC_RC_MODE_CBR);
    mpp_enc_cfg_set_s32(cfg, "rc:bps_target", bps);
    mpp_enc_cfg_set_s32(cfg, "rc:bps_max", bps * 17 / 16);
    mpp_enc_cfg_set_s32(cfg, "rc:bps_min", bps * 15 / 16);
    mpp_enc_cfg_set_s32(cfg, "rc:fps_in_flex", 0);
    mpp_enc_cfg_set_s32(cfg, "rc:fps_in_num", fps);
    mpp_enc_cfg_set_s32(cfg, "rc:fps_in_denorm", 1);
    mpp_enc_cfg_set_s32(cfg, "rc:fps_out_flex", 0);
    mpp_enc_cfg_set_s32(cfg, "rc:fps_out_num", fps);
    mpp_enc_cfg_set_s32(cfg, "rc:fps_out_denorm", 1);
    mpp_enc_cfg_set_s32(cfg, "rc:gop", gop > 0 ? gop : fps * 2);

    mpp_enc_cfg_set_s32(cfg, "codec:type", MPP_VIDEO_CodingAVC);
    mpp_enc_cfg_set_s32(cfg, "h264:profile", 100);
    mpp_enc_cfg_set_s32(cfg, "h264:level", 41);
    mpp_enc_cfg_set_s32(cfg, "h264:cabac_en", 1);
    mpp_enc_cfg_set_s32(cfg, "h264:cabac_idc", 0);
    mpp_enc_cfg_set_s32(cfg, "h264:trans8x8", 1);
    MPP_RET ret = mpi_->control(ctx_, MPP_ENC_SET_CFG, cfg);
    mpp_enc_cfg_deinit(cfg);
    if (ret != MPP_OK)
    {
        LOGE("mpp enc set cfg failed: %d", ret);
        release();
        return false;
    }

    // 每个 IDR 前都输出 SPS/PPS
    MppEncHeaderMode header_mode = MPP_ENC_HEADER_MODE_EACH_IDR;
    mpi_->control(ctx_, MPP_ENC_SET_HEADER_MODE, &header_mode);

    if (mpp_buffer_group_get_internal(&grp_, MPP_BUFFER_TYPE_DRM) != MPP_OK ||
        mpp_buffer_get(grp_, &frm_buf_, frame_size_) != MPP_OK || mpp_buffer_get(grp_, &hdr_buf_, 4096) != MPP_OK)
    {
        LOGE("mpp enc alloc buffer failed");
        release();
        return false;
    }

    // 取 SPS/PPS 作为 extradata
    MppPacket hdr = nullptr;
    mpp_packet_init_with_buffer(&hdr, hdr_buf_);
    mpp_packet_set_length(hdr, 0);
    if (mpi_->control(ctx_, MPP_ENC_GET_HDR_SYNC, hdr) == MPP_OK)
    {
        const uint8_t *p = (const uint8_t *)mpp_packet_get_pos(hdr);
        size_t len = mpp_packet_get_length(hdr);
        header_.assign(p, p + len);
    }
    mpp_packet_deinit(&hdr);

    LOGI("mpp h264 encoder ready: %dx%d stride %dx%d %dfps %dkbps gop %d", width_, height_, hor_stride_, ver_stride_,
         fps, bitrate_kbps, gop);
    return true;
}

static void bgrToNv12Cpu(const cv::Mat &bgr, uint8_t *dst, int hor_stride, int ver_stride)
{
    int w = bgr.cols, h = bgr.rows;
    cv::Mat i420;
    cv::cvtColor(bgr, i420, cv::COLOR_BGR2YUV_I420);
    const uint8_t *y = i420.data;
    const uint8_t *u = y + w * h;
    const uint8_t *v = u + (w / 2) * (h / 2);
    for (int r = 0; r < h; r++)
        memcpy(dst + (size_t)r * hor_stride, y + (size_t)r * w, w);
    uint8_t *uv = dst + (size_t)hor_stride * ver_stride;
    for (int r = 0; r < h / 2; r++)
    {
        uint8_t *row = uv + (size_t)r * hor_stride;
        const uint8_t *ur = u + (size_t)r * (w / 2);
        const uint8_t *vr = v + (size_t)r * (w / 2);
        for (int c = 0; c < w / 2; c++)
        {
            row[2 * c] = ur[c];
            row[2 * c + 1] = vr[c];
        }
    }
}

bool MppEncoder::encode(const cv::Mat &bgr_in, int64_t pts_ms, std::vector<EncodedPacket> &out)
{
    if (!ctx_)
        return false;
    cv::Mat bgr = bgr_in;
    if (bgr.cols != width_ || bgr.rows != height_ || !bgr.isContinuous())
    {
        cv::Mat tmp;
        cv::resize(bgr_in, tmp, cv::Size(width_, height_));
        bgr = tmp;
    }

    // BGR -> NV12 写入 MPP 输入 buffer(RGA 直接写 DMA-BUF)
    void *ptr = mpp_buffer_get_ptr(frm_buf_);
    int fd = mpp_buffer_get_fd(frm_buf_);
    bool done = false;
    if (use_rga_)
    {
        done = rga::bgrToNv12(bgr, fd, ptr, hor_stride_, ver_stride_);
        if (!done)
        {
            LOGW("rga bgr->nv12 failed, encoder falls back to cpu conversion");
            use_rga_ = false;
        }
    }
    if (!done)
        bgrToNv12Cpu(bgr, (uint8_t *)ptr, hor_stride_, ver_stride_);

    if (force_idr_)
    {
        mpi_->control(ctx_, MPP_ENC_SET_IDR_FRAME, nullptr);
        force_idr_ = false;
    }

    MppFrame frame = nullptr;
    mpp_frame_init(&frame);
    mpp_frame_set_width(frame, width_);
    mpp_frame_set_height(frame, height_);
    mpp_frame_set_hor_stride(frame, hor_stride_);
    mpp_frame_set_ver_stride(frame, ver_stride_);
    mpp_frame_set_fmt(frame, MPP_FMT_YUV420SP);
    mpp_frame_set_eos(frame, 0);
    mpp_frame_set_pts(frame, pts_ms);
    mpp_frame_set_buffer(frame, frm_buf_);
    MPP_RET ret = mpi_->encode_put_frame(ctx_, frame);
    mpp_frame_deinit(&frame);
    if (ret != MPP_OK)
    {
        LOGW("mpp encode_put_frame failed: %d", ret);
        return false;
    }

    MppPacket packet = nullptr;
    ret = mpi_->encode_get_packet(ctx_, &packet);
    if (ret != MPP_OK || !packet)
        return ret == MPP_OK;

    EncodedPacket pkt;
    const uint8_t *p = (const uint8_t *)mpp_packet_get_pos(packet);
    size_t len = mpp_packet_get_length(packet);
    pkt.data.assign(p, p + len);
    pkt.pts = pts_ms;
    mpp_packet_deinit(&packet);
    finalizePacket(pkt);
    out.push_back(std::move(pkt));
    return true;
}

#endif
