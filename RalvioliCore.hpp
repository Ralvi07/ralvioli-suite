// Implements HP-GL parsing, geometry, unit conversion, and output generation shared by the Suite applications.
#pragma once

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <tuple>
#include <string>
#include <utility>
#include <vector>

namespace rm {

constexpr double kUnitsPerCm = 400.0;
constexpr int kChunk = 10;
constexpr double kDeg = 0.017453292519943295;

struct Pt {
    double x = 0.0;
    double y = 0.0;
};

inline bool operator==(const Pt& a, const Pt& b) {
    return a.x == b.x && a.y == b.y;
}

struct Box {
    double x0 = 0.0;
    double y0 = 0.0;
    double x1 = 0.0;
    double y1 = 0.0;
};

struct HpglError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

inline std::vector<std::string>& warnings() {
    static std::vector<std::string> w;
    return w;
}

inline void warn(const std::string& message) {
    warnings().push_back(message);
}

enum class K { Header, Pts, Arc, Three, Rect, Wedge, Label, Dir, Comment, Raw, Dt, Eof };

struct Op {
    K kind;
    std::string verb;
    std::string text;
    std::vector<Pt> pts;
    Pt p;
    Pt q;
    Pt end;
    std::vector<double> n;
    bool hasChord = false;
    double chord = 0.0;
    bool flag = false;
    explicit Op(K k) : kind(k) {}
};

struct Piece {
    std::vector<Op> ops;
    Pt start;
};

struct Parsed {
    std::vector<Op> ops;
    std::vector<Pt> seen;
};

struct Token {
    std::string mn;
    std::string arg;
    size_t start;
};

struct Req {
    std::string path;
    double deg = 0.0;
    double unit = 0.025;
    bool noHeader = false;
    std::optional<Pt> pos;
};

struct Item {
    int type = 0;
    std::vector<Pt> pts;
    Pt c;
    double r = 0.0;
    std::string text;
    double dx = 0.0;
    double dy = 0.0;
    double height = 0.0;
};

struct Cache {
    std::map<std::pair<std::string, double>, Parsed> parsed;
    std::map<std::tuple<std::string, double, double, bool>, Piece> rotated;
};

inline bool isSpaceChar(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f' ||
           (c >= 0x1c && c <= 0x1f);
}

inline bool isBlank(const std::string& s) {
    for (char c : s) {
        if (!isSpaceChar(c)) {
            return false;
        }
    }
    return true;
}

inline std::string stripTrailing(std::string s) {
    while (!s.empty() && isSpaceChar(s.back())) {
        s.pop_back();
    }
    return s;
}

inline bool isLetter(char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
}

inline std::vector<double> numbers(const std::string& s) {
    std::vector<double> out;
    size_t n = s.size();
    auto digit = [&](size_t k) { return k < n && s[k] >= '0' && s[k] <= '9'; };
    size_t i = 0;
    while (i < n) {
        size_t j = i;
        if (s[j] == '-' || s[j] == '+') {
            ++j;
        }
        bool ok = false;
        if (digit(j)) {
            while (digit(j)) {
                ++j;
            }
            if (j < n && s[j] == '.') {
                ++j;
            }
            while (digit(j)) {
                ++j;
            }
            ok = true;
        } else if (j < n && s[j] == '.' && digit(j + 1)) {
            ++j;
            while (digit(j)) {
                ++j;
            }
            ok = true;
        }
        if (ok) {
            out.push_back(std::strtod(s.substr(i, j - i).c_str(), nullptr));
            i = j;
        } else {
            ++i;
        }
    }
    return out;
}

inline std::string fmt(double v) {
    return std::to_string(static_cast<long long>(std::rint(v)));
}

inline std::string num(double v) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.4f", v);
    std::string t = buf;
    while (!t.empty() && t.back() == '0') {
        t.pop_back();
    }
    if (!t.empty() && t.back() == '.') {
        t.pop_back();
    }
    return (t.empty() || t == "-0") ? "0" : t;
}

