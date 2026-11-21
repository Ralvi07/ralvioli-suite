#include <cassert>
#include <iostream>
#include "Protocol.hpp"

int main() {
    // known vector worked out by hand: type 0x3F, data 00 1A
    std::string f = proto::frame(0x3F, std::string("\x00\x1A", 2));
    const unsigned char want[] = {0x3F, 0xC0, 0x00, 0xFF, 0x00, 0x02, 0xFF, 0xFD, 0x00, 0x1A, 0x04, 0x16};
    assert(f.size() == sizeof(want) && std::string(f) == std::string(reinterpret_cast<const char*>(want), sizeof(want)));

    // a reply built the same way decodes and verifies
    std::string reply = proto::frame(0x1A, std::string(10, '\x07'));
    std::string hdr = reply.substr(0, 8), body = reply.substr(8);
    size_t len = 0; std::string err;
    assert(proto::checkHeader(hdr, 0x1A, len, err) && len == 10);
    assert(proto::checkBody(hdr, body, err));
    assert(!proto::checkHeader(hdr, 0x1B, len, err));                 // wrong command type
    std::string bad = hdr; bad[7] = static_cast<char>(bad[7] ^ 1);
    assert(!proto::checkHeader(bad, 0x1A, len, err));                 // complement byte wrong
    std::string badBody = body; badBody[3] = static_cast<char>(badBody[3] ^ 0x10);
    assert(!proto::checkBody(hdr, badBody, err));                     // checksum wrong
    std::string big = proto::frame(0x20, std::string(300, 'x'));      // 16-bit length, big-endian
    assert(static_cast<unsigned char>(big[4]) == 0x01 && static_cast<unsigned char>(big[5]) == 0x2C);

    // versions
    proto::Version v = proto::parseVersion("Ver.15.01.00");
    assert(v.valid && v.major == 15 && v.minor == 1 && v.patch == 0 && v.text() == "15.01.00");
    assert(!proto::parseVersion("none").valid && proto::parseVersion("15.1").valid);
    proto::Version fw = proto::parseVersion("15.01.00"), mon = proto::parseVersion("15.01.00");
    proto::Compat c = proto::checkVersions(fw, mon);
    assert(c.known && c.allowed && c.extended);                       // the user's plotter: accepted
    assert(!proto::checkVersions(proto::parseVersion("14.09.00"), mon).allowed);
    assert(!proto::checkVersions(proto::parseVersion("8.01.00"), mon).extended);
    assert(!proto::checkVersions(fw, proto::parseVersion("0.02.00")).extended);
    assert(!proto::checkVersions(proto::Version(), mon).known);
    std::cout << "PROTOCOL OK\n";
}
