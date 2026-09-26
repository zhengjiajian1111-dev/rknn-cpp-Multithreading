#ifndef OUTPUT_STREAM_SINK_HPP
#define OUTPUT_STREAM_SINK_HPP

#include <memory>
#include <string>
#include <vector>

#include "common/Config.hpp"
#include "output/VideoEncoder.hpp"

struct StreamInfo
{
    int width = 0;
    int height = 0;
    double fps = 25;
    std::vector<uint8_t> header; // SPS/PPS
};

// 推流协议出口的统一接口: RTMP / RTSP / GB28181 通过配置开关组合, 推流线程不关心具体协议
class IStreamSink
{
public:
    virtual ~IStreamSink() = default;

    virtual bool open(const StreamInfo &info) = 0;
    // 返回 false 表示连接已断开, 由推流线程负责关闭并按退避策略重连
    virtual bool write(const EncodedPacket &pkt) = 0;
    virtual void close() = 0;
    virtual bool isOpen() const = 0;

    // open 前是否需要编码器的 SPS/PPS(FLV/RTSP 需要; GB28181 先注册, 等平台点播)
    virtual bool needsHeaderToOpen() const { return true; }
    // 当前是否有人消费媒体数据(GB28181 未点播时不必编码)
    virtual bool wantsMedia() const { return true; }
    // 新会话需要立即出关键帧, 缩短首屏时间
    virtual bool takeKeyframeRequest() { return false; }
    virtual std::string describe() const = 0;
};

std::unique_ptr<IStreamSink> createSink(const PushConfig &cfg);

#endif