inline std::string readFile(const std::string& path) {
    std::ifstream f(std::filesystem::u8path(path), std::ios::binary);
    if (!f) {
        throw HpglError("Cannot read " + path);
    }
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

inline std::string stripEsc(const std::string& raw) {
    std::string out;
    size_t n = raw.size();
    size_t i = 0;
    while (i < n) {
        if (raw[i] == 0x1b && i + 1 < n) {
            if (raw[i + 1] == '%') {
                size_t j = i + 2;
                if (j < n && raw[j] == '-') {
                    ++j;
                }
                size_t d = j;
                while (j < n && raw[j] >= '0' && raw[j] <= '9') {
                    ++j;
                }
                if (j > d && j < n && raw[j] >= 'A' && raw[j] <= 'Z') {
                    i = j + 1;
                    continue;
                }
            } else if (raw[i + 1] >= 'A' && raw[i + 1] <= 'Z') {
                i += 2;
                continue;
            }
        }
        out.push_back(raw[i]);
        ++i;
    }
    return out;
}

// Split the HP-GL stream into commands before interpreting its geometry.
inline std::vector<Token> scan(const std::string& data) {
    std::vector<Token> out;
    size_t n = data.size();
    size_t i = 0;
    char term = 3;
    while (i < n) {
        char c = data[i];
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == ';') {
            ++i;
            continue;
        }
        size_t start = i;
        if (c == 0x1c) {
            out.push_back({"FS", "", start});
            ++i;
            continue;
        }
        if (i + 2 > n) {
            ++i;
            continue;
        }
        if (!isLetter(data[i]) || !isLetter(data[i + 1])) {
            ++i;
            continue;
        }
        std::string mn = data.substr(i, 2);
        mn[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(mn[0])));
        mn[1] = static_cast<char>(std::toupper(static_cast<unsigned char>(mn[1])));
        i += 2;
        if (mn == "CO" && i < n && data[i] == '"') {
            size_t j = data.find('"', i + 1);
            if (j == std::string::npos) {
                j = n;
            }
            out.push_back({mn, data.substr(i + 1, j - i - 1), start});
            i = j + 1;
            continue;
        }
        if (mn == "DT") {
            if (i < n && data[i] != ';') {
                term = data[i];
                ++i;
            } else {
                term = 3;
            }
            out.push_back({"DT", "", start});
            continue;
        }
        if (mn == "LB") {
            size_t j = data.find(term, i);
            if (j == std::string::npos) {
                j = n;
            }
            out.push_back({mn, data.substr(i, j - i), start});
            i = j + 1;
            continue;
        }
        size_t j = i;
        while (j < n && data[j] != ';' && data[j] != '\r' && data[j] != '\n' && !isLetter(data[j])) {
            ++j;
        }
        out.push_back({mn, data.substr(i, j - i), start});
        i = j;
    }
    return out;
}

