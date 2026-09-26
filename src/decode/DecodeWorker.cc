#include "decode/DecodeWorker.hpp"

#include <string.h>

#include "common/Logger.hpp"
#include "common/PerfMonitor.hpp"
#include "common/RgaUtils.hpp"
#include "opencv2/imgproc/imgproc.hpp"

DecodeWorker::DecodeWorker(int channel, const DecoderConfig &cfg, bool use_rga, BlockingQueue<VideoPacketPtr> *in,
                           Mbuffer *out)
    : ch_(channel), cfg_(cfg), use_rga_(use_rga && rga::available()), in_(in), out_(out)
{
    std::string p = "ch" + std::to_string(ch_);
    st_decode_ = PerfMonitor::instance().stat(p + ".decode");
    st_cvt_ = PerfMonitor::instance().stat(p + ".yuv2bgr");
}

DecodeWorker::~DecodeWorker()
{
    requestStop();
    join();
}

void DecodeWorker::start()
{
    running_ = true;
    thread_.start("ch" + std::to_string(ch_) + "-dec", [this] { run(); });
}

void DecodeWorker::requestStop() { running_ = false; }

void DecodeWorker::join() { thread_.join(); }

void DecodeWorker::mpp_decoder_cb(void *userdata, const DecodedFrame &frame)
{
    static_cast<DecodeWorker *>(userdata)->onFrame(frame);
}

void DecodeWorker::onFrame(const DecodedFrame &f)
{
    ScopedTimer timer(st_cvt_);
    cv::Mat bgr;
    if (f.format == DecodedFrame::BGR)
    {
        bgr = f.bgr;
    }
    else
    {
        bool done = false;
        if (use_rga_)
        {
            // RGA 直接从 MPP 的 DMA-BUF 转换, CPU 不参与像素搬运
            done = rga::nv12ToBgr(f.fd, f.data, f.width, f.height, f.hor_stride, f.ver_stride, bgr);
            if (!done)
            {
                LOGW("ch%d: rga nv12->bgr failed, fallback to cpu", ch_);
                use_rga_ = false;
            }
        }
        if (!done)
        {
            if (!f.data)
                return;
            // 拷贝 YUV 数据到内存: 去掉 stride 填充. 注意 UV 平面起始于 hor_stride*ver_stride,
            // 而不是 width*height(1080p 的 ver_stride 通常是 1088), 直接按宽高取会导致色彩错位
            int w = f.width & ~1;
            int h = f.height & ~1;
            yuv_.resize((size_t)w * h * 3 / 2);
            const uint8_t *src_y = f.data;
            const uint8_t *src_uv = f.data + (size_t)f.hor_stride * f.ver_stride;
            for (int r = 0; r < h; r++)
                memcpy(&yuv_[(size_t)r * w], src_y + (size_t)r * f.hor_stride, w);
            for (int r = 0; r < h / 2; r++)
                memcpy(&yuv_[(size_t)w * h + (size_t)r * w], src_uv + (size_t)r * f.hor_stride, w);
            cv::Mat yuv(h * 3 / 2, w, CV_8UC1, yuv_.data());
            cv::cvtColor(yuv, bgr, cv::COLOR_YUV2BGR_NV12);
        }
    }
    frames_++;
    // 写入 Mbuffer: 推理线程总是读取最新一帧
    out_->write(bgr, f.pts);
}

void DecodeWorker::run()
{
    VideoPacketPtr pkt;
    while (running_)
    {
        if (!in_->pop(pkt, 200))
        {
            if (in_->closed())
                break;
            continue;
        }
        // 重连后码流参数可能变化(分辨率/编码格式), 重建解码器
        if (pkt->params != cur_params_)
        {
            decoder_.reset();
            cur_params_ = pkt->params;
            if (cur_params_)
            {
                decoder_ = createDecoder(cfg_.backend, *cur_params_, cfg_.mpp_split_parse);
                if (decoder_)
                {
                    decoder_->setCallback(&DecodeWorker::mpp_decoder_cb, this);
                    LOGI("ch%d: decoder '%s' created", ch_, decoder_->name());
                }
                else
                {
                    LOGE("ch%d: no usable decoder", ch_);
                }
            }
        }
        if (!decoder_)
            continue;

        int ret;
        {
            ScopedTimer timer(st_decode_);
            ret = decoder_->decode(*pkt);
        }
        if (ret < 0)
        {
            LOGW("ch%d: decoder error, rebuild on next packet", ch_);
            decoder_.reset();
            cur_params_.reset();
        }
        if (pkt->eos)
            LOGI("ch%d: decoder reached eos", ch_);
    }
    decoder_.reset();
}
