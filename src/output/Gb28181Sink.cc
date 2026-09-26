#include "output/Gb28181Sink.hpp"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>

#include "common/Logger.hpp"
#include "output/gb28181/Md5.hpp"

static const char *kUserAgent = "rknn-multi-stream";

static std::string addrStr(const sockaddr_in &a)
{
    char ip[INET_ADDRSTRLEN] = {0};
    inet_ntop(AF_INET, &a.sin_addr, ip, sizeof(ip));
    return std::string(ip) + ":" + std::to_string(ntohs(a.sin_port));
}

static void closeFd(int &fd)
{
    if (fd >= 0)
    {
        ::close(fd);
        fd = -1;
    }
}

static int localPort(int sock)
{
    sockaddr_in a{};
    socklen_t len = sizeof(a);
    if (getsockname(sock, (sockaddr *)&a, &len) < 0)
        return 0;
    return ntohs(a.sin_port);
}

// 绑定媒体端口: 指定端口被占用时回退到系统分配
static bool bindPort(int sock, int port)
{
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    a.sin_port = htons((uint16_t)port);
    if (bind(sock, (sockaddr *)&a, sizeof(a)) == 0)
        return true;
    if (port != 0 && errno == EADDRINUSE)
    {
        LOGW("gb28181: media port %d in use, fallback to ephemeral port", port);
        a.sin_port = 0;
        return bind(sock, (sockaddr *)&a, sizeof(a)) == 0;
    }
    return false;
}

Gb28181Sink::Gb28181Sink(const PushConfig &cfg) : cfg_(cfg), gb_(cfg.gb), rng_(std::random_device{}()) {}

Gb28181Sink::~Gb28181Sink() { close(); }

std::string Gb28181Sink::describe() const
{
    return "gb28181 device " + gb_.device_id + " -> " + gb_.sip_server_id + "@" + gb_.sip_server_ip + ":" +
           std::to_string(gb_.sip_server_port);
}

std::string Gb28181Sink::randomHex(int n)
{
    static const char hex[] = "0123456789abcdef";
    std::string s;
    for (int i = 0; i < n; i++)
        s.push_back(hex[rng_() % 16]);
    return s;
}

std::string Gb28181Sink::viaHeader()
{
    return "Via: SIP/2.0/UDP " + local_ip_ + ":" + std::to_string(local_sip_port_) + ";rport;branch=z9hG4bK" +
           randomHex(12) + "\r\n";
}

bool Gb28181Sink::setupSipSocket()
{
    addrinfo hints{}, *res = nullptr;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    if (getaddrinfo(gb_.sip_server_ip.c_str(), std::to_string(gb_.sip_server_port).c_str(), &hints, &res) != 0 || !res)
    {
        LOGE("gb28181: cannot resolve sip server %s", gb_.sip_server_ip.c_str());
        return false;
    }
    memcpy(&server_addr_, res->ai_addr, sizeof(server_addr_));
    freeaddrinfo(res);

    sip_sock_ = socket(AF_INET, SOCK_DGRAM, 0);
    if (sip_sock_ < 0)
        return false;
    // 注意: 不设置 SO_REUSEADDR. 对 UDP 而言它允许多个进程绑定同一端口, 会让端口冲突"静默"发生
    for (int port = gb_.local_sip_port; port < gb_.local_sip_port + 20; port++)
    {
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_ANY);
        a.sin_port = htons((uint16_t)port);
        if (bind(sip_sock_, (sockaddr *)&a, sizeof(a)) == 0)
        {
            local_sip_port_ = port;
            if (port != gb_.local_sip_port)
                LOGW("gb28181: sip port %d in use, using %d instead", gb_.local_sip_port, port);
            return true;
        }
        if (errno != EADDRINUSE)
            break;
    }
    LOGE("gb28181: bind local sip port failed: %s", strerror(errno));
    closeFd(sip_sock_);
    return false;
}

std::string Gb28181Sink::detectLocalIp()
{
    // UDP connect 不发包, 只让内核选出到平台的出口网卡地址
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    std::string ip = "127.0.0.1";
    if (s >= 0 && connect(s, (sockaddr *)&server_addr_, sizeof(server_addr_)) == 0)
    {
        sockaddr_in a{};
        socklen_t len = sizeof(a);
        if (getsockname(s, (sockaddr *)&a, &len) == 0)
        {
            char buf[INET_ADDRSTRLEN] = {0};
            inet_ntop(AF_INET, &a.sin_addr, buf, sizeof(buf));
            ip = buf;
        }
    }
    if (s >= 0)
        ::close(s);
    return ip;
}