// Interpret commands using the selected input step size and collect the drawing operations.
inline Parsed parse(const std::string& path, double unitMm = 0.025) {
    const double f = unitMm / 0.025;
    std::string raw = stripEsc(readFile(path));
    std::vector<Token> tokens = scan(raw);

    size_t headerEnd = raw.size();
    bool hasIn = false;
    for (const Token& t : tokens) {
        if (t.mn == "IN") {
            hasIn = true;
        } else if (t.mn == "CO" || t.mn == "DT" || t.mn == "LM" ||
                   (t.mn == "PA" && numbers(t.arg).empty())) {
            continue;
        } else {
            headerEnd = t.start;
            break;
        }
    }

    Parsed out;
    std::string head = raw.substr(0, headerEnd);
    if (!isBlank(head)) {
        Op h(K::Header);
        h.text = stripTrailing(head);
        h.flag = hasIn;
        out.ops.push_back(h);
    }

    Pt cur{0.0, 0.0};
    bool rel = false;
    static const std::set<std::string> dropped = {"IN", "DF", "PG", "NR", "BP", "IP", "IW"};
    for (const Token& t : tokens) {
        if (t.start < headerEnd) {
            continue;
        }
        const std::string& mn = t.mn;
        const std::string& arg = t.arg;
        if (mn == "PE" || mn == "SC" || mn == "UC") {
            throw HpglError(path + ": command " + mn + " is not supported");
        }
        if (dropped.count(mn)) {
            if (mn == "IP" || mn == "IW") {
                warn(path + ": " + mn + " dropped");
            }
            continue;
        }
        if (mn == "PA" || mn == "PR" || mn == "PU" || mn == "PD") {
            if (mn == "PA") {
                rel = false;
            } else if (mn == "PR") {
                rel = true;
            }
            std::vector<double> nums = numbers(arg);
            if (nums.size() % 2 != 0) {
                throw HpglError(path + ": odd number of coordinates in " + mn + arg);
            }
            std::vector<Pt> pts;
            for (size_t k = 0; k < nums.size(); k += 2) {
                double x = nums[k] * f;
                double y = nums[k + 1] * f;
                if (rel) {
                    x += cur.x;
                    y += cur.y;
                }
                cur = Pt{x, y};
                pts.push_back(cur);
            }
            out.seen.insert(out.seen.end(), pts.begin(), pts.end());
            std::string verb = (mn == "PA" || mn == "PR") ? "PA" : mn;
            if (!pts.empty() || verb != "PA") {
                Op o(K::Pts);
                o.verb = verb;
                o.pts = pts;
                out.ops.push_back(o);
            }
        } else if (mn == "AA" || mn == "AR") {
            std::vector<double> nums = numbers(arg);
            if (nums.size() < 3) {
                throw HpglError(path + ": bad arc " + mn + arg);
            }
            double cx = nums[0] * f;
            double cy = nums[1] * f;
            double sweep = nums[2];
            if (mn == "AR") {
                cx += cur.x;
                cy += cur.y;
            }
            double a = sweep * kDeg;
            double dx = cur.x - cx;
            double dy = cur.y - cy;
            cur = Pt{cx + dx * std::cos(a) - dy * std::sin(a), cy + dx * std::sin(a) + dy * std::cos(a)};
            out.seen.push_back(cur);
            Op o(K::Arc);
            o.p = Pt{cx, cy};
            o.n = {sweep};
            if (nums.size() > 3) {
                o.hasChord = true;
                o.chord = nums[3];
            }
            o.end = cur;
            out.ops.push_back(o);
        } else if (mn == "RA" || mn == "RR" || mn == "EA" || mn == "ER") {
            std::vector<double> nums = numbers(arg);
            if (nums.size() < 2) {
                throw HpglError(path + ": bad rectangle " + mn + arg);
            }
            Pt q{nums[0] * f, nums[1] * f};
            if (mn[1] == 'R') {
                q = Pt{q.x + cur.x, q.y + cur.y};
            }
            Op o(K::Rect);
            o.verb = std::string(1, mn[0]) + "A";
            o.p = cur;
            o.q = q;
            out.ops.push_back(o);
            out.seen.push_back(cur);
            out.seen.push_back(q);
        } else if (mn == "AT" || mn == "RT") {
            std::vector<double> nums = numbers(arg);
            if (nums.size() < 4) {
                throw HpglError(path + ": bad arc " + mn + arg);
            }
            Pt p1{nums[0] * f, nums[1] * f};
            Pt p2{nums[2] * f, nums[3] * f};
            if (mn == "RT") {
                p1 = Pt{p1.x + cur.x, p1.y + cur.y};
                p2 = Pt{p2.x + cur.x, p2.y + cur.y};
            }
            cur = p2;
            out.seen.push_back(p1);
            out.seen.push_back(p2);
            Op o(K::Three);
            o.p = p1;
            o.q = p2;
            if (nums.size() > 4) {
                o.hasChord = true;
                o.chord = nums[4];
            }
            out.ops.push_back(o);
        } else if (mn == "EW" || mn == "WG") {
            std::vector<double> nums = numbers(arg);
            if (nums.size() < 3) {
                throw HpglError(path + ": bad wedge " + mn + arg);
            }
            Op o(K::Wedge);
            o.verb = mn;
            o.n = nums;
            o.n[0] *= f;
            out.ops.push_back(o);
        } else if (mn == "CO") {
            Op o(K::Comment);
            o.text = arg;
            out.ops.push_back(o);
        } else if (mn == "DT") {
            out.ops.push_back(Op(K::Dt));
        } else if (mn == "FS") {
            out.ops.push_back(Op(K::Eof));
        } else if (mn == "DI") {
            std::vector<double> nums = numbers(arg);
            Op o(K::Dir);
            if (nums.size() >= 2) {
                o.n = {nums[0], nums[1]};
            } else {
                o.n = {1000.0, 0.0};
            }
            out.ops.push_back(o);
        } else if (mn == "LB") {
            Op o(K::Label);
            o.text = arg;
            out.ops.push_back(o);
        } else {
            Op o(K::Raw);
            o.text = mn + arg;
            if (mn == "CI" && f != 1.0) {
                std::vector<double> nums = numbers(arg);
                if (!nums.empty()) {
                    o.text = "CI" + num(nums[0] * f);
                    if (nums.size() > 1) {
                        o.text += "," + num(nums[1]);
                    }
                }
            }
            out.ops.push_back(o);
        }
    }
    return out;
}

inline Pt rot(const Pt& p, const Pt& pivot, double c, double s) {
    double x = p.x - pivot.x;
    double y = p.y - pivot.y;
    return Pt{pivot.x + x * c - y * s, pivot.y + x * s + y * c};
}

inline double mod360(double deg) {
    return std::fmod(std::fmod(deg, 360.0) + 360.0, 360.0);
}

