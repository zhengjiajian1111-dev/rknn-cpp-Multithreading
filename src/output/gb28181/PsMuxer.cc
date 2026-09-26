#include "output/gb28181/PsMuxer.hpp"

#include <algorithm>

static const uint32_t kMuxRate = 6106; // 单位 50 字节/秒, 仅作标识
static const size_t kMaxPesPayload = 65000;

static uint32_t crc32Mpeg2(const uint8_t *data, size_t len)
{
    uint32_t crc = 0xFFFFFFFF;
    for (size_t i = 0; i < len; i++)
    {
        crc ^= (uint32_t)data[i] << 24;
        for (int k = 0; k < 8; k++)
            crc = (crc & 0x80000000) ? (crc << 1) ^ 0x04C11DB7 : (crc << 1);
    }
    return crc;
}

void PsMuxer::packHeader(std::vector<uint8_t> &out, uint64_t scr)
{
    const uint64_t ext = 0;
    uint8_t h[14];
    h[0] = 0x00;
    h[1] = 0x00;
    h[2] = 0x01;
    h[3] = 0xBA;
    h[4] = 0x40 | (uint8_t)(((scr >> 30) & 0x07) << 3) | 0x04 | (uint8_t)((scr >> 28) & 0x03);
    h[5] = (uint8_t)((scr >> 20) & 0xFF);
    h[6] = (uint8_t)(((scr >> 15) & 0x1F) << 3) | 0x04 | (uint8_t)((scr >> 13) & 0x03);
    h[7] = (uint8_t)((scr >> 5) & 0xFF);
    h[8] = (uint8_t)((scr & 0x1F) << 3) | 0x04 | (uint8_t)((ext >> 7) & 0x03);
    h[9] = (uint8_t)((ext & 0x7F) << 1) | 0x01;
    h[10] = (uint8_t)((kMuxRate >> 14) & 0xFF);
    h[11] = (uint8_t)((kMuxRate >> 6) & 0xFF);
    h[12] = (uint8_t)(((kMuxRate & 0x3F) << 2) | 0x03);
    h[13] = 0xF8; // reserved + pack_stuffing_length = 0
    out.insert(out.end(), h, h + sizeof(h));
}

void PsMuxer::systemHeader(std::vector<uint8_t> &out)
{
    const uint8_t h[] = {
        0x00, 0x00, 0x01, 0xBB,
        0x00, 0x09,                                                  // header_length = 6 + 3 * 1
        (uint8_t)(0x80 | ((kMuxRate >> 15) & 0x7F)),                  // marker + rate_bound[21..15]
        (uint8_t)((kMuxRate >> 7) & 0xFF),                           // rate_bound[14..7]
        (uint8_t)(((kMuxRate & 0x7F) << 1) | 0x01),                  // rate_bound[6..0] + marker
        0x00,                                                        // audio_bound=0, fixed=0, CSPS=0
        0xE1,                                                        // audio/video lock, marker, video_bound=1
        0xFF,                                                        // packet_rate_restriction + reserved
        0xE0, 0xE8, 0x00,                                            // video stream: P-STD scale=1, size=2048KB
    };
    out.insert(out.end(), h, h + sizeof(h));
}

void PsMuxer::psm(std::vector<uint8_t> &out)
{
    size_t start = out.size();
    const uint8_t h[] = {
        0x00, 0x00, 0x01, 0xBC,
        0x00, 0x0E,       // program_stream_map_length = 14
        0xE0,             // current_next_indicator=1, version=0
        0xFF,             // reserved + marker
        0x00, 0x00,       // program_stream_info_length = 0
        0x00, 0x04,       // elementary_stream_map_length = 4
        stream_type_, 0xE0, 0x00, 0x00, // stream_type, elementary_stream_id, es_info_length=0
    };
    out.insert(out.end(), h, h + sizeof(h));
    uint32_t crc = crc32Mpeg2(out.data() + start, out.size() - start);
    out.push_back((uint8_t)(crc >> 24));
    out.push_back((uint8_t)(crc >> 16));
    out.push_back((uint8_t)(crc >> 8));
    out.push_back((uint8_t)crc);
}

void PsMuxer::pes(std::vector<uint8_t> &out, const uint8_t *data, size_t size, bool with_pts, uint64_t pts)
{
    size_t hdr_data_len = with_pts ? 5 : 0;
    size_t pes_len = 3 + hdr_data_len + size;
    out.push_back(0x00);
    out.push_back(0x00);
    out.push_back(0x01);
    out.push_back(0xE0);
    out.push_back((uint8_t)(pes_len >> 8));
    out.push_back((uint8_t)pes_len);
    out.push_back(with_pts ? 0x84 : 0x80); // '10' + data_alignment_indicator(首个 PES)
    out.push_back(with_pts ? 0x80 : 0x00); // PTS_DTS_flags = '10'
    out.push_back((uint8_t)hdr_data_len);
    if (with_pts)
    {
        out.push_back((uint8_t)(0x20 | (((pts >> 30) & 0x07) << 1) | 0x01));
        out.push_back((uint8_t)((pts >> 22) & 0xFF));
        out.push_back((uint8_t)((((pts >> 15) & 0x7F) << 1) | 0x01));
        out.push_back((uint8_t)((pts >> 7) & 0xFF));
        out.push_back((uint8_t)(((pts & 0x7F) << 1) | 0x01));
    }
    out.insert(out.end(), data, data + size);
}

void PsMuxer::mux(const uint8_t *frame, size_t size, uint64_t pts90k, bool key, std::vector<uint8_t> &out)
{
    out.clear();
    pts90k &= 0x1FFFFFFFFULL; // 33 bit
    packHeader(out, pts90k);
    if (key)
    {
        systemHeader(out);
        psm(out);
    }
    size_t off = 0;
    bool first = true;
    while (off < size)
    {
        size_t n = std::min(size - off, kMaxPesPayload);
        pes(out, frame + off, n, first, pts90k);
        off += n;
        first = false;
    }
}
