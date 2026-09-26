#ifndef OUTPUT_GB28181_SIP_MESSAGE_HPP
#define OUTPUT_GB28181_SIP_MESSAGE_HPP

#include <map>
#include <string>
#include <utility>
#include <vector>

// 极简 SIP 报文解析(只覆盖 GB28181 设备侧用到的部分)
struct SipMessage
{
    bool is_request = false;
    std::string method; // 请求: INVITE / ACK / BYE / MESSAGE ...
    std::string uri;
    int status = 0; // 响应: 200 / 401 ...
    std::string reason;
    std::vector<std::pair<std::string, std::string>> headers; // 头名已规范化为完整形式
    std::string body;

    static bool parse(const std::string &raw, SipMessage &out);

    std::string header(const std::string &name) const;
    std::vector<std::string> headerAll(const std::string &name) const;
    int cseq() const;
    std::string cseqMethod() const;
    std::string callId() const { return header("Call-ID"); }
};

// 取头部参数: sipParam("<sip:x@y>;tag=abc", "tag") -> "abc"
std::string sipParam(const std::string &value, const std::string &name);
// 取 <> 中的 URI
std::string sipUri(const std::string &value);
// 解析 Digest 认证参数: realm/nonce/qop/algorithm/opaque
std::map<std::string, std::string> parseAuthParams(const std::string &value);
// 取 XML 标签内容(MANSCDP 报文结构简单, 不需要完整 XML 解析器)
std::string xmlValue(const std::string &xml, const std::string &tag);

// 平台 INVITE 中的 SDP
struct SdpOffer
{
    std::string ip;
    int port = 0;
    bool tcp = false;
    std::string setup; // TCP: active(平台主动连设备) / passive(设备主动连平台)
    std::string ssrc;  // y= 字段
    std::string session_name;
};
bool parseSdp(const std::string &body, SdpOffer &out);

#endif