// Find and remove the printed file-information label from the operation list.
inline bool removeHeaderText(std::vector<Op>& ops) {
    size_t best = 0;
    size_t second = 0;
    int bestIndex = -1;
    for (size_t i = 0; i < ops.size(); ++i) {
        if (ops[i].kind != K::Label) {
            continue;
        }
        size_t len = ops[i].text.size();
        if (len > best) {
            second = best;
            best = len;
            bestIndex = static_cast<int>(i);
        } else if (len > second) {
            second = len;
        }
    }
    if (bestIndex < 0 || best < 40 || best < 2 * second) {
        return false;
    }
    ops.erase(ops.begin() + bestIndex);
    return true;
}

inline Piece rotateLayer(const std::vector<Op>& ops, const Pt& pivot, double deg) {
    double dm = mod360(deg);
    double th = dm != 0.0 ? deg * kDeg : 0.0;
    double c = std::cos(th);
    double s = std::sin(th);
    Piece out;
    bool warned = false;
    bool hasLabel = false;
    for (const Op& op : ops) {
        Op o = op;
        switch (op.kind) {
            case K::Pts:
                for (Pt& p : o.pts) {
                    p = rot(p, pivot, c, s);
                }
                break;
            case K::Arc:
                o.p = rot(op.p, pivot, c, s);
                o.end = rot(op.end, pivot, c, s);
                break;
            case K::Dir:
                o.n = {op.n[0] * c - op.n[1] * s, op.n[0] * s + op.n[1] * c};
                break;
            case K::Three:
                o.p = rot(op.p, pivot, c, s);
                o.q = rot(op.q, pivot, c, s);
                break;
            case K::Wedge:
                o.n[1] += deg;
                break;
            case K::Label:
                hasLabel = true;
                break;
            case K::Rect:
                if (std::fmod(dm, 90.0) == 0.0) {
                    o.p = rot(op.p, pivot, c, s);
                    o.q = rot(op.q, pivot, c, s);
                } else {
                    if (!warned) {
                        warn("rectangles in a layer turned by a non-90 angle are drawn as outlines");
                        warned = true;
                    }
                    Op pd(K::Pts);
                    pd.verb = "PD";
                    pd.pts.push_back(rot(op.p, pivot, c, s));
                    pd.pts.push_back(rot(Pt{op.q.x, op.p.y}, pivot, c, s));
                    pd.pts.push_back(rot(op.q, pivot, c, s));
                    pd.pts.push_back(rot(Pt{op.p.x, op.q.y}, pivot, c, s));
                    pd.pts.push_back(rot(op.p, pivot, c, s));
                    out.ops.push_back(pd);
                    Op pu(K::Pts);
                    pu.verb = "PU";
                    out.ops.push_back(pu);
                    continue;
                }
                break;
            default:
                break;
        }
        out.ops.push_back(o);
    }
    if (hasLabel) {
        Op d(K::Dir);
        d.n = {1000.0 * c, 1000.0 * s};
        out.ops.insert(out.ops.begin(), d);
    }
    out.start = rot(Pt{0.0, 0.0}, pivot, c, s);
    return out;
}

inline std::vector<Pt> layerPoints(const std::vector<Op>& ops) {
    std::vector<Pt> pts;
    for (const Op& op : ops) {
        switch (op.kind) {
            case K::Pts:
                pts.insert(pts.end(), op.pts.begin(), op.pts.end());
                break;
            case K::Arc:
                pts.push_back(op.end);
                break;
            case K::Rect:
            case K::Three:
                pts.push_back(op.p);
                pts.push_back(op.q);
                break;
            default:
                break;
        }
    }
    return pts;
}

inline std::optional<Box> boxOf(const std::vector<Op>& ops) {
    std::vector<Pt> pts = layerPoints(ops);
    if (pts.empty()) {
        return std::nullopt;
    }
    Box b{pts[0].x, pts[0].y, pts[0].x, pts[0].y};
    for (const Pt& p : pts) {
        b.x0 = std::min(b.x0, p.x);
        b.y0 = std::min(b.y0, p.y);
        b.x1 = std::max(b.x1, p.x);
        b.y1 = std::max(b.y1, p.y);
    }
    return b;
}

inline Piece moveLayer(const std::vector<Op>& ops, const Pt& start, double dx, double dy) {
    Piece out;
    for (const Op& op : ops) {
        Op o = op;
        switch (op.kind) {
            case K::Pts:
                for (Pt& p : o.pts) {
                    p = Pt{p.x + dx, p.y + dy};
                }
                break;
            case K::Arc:
                o.p = Pt{op.p.x + dx, op.p.y + dy};
                o.end = Pt{op.end.x + dx, op.end.y + dy};
                break;
            case K::Three:
            case K::Rect:
                o.p = Pt{op.p.x + dx, op.p.y + dy};
                o.q = Pt{op.q.x + dx, op.q.y + dy};
                break;
            default:
                break;
        }
        out.ops.push_back(o);
    }
    out.start = Pt{start.x + dx, start.y + dy};
    return out;
}

