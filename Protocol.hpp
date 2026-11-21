#pragma once
// Plotter frames and firmware rules as used by the old Algotex engine (AlgotexOcx.ocx) and the Algotex
// server. No Windows dependencies.
//
// Frame (read from the old engine's send and receive code):
//   [type] [~type] [0x00] [0xFF] [len hi] [len lo] [~len hi] [~len lo] [data ...] [sum hi] [sum lo]
//   len = number of data bytes, sum = sum of all bytes before it, modulo 65536; 16-bit values are big-endian.
// The plotter answers a frame with one acknowledge byte (0 = ok, 1 = send again), and a reply frame carries
// the type of the command it answers.

#include <cstdint>
#include <cstdio>
#include <string>
#include <stdexcept>

namespace proto {

inline std::string be16(unsigned v) {
    std::string s;
    s += static_cast<char>((v >> 8) & 0xFF);
    s += static_cast<char>(v & 0xFF);
    return s;
}

inline std::string frame(uint8_t type, const std::string& data) {
    if (data.size() > 0xFFFF) throw std::length_error("Algotex frame payload exceeds 65535 bytes");
    std::string f;
    f += static_cast<char>(type);
    f += static_cast<char>(static_cast<uint8_t>(~type));
    f += static_cast<char>(0x00);
    f += static_cast<char>(0xFF);
    unsigned n = static_cast<unsigned>(data.size()) & 0xFFFF;
    f += be16(n);
    f += static_cast<char>(static_cast<uint8_t>(~(n >> 8)));
    f += static_cast<char>(static_cast<uint8_t>(~n));
    f += data;
    unsigned sum = 0;
    for (unsigned char c : f) {
        sum += c;
    }
    f += be16(sum & 0xFFFF);
    return f;
}

// Checks the 8 header bytes of a reply. On success returns true and the number of data bytes that follow
// (followed by two checksum bytes).
inline bool checkHeader(const std::string& h, uint8_t expectType, size_t& len, std::string& err) {
    if (h.size() < 8) {
        err = "short header";
        return false;
    }
    auto b = [&](size_t i) { return static_cast<uint8_t>(h[i]); };
    if (b(0) != static_cast<uint8_t>(~b(1)) || b(2) != static_cast<uint8_t>(~b(3)) ||
        b(6) != static_cast<uint8_t>(~b(4)) || b(7) != static_cast<uint8_t>(~b(5))) {
        err = "header complement bytes do not match";
        return false;
    }
    if (b(0) != expectType) {
        char t[64];
        std::snprintf(t, sizeof(t), "reply type 0x%02X, expected 0x%02X", b(0), expectType);
        err = t;
        return false;
    }
    len = (static_cast<size_t>(b(4)) << 8) | b(5);
    return true;
}

// body = data bytes + 2 checksum bytes. Returns true when the checksum matches.
inline bool checkBody(const std::string& header, const std::string& body, std::string& err) {
    if (body.size() < 2) {
        err = "short body";
        return false;
    }
    size_t n = body.size() - 2;
    unsigned sum = 0;
    for (unsigned char c : header) {
        sum += c;
    }
    for (size_t i = 0; i < n; ++i) {
        sum += static_cast<unsigned char>(body[i]);
    }
    unsigned got = (static_cast<unsigned char>(body[n]) << 8) | static_cast<unsigned char>(body[n + 1]);
    if ((sum & 0xFFFF) != got) {
        err = "checksum mismatch";
        return false;
    }
    return true;
}

// ---------------------------------------------------------------- firmware rules
struct Version {
    bool valid = false;
    int major = 0;
    int minor = 0;
    int patch = 0;
    std::string text() const {
        char b[48];
        std::snprintf(b, sizeof(b), "%d.%02d.%02d", major, minor, patch);
        return valid ? std::string(b) : std::string("unknown");
    }
};

// Accepts "15.01.00", "Ver.15.01.00", "15.1.0".
inline Version parseVersion(const std::string& s) {
    Version v;
    size_t p = 0;
    while (p < s.size() && !(s[p] >= '0' && s[p] <= '9')) {
        ++p;
    }
    int a = 0, b = 0, c = 0;
    int n = std::sscanf(s.c_str() + p, "%d.%d.%d", &a, &b, &c);
    if (n >= 2) {
        v.valid = true;
        v.major = a;
        v.minor = b;
        v.patch = n >= 3 ? c : 0;
    }
    return v;
}

struct Compat {
    bool known = false;        // a firmware version was given
    bool allowed = true;       // both Algotex programs accept this firmware
    bool extended = true;      // firmware >= 9 and monitor >= 0.3: the old engine switches its extended mode on
    std::string message;
};

// Rules found in the two Algotex programs:
//  - old engine and new server: firmware first number below 15 is refused ("update the firmware" / "too old");
//  - old engine: firmware below 9, or a monitor version below 0.3, turns its extended mode off.
inline Compat checkVersions(const Version& fw, const Version& monitor) {
    Compat c;
    if (!fw.valid) {
        c.message = "Firmware version not set";
        return c;
    }
    c.known = true;
    if (fw.major < 15) {
        c.allowed = false;
        c.message = "Firmware " + fw.text() + " is older than 15: the Algotex programs refuse it";
    }
    c.extended = !(fw.major < 9) && !(monitor.valid && monitor.major == 0 && monitor.minor < 3);
    return c;
}

}  // namespace proto