bool Gb28181Sink::open(const StreamInfo &)
{
    if (sip_running_)
        return true;
    thread_.join();
    if (!setupSipSocket())
        return false;
    local_ip_ = gb_.local_ip.empty() ? detectLocalIp() : gb_.local_ip;
    registered_ = false;
    reg_waiting_ = false;
    reg_next_ms_ = 0;
    reg_call_id_ = randomHex(16) + "@" + local_ip_;
    reg_from_tag_ = randomHex(8);
    ka_waiting_ = false;
    ka_miss_ = 0;
    auth_attempts_ = 0;
    media_failed_ = false;
    sip_running_ = true;
    thread_.start(cfg_.name + "-sip", [this] { sipLoop(); });
    LOGI("%s: sip %s:%d -> %s", cfg_.name.c_str(), local_ip_.c_str(), local_sip_port_, describe().c_str());
    return true;
}

void Gb28181Sink::close()
{
    sip_running_ = false;
    thread_.join();
    closeFd(sip_sock_);
}

void Gb28181Sink::sendRaw(const std::string &msg, const sockaddr_in &to)
{
    LOGD("sip >>> %s\n%s", addrStr(to).c_str(), msg.c_str());
    if (sendto(sip_sock_, msg.data(), msg.size(), 0, (const sockaddr *)&to, sizeof(to)) < 0)
        LOGW("gb28181: sendto %s failed: %s", addrStr(to).c_str(), strerror(errno));
}

std::string Gb28181Sink::authorization(const std::string &method, const std::string &uri)
{
    std::string ha1 = md5Hex(gb_.device_id + ":" + realm_ + ":" + gb_.password);
    std::string ha2 = md5Hex(method + ":" + uri);
    std::string auth = "Digest username=\"" + gb_.device_id + "\", realm=\"" + realm_ + "\", nonce=\"" + nonce_ +
                       "\", uri=\"" + uri + "\"";
    if (qop_.find("auth") != std::string::npos)
    {
        std::string nc = "00000001";
        std::string cnonce = randomHex(16);
        std::string resp = md5Hex(ha1 + ":" + nonce_ + ":" + nc + ":" + cnonce + ":auth:" + ha2);
        auth += ", response=\"" + resp + "\", algorithm=MD5, qop=auth, nc=" + nc + ", cnonce=\"" + cnonce + "\"";
    }
    else
    {
        auth += ", response=\"" + md5Hex(ha1 + ":" + nonce_ + ":" + ha2) + "\", algorithm=MD5";
    }
    if (!opaque_.empty())
        auth += ", opaque=\"" + opaque_ + "\"";
    return auth;
}

void Gb28181Sink::sendRegister(int expires, bool with_auth)
{
    std::string uri = "sip:" + gb_.sip_server_id + "@" + gb_.sip_domain;
    std::string msg = "REGISTER " + uri + " SIP/2.0\r\n" + viaHeader() + "From: <sip:" + gb_.device_id + "@" +
                      gb_.sip_domain + ">;tag=" + reg_from_tag_ + "\r\n" + "To: <sip:" + gb_.device_id + "@" +
                      gb_.sip_domain + ">\r\n" + "Call-ID: " + reg_call_id_ + "\r\n" + "CSeq: " +
                      std::to_string(++reg_cseq_) + " REGISTER\r\n" + "Contact: <sip:" + gb_.device_id + "@" +
                      local_ip_ + ":" + std::to_string(local_sip_port_) + ">\r\n" + "Max-Forwards: 70\r\n" +
                      "User-Agent: " + kUserAgent + "\r\n" + "Expires: " + std::to_string(expires) + "\r\n";
    if (with_auth && !nonce_.empty())
        msg += "Authorization: " + authorization("REGISTER", uri) + "\r\n";
    msg += "Content-Length: 0\r\n\r\n";
    sendRaw(msg, server_addr_);
    reg_waiting_ = true;
    reg_sent_ms_ = nowMs();
}