inline const Piece& loadRotated(Cache& cache, const std::string& path, double deg, double unitMm = 0.025,
                                bool noHeader = false) {
    auto key = std::make_tuple(path, mod360(deg), unitMm, noHeader);
    auto it = cache.rotated.find(key);
    if (it != cache.rotated.end()) {
        return it->second;
    }
    auto parsedKey = std::make_pair(path, unitMm);
    auto pit = cache.parsed.find(parsedKey);
    if (pit == cache.parsed.end()) {
        pit = cache.parsed.emplace(parsedKey, parse(path, unitMm)).first;
    }
    const Parsed& parsed = pit->second;
    Pt pivot{0.0, 0.0};
    if (!parsed.seen.empty()) {
        double minx = parsed.seen[0].x;
        double miny = parsed.seen[0].y;
        double maxx = minx;
        double maxy = miny;
        for (const Pt& p : parsed.seen) {
            minx = std::min(minx, p.x);
            miny = std::min(miny, p.y);
            maxx = std::max(maxx, p.x);
            maxy = std::max(maxy, p.y);
        }
        pivot = Pt{(minx + maxx) / 2, (miny + maxy) / 2};
    }
    std::vector<Op> ops = parsed.ops;
    if (noHeader) {
        removeHeaderText(ops);
    }
    return cache.rotated.emplace(key, rotateLayer(ops, pivot, deg)).first->second;
}

inline std::vector<Piece> placeLayers(Cache& cache, const std::vector<Req>& items,
                                      std::optional<double> width) {
    std::vector<Piece> placed;
    bool haveRight = false;
    bool haveTop = false;
    bool haveColRight = false;
    double colX = 0.0;
    double baseY = 0.0;
    double top = 0.0;
    double right = 0.0;
    double colRight = 0.0;
    for (const Req& r : items) {
        const Piece& piece = loadRotated(cache, r.path, r.deg, r.unit, r.noHeader);
        std::optional<Box> box = boxOf(piece.ops);
        if (!box) {
            placed.push_back(piece);
            continue;
        }
        double h = box->y1 - box->y0;
        double dx = 0.0;
        double dy = 0.0;
        if (r.pos) {
            dx = r.pos->x - box->x0;
            dy = r.pos->y - box->y0;
        } else if (!haveRight) {
            dx = std::max(0.0, -box->x0);
            dy = std::max(0.0, -box->y0);
        } else {
            if (width && *width > 0 && haveTop && (top - baseY) + h > *width + 0.5 &&
                h <= *width + 0.5) {
                colX = colRight;
                haveTop = false;
                haveColRight = false;
            }
            dx = colX - box->x0;
            dy = (haveTop ? top : baseY) - box->y0;
        }
        placed.push_back(moveLayer(piece.ops, piece.start, dx, dy));
        double x0 = box->x0 + dx;
        double y0 = box->y0 + dy;
        double x1 = box->x1 + dx;
        double y1 = box->y1 + dy;
        if (!haveRight) {
            colX = x0;
            baseY = y0;
        }
        right = haveRight ? std::max(right, x1) : x1;
        haveRight = true;
        colRight = haveColRight ? std::max(colRight, x1) : x1;
        haveColRight = true;
        top = haveTop ? std::max(top, y1) : y1;
        haveTop = true;
    }
    return placed;
}

inline std::string pairText(const Pt& p, double dx, double dy, double g = 1.0) {
    return fmt((p.x + dx) * g) + "," + fmt((p.y + dy) * g);
}

