#ifndef OUTPUT_GB28181_MD5_HPP
#define OUTPUT_GB28181_MD5_HPP

#include <string>

// SIP Digest 认证用的 MD5(RFC 1321), 返回 32 位小写十六进制
std::string md5Hex(const std::string &input);

#endif
