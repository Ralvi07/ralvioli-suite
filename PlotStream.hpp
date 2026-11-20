// Plans the bytes sent to the plotter and maps stream positions back to the preview for progress and resume.
#pragma once
// Print-stream planning on top of RalvioliCore.hpp (no Windows dependencies).
// makeStream() reproduces rm::merge() for a single piece line by line, so every output line is
// known and can be mapped back to the drawing for the progress display.

#include <cmath>
#include <charconv>
#include <optional>
#include <string>
#include <vector>

#include "RalvioliCore.hpp"
#include "Calibration.hpp"

namespace rp {

struct Line {
    std::string text;       // without the trailing newline
    int op = -1;            // piece op that produced it (-1: preamble / trailer)
    bool draw = false;      // drawing or positioning line (not replayed when starting mid-file)
    rm::Pt cur;             // pen position before this line (piece units)
    bool down = false;      // pen state before this line
};

struct Stream {
    std::vector<Line> lines;
    std::vector<std::vector<int>> opLines;   // op index -> line indices
    bool eof = false;
    bool calibrated = false;
    double dx = 0.0;
    double dy = 0.0;
    double g = 1.0;
};

inline std::string pairOf(const Stream& s, const rm::Pt& p) {
    return rm::pairText(p, s.dx, s.dy, s.g);
}

inline bool movesKind(const rm::Op& o) {
    return (o.kind == rm::K::Pts && !o.pts.empty()) || o.kind == rm::K::Arc || o.kind == rm::K::Three ||
           o.kind == rm::K::Rect || o.kind == rm::K::Wedge || o.kind == rm::K::Label;
}

inline Stream makeStream(const rm::Piece& piece, double outUnit, cal::Factors calibration = {}) {
    Stream s;
    std::vector<rm::Pt> all = rm::layerPoints(piece.ops);
    if (all.empty()) {
        throw rm::HpglError("no drawing commands found in the input file");
    }
    double minx = all[0].x;
    double miny = all[0].y;
    for (const rm::Pt& p : all) {
        minx = std::min(minx, p.x);
        miny = std::min(miny, p.y);
    }
    s.dx = minx < 0 ? -minx : 0.0;
    s.dy = miny < 0 ? -miny : 0.0;
    s.g = 0.025 / outUnit;
    s.opLines.assign(piece.ops.size(), std::vector<int>());

    auto add = [&](const std::string& text, int op, bool draw, const rm::Pt& cur, bool down) {
        Line l;
        l.text = text;
        l.op = op;
        l.draw = draw;
        l.cur = cur;
        l.down = down;
        s.lines.push_back(l);
        if (op >= 0) {
            s.opLines[static_cast<size_t>(op)].push_back(static_cast<int>(s.lines.size()) - 1);
        }
    };

    rm::Pt cur = piece.start;
    bool down = false;
    const rm::Op* head = nullptr;
    for (const rm::Op& op : piece.ops) {
        if (op.kind == rm::K::Header) {
            head = &op;
            break;
        }
    }
    if (head == nullptr || !head->flag) {
        add("IN;", -1, false, cur, down);
    }
    if (head != nullptr) {
        add(head->text, -1, false, cur, down);
    }
    if (calibration.active()) {
        auto factorText = [](double factor) {
            char text[64];
            auto result = std::to_chars(text, text + sizeof(text), factor, std::chars_format::fixed);
            if (result.ec != std::errc{} || !(factor > 0.0) || !std::isfinite(factor)) {
                throw rm::HpglError("Calibration factor is out of range");
            }
            return std::string(text, result.ptr);
        };
        add("SC0," + factorText(calibration.feed) + ",0," + factorText(calibration.across) + ",2;",
            -1, false, cur, down);
        s.calibrated = true;
    }
    const rm::Op* first = nullptr;
    for (const rm::Op& o : piece.ops) {
        if (movesKind(o)) {
            first = &o;
            break;
        }
    }
    bool needsStart = !(first != nullptr && first->kind == rm::K::Pts && first->verb == "PU");
    if (pairOf(s, piece.start) == "0,0") {
        needsStart = false;
    }
    if (needsStart) {
        add("PU" + pairOf(s, piece.start) + ";", -1, true, cur, down);
    }
    for (size_t j = 0; j < piece.ops.size(); ++j) {
        const rm::Op& op = piece.ops[j];
        int oi = static_cast<int>(j);
        switch (op.kind) {
            case rm::K::Pts: {
                if (op.pts.empty()) {
                    add(op.verb + ";", oi, true, cur, down);
                }
                rm::Pt before = cur;
                bool d = down;
                if (op.verb == "PU") {
                    d = false;
                } else if (op.verb == "PD") {
                    d = true;
                }
                for (size_t k = 0; k < op.pts.size(); k += static_cast<size_t>(rm::kChunk)) {
                    std::string line = op.verb;
                    size_t stop = std::min(k + static_cast<size_t>(rm::kChunk), op.pts.size());
                    for (size_t m = k; m < stop; ++m) {
                        if (m > k) {
                            line += ",";
                        }
                        line += rm::pairText(op.pts[m], s.dx, s.dy, s.g);
                    }
                    add(line + ";", oi, true, before, k == 0 ? down : d);
                    before = op.pts[stop - 1];
                }
                down = d;
                if (!op.pts.empty()) {
                    cur = op.pts.back();
                }
                break;
            }
            case rm::K::Arc: {
                std::string text = "AA" + pairOf(s, op.p) + "," + rm::num(op.n[0]);
                if (op.hasChord) {
                    text += "," + rm::num(op.chord);
                }
                add(text + ";", oi, true, cur, down);
                cur = op.end;
                break;
            }
            case rm::K::Three: {
                std::string text = "AT" + pairOf(s, op.p) + "," + pairOf(s, op.q);
                if (op.hasChord) {
                    text += "," + rm::num(op.chord);
                }
                add(text + ";", oi, true, cur, down);
                cur = op.q;
                break;
            }
            case rm::K::Rect:
                add(op.verb + pairOf(s, op.q) + ";", oi, true, cur, down);
                break;
            case rm::K::Wedge: {
                std::string line = op.verb;
                for (size_t m = 0; m < op.n.size(); ++m) {
                    if (m > 0) {
                        line += ",";
                    }
                    line += rm::num(m == 0 ? op.n[m] * s.g : op.n[m]);
                }
                add(line + ";", oi, true, cur, down);
                break;
            }
            case rm::K::Label:
                add("LB" + op.text + std::string(1, '\x03') + ";", oi, true, cur, down);
                break;
            case rm::K::Dir: {
                bool plain = j == 0 && std::fabs(op.n[0] - 1000) < 1e-6 && std::fabs(op.n[1]) < 1e-6;
                if (!plain) {
                    add("DI" + rm::fmt(op.n[0]) + "," + rm::fmt(op.n[1]) + ";", oi, false, cur, down);
                }
                break;
            }
            case rm::K::Comment:
                add("CO\"" + op.text + "\";", oi, false, cur, down);
                break;
            case rm::K::Header:
            case rm::K::Dt:
            case rm::K::Eof:
                break;
            default: {
                std::string text = op.text;
                if (s.g != 1.0 && op.kind == rm::K::Raw && text.compare(0, 2, "CI") == 0) {
                    std::vector<double> v = rm::numbers(text.substr(2));
                    if (!v.empty()) {
                        text = "CI" + rm::fmt(v[0] * s.g);
                        if (v.size() > 1) {
                            text += "," + rm::num(v[1]);
                        }
                    }
                }
                add(text + ";", oi, false, cur, down);
                break;
            }
        }
    }
    bool sp0 = false;
    for (const rm::Op& o : piece.ops) {
        if (o.kind == rm::K::Raw && o.text == "SP0") {
            sp0 = true;
        }
        if (o.kind == rm::K::Eof) {
            s.eof = true;
        }
    }
    if (sp0) {
        add("SP0;", -1, false, cur, down);
    }
    return s;
}

inline std::string streamText(const Stream& s) {
    std::string t;
    for (const Line& l : s.lines) {
        t += l.text;
        t += "\n";
    }
    if (s.calibrated) {
        t += "SC;\n";
    }
    if (s.eof) {
        t += '\x1c';
    }
    return t;
}

struct Plan {
    std::string bytes;               // exactly what is sent to the plotter
    std::vector<size_t> lineEnd;     // per stream line: end offset inside bytes
    std::vector<char> lineIn;        // per stream line: part of this print
};

// Translate the requested progress range into a printable slice of the stream.
inline Plan makePlan(const Stream& s, double fromPct, double toPct) {
    Plan p;
    size_t n = s.lines.size();
    p.lineEnd.assign(n, 0);
    p.lineIn.assign(n, 0);
    size_t total = 0;
    for (const Line& l : s.lines) {
        total += l.text.size() + 1;
    }
    fromPct = std::max(0.0, std::min(100.0, fromPct));
    toPct = std::max(fromPct, std::min(100.0, toPct));
    size_t fromB = static_cast<size_t>(static_cast<double>(total) * fromPct / 100.0);
    size_t toB = static_cast<size_t>(static_cast<double>(total) * toPct / 100.0);
    size_t a = n;
    size_t b = n;
    size_t cum = 0;
    for (size_t j = 0; j < n; ++j) {
        if (a == n && cum >= fromB) {
            a = j;
        }
        if (b == n && cum >= toB) {
            b = j;
        }
        cum += s.lines[j].text.size() + 1;
    }
    if (b < a) {
        b = a;
    }
    if (a >= b) {
        return p;
    }
    if (a > 0) {
        for (size_t j = 0; j < a; ++j) {
            if (!s.lines[j].draw) {
                p.bytes += s.lines[j].text + "\n";
            }
        }
        p.bytes += "PU" + pairOf(s, s.lines[a].cur) + ";\n";
        if (s.lines[a].down) {
            p.bytes += "PD;\n";
        }
    }
    for (size_t j = a; j < b; ++j) {
        p.bytes += s.lines[j].text + "\n";
        p.lineEnd[j] = p.bytes.size();
        p.lineIn[j] = 1;
    }
    if (b < n) {
        p.bytes += "PU;\n";
    }
    if (s.calibrated) {
        p.bytes += "SC;\n";
    }
    if (b == n && s.eof) {
        p.bytes += '\x1c';
    }
    return p;
}

struct DItem {
    int type = 0;                    // 0 polyline, 1 circle, 2 label
    std::vector<rm::Pt> pts;
    std::vector<int> ln;             // stream line of each polyline point
    rm::Pt c;
    double r = 0.0;
    std::string text;
    double dx = 0.0;
    double dy = 0.0;
    double height = 0.0;
    int line = -1;
};

// Associate preview geometry with stream positions for progress display.
inline std::vector<DItem> buildDrawing(const rm::Piece& piece, const Stream& s) {
    std::vector<DItem> items;
    DItem poly;
    auto flush = [&]() {
        if (poly.pts.size() > 1) {
            items.push_back(poly);
        }
        poly.pts.clear();
        poly.ln.clear();
    };
    rm::Pt cur = piece.start;
    bool down = false;
    double dirx = 1.0;
    double diry = 0.0;
    double height = 0.27 * 400;
    for (size_t j = 0; j < piece.ops.size(); ++j) {
        const rm::Op& op = piece.ops[j];
        const std::vector<int>& L = s.opLines[j];
        int first = L.empty() ? -1 : L[0];
        switch (op.kind) {
            case rm::K::Pts:
                if (op.verb == "PU") {
                    down = false;
                    flush();
                } else if (op.verb == "PD") {
                    down = true;
                }
                for (size_t k = 0; k < op.pts.size(); ++k) {
                    const rm::Pt& p = op.pts[k];
                    int ln = L.empty() ? -1 : L[std::min(k / static_cast<size_t>(rm::kChunk), L.size() - 1)];
                    if (down) {
                        if (poly.pts.empty() || !(poly.pts.back() == cur)) {
                            flush();
                            poly.pts.push_back(cur);
                            poly.ln.push_back(ln);
                        }
                        poly.pts.push_back(p);
                        poly.ln.push_back(ln);
                    } else {
                        flush();
                    }
                    cur = p;
                }
                break;
            case rm::K::Arc: {
                flush();
                const rm::Pt& c = op.p;
                double sweep = op.n[0];
                double r = std::hypot(cur.x - c.x, cur.y - c.y);
                double a0 = std::atan2(cur.y - c.y, cur.x - c.x);
                int steps = std::max(2, static_cast<int>(std::fabs(sweep) / 5) + 1);
                DItem it;
                for (int i = 0; i <= steps; ++i) {
                    double a = a0 + sweep * rm::kDeg * i / steps;
                    it.pts.push_back(rm::Pt{c.x + r * std::cos(a), c.y + r * std::sin(a)});
                    it.ln.push_back(first);
                }
                items.push_back(it);
                cur = op.end;
                break;
            }
            case rm::K::Three: {
                flush();
                DItem it;
                it.pts = {cur, op.p, op.q};
                it.ln = {first, first, first};
                items.push_back(it);
                cur = op.q;
                break;
            }
            case rm::K::Rect: {
                flush();
                DItem it;
                it.pts = {op.p, rm::Pt{op.q.x, op.p.y}, op.q, rm::Pt{op.p.x, op.q.y}, op.p};
                it.ln.assign(5, first);
                items.push_back(it);
                break;
            }
            case rm::K::Dir:
                dirx = op.n[0];
                diry = op.n[1];
                break;
            case rm::K::Label: {
                DItem it;
                it.type = 2;
                it.c = cur;
                it.text = op.text;
                it.dx = dirx;
                it.dy = diry;
                it.height = height;
                it.line = first;
                items.push_back(it);
                break;
            }
            case rm::K::Raw:
                if (op.text.compare(0, 2, "SI") == 0) {
                    std::vector<double> v = rm::numbers(op.text.substr(2));
                    if (v.size() >= 2) {
                        height = v[1] * 400;
                    }
                } else if (op.text.compare(0, 2, "CI") == 0) {
                    std::vector<double> v = rm::numbers(op.text.substr(2));
                    if (!v.empty()) {
                        DItem it;
                        it.type = 1;
                        it.c = cur;
                        it.r = v[0];
                        it.line = first;
                        items.push_back(it);
                    }
                }
                break;
            default:
                break;
        }
    }
    flush();
    return items;
}

// Picks the data step (index into {0.025, 0.01, 0.0254} mm) from the "Larg" / "Long" values of the
// header sentence. Returns 0 when there is no header or nothing matches.
inline int guessUnit(const std::string& path) {
    static const double unitMm[3] = {0.025, 0.01, 0.0254};
    try {
        rm::Parsed p = rm::parse(path, 0.025);
        size_t best = 0;
        size_t second = 0;
        int bi = -1;
        for (size_t i = 0; i < p.ops.size(); ++i) {
            if (p.ops[i].kind != rm::K::Label) {
                continue;
            }
            size_t len = p.ops[i].text.size();
            if (len > best) {
                second = best;
                best = len;
                bi = static_cast<int>(i);
            } else if (len > second) {
                second = len;
            }
        }
        if (bi < 0 || best < 40 || best < 2 * second || p.seen.empty()) {
            return 0;
        }
        std::string low = p.ops[static_cast<size_t>(bi)].text;
        for (char& c : low) {
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
        auto after = [&](const char* key) {
            size_t q = low.find(key);
            if (q == std::string::npos) {
                return -1.0;
            }
            q = low.find(':', q);
            if (q == std::string::npos) {
                return -1.0;
            }
            ++q;
            while (q < low.size() && low[q] == ' ') {
                ++q;
            }
            std::string num;
            while (q < low.size() && (std::isdigit(static_cast<unsigned char>(low[q])) || low[q] == ',' || low[q] == '.')) {
                num += low[q] == ',' ? '.' : low[q];
                ++q;
            }
            return num.empty() ? -1.0 : std::atof(num.c_str());
        };
        double larg = after("larg");
        double lng = after("long");
        if (larg <= 0 && lng <= 0) {
            return 0;
        }
        // extents of what is actually drawn (pen down), so label anchors do not skew the size
        bool have = false;
        double minx = 0, maxx = 0, miny = 0, maxy = 0;
        auto include = [&](const rm::Pt& q) {
            if (!have) {
                minx = maxx = q.x;
                miny = maxy = q.y;
                have = true;
                return;
            }
            minx = std::min(minx, q.x);
            maxx = std::max(maxx, q.x);
            miny = std::min(miny, q.y);
            maxy = std::max(maxy, q.y);
        };
        rm::Pt cur{0.0, 0.0};
        bool down = false;
        for (const rm::Op& op : p.ops) {
            if (op.kind == rm::K::Pts) {
                if (op.verb == "PU") {
                    down = false;
                } else if (op.verb == "PD") {
                    down = true;
                }
                for (const rm::Pt& q : op.pts) {
                    if (down) {
                        include(cur);
                        include(q);
                    }
                    cur = q;
                }
            } else if (op.kind == rm::K::Arc) {
                cur = op.end;
            } else if (op.kind == rm::K::Three) {
                include(op.p);
                include(op.q);
                cur = op.q;
            } else if (op.kind == rm::K::Rect) {
                include(op.p);
                include(op.q);
            }
        }
        if (!have) {
            return 0;
        }
        double ex = maxx - minx;
        double ey = maxy - miny;
        double bestErr = 1e9;
        int pick = 0;
        for (int u = 0; u < 3; ++u) {
            for (int o = 0; o < 2; ++o) {
                double wm = (o ? ey : ex) * unitMm[u] / 1000.0;
                double lm = (o ? ex : ey) * unitMm[u] / 1000.0;
                double e = 0;
                if (larg > 0) {
                    e += std::fabs(wm - larg) / larg;
                }
                if (lng > 0) {
                    e += std::fabs(lm - lng) / lng;
                }
                if (e < bestErr) {
                    bestErr = e;
                    pick = u;
                }
            }
        }
        return bestErr > 0.2 ? 0 : pick;
    } catch (const std::exception&) {
        return 0;
    }
}

}  // namespace rp
