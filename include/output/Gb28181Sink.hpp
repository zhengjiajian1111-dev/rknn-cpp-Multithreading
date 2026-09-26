#ifndef OUTPUT_GB28181_SINK_HPP
#define OUTPUT_GB28181_SINK_HPP

#include <netinet/in.h>

#include <atomic>
#include <memory>
#include <mutex>
#include <random>
#include <string>
#include <vector>

#include "common/WorkerThread.hpp"
#include "output/StreamSink.hpp"
#include "output/gb28181/PsMuxer.hpp"
#include "output/gb28181/SipMessage.hpp"

// GB28181 设备端接入(SIP over UDP + PS over RTP)
//  信令(独立 SIP 线程):
//   * REGISTER + Digest 认证, 过期前自动刷新; 心跳 MESSAGE(Keepalive) 连续无应答判定掉线并重新注册;
//   * 响应平台 Catalog / DeviceInfo / DeviceStatus 查询, DeviceControl 的 IFameCmd 强制关键帧;
//   * INVITE -> 100 Trying + 200 OK(SDP) -> ACK 后开始推流; BYE/CANCEL 结束会话.
//  端口占用: 本地 SIP 端口被占用时自动顺延(最多 20 个); 媒体端口被占用时回退到系统分配.
//  重复会话:
//   * 同一 Call-ID 的 INVITE 重传 -> 原样重发缓存的 200 OK, 不会建立第二路媒体;
//   * 推流中收到新的点播 -> on_duplicate_invite=replace 时 BYE 旧会话后接受新会话, reject 时回 486.
//  媒体: UDP / TCP 主动(a=setup:passive) / TCP 被动(a=setup:active), SSRC 使用平台下发的 y= 值.
class Gb28181Sink : public IStreamSink
{
public:
    explicit Gb28181Sink(const PushConfig &cfg);
    ~Gb28181Sink() override;

    bool open(const StreamInfo &info) override;
    bool write(const EncodedPacket &pkt) override;
    void close() override;
    bool isOpen() const override { return sip_running_.load(); }
    bool needsHeaderToOpen() const override { return false; }
    bool wantsMedia() const override { return media_active_.load(); }
    bool takeKeyframeRequest() override { return keyframe_req_.exchange(false); }
    std::string describe() const override;

private:
    struct Session
    {
        std::string call_id;
        std::string from;        // 平台(INVITE 的 From)
        std::string to;          // 本端(INVITE 的 To + 本端 tag)
        std::string remote_uri;  // BYE 的 Request-URI(平台 Contact)
        sockaddr_in remote_sip{}; // INVITE 来源地址
        std::string ok_response; // 缓存 200 OK, 应对 INVITE 重传
        std::string dst_ip;
        int dst_port = 0;
        bool tcp = false;
        bool tcp_connect = false; // true: 设备主动连接平台
        uint32_t ssrc = 0;
        int sock = -1;
        int listen_sock = -1;
        int local_port = 0;
        bool acked = false;
        int64_t created_ms = 0;
        int64_t acked_ms = 0;
        uint16_t rtp_seq = 0;
        int send_errors = 0;
    };

    void sipLoop();
    bool setupSipSocket();
    std::string detectLocalIp();
    void sendRaw(const std::string &msg, const sockaddr_in &to);
    std::string viaHeader();
    std::string randomHex(int n);
    std::string authorization(const std::string &method, const std::string &uri);

    void sendRegister(int expires, bool with_auth);
    std::string sendManscdp(const std::string &xml);
    void timers();

    void onPacket(const std::string &data, const sockaddr_in &from);
    void onResponse(const SipMessage &msg);
    void onRequest(const SipMessage &msg, const sockaddr_in &from);
    void onInvite(const SipMessage &msg, const sockaddr_in &from);
    void onAck(const SipMessage &msg);
    void onQuery(const SipMessage &msg, const sockaddr_in &from);
    std::string buildResponse(const SipMessage &req, int code, const std::string &reason, const std::string &to_tag,
                              const std::string &extra = "", const std::string &content_type = "",
                              const std::string &body = "");

    bool openMediaSocket(Session &s);
    bool connectMedia(Session &s);
    void pollMediaAccept();
    void closeSession(bool send_bye);
    void sendBye(const Session &s);
    bool sendAll(int sock, const uint8_t *data, size_t len);

    PushConfig cfg_;
    Gb28181Config gb_;
    WorkerThread thread_;
    std::atomic<bool> sip_running_{false};
    int sip_sock_ = -1;
    int local_sip_port_ = 0;
    std::string local_ip_;
    sockaddr_in server_addr_{};
    std::mt19937 rng_;

    // 注册状态(仅 SIP 线程访问)
    bool registered_ = false;
    bool reg_waiting_ = false;
    std::string reg_call_id_;
    std::string reg_from_tag_;
    int reg_cseq_ = 0;
    int64_t reg_next_ms_ = 0;
    int64_t reg_sent_ms_ = 0;
    int auth_attempts_ = 0;
    std::string realm_, nonce_, qop_, opaque_;
    // 心跳
    int64_t ka_next_ms_ = 0;
    int ka_miss_ = 0;
    bool ka_waiting_ = false;
    std::string ka_call_id_;
    int msg_cseq_ = 1;
    int sn_ = 1;

    // 媒体(SIP 线程与推流线程共享)
    std::mutex media_mtx_;
    std::unique_ptr<Session> session_;
    std::atomic<bool> media_active_{false};
    std::atomic<bool> media_failed_{false};
    std::atomic<bool> keyframe_req_{false};
    PsMuxer ps_;
    std::vector<uint8_t> ps_buf_;
    std::vector<uint8_t> rtp_buf_;
};

#endif
