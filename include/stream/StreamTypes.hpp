#ifndef STREAM_STREAM_TYPES_HPP
#define STREAM_STREAM_TYPES_HPP

#include <cstdint>
#include <memory>
#include <vector>

extern "C"
{
#include <libavcodec/avcodec.h>
}

enum class CodecType
{
    H264,
    H265,
    Other,
};

// 码流参数. 每次(重新)打开输入都会生成新的对象, 解码线程据此判断是否需要重建解码器
struct StreamParams
{
    CodecType codec = CodecType::Other;
    int width = 0;
    int height = 0;
    double fps = 25.0;
    uint64_t generation = 0;
    std::shared_ptr<AVCodecParameters> par; // 给 FFmpeg 软解使用(含 Annex-B extradata)
};

// 拉流线程 -> 解码线程 的数据单元(Annex-B 格式的一帧码流)
struct VideoPacket
{
    std::vector<uint8_t> data;
    int64_t pts = 0; // 毫秒
    bool key = false;
    bool eos = false;
    std::shared_ptr<const StreamParams> params;
};

using VideoPacketPtr = std::shared_ptr<VideoPacket>;

#endif