// Serialize the layers using the requested output step size and origin policy.
inline std::string merge(const std::vector<Piece>& layers, bool toOrigin, double outUnit = 0.025) {
    std::vector<Pt> all;
    for (const Piece& piece : layers) {
        std::vector<Pt> pts = layerPoints(piece.ops);
        all.insert(all.end(), pts.begin(), pts.end());
    }
    if (all.empty()) {
        throw HpglError("no drawing commands found in the input files");
    }
    double minx = all[0].x;
    double miny = all[0].y;
    for (const Pt& p : all) {
        minx = std::min(minx, p.x);
        miny = std::min(miny, p.y);
    }
    const double dx = (toOrigin || minx < 0) ? -minx : 0.0;
    const double dy = (toOrigin || miny < 0) ? -miny : 0.0;
    const double g = 0.025 / outUnit;

    std::vector<std::string> out;
    const Op* head = nullptr;
    for (const Op& op : layers[0].ops) {
        if (op.kind == K::Header) {
            head = &op;
            break;
        }
    }
    if (head == nullptr || !head->flag) {
        out.push_back("IN;");
    }
    if (head != nullptr) {
        out.push_back(head->text);
    }

    for (size_t index = 0; index < layers.size(); ++index) {
        const Piece& piece = layers[index];
        const std::vector<Op>& ops = piece.ops;
        const Op* first = nullptr;
        for (const Op& o : ops) {
            bool moves = (o.kind == K::Pts && !o.pts.empty()) || o.kind == K::Arc ||
                         o.kind == K::Three || o.kind == K::Rect || o.kind == K::Wedge ||
                         o.kind == K::Label;
            if (moves) {
                first = &o;
                break;
            }
        }
        bool needsStart = !(first != nullptr && first->kind == K::Pts && first->verb == "PU");
        if (index == 0 && pairText(piece.start, dx, dy, g) == "0,0") {
            needsStart = false;
        }
        if (needsStart) {
            out.push_back("PU" + pairText(piece.start, dx, dy, g) + ";");
        }
        for (size_t j = 0; j < ops.size(); ++j) {
            const Op& op = ops[j];
            switch (op.kind) {
                case K::Pts: {
                    if (op.pts.empty()) {
                        out.push_back(op.verb + ";");
                    }
                    for (size_t k = 0; k < op.pts.size(); k += kChunk) {
                        std::string line = op.verb;
                        size_t stop = std::min(k + static_cast<size_t>(kChunk), op.pts.size());
                        for (size_t m = k; m < stop; ++m) {
                            if (m > k) {
                                line += ",";
                            }
                            line += pairText(op.pts[m], dx, dy, g);
                        }
                        out.push_back(line + ";");
                    }
                    break;
                }
                case K::Arc: {
                    std::string text = "AA" + pairText(op.p, dx, dy, g) + "," + num(op.n[0]);
                    if (op.hasChord) {
                        text += "," + num(op.chord);
                    }
                    out.push_back(text + ";");
                    break;
                }
                case K::Three: {
                    std::string text = "AT" + pairText(op.p, dx, dy, g) + "," + pairText(op.q, dx, dy, g);
                    if (op.hasChord) {
                        text += "," + num(op.chord);
                    }
                    out.push_back(text + ";");
                    break;
                }
                case K::Rect:
                    out.push_back(op.verb + pairText(op.q, dx, dy, g) + ";");
                    break;
                case K::Wedge: {
                    std::string line = op.verb;
                    for (size_t m = 0; m < op.n.size(); ++m) {
                        if (m > 0) {
                            line += ",";
                        }
                        line += num(m == 0 ? op.n[m] * g : op.n[m]);
                    }
                    out.push_back(line + ";");
                    break;
                }
                case K::Label:
                    out.push_back("LB" + op.text + std::string(1, '\x03') + ";");
                    break;
                case K::Dir: {
                    bool plain = index == 0 && j == 0 && std::fabs(op.n[0] - 1000) < 1e-6 &&
                                 std::fabs(op.n[1]) < 1e-6;
                    if (!plain) {
                        out.push_back("DI" + fmt(op.n[0]) + "," + fmt(op.n[1]) + ";");
                    }
                    break;
                }
                case K::Comment:
                    if (index == 0) {
                        out.push_back("CO\"" + op.text + "\";");
                    }
                    break;
                case K::Header:
                case K::Dt:
                case K::Eof:
                    break;
                default: {
                    std::string text = op.text;
                    if (g != 1.0 && op.kind == K::Raw && text.compare(0, 2, "CI") == 0) {
                        std::vector<double> v = numbers(text.substr(2));
                        if (!v.empty()) {
                            text = "CI" + fmt(v[0] * g);
                            if (v.size() > 1) {
                                text += "," + num(v[1]);
                            }
                        }
                    }
                    out.push_back(text + ";");
                    break;
                }
            }
        }
    }

    bool sp0 = false;
    bool eof = false;
    for (const Piece& piece : layers) {
        for (const Op& o : piece.ops) {
            if (o.kind == K::Raw && o.text == "SP0") {
                sp0 = true;
            }
            if (o.kind == K::Eof) {
                eof = true;
            }
        }
    }
    if (sp0) {
        out.push_back("SP0;");
    }
    std::string text;
    for (size_t i = 0; i < out.size(); ++i) {
        if (i > 0) {
            text += "\n";
        }
        text += out[i];
    }
    text += "\n";
    if (eof) {
        text += '\x1c';
    }
    return text;
}