std::string Gb28181Sink::sendManscdp(const std::string &xml)
{
    std::string call_id = randomHex(16) + "@" + local_ip_;
    std::string msg = "MESSAGE sip:" + gb_.sip_server_id + "@" + gb_.sip_domain + " SIP/2.0\r\n" + viaHeader() +
                      "From: <sip:" + gb_.device_id + "@" + gb_.sip_domain + ">;tag=" + randomHex(8) + "\r\n" +
                      "To: <sip:" + gb_.sip_server_id + "@" + gb_.sip_domain + ">\r\n" + "Call-ID: " + call_id +
                      "\r\n" + "CSeq: " + std::to_string(++msg_cseq_) + " MESSAGE\r\n" +
                      "Content-Type: Application/MANSCDP+xml\r\n" + "Max-Forwards: 70\r\n" + "User-Agent: " +
                      kUserAgent + "\r\n" + "Content-Length: " + std::to_string(xml.size()) + "\r\n\r\n" + xml;
    sendRaw(msg, server_addr_);
    return call_id;
}

std::string Gb28181Sink::buildResponse(const SipMessage &req, int code, const std::string &reason,
                                       const std::string &to_tag, const std::string &extra,
                                       const std::string &content_type, const std::string &body)
{
    std::string msg = "SIP/2.0 " + std::to_string(code) + " " + reason + "\r\n";
    for (auto &v : req.headerAll("Via"))
        msg += "Via: " + v + "\r\n";
    msg += "From: " + req.header("From") + "\r\n";
    std::string to = req.header("To");
    if (!to_tag.empty() && sipParam(to, "tag").empty())
        to += ";tag=" + to_tag;
    msg += "To: " + to + "\r\n";
    msg += "Call-ID: " + req.callId() + "\r\n";
    msg += "CSeq: " + req.header("CSeq") + "\r\n";
    msg += std::string("User-Agent: ") + kUserAgent + "\r\n";
    msg += extra;
    if (!body.empty())
        msg += "Content-Type: " + content_type + "\r\n";
    msg += "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body;
    return msg;
}

void Gb28181Sink::sipLoop()
{
    std::string buf(65536, '\0');
    while (sip_running_)
    {
        pollfd pfd{sip_sock_, POLLIN, 0};
        if (poll(&pfd, 1, 100) > 0 && (pfd.revents & POLLIN))
        {
            sockaddr_in from{};
            socklen_t len = sizeof(from);
            ssize_t n = recvfrom(sip_sock_, &buf[0], buf.size(), 0, (sockaddr *)&from, &len);
            if (n > 0)
                onPacket(std::string(buf.data(), (size_t)n), from);
        }
        timers();
    }
    // 退出: 结束会话(BYE) -> 注销(Expires: 0)
    closeSession(true);
    if (registered_)
        sendRegister(0, true);
    registered_ = false;
    LOGI("%s: sip stopped", cfg_.name.c_str());
}

void Gb28181Sink::timers()
{
    int64_t now = nowMs();

    // 注册 / 刷新注册 / 应答超时重发
    if (reg_waiting_ && now - reg_sent_ms_ > 5000)
    {
        LOGW("%s: REGISTER timeout, platform unreachable?", cfg_.name.c_str());
        reg_waiting_ = false;
        registered_ = false;
        reg_next_ms_ = now + 5000;
    }
    if (!reg_waiting_ && now >= reg_next_ms_)
    {
        if (!registered_)
        {
            // 全新注册: 使用新的 Call-ID, 平台重启后不会与旧注册混淆
            reg_call_id_ = randomHex(16) + "@" + local_ip_;
            reg_from_tag_ = randomHex(8);
            nonce_.clear();
            auth_attempts_ = 0;
        }
        sendRegister(gb_.expires, registered_);
    }

    // 心跳: 连续 keepalive_max_miss 次无应答判定掉线, 重新注册
    if (registered_ && now >= ka_next_ms_)
    {
        if (ka_waiting_)
        {
            ka_miss_++;
            LOGW("%s: keepalive no response (%d/%d)", cfg_.name.c_str(), ka_miss_, gb_.keepalive_max_miss);
            if (ka_miss_ >= gb_.keepalive_max_miss)
            {
                LOGW("%s: platform offline, re-register", cfg_.name.c_str());
                registered_ = false;
                ka_waiting_ = false;
                ka_miss_ = 0;
                reg_next_ms_ = now;
                closeSession(false);
                return;
            }
        }
        std::string xml = "<?xml version=\"1.0\" encoding=\"GB2312\"?>\r\n<Notify>\r\n<CmdType>Keepalive</CmdType>\r\n"
                          "<SN>" + std::to_string(sn_++) + "</SN>\r\n<DeviceID>" + gb_.device_id +
                          "</DeviceID>\r\n<Status>OK</Status>\r\n</Notify>\r\n";
        ka_call_id_ = sendManscdp(xml);
        ka_waiting_ = true;
        ka_next_ms_ = now + gb_.keepalive * 1000;
    }

    // 媒体会话维护
    if (media_failed_.exchange(false))
    {
        LOGW("%s: media connection broken, send BYE", cfg_.name.c_str());
        closeSession(true);
    }
    pollMediaAccept();
    bool timeout = false;
    {
        std::lock_guard<std::mutex> lock(media_mtx_);
        if (session_ && !session_->acked && now - session_->created_ms > 10000)
            timeout = true;
    }
    if (timeout)
    {
        LOGW("%s: no ACK for INVITE within 10s, drop session", cfg_.name.c_str());
        closeSession(false);
    }
}

