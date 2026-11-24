#pragma once

#include "VegaNozzles.hpp"
#include "Protocol.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace vega {

// Raster scales inferred from the XP square. The job record stores travel in controller ticks.
inline constexpr double columnsPerMm = 9.41;
inline constexpr double rowsPerMm = 11.90;
inline constexpr double feedTicksPerMm = 15.625;
inline constexpr int rowsPerBand = 150;
inline constexpr int nozzleBankOffset = 39;
inline constexpr int minScanColumns = 8330;
inline constexpr int scanOverrunColumns = 801;
inline constexpr unsigned packetSize = 1024;

inline void appendLE16(std::string& bytes, unsigned value) {
    bytes.push_back(static_cast<char>(value & 255));
    bytes.push_back(static_cast<char>((value >> 8) & 255));
}

inline void appendBE32(std::string& bytes, unsigned value) {
    bytes += proto::be16(value >> 16);
    bytes += proto::be16(value & 65535);
}

class Band {
    std::vector<std::array<uint16_t, 22>> columns;
public:
    explicit Band(int width) : columns(static_cast<size_t>(width)) {}

    void dot(int x, int row) {
        if (x < 0 || row < 0 || row >= rowsPerBand) return;
        for (int bank = 0; bank < 2; ++bank) {
            const int column = x + (bank == 0 ? nozzleBankOffset : 0);
            if (static_cast<size_t>(column) >= columns.size()) continue;
            const unsigned slot = nozzleForRow[row * 2 + bank];
            columns[column][slot >> 4] |= static_cast<uint16_t>(1u << (slot & 15));
        }
    }

    std::string encode(bool reverseColumns = false) const {
        std::string bytes;
        bytes.reserve(columns.size() * 4);
        // Reverse the entire carriage scan, including margins and the offset between nozzle banks.
        for (size_t i = 0; i < columns.size(); ++i) {
            const auto& masks = columns[reverseColumns ? columns.size() - i - 1 : i];
            for (unsigned phase = 21; phase > 0; --phase) {
                if (!masks[phase]) continue;
                appendLE16(bytes, phase * 4);
                appendLE16(bytes, masks[phase]);
            }
            appendLE16(bytes, 0xC000);
            appendLE16(bytes, 0xC000 | masks[0]);
        }
        bytes.resize((bytes.size() + packetSize - 1) / packetSize * packetSize, '\0');
        return bytes;
    }
};

// The controller uses this value for the final band's feed instead of the standard band feed.
inline unsigned finalBandAdvance(double remainingTicks, double spacingMm) {
    const double ticks = remainingTicks + spacingMm * feedTicksPerMm;
    if (!std::isfinite(ticks) || spacingMm < 0 || ticks < 0 || ticks > 65535)
        throw std::runtime_error("Final paper advance exceeds the VEGA command range.");
    return static_cast<unsigned>(std::lround(ticks));
}

inline std::string capturedSquareRecord(std::string_view frame, double spacingMm) {
    // The XP square recorded 334 ticks on the final band with 10 mm of drawing spacing.
    constexpr size_t advanceOffset = 48;
    std::string data(frame.substr(8, 54));
    data.replace(advanceOffset, 2, proto::be16(finalBandAdvance(334 - 10 * feedTicksPerMm, spacingMm)));
    return proto::frame(0x0D, data);
}

inline std::string jobRecord(std::string title, unsigned bands, unsigned columns, double rightMarginMm,
                             unsigned finalAdvance, bool bidi = false) {
    title.resize(32, '\0');
    std::string data = std::move(title);
    data += proto::be16(bidi ? 1 : 0);
    appendBE32(data, static_cast<unsigned>(std::floor(rightMarginMm * feedTicksPerMm)));
    appendBE32(data, bands);
    appendBE32(data, columns);
    data += proto::be16(0x00C0);  // Standard band feed for this VEGA profile
    data += proto::be16(finalAdvance);
    data += proto::be16(0x0160);  // Uncompressed column record size
    data += proto::be16(0);       // Channel
    return proto::frame(0x0D, data);
}

inline unsigned bandTiming(size_t packets, bool first) {
    if (first) return 24;
    // Legacy engine 0x1001341D scales the speed setting by the transfer packet count.
    return static_cast<unsigned>(std::max(1L, std::lround(70.0 * (600.0 - packets * 0.5) / 900.0)));
}

}  // namespace vega
