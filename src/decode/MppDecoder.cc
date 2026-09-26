#include "decode/MppDecoder.hpp"

#ifdef HAVE_MPP
#include <unistd.h>

#include "common/Logger.hpp"

// mpp_buffer_group_get_internal 等宏需要 MODULE_TAG
#ifndef MODULE_TAG
#define MODULE_TAG "rknn_ms"
#endif

MppDecoder::~MppDecoder() { release(); }

void MppDecoder::release()
{
    // 先销毁解码器, 再释放它使用的外部 buffer group
    if (ctx_)
    {
        mpi_->reset(ctx_);
        mpp_destroy(ctx_);
        ctx_ = nullptr;
        mpi_ = nullptr;
    }
    if (frm_grp_)
    {
        mpp_buffer_group_put(frm_grp_);
        frm_grp_ = nullptr;
    }
}

int MppDecoder::init(const StreamParams &params)
{
    MppCodingType type;
    if (params.codec == CodecType::H264)
        type = MPP_VIDEO_CodingAVC;
    else if (params.codec == CodecType::H265)
        type = MPP_VIDEO_CodingHEVC;
    else
        return -1;

    MPP_RET ret = mpp_create(&ctx_, &mpi_);
    if (ret != MPP_OK)
    {
        LOGE("mpp_create failed: %d", ret);
        ctx_ = nullptr;
        return -1;
    }
    ret = mpp_init(ctx_, MPP_CTX_DEC, type);
    if (ret != MPP_OK)
    {
        LOGE("mpp_init failed: %d", ret);
        release();
        return -1;
    }

    // split_parse: 由 MPP 内部按帧切分码流. 输入不是整帧时必须打开;
    // FFmpeg 读出的是整帧, 关闭可以减少一帧延迟(mpp_split_parse=0)
    MppDecCfg cfg = nullptr;
    mpp_dec_cfg_init(&cfg);
    ret = mpi_->control(ctx_, MPP_DEC_GET_CFG, cfg);
    if (ret == MPP_OK)
    {
        mpp_dec_cfg_set_u32(cfg, "base:split_parse", split_parse_ ? 1 : 0);
        ret = mpi_->control(ctx_, MPP_DEC_SET_CFG, cfg);
    }
    mpp_dec_cfg_deinit(cfg);
    if (ret != MPP_OK)
    {
        LOGE("mpp set decoder cfg failed: %d", ret);
        release();
        return -1;
    }
    LOGI("mpp decoder ready: %s %dx%d split_parse=%d", type == MPP_VIDEO_CodingAVC ? "H.264" : "H.265",
         params.width, params.height, split_parse_);
    return 0;
}

int MppDecoder::drainFrames()
{
    int got = 0;
    while (true)
    {
        MppFrame frame = nullptr;
        MPP_RET ret = mpi_->decode_get_frame(ctx_, &frame);
        if (ret == MPP_ERR_TIMEOUT || (ret == MPP_OK && !frame))
            break;
        if (ret != MPP_OK)
        {
            LOGW("decode_get_frame failed: %d", ret);
            return -1;
        }

        if (mpp_frame_get_info_change(frame))
        {
            // 首帧/分辨率变化: 按新的 buffer 大小重新配置外部 buffer group
            RK_U32 w = mpp_frame_get_width(frame);
            RK_U32 h = mpp_frame_get_height(frame);
            RK_U32 hs = mpp_frame_get_hor_stride(frame);
            RK_U32 vs = mpp_frame_get_ver_stride(frame);
            size_t buf_size = mpp_frame_get_buf_size(frame);
            LOGI("mpp info change: %ux%u stride %ux%u buf_size %zu", w, h, hs, vs, buf_size);
            if (!frm_grp_)
            {
                ret = mpp_buffer_group_get_internal(&frm_grp_, MPP_BUFFER_TYPE_DRM);
                if (ret != MPP_OK)
                {
                    LOGE("mpp get buffer group failed: %d", ret);
                    mpp_frame_deinit(&frame);
                    return -1;
                }
                mpi_->control(ctx_, MPP_DEC_SET_EXT_BUF_GROUP, frm_grp_);
            }
            else
            {
                mpp_buffer_group_clear(frm_grp_);
            }
            // 限制 buffer 数量, 防止下游阻塞时解码器无限占用内存
            mpp_buffer_group_limit_config(frm_grp_, buf_size, 24);
            mpi_->control(ctx_, MPP_DEC_SET_INFO_CHANGE_READY, nullptr);
        }
        else
        {
            RK_U32 err = mpp_frame_get_errinfo(frame) | mpp_frame_get_discard(frame);
            MppBuffer buf = mpp_frame_get_buffer(frame);
            MppFrameFormat fmt = mpp_frame_get_fmt(frame);
            if (err)
            {
                error_count_++;
                LOGD("mpp frame err info: %u", err);
            }
            else if (buf && (fmt & MPP_FRAME_FMT_MASK) == MPP_FMT_YUV420SP)
            {
                DecodedFrame f;
                f.format = DecodedFrame::NV12;
                f.width = mpp_frame_get_width(frame);
                f.height = mpp_frame_get_height(frame);
                f.hor_stride = mpp_frame_get_hor_stride(frame);
                f.ver_stride = mpp_frame_get_ver_stride(frame);
                f.fd = mpp_buffer_get_fd(buf);
                f.data = (const uint8_t *)mpp_buffer_get_ptr(buf);
                f.pts = mpp_frame_get_pts(frame);
                // s3. 回调 mpp_decoder_cb: 在帧被释放前完成拷贝/颜色转换
                emit(f);
                frame_count_++;
                error_count_ = 0;
                got++;
            }
            else if (buf)
            {
                LOGW("mpp output format 0x%x not supported (10bit?)", (unsigned)fmt);
            }
        }
        bool eos = mpp_frame_get_eos(frame) != 0;
        mpp_frame_deinit(&frame);
        if (eos)
            break;
    }
    return got;
}

int MppDecoder::decode(const VideoPacket &pkt)
{
    if (!ctx_)
        return -1;

    MppPacket packet = nullptr;
    if (pkt.eos || pkt.data.empty())
    {
        mpp_packet_init(&packet, nullptr, 0);
        mpp_packet_set_eos(packet);
    }
    else
    {
        mpp_packet_init(&packet, (void *)pkt.data.data(), pkt.data.size());
        mpp_packet_set_pts(packet, pkt.pts);
    }

    // decode_put_packet 在输入队列满时返回失败: 先取帧腾出空间再重试
    bool put_done = false;
    int result = 0;
    for (int tries = 0; tries < 250; tries++)
    {
        if (!put_done && mpi_->decode_put_packet(ctx_, packet) == MPP_OK)
            put_done = true;
        if (drainFrames() < 0)
        {
            result = -1;
            break;
        }
        if (put_done)
            break;
        usleep(2000);
    }
    if (!put_done && result == 0)
    {
        LOGW("mpp decoder busy, packet dropped");
        error_count_++;
    }
    mpp_packet_deinit(&packet);

    // 连续错误帧过多: 通知上层重建解码器
    if (error_count_ > 50)
    {
        error_count_ = 0;
        return -1;
    }
    return result;
}

#endif