void Gb28181Sink::onPacket(const std::string &data, const sockaddr_in &from)
{
    if (data.size() < 4 || data == "\r\n\r\n")
        return; // NAT 保活包
    SipMessage msg;
    if (!SipMessage::parse(data, msg))
    {
        LOGW("gb28181: bad sip message from %s", addrStr(from).c_str());
        return;
    }
    LOGD("sip <<< %s\n%s", addrStr(from).c_str(), data.c_str());
    if (msg.is_request)
        onRequest(msg, from);
    else
        onResponse(msg);
}

void Gb28181Sink::onResponse(const SipMessage &msg)
{
    const std::string method = msg.cseqMethod();
    if (msg.status < 200)
        return; // 临时响应
    int64_t now = nowMs();
    if (method == "REGISTER" && msg.callId() == reg_call_id_)
    {
        reg_waiting_ = false;
        if (msg.status == 401 || msg.status == 407)
        {
            auto p = parseAuthParams(msg.header(msg.status == 401 ? "WWW-Authenticate" : "Proxy-Authenticate"));
            realm_ = p["realm"];
            nonce_ = p["nonce"];
            qop_ = p["qop"];
            opaque_ = p["opaque"];
            if (++auth_attempts_ > 3)
            {
                LOGE("%s: register rejected (401) repeatedly, check device_id/password", cfg_.name.c_str());
                registered_ = false;
                reg_next_ms_ = now + 30000;
                return;
            }
            sendRegister(gb_.expires, true);
            return;
        }
        if (msg.status == 200)
        {
            bool fresh = !registered_;
            registered_ = true;
            auth_attempts_ = 0;
            int expires = gb_.expires;
            std::string e = msg.header("Expires");
            if (!e.empty() && atoi(e.c_str()) > 0)
                expires = atoi(e.c_str());
            reg_next_ms_ = now + std::max(30, expires * 4 / 5) * 1000;
            if (fresh)
            {
                LOGI("%s: registered to %s (expires %ds)", cfg_.name.c_str(), gb_.sip_server_id.c_str(), expires);
                ka_next_ms_ = now + 1000;
                ka_waiting_ = false;
                ka_miss_ = 0;
            }
            return;
        }
        LOGW("%s: register failed: %d %s", cfg_.name.c_str(), msg.status, msg.reason.c_str());
        registered_ = false;
        reg_next_ms_ = now + 30000;
        return;
    }
    if (method == "MESSAGE" && msg.callId() == ka_call_id_ && msg.status == 200)
    {
        ka_waiting_ = false;
        ka_miss_ = 0;
    }
}

