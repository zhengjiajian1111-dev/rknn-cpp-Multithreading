#include "output/VideoEncoder.hpp"

#include <sstream>

#include "common/Logger.hpp"
#include "output/FfmpegEncoder.hpp"
#include "output/MppEncoder.hpp"

namespace h264
{
    // 遍历 Annex-B 码流中的 NAL: cb(nal_start(含起始码), nal_payload, end)
    template <typename F>
    static void forEachNal(const uint8_t *data, size_t size, F cb)
    {
        size_t i = 0;
        auto findStart = [&](size_t from, size_t &sc_len) -> size_t {
            for (size_t k = from; k + 3 <= size; k++)
            {
                if (data[k] == 0 && data[k + 1] == 0)
                {
                    if (data[k + 2] == 1)
                    {
                        sc_len = 3;
                        return k;
                    }
                    if (k + 4 <= size && data[k + 2] == 0 && data[k + 3] == 1)
                    {
                        sc_len = 4;
                        return k;
                    }
                }
            }
            sc_len = 0;
            return size;
        };
        size_t sc_len = 0;
        i = findStart(0, sc_len);
        while (i < size)
        {
            size_t payload = i + sc_len;
            size_t next_len = 0;
            size_t next = findStart(payload, next_len);
            if (payload < size)
                cb(i, payload, next);
            i = next;
            sc_len = next_len;
        }
    }

    bool hasNalType(const uint8_t *data, size_t size, int type)
    {
        bool found = false;
        forEachNal(data, size, [&](size_t, size_t payload, size_t) {
            if ((data[payload] & 0x1f) == type)
                found = true;
        });
        return found;
    }

    std::vector<uint8_t> extractParamSets(const uint8_t *data, size_t size)
    {
        std::vector<uint8_t> out;
        forEachNal(data, size, [&](size_t start, size_t payload, size_t end) {
            int t = data[payload] & 0x1f;
            if (t == 7 || t == 8)
                out.insert(out.end(), data + start, data + end);
        });
        return out;
    }
}

void VideoEncoder::finalizePacket(EncodedPacket &pkt)
{
    const uint8_t *d = pkt.data.data();
    size_t n = pkt.data.size();
    pkt.key = h264::hasNalType(d, n, 5);
    if (!pkt.key)
        return;
    if (h264::hasNalType(d, n, 7))
    {
        if (header_.empty())
            header_ = h264::extractParamSets(d, n);
    }
    else if (!header_.empty())
    {
        pkt.data.insert(pkt.data.begin(), header_.begin(), header_.end());
    }
}

std::unique_ptr<VideoEncoder> createEncoder(const std::string &pref, bool use_rga, int width, int height, int fps,
                                            int bitrate_kbps, int gop)
{
    if (pref == "auto" || pref == "mpp")
    {
#ifdef HAVE_MPP
        std::unique_ptr<VideoEncoder> enc(new MppEncoder(use_rga));
        if (enc->init(width, height, fps, bitrate_kbps, gop))
            return enc;
        LOGW("mpp encoder init failed");
#else
        (void)use_rga;
        LOGW("built without MPP encoder");
#endif
        if (pref == "mpp")
            return nullptr;
    }
    // ffmpeg / ffmpeg:<name>; 默认候选依次尝试 ffmpeg-rockchip 硬编、x264、openh264
    std::string names = "h264_rkmpp,libx264,libopenh264,h264_v4l2m2m,h264";
    if (pref.compare(0, 7, "ffmpeg:") == 0)
        names = pref.substr(7);
    std::unique_ptr<VideoEncoder> enc(new FfmpegEncoder(names));
    if (enc->init(width, height, fps, bitrate_kbps, gop))
        return enc;
    LOGE("no usable h264 encoder (tried: %s)", names.c_str());
    return nullptr;
}
