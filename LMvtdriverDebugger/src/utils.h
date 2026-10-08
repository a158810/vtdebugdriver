#pragma once
#include <windows.h>
#include <string>
#include <vector>
#include <sstream>
#include <iomanip>

// ============================================================================
// 工具函数
// ============================================================================

inline std::string FormatHex8(uint8_t v) {
    std::ostringstream oss;
    oss << std::hex << std::uppercase << std::setfill('0') << std::setw(2) << (int)v;
    return oss.str();
}

inline std::string FormatHex16(uint16_t v) {
    std::ostringstream oss;
    oss << std::hex << std::uppercase << std::setfill('0') << std::setw(4) << v;
    return oss.str();
}

inline std::string FormatHex32(uint32_t v) {
    std::ostringstream oss;
    oss << std::hex << std::uppercase << std::setfill('0') << std::setw(8) << v;
    return oss.str();
}

inline std::string FormatHex64(uint64_t v) {
    std::ostringstream oss;
    oss << std::hex << std::uppercase << std::setfill('0') << std::setw(16) << v;
    return oss.str();
}

inline std::string FormatHexBytes(const uint8_t* data, size_t len) {
    std::ostringstream oss;
    for (size_t i = 0; i < len; i++) {
        if (i > 0) oss << " ";
        oss << FormatHex8(data[i]);
    }
    return oss.str();
}

inline uint64_t ParseHex64(const std::string& s) {
    return std::stoull(s, nullptr, 16);
}

inline uint32_t ParseHex32(const std::string& s) {
    return std::stoul(s, nullptr, 16);
}

inline std::wstring AnsiToWide(const std::string& s) {
    if (s.empty()) return L"";
    int len = MultiByteToWideChar(CP_ACP, 0, s.c_str(), (int)s.size(), NULL, 0);
    std::wstring ws(len, 0);
    MultiByteToWideChar(CP_ACP, 0, s.c_str(), (int)s.size(), &ws[0], len);
    return ws;
}

inline std::string WideToAnsi(const std::wstring& ws) {
    if (ws.empty()) return "";
    int len = WideCharToMultiByte(CP_ACP, 0, ws.c_str(), (int)ws.size(), NULL, 0, NULL, NULL);
    std::string s(len, 0);
    WideCharToMultiByte(CP_ACP, 0, ws.c_str(), (int)ws.size(), &s[0], len, NULL, NULL);
    return s;
}

inline std::vector<std::string> SplitArgs(const std::string& line) {
    std::vector<std::string> args;
    std::istringstream iss(line);
    std::string token;
    while (iss >> token) {
        args.push_back(token);
    }
    return args;
}