void Gb28181Sink::onRequest(const SipMessage &msg, const sockaddr_in &from)
{
    const std::string &m = msg.method;
    if (m == "INVITE")
        onInvite(msg, from);
    else if (m == "ACK")
        onAck(msg);
    else if (m == "BYE" || m == "CANCEL")
    {
        sendRaw(buildResponse(msg, 200, "OK", randomHex(8)), from);
        bool match = false;
        {
            std::lock_guard<std::mutex> lock(media_mtx_);
            match = session_ && session_->call_id == msg.callId();
        }
        if (match)
        {
            LOGI("%s: %s from platform, stop streaming", cfg_.name.c_str(), m.c_str());
            closeSession(false);
        }
    }
    else if (m == "MESSAGE")
        onQuery(msg, from);
    else if (m == "OPTIONS" || m == "INFO" || m == "NOTIFY" || m == "SUBSCRIBE")
        sendRaw(buildResponse(msg, 200, "OK", randomHex(8)), from);
    else
        sendRaw(buildResponse(msg, 405, "Method Not Allowed", randomHex(8)), from);
}

void Gb28181Sink::onQuery(const SipMessage &msg, const sockaddr_in &from)
{
    sendRaw(buildResponse(msg, 200, "OK", randomHex(8)), from);
    std::string cmd = xmlValue(msg.body, "CmdType");
    std::string sn = xmlValue(msg.body, "SN");
    const std::string head = "<?xml version=\"1.0\" encoding=\"GB2312\"?>\r\n<Response>\r\n<CmdType>" + cmd +
                             "</CmdType>\r\n<SN>" + sn + "</SN>\r\n<DeviceID>" + gb_.device_id + "</DeviceID>\r\n";
    if (cmd == "Catalog")
    {
        std::string civil = gb_.sip_domain.substr(0, std::min<size_t>(6, gb_.sip_domain.size()));
        sendManscdp(head + "<SumNum>1</SumNum>\r\n<DeviceList Num=\"1\">\r\n<Item>\r\n<DeviceID>" + gb_.channel_id +
                    "</DeviceID>\r\n<Name>" + gb_.device_name +
                    "</Name>\r\n<Manufacturer>Rockchip</Manufacturer>\r\n<Model>RK3588</Model>\r\n<Owner>Owner</Owner>\r\n"
                    "<CivilCode>" + civil + "</CivilCode>\r\n<Address>Address</Address>\r\n<Parental>0</Parental>\r\n"
                    "<ParentID>" + gb_.device_id + "</ParentID>\r\n<SafetyWay>0</SafetyWay>\r\n"
                    "<RegisterWay>1</RegisterWay>\r\n<Secrecy>0</Secrecy>\r\n<Status>ON</Status>\r\n</Item>\r\n"
                    "</DeviceList>\r\n</Response>\r\n");
    }
    else if (cmd == "DeviceInfo")
    {
        sendManscdp(head + "<Result>OK</Result>\r\n<DeviceName>" + gb_.device_name +
                    "</DeviceName>\r\n<Manufacturer>Rockchip</Manufacturer>\r\n<Model>RK3588</Model>\r\n"
                    "<Firmware>1.0</Firmware>\r\n<Channel>1</Channel>\r\n</Response>\r\n");
    }
    else if (cmd == "DeviceStatus")
    {
        sendManscdp(head + "<Result>OK</Result>\r\n<Online>ONLINE</Online>\r\n<Status>OK</Status>\r\n"
                           "<Encode>ON</Encode>\r\n<Record>OFF</Record>\r\n</Response>\r\n");
    }
    else if (cmd == "DeviceControl")
    {
        // 标准中字段名就是 IFameCmd(拼写如此), 兼容 IFrameCmd
        if (!xmlValue(msg.body, "IFameCmd").empty() || !xmlValue(msg.body, "IFrameCmd").empty())
            keyframe_req_ = true;
        sendManscdp(head + "<Result>OK</Result>\r\n</Response>\r\n");
    }
}

