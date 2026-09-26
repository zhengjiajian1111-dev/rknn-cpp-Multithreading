#include "output/gb28181/SipMessage.hpp"

#include <stdlib.h>
#include <strings.h>

#include <algorithm>
#include <sstream>

static std::string trim(const std::string &s)
{
    size_t b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos)
        return "";
    size_t e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

// SIP 紧凑头名 -> 完整头名
static std::string canonicalHeader(const std::string &name)
{
    static const std::map<std::string, std::string> compact = {
        {"v", "Via"}, {"f", "From"}, {"t", "To"}, {"i", "Call-ID"}, {"m", "Contact"},
        {"l", "Content-Length"}, {"c", "Content-Type"}, {"s", "Subject"}, {"k", "Supported"},
    };
    std::string lower = name;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
    auto it = compact.find(lower);
    return it != compact.end() ? it->second : name;
}

bool SipMessage::parse(const std::string &raw, SipMessage &out)
{
    out = SipMessage();
    size_t hdr_end = raw.find("\r\n\r\n");
    std::string head = hdr_end == std::string::npos ? raw : raw.substr(0, hdr_end);
    std::istringstream iss(head);
    std::string line;
    if (!std::getline(iss, line))
        return false;
    line = trim(line);
    if (line.compare(0, 8, "SIP/2.0 ") == 0)
    {
        out.is_request = false;
        out.status = atoi(line.c_str() + 8);
        size_t sp = line.find(' ', 8);
        out.reason = sp == std::string::npos ? "" : line.substr(sp + 1);
    }
    else
    {
        size_t sp1 = line.find(' ');
        size_t sp2 = line.rfind(' ');
        if (sp1 == std::string::npos || sp2 == sp1)
            return false;
        out.is_request = true;
        out.method = line.substr(0, sp1);
        out.uri = line.substr(sp1 + 1, sp2 - sp1 - 1);
    }
    while (std::getline(iss, line))
    {
        line = trim(line);
        if (line.empty())
            continue;
        size_t colon = line.find(':');
        if (colon == std::string::npos)
            continue;
        out.headers.emplace_back(canonicalHeader(trim(line.substr(0, colon))), trim(line.substr(colon + 1)));
    }
    if (hdr_end != std::string::npos)
    {
        out.body = raw.substr(hdr_end + 4);
        std::string cl = out.header("Content-Length");
        if (!cl.empty())
        {
            size_t n = (size_t)atoi(cl.c_str());
            if (n < out.body.size())
                out.body.resize(n);
        }
    }
    return true;
}

std::string SipMessage::header(const std::string &name) const
{
    for (auto &h : headers)
        if (strcasecmp(h.first.c_str(), name.c_str()) == 0)
            return h.second;
    return "";
}

std::vector<std::string> SipMessage::headerAll(const std::string &name) const
{
    std::vector<std::string> v;
    for (auto &h : headers)
        if (strcasecmp(h.first.c_str(), name.c_str()) == 0)
            v.push_back(h.second);
    return v;
}

int SipMessage::cseq() const { return atoi(header("CSeq").c_str()); }

std::string SipMessage::cseqMethod() const
{
    std::string v = header("CSeq");
    size_t sp = v.find(' ');
    return sp == std::string::npos ? "" : trim(v.substr(sp + 1));
}

std::string sipParam(const std::string &value, const std::string &name)
{
    // 只在 '>' 之后查找, 避免匹配到 URI 内部的参数
    size_t start = value.find('>');
    start = start == std::string::npos ? 0 : start;
    std::string key = ";" + name + "=";
    size_t p = value.find(key, start);
    if (p == std::string::npos)
        return "";
    p += key.size();
    size_t e = value.find_first_of(";, >", p);
    return value.substr(p, e == std::string::npos ? std::string::npos : e - p);
}

std::string sipUri(const std::string &value)
{
    size_t b = value.find('<');
    size_t e = value.find('>');
    if (b != std::string::npos && e != std::string::npos && e > b)
        return value.substr(b + 1, e - b - 1);
    size_t semi = value.find(';');
    return trim(value.substr(0, semi));
}

std::map<std::string, std::string> parseAuthParams(const std::string &value)
{
    std::map<std::string, std::string> out;
    std::string v = value;
    if (strncasecmp(v.c_str(), "Digest", 6) == 0)
        v = v.substr(6);
    size_t i = 0;
    while (i < v.size())
    {
        while (i < v.size() && (v[i] == ' ' || v[i] == ','))
            i++;
        size_t eq = v.find('=', i);
        if (eq == std::string::npos)
            break;
        std::string key = trim(v.substr(i, eq - i));
        std::transform(key.begin(), key.end(), key.begin(), ::tolower);
        size_t j = eq + 1;
        std::string val;
        if (j < v.size() && v[j] == '"')
        {
            size_t q = v.find('"', j + 1);
            val = v.substr(j + 1, q == std::string::npos ? std::string::npos : q - j - 1);
            i = q == std::string::npos ? v.size() : q + 1;
        }
        else
        {
            size_t c = v.find(',', j);
            val = trim(v.substr(j, c == std::string::npos ? std::string::npos : c - j));
            i = c == std::string::npos ? v.size() : c;
        }
        out[key] = val;
    }
    return out;
}

std::string xmlValue(const std::string &xml, const std::string &tag)
{
    std::string open = "<" + tag + ">";
    std::string close = "</" + tag + ">";
    size_t b = xml.find(open);
    if (b == std::string::npos)
        return "";
    b += open.size();
    size_t e = xml.find(close, b);
    return e == std::string::npos ? "" : trim(xml.substr(b, e - b));
}

bool parseSdp(const std::string &body, SdpOffer &out)
{
    out = SdpOffer();
    std::istringstream iss(body);
    std::string line;
    bool video = false;
    while (std::getline(iss, line))
    {
        line = trim(line);
        if (line.size() < 2 || line[1] != '=')
            continue;
        char type = line[0];
        std::string val = line.substr(2);
        if (type == 's')
            out.session_name = val;
        else if (type == 'c')
        {
            // c=IN IP4 192.168.1.100
            size_t sp = val.rfind(' ');
            if (sp != std::string::npos)
                out.ip = val.substr(sp + 1);
        }
        else if (type == 'm')
        {
            // m=video 30000 RTP/AVP 96 97 98 / m=video 30000 TCP/RTP/AVP 96
            std::istringstream ms(val);
            std::string media, port, proto;
            ms >> media >> port >> proto;
            video = media == "video";
            if (video)
            {
                out.port = atoi(port.c_str());
                out.tcp = proto.find("TCP") != std::string::npos;
            }
        }
        else if (type == 'a' && video && val.compare(0, 6, "setup:") == 0)
            out.setup = val.substr(6);
        else if (type == 'y')
            out.ssrc = val;
    }
    return !out.ip.empty() && out.port > 0;
}
