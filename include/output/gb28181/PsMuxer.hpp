#ifndef OUTPUT_GB28181_PS_MUXER_HPP
#define OUTPUT_GB28181_PS_MUXER_HPP

#include <cstddef>
#include <cstdint>
#include <vector>

// GB28181 媒体封装: H.264 Annex-B -> MPEG-2 PS(ISO/IEC 13818-1)
//   每帧: PS header [+ system header + PSM (关键帧)] + PES(0xE0, 首个 PES 带 PTS)...
class PsMuxer
{
public:
    explicit PsMuxer(uint8_t stream_type = 0x1B) : stream_type_(stream_type) {}
    void mux(const uint8_t *frame, size_t size, uint64_t pts90k, bool key, std::vector<uint8_t> &out);

private:
    void packHeader(std::vector<uint8_t> &out, uint64_t scr);
    void systemHeader(std::vector<uint8_t> &out);
    void psm(std::vector<uint8_t> &out);
    void pes(std::vector<uint8_t> &out, const uint8_t *data, size_t size, bool with_pts, uint64_t pts);

    uint8_t stream_type_; // 0x1B = H.264, 0x24 = H.265
};

#endif