void Gb28181Sink::onInvite(const SipMessage &msg, const sockaddr_in &from)
{
    const std::string call_id = msg.callId();
    bool replace = false;
    {
        std::lock_guard<std::mutex> lock(media_mtx_);
        if (session_ && session_->call_id == call_id)
        {
            // INVITE 重传(平台没收到 200 OK): 原样重发, 不创建第二个会话
            LOGD("%s: INVITE retransmission, resend 200 OK", cfg_.name.c_str());
            sendRaw(session_->ok_response, from);
            return;
        }
        if (session_)
        {
            if (gb_.on_duplicate_invite == "reject")
            {
                LOGW("%s: already streaming, reject new INVITE with 486", cfg_.name.c_str());
                sendRaw(buildResponse(msg, 486, "Busy Here", randomHex(8)), from);
                return;
            }
            replace = true;
        }
    }
    if (replace)
    {
        LOGW("%s: new INVITE while streaming, replace old session", cfg_.name.c_str());
        closeSession(true);
    }

    SdpOffer offer;
    if (!parseSdp(msg.body, offer))
    {
        sendRaw(buildResponse(msg, 400, "Bad Request", randomHex(8)), from);
        return;
    }
    sendRaw(buildResponse(msg, 100, "Trying", ""), from);

    std::unique_ptr<Session> s(new Session());
    std::string to_tag = randomHex(8);
    s->call_id = call_id;
    s->from = msg.header("From");
    s->to = msg.header("To");
    if (sipParam(s->to, "tag").empty())
        s->to += ";tag=" + to_tag;
    s->remote_uri = sipUri(msg.header("Contact"));
    if (s->remote_uri.empty())
        s->remote_uri = "sip:" + gb_.sip_server_id + "@" + addrStr(from);
    s->remote_sip = from;
    s->dst_ip = offer.ip;
    s->dst_port = offer.port;
    s->tcp = offer.tcp;
    s->tcp_connect = offer.tcp && offer.setup != "active"; // 平台 passive/未声明 -> 设备主动连接
    // 平台未下发 y= 时按 GB28181 规则生成: 0(实时) + 域 ID 第 4~8 位 + 4 位序号
    std::string ssrc = offer.ssrc;
    if (ssrc.empty())
        ssrc = "0" + (gb_.sip_domain.size() >= 8 ? gb_.sip_domain.substr(3, 5) : std::string("00000")) +
               std::to_string(1000 + rng_() % 9000);
    s->ssrc = (uint32_t)strtoul(ssrc.c_str(), nullptr, 10);

    if (!openMediaSocket(*s))
    {
        sendRaw(buildResponse(msg, 500, "Server Internal Error", to_tag), from);
        return;
    }

    std::string sdp = "v=0\r\no=" + gb_.channel_id + " 0 0 IN IP4 " + local_ip_ + "\r\ns=" +
                      (offer.session_name.empty() ? "Play" : offer.session_name) + "\r\nc=IN IP4 " + local_ip_ +
                      "\r\nt=0 0\r\nm=video " + std::to_string(s->local_port) +
                      (s->tcp ? " TCP/RTP/AVP 96" : " RTP/AVP 96") + "\r\na=sendonly\r\na=rtpmap:96 PS/90000\r\n";
    if (s->tcp)
        sdp += std::string("a=setup:") + (s->tcp_connect ? "active" : "passive") + "\r\na=connection:new\r\n";
    sdp += "y=" + ssrc + "\r\nf=\r\n";
    std::string contact = "Contact: <sip:" + gb_.channel_id + "@" + local_ip_ + ":" + std::to_string(local_sip_port_) +
                          ">\r\n";
    s->ok_response = buildResponse(msg, 200, "OK", to_tag, contact, "APPLICATION/SDP", sdp);
    s->created_ms = nowMs();
    sendRaw(s->ok_response, from);
    LOGI("%s: INVITE accepted, media -> %s:%d %s ssrc=%s", cfg_.name.c_str(), s->dst_ip.c_str(), s->dst_port,
         s->tcp ? (s->tcp_connect ? "TCP(active)" : "TCP(passive)") : "UDP", ssrc.c_str());

    std::lock_guard<std::mutex> lock(media_mtx_);
    session_ = std::move(s);
}

void Gb28181Sink::onAck(const SipMessage &msg)
{
    bool failed = false;
    std::string dst;
    {
        std::lock_guard<std::mutex> lock(media_mtx_);
        if (!session_ || session_->call_id != msg.callId() || session_->acked)
            return;
        session_->acked = true;
        session_->acked_ms = nowMs();
        dst = session_->dst_ip + ":" + std::to_string(session_->dst_port);
        if (session_->tcp && session_->tcp_connect)
            failed = !connectMedia(*session_);
        if (!failed && (!session_->tcp || session_->tcp_connect))
        {
            media_active_ = true;
            keyframe_req_ = true; // 新会话立即出 IDR, 缩短平台首屏时间
            LOGI("%s: ACK received, streaming started", cfg_.name.c_str());
        }
    }
    if (failed)
    {
        LOGW("%s: connect media %s failed", cfg_.name.c_str(), dst.c_str());
        closeSession(true);
    }
}