inline Piece rotatePiece(const Piece& p, double deg) {
    std::optional<Box> before = boxOf(p.ops);
    Pt pivot{0.0, 0.0};
    if (before) {
        pivot = Pt{(before->x0 + before->x1) / 2, (before->y0 + before->y1) / 2};
    }
    Piece r = rotateLayer(p.ops, pivot, deg);
    double th = mod360(deg) != 0.0 ? deg * kDeg : 0.0;
    r.start = rot(p.start, pivot, std::cos(th), std::sin(th));
    if (r.ops.size() > 1 && r.ops[0].kind == K::Dir && r.ops[1].kind == K::Dir) {
        r.ops.erase(r.ops.begin());
    }
    std::optional<Box> after = boxOf(r.ops);
    if (before && after) {
        return moveLayer(r.ops, r.start, before->x0 - after->x0, before->y0 - after->y0);
    }
    return r;
}

inline bool clipSegment(const Pt& a, const Pt& b, const Box& r, Pt& ca, Pt& cb) {
    double dx = b.x - a.x;
    double dy = b.y - a.y;
    double t0 = 0.0;
    double t1 = 1.0;
    auto test = [&](double p, double q) {
        if (p == 0.0) {
            return q >= 0.0;
        }
        double t = q / p;
        if (p < 0.0) {
            if (t > t1) {
                return false;
            }
            if (t > t0) {
                t0 = t;
            }
        } else {
            if (t < t0) {
                return false;
            }
            if (t < t1) {
                t1 = t;
            }
        }
        return true;
    };
    if (test(-dx, a.x - r.x0) && test(dx, r.x1 - a.x) && test(-dy, a.y - r.y0) && test(dy, r.y1 - a.y)) {
        ca = t0 == 0.0 ? a : Pt{a.x + t0 * dx, a.y + t0 * dy};
        cb = t1 == 1.0 ? b : Pt{a.x + t1 * dx, a.y + t1 * dy};
        return true;
    }
    return false;
}

// Clip the drawing operations to the requested crop rectangle.
inline Piece cropPiece(const Piece& in, const Box& r) {
    Piece out;
    Pt cur = in.start;
    bool down = false;
    const Pt unknown{1e300, 1e300};
    Pt ePos = unknown;
    bool eDown = false;
    auto emitMove = [&](const Pt& p) {
        Op o(K::Pts);
        o.verb = "PU";
        o.pts.push_back(p);
        out.ops.push_back(o);
        ePos = p;
        eDown = false;
    };
    auto emitDraw = [&](const Pt& p) {
        if (!out.ops.empty() && out.ops.back().kind == K::Pts && out.ops.back().verb == "PD") {
            out.ops.back().pts.push_back(p);
        } else {
            Op o(K::Pts);
            o.verb = "PD";
            o.pts.push_back(p);
            out.ops.push_back(o);
        }
        ePos = p;
        eDown = true;
    };
    auto segment = [&](const Pt& a, const Pt& b) {
        Pt ca;
        Pt cb;
        if (!clipSegment(a, b, r, ca, cb) || ca == cb) {
            return;
        }
        if (!(eDown && ePos == ca)) {
            emitMove(ca);
        }
        emitDraw(cb);
    };
    auto polyline = [&](const std::vector<Pt>& pts) {
        for (size_t i = 1; i < pts.size(); ++i) {
            segment(pts[i - 1], pts[i]);
        }
    };
    auto inside = [&](const Pt& p) { return p.x >= r.x0 && p.x <= r.x1 && p.y >= r.y0 && p.y <= r.y1; };

    for (const Op& op : in.ops) {
        switch (op.kind) {
            case K::Pts:
                if (op.verb == "PU") {
                    down = false;
                } else if (op.verb == "PD") {
                    down = true;
                }
                for (const Pt& p : op.pts) {
                    if (down) {
                        segment(cur, p);
                    }
                    cur = p;
                }
                break;
            case K::Arc: {
                double rad = std::hypot(cur.x - op.p.x, cur.y - op.p.y);
                double a0 = std::atan2(cur.y - op.p.y, cur.x - op.p.x);
                int steps = std::max(2, static_cast<int>(std::fabs(op.n[0]) / 5) + 1);
                std::vector<Pt> arc;
                for (int i = 0; i <= steps; ++i) {
                    double a = a0 + op.n[0] * kDeg * i / steps;
                    arc.push_back(Pt{op.p.x + rad * std::cos(a), op.p.y + rad * std::sin(a)});
                }
                polyline(arc);
                cur = op.end;
                break;
            }
            case K::Three:
                polyline({cur, op.p, op.q});
                cur = op.q;
                break;
            case K::Rect:
                polyline({op.p, Pt{op.q.x, op.p.y}, op.q, Pt{op.p.x, op.q.y}, op.p});
                break;
            case K::Wedge: {
                double a0 = op.n[1] * kDeg;
                int steps = std::max(2, static_cast<int>(std::fabs(op.n[2]) / 5) + 1);
                std::vector<Pt> w;
                w.push_back(cur);
                for (int i = 0; i <= steps; ++i) {
                    double a = a0 + op.n[2] * kDeg * i / steps;
                    w.push_back(Pt{cur.x + op.n[0] * std::cos(a), cur.y + op.n[0] * std::sin(a)});
                }
                w.push_back(cur);
                polyline(w);
                break;
            }
            case K::Label:
                if (inside(cur)) {
                    emitMove(cur);
                    out.ops.push_back(op);
                    ePos = unknown;
                    eDown = false;
                }
                break;
            case K::Raw:
                if (op.text.compare(0, 2, "CI") == 0) {
                    std::vector<double> v = numbers(op.text.substr(2));
                    if (!v.empty()) {
                        std::vector<Pt> circle;
                        for (int i = 0; i <= 72; ++i) {
                            double a = 2.0 * 3.14159265358979323846 * i / 72;
                            circle.push_back(Pt{cur.x + v[0] * std::cos(a), cur.y + v[0] * std::sin(a)});
                        }
                        polyline(circle);
                    }
                } else {
                    out.ops.push_back(op);
                }
                break;
            default:
                out.ops.push_back(op);
                break;
        }
    }
    return moveLayer(out.ops, Pt{in.start.x, in.start.y}, -r.x0, -r.y0);
}

