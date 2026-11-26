#pragma once

#include <windows.h>
#include <algorithm>
#include <cmath>
#include <string_view>

inline double plotTextWidth(double lineWidth, double offset) {
    return std::max(0.0, lineWidth + offset);
}

// Each pair is an X/Y point on a six-unit cap-height grid. A slash lifts the pen.
inline std::string_view plotGlyph(unsigned char ch) {
    constexpr std::string_view uppercase[] = {
        "062046/1545", "060030413303/033343453606", "40100105063645", "06003041453606", "40000646/0333", "400006/0333",
        "401001050636454323", "0006/4046/0343", "2026", "404536160505", "0006/400346", "000646",
        "0600234046", "06004640", "103041453606050110", "06003041423303", "103041453606050110/3446", "06003041423303/2346",
        "40100102133344453606", "0040/2026", "000516364540", "002640", "0016233640", "0046/4006",
        "002340/2326", "00400646"
    };
    constexpr std::string_view digits[] = {"103041453606050110", "102026/1636", "0010304142320646", "003041423344453606", "300444/4046", "4000033344453606", "40100105063645443303", "004026", "103041423303021030/033344453606050403", "4041360605/434301103040"};
    constexpr std::string_view lowercase[] = {
        "1423334446/44341405163645", "0006/031333434536160503", "43130405163646", "4046/4333130305163646", "04444333130305163646", "06013140/0333",
        "43130305163646/4347381807", "0006/0313334346", "2021/2326", "3031/33371708", "0006/430546", "2026",
        "0603132426/2433434446", "060313334346", "133343453616050313", "080313334345361605", "4348/4333130405163645", "0603133343",
        "43130304244445361606", "20253646/0343", "0305163646/4346", "032643", "0316243643", "0346/4306",
        "032643/260817", "03430646"
    };
    if (ch >= 'A' && ch <= 'Z') return uppercase[ch - 'A'];
    if (ch >= '0' && ch <= '9') return digits[ch - '0'];
    if (ch >= 'a' && ch <= 'z') return lowercase[ch - 'a'];
    switch (ch) {
        case ' ': return {};
        case '.': return "2526";
        case ',': return "2517";
        case ':': return "2223/2526";
        case ';': return "2223/2517";
        case '-': return "0343";
        case '_': return "0646";
        case '+': return "0343/2125";
        case '=': return "0242/0444";
        case '/': return "4006";
        case '\\': return "0046";
        case '(': return "301102041536";
        case ')': return "103142443516";
        case '[': return "30101636";
        case ']': return "10303616";
        case '%': return "0010110100/3545463635/4006";
        case '#': return "1116/3136/0343/0545";
        case '!': return "2024/2526";
        case '\'': return "2022";
        case '"': return "1012/3032";
        case '&': return "46021030414233230405063645";
        default: return "0010304142322425/2526";
    }
}

inline bool drawPlotText(HDC dc, double x, double y, std::string_view text, double height,
                         double angle, HPEN pen) {
    const double step = height / 6;
    const double alongX = std::cos(angle) * step;
    const double alongY = std::sin(angle) * step;
    const HGDIOBJ previous = SelectObject(dc, pen);
    bool drawn = true;
    double advance = 0;
    for (unsigned char ch : text) {
        if (ch == '\r') continue;
        if (ch == '\n') {
            x -= 8 * alongY;
            y += 8 * alongX;
            advance = 0;
            continue;
        }
        const auto glyph = plotGlyph(ch);
        POINT points[16];
        int count = 0;
        const auto flush = [&] {
            if (count > 1) drawn = Polyline(dc, points, count) && drawn;
            count = 0;
        };
        for (size_t i = 0; i < glyph.size();) {
            if (glyph[i] == '/') {
                flush();
                ++i;
                continue;
            }
            const double u = advance + glyph[i++] - '0';
            const double v = glyph[i++] - '0' - 6;
            points[count++] = {
                static_cast<LONG>(std::lround(x + u * alongX - v * alongY)),
                static_cast<LONG>(std::lround(y + u * alongY + v * alongX))
            };
        }
        flush();
        advance += 5;
    }
    SelectObject(dc, previous);
    return drawn;
}