bool Gb28181Sink::openMediaSocket(Session &s)
{
    if (!s.tcp)
    {
        s.sock = socket(AF_INET, SOCK_DGRAM, 0);
        if (s.sock < 0 || !bindPort(s.sock, gb_.media_port))
        {
            closeFd(s.sock);
            return false;
        }
        int sndbuf = 1 << 20;
        setsockopt(s.sock, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));
        sockaddr_in dst{};
        dst.sin_family = AF_INET;
        dst.sin_port = htons((uint16_t)s.dst_port);
        if (inet_pton(AF_INET, s.dst_ip.c_str(), &dst.sin_addr) != 1 ||
            connect(s.sock, (sockaddr *)&dst, sizeof(dst)) < 0)
        {
            closeFd(s.sock);
            return false;
        }
        s.local_port = localPort(s.sock);
        return true;
    }
    int &fd = s.tcp_connect ? s.sock : s.listen_sock;
    fd = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one)); // TCP: 允许快速复用 TIME_WAIT 端口
    if (fd < 0 || !bindPort(fd, gb_.media_port))
    {
        closeFd(fd);
        return false;
    }
    if (!s.tcp_connect)
    {
        if (listen(fd, 1) < 0)
        {
            closeFd(fd);
            return false;
        }
        fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
    }
    s.local_port = localPort(fd);
    return true;
}

static void configureTcp(int fd)
{
    int flags = fcntl(fd, F_GETFL);
    fcntl(fd, F_SETFL, flags & ~O_NONBLOCK);
    timeval tv{2, 0};
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
}

bool Gb28181Sink::connectMedia(Session &s)
{
    sockaddr_in dst{};
    dst.sin_family = AF_INET;
    dst.sin_port = htons((uint16_t)s.dst_port);
    if (inet_pton(AF_INET, s.dst_ip.c_str(), &dst.sin_addr) != 1)
        return false;
    fcntl(s.sock, F_SETFL, fcntl(s.sock, F_GETFL) | O_NONBLOCK);
    int ret = connect(s.sock, (sockaddr *)&dst, sizeof(dst));
    if (ret < 0 && errno != EINPROGRESS)
        return false;
    if (ret < 0)
    {
        pollfd pfd{s.sock, POLLOUT, 0};
        if (poll(&pfd, 1, 3000) <= 0)
            return false;
        int err = 0;
        socklen_t len = sizeof(err);
        getsockopt(s.sock, SOL_SOCKET, SO_ERROR, &err, &len);
        if (err != 0)
            return false;
    }
    configureTcp(s.sock);
    return true;
}

void Gb28181Sink::pollMediaAccept()
{
    bool timeout = false;
    {
        std::lock_guard<std::mutex> lock(media_mtx_);
        if (!session_ || !session_->tcp || session_->tcp_connect || !session_->acked || session_->sock >= 0)
            return;
        int fd = accept(session_->listen_sock, nullptr, nullptr);
        if (fd >= 0)
        {
            configureTcp(fd);
            session_->sock = fd;
            closeFd(session_->listen_sock);
            media_active_ = true;
            keyframe_req_ = true;
            LOGI("%s: platform connected (TCP passive), streaming started", cfg_.name.c_str());
            return;
        }
        timeout = nowMs() - session_->acked_ms > 10000;
    }
    if (timeout)
    {
        LOGW("%s: platform did not connect media within 10s", cfg_.name.c_str());
        closeSession(true);
    }
}

void Gb28181Sink::sendBye(const Session &s)
{
    std::string msg = "BYE " + s.remote_uri + " SIP/2.0\r\n" + viaHeader() + "From: " + s.to + "\r\n" + "To: " +
                      s.from + "\r\n" + "Call-ID: " + s.call_id + "\r\n" + "CSeq: " + std::to_string(++msg_cseq_) +
                      " BYE\r\n" + "Max-Forwards: 70\r\n" + "User-Agent: " + kUserAgent + "\r\n" +
                      "Content-Length: 0\r\n\r\n";
    sendRaw(msg, s.remote_sip);
}