inline void flushPoly(std::vector<Item>& items, std::vector<Pt>& poly) {
    if (poly.size() > 1) {
        Item it;
        it.type = 0;
        it.pts = poly;
        items.push_back(it);
    }
    poly.clear();
}

inline std::vector<Item> renderLayer(const Piece& piece) {
    std::vector<Item> items;
    std::vector<Pt> poly;
    Pt cur = piece.start;
    bool down = false;
    double dirx = 1.0;
    double diry = 0.0;
    double height = 0.27 * 400;
    for (const Op& op : piece.ops) {
        switch (op.kind) {
            case K::Pts:
                if (op.verb == "PU") {
                    down = false;
                    flushPoly(items, poly);
                } else if (op.verb == "PD") {
                    down = true;
                }
                for (const Pt& p : op.pts) {
                    if (down) {
                        if (poly.empty() || !(poly.back() == cur)) {
                            flushPoly(items, poly);
                            poly.push_back(cur);
                        }
                        poly.push_back(p);
                    } else {
                        flushPoly(items, poly);
                    }
                    cur = p;
                }
                break;
            case K::Arc: {
                flushPoly(items, poly);
                const Pt& c = op.p;
                double sweep = op.n[0];
                double r = std::hypot(cur.x - c.x, cur.y - c.y);
                double a0 = std::atan2(cur.y - c.y, cur.x - c.x);
                int steps = std::max(2, static_cast<int>(std::fabs(sweep) / 5) + 1);
                Item it;
                it.type = 0;
                for (int i = 0; i <= steps; ++i) {
                    double a = a0 + sweep * kDeg * i / steps;
                    it.pts.push_back(Pt{c.x + r * std::cos(a), c.y + r * std::sin(a)});
                }
                items.push_back(it);
                cur = op.end;
                break;
            }
            case K::Three: {
                flushPoly(items, poly);
                Item it;
                it.type = 0;
                it.pts = {cur, op.p, op.q};
                items.push_back(it);
                cur = op.q;
                break;
            }
            case K::Rect: {
                flushPoly(items, poly);
                Item it;
                it.type = 0;
                it.pts = {op.p, Pt{op.q.x, op.p.y}, op.q, Pt{op.p.x, op.q.y}, op.p};
                items.push_back(it);
                break;
            }
            case K::Dir:
                dirx = op.n[0];
                diry = op.n[1];
                break;
            case K::Label: {
                Item it;
                it.type = 2;
                it.c = cur;
                it.text = op.text;
                it.dx = dirx;
                it.dy = diry;
                it.height = height;
                items.push_back(it);
                break;
            }
            case K::Raw:
                if (op.text.compare(0, 2, "SI") == 0) {
                    std::vector<double> v = numbers(op.text.substr(2));
                    if (v.size() >= 2) {
                        height = v[1] * 400;
                    }
                } else if (op.text.compare(0, 2, "CI") == 0) {
                    std::vector<double> v = numbers(op.text.substr(2));
                    if (!v.empty()) {
                        Item it;
                        it.type = 1;
                        it.c = cur;
                        it.r = v[0];
                        items.push_back(it);
                    }
                }
                break;
            default:
                break;
        }
    }
    flushPoly(items, poly);
    return items;
}

}