void Gb28181Sink::closeSession(bool send_bye)
{
    std::unique_ptr<Session> s;
    {
        std::lock_guard<std::mutex> lock(media_mtx_);
        s = std::move(session_);
        media_active_ = false;
    }
    if (!s)
        return;
    if (send_bye && s->acked)
        sendBye(*s);
    closeFd(s->sock);
    closeFd(s->listen_sock);
    LOGI("%s: session %s closed", cfg_.name.c_str(), s->call_id.c_str());
}

bool Gb28181Sink::sendAll(int sock, const uint8_t *data, size_t len)
{
    while (len > 0)
    {
        ssize_t n = send(sock, data, len, MSG_NOSIGNAL);
        if (n < 0)
        {
            if (errno == EINTR)
                continue;
            return false;
        }
        data += n;
        len -= (size_t)n;
    }
    return true;
}

bool Gb28181Sink::write(const EncodedPacket &pkt)
{
    if (!media_active_)
        return true;
    std::lock_guard<std::mutex> lock(media_mtx_);
    if (!session_ || session_->sock < 0)
        return true;
    Session &s = *session_;

    uint32_t ts = (uint32_t)((uint64_t)pkt.pts * 90); // 90kHz
    ps_.mux(pkt.data.data(), pkt.data.size(), (uint64_t)pkt.pts * 90, pkt.key, ps_buf_);

    // RTP 分包: 每包负载 <= 1400 字节(避免 IP 分片), 一帧最后一个包置 marker
    const size_t kMaxPayload = 1400;
    const size_t hdr_off = s.tcp ? 2 : 0; // TCP 使用 RFC4571: 每个 RTP 包前加 2 字节长度
    size_t off = 0;
    bool frame_error = false;
    while (off < ps_buf_.size())
    {
        size_t n = std::min(kMaxPayload, ps_buf_.size() - off);
        bool last = off + n == ps_buf_.size();
        size_t rtp_len = 12 + n;
        rtp_buf_.resize(hdr_off + rtp_len);
        uint8_t *p = rtp_buf_.data();
        if (s.tcp)
        {
            p[0] = (uint8_t)(rtp_len >> 8);
            p[1] = (uint8_t)rtp_len;
        }
        uint8_t *h = p + hdr_off;
        h[0] = 0x80;                        // V=2
        h[1] = (uint8_t)((last ? 0x80 : 0) | 96); // M + PT=96(PS)
        h[2] = (uint8_t)(s.rtp_seq >> 8);
        h[3] = (uint8_t)s.rtp_seq;
        h[4] = (uint8_t)(ts >> 24);
        h[5] = (uint8_t)(ts >> 16);
        h[6] = (uint8_t)(ts >> 8);
        h[7] = (uint8_t)ts;
        h[8] = (uint8_t)(s.ssrc >> 24);
        h[9] = (uint8_t)(s.ssrc >> 16);
        h[10] = (uint8_t)(s.ssrc >> 8);
        h[11] = (uint8_t)s.ssrc;
        memcpy(h + 12, ps_buf_.data() + off, n);
        s.rtp_seq++;

        bool fatal = false;
        if (s.tcp)
        {
            fatal = !sendAll(s.sock, rtp_buf_.data(), rtp_buf_.size());
        }
        else if (send(s.sock, rtp_buf_.data(), rtp_buf_.size(), MSG_NOSIGNAL) < 0)
        {
            // UDP: 对端端口未监听(ICMP 不可达)/缓冲区满属于瞬时错误, 只计数
            if (errno == ECONNREFUSED || errno == EAGAIN || errno == ENOBUFS)
                frame_error = true;
            else
                fatal = true;
        }
        if (fatal)
        {
            // 媒体链路断开: 交给 SIP 线程发送 BYE 并清理会话
            LOGW("%s: send rtp failed: %s", cfg_.name.c_str(), strerror(errno));
            media_active_ = false;
            media_failed_ = true;
            return true;
        }
        off += n;
    }
    // 连续 100 帧发送失败(平台已不再接收)则结束会话
    s.send_errors = frame_error ? s.send_errors + 1 : 0;
    if (s.send_errors > 100)
    {
        LOGW("%s: platform not receiving rtp, stop session", cfg_.name.c_str());
        media_active_ = false;
        media_failed_ = true;
    }
    return true;
}
