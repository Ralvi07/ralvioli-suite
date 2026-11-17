#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <commdlg.h>
#include <stdio.h>

#include <algorithm>
#include <cmath>
#include <cwchar>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "RalvioliCore.hpp"

namespace {

enum {
    ID_ADD = 101,
    ID_REMOVE,
    ID_LEFT,
    ID_RIGHT,
    ID_WIDTH,
    ID_ZOOMOUT,
    ID_ZOOMIN,
    ID_FIT,
    ID_SAVE,
    ID_LIST,
    ID_STATUS,
    ID_CANCEL,
    ID_UNITS_OUT
};

const COLORREF kColors[] = {RGB(0x15, 0x65, 0xc0), RGB(0xc6, 0x28, 0x28), RGB(0x2e, 0x7d, 0x32),
                            RGB(0xef, 0x6c, 0x00), RGB(0x6a, 0x1b, 0x9a), RGB(0x00, 0x83, 0x8f)};
const int kColorCount = 6;
const double kUnitMm[3] = {0.025, 0.01, 0.0254};
const wchar_t* const kUnitText[3] = {L"0.025 mm (HP-GL/2)", L"0.01 mm", L"0.0254 mm"};
const wchar_t* const kUnitShort[3] = {L"0.025 mm", L"0.01 mm", L"0.0254 mm"};
const LPCWSTR kCursorArrow = MAKEINTRESOURCEW(32512);
const LPCWSTR kCursorMove = MAKEINTRESOURCEW(32646);

struct Layer {
    std::string path;
    double deg = 0.0;
    int unit = 0;
    bool noHeader = false;
    std::optional<rm::Pt> pos;
};

struct Slot {
    HWND hwnd;
    int width;
    int height;
    int gapAfter;
    bool glue;
    int extra;
};

struct State {
    HINSTANCE inst = nullptr;
    HWND hwnd = nullptr;
    HWND list = nullptr;
    HWND preview = nullptr;
    HWND edit = nullptr;
    HWND status = nullptr;
    HWND cbOut = nullptr;
    int outIdx = 0;
    HFONT font = nullptr;
    HFONT bold = nullptr;
    int dpi = 96;
    std::vector<Slot> slots;
    int toolbarWidth = 0;
    std::vector<Layer> layers;
    rm::Cache cache;
    std::vector<std::vector<rm::Item>> shapes;
    std::vector<std::optional<rm::Box>> boxes;
    std::string message;
    std::wstring statusText;
    int holding = -1;
    double grabX = 0.0;
    double grabY = 0.0;
    double holdOffX = 0.0;
    double holdOffY = 0.0;
    std::vector<double> snapDeg;
    std::vector<std::optional<rm::Pt>> snapPos;
    bool autoFit = true;
    double vx = 0.0;
    double vy = 0.0;
    double vs = 1.0;
    int cw = 0;
    int ch = 0;
    bool panning = false;
    POINT panLast{0, 0};
    std::wstring lastDir;
};

State S;

int dp(int v) {
    return MulDiv(v, S.dpi, 96);
}

std::wstring wide(const std::string& s) {
    if (s.empty()) {
        return std::wstring();
    }
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), &w[0], n);
    return w;
}

std::string utf8(const std::wstring& w) {
    if (w.empty()) {
        return std::string();
    }
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), &s[0], n, nullptr, nullptr);
    return s;
}

std::wstring baseName(const std::string& path) {
    std::wstring w = wide(path);
    size_t slash = w.find_last_of(L"\\/");
    return slash == std::wstring::npos ? w : w.substr(slash + 1);
}

int textWidth(const wchar_t* text, HFONT font) {
    HDC dc = GetDC(S.hwnd);
    HGDIOBJ old = SelectObject(dc, font);
    SIZE sz{0, 0};
    GetTextExtentPoint32W(dc, text, static_cast<int>(wcslen(text)), &sz);
    SelectObject(dc, old);
    ReleaseDC(S.hwnd, dc);
    return sz.cx;
}

int clampInt(double v) {
    if (v > 1e8) {
        return 100000000;
    }
    if (v < -1e8) {
        return -100000000;
    }
    return static_cast<int>(std::lround(v));
}

double clampScale(double s) {
    return std::max(0.0003, std::min(3.0, s));
}

void message(const std::wstring& text, const wchar_t* title, UINT icon) {
    MessageBoxW(S.hwnd, text.c_str(), title, MB_OK | icon);
}

std::optional<double> paperUnits() {
    wchar_t buf[64] = {0};
    GetWindowTextW(S.edit, buf, 64);
    std::wstring t(buf);
    for (wchar_t& c : t) {
        if (c == L',') {
            c = L'.';
        }
    }
    size_t a = t.find_first_not_of(L" \t");
    if (a == std::wstring::npos) {
        return std::nullopt;
    }
    size_t b = t.find_last_not_of(L" \t");
    t = t.substr(a, b - a + 1);
    wchar_t* end = nullptr;
    double cm = std::wcstod(t.c_str(), &end);
    if (end == t.c_str() || *end != L'\0' || !(cm > 0)) {
        return std::nullopt;
    }
    return cm * rm::kUnitsPerCm;
}

std::vector<int> tooWide() {
    std::vector<int> bad;
    std::optional<double> w = paperUnits();
    if (!w) {
        return bad;
    }
    for (size_t n = 0; n < S.boxes.size(); ++n) {
        if (S.boxes[n] && (S.boxes[n]->y1 > *w + 0.5 || S.boxes[n]->y0 < -0.5)) {
            bad.push_back(static_cast<int>(n));
        }
    }
    return bad;
}

void showStatus() {
    std::wstring t;
    if (S.holding < 0) {
        std::vector<int> bad = tooWide();
        if (!bad.empty()) {
            t = L"Outside the paper: piece ";
            for (size_t i = 0; i < bad.size(); ++i) {
                if (i > 0) {
                    t += L", ";
                }
                t += std::to_wstring(bad[i] + 1);
            }
        }
    }
    if (t != S.statusText) {
        S.statusText = t;
        SetWindowTextW(S.status, t.c_str());
    }
}

std::wstring turnedText(double deg) {
    double d = rm::mod360(deg);
    if (d == 0) {
        return L"not turned";
    }
    if (d == 90) {
        return L"turned left 90\u00b0";
    }
    if (d == 180) {
        return L"turned 180\u00b0";
    }
    if (d == 270) {
        return L"turned right 90\u00b0";
    }
    return L"turned " + wide(rm::num(d)) + L"\u00b0";
}

std::vector<rm::Req> requests() {
    std::vector<rm::Req> r;
    for (const Layer& l : S.layers) {
        rm::Req q;
        q.path = l.path;
        q.deg = l.deg;
        q.unit = kUnitMm[l.unit];
        q.noHeader = l.noHeader;
        q.pos = l.pos;
        r.push_back(q);
    }
    return r;
}

void fillList() {
    int sel = static_cast<int>(SendMessageW(S.list, LB_GETCURSEL, 0, 0));
    SendMessageW(S.list, LB_RESETCONTENT, 0, 0);
    for (size_t n = 0; n < S.layers.size(); ++n) {
        std::wstring line = std::to_wstring(n + 1) + L". " + baseName(S.layers[n].path) + L"  (" +
                            turnedText(S.layers[n].deg) + L")";
        SendMessageW(S.list, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(line.c_str()));
    }
    if (sel >= 0 && sel < static_cast<int>(S.layers.size())) {
        SendMessageW(S.list, LB_SETCURSEL, sel, 0);
    }
}

void updatePreview() {
    S.shapes.clear();
    S.boxes.clear();
    S.holdOffX = 0.0;
    S.holdOffY = 0.0;
    S.message.clear();
    if (!S.layers.empty()) {
        try {
            std::vector<rm::Piece> placed = rm::placeLayers(S.cache, requests(), paperUnits());
            bool any = false;
            for (const rm::Piece& p : placed) {
                S.shapes.push_back(rm::renderLayer(p));
                S.boxes.push_back(rm::boxOf(p.ops));
                any = any || !S.shapes.back().empty();
            }
            if (!any) {
                S.shapes.clear();
                S.boxes.clear();
                S.message = "Nothing to draw in these files.";
            }
        } catch (const rm::HpglError& e) {
            S.shapes.clear();
            S.boxes.clear();
            S.message = std::string("Cannot show a preview:\n") + e.what();
        } catch (const std::exception& e) {
            S.shapes.clear();
            S.boxes.clear();
            S.message = std::string("Cannot show a preview: ") + e.what();
        }
        rm::warnings().clear();
    }
    InvalidateRect(S.preview, nullptr, FALSE);
    showStatus();
}

void refreshList(int select) {
    fillList();
    if (select >= 0 && select < static_cast<int>(S.layers.size())) {
        SendMessageW(S.list, LB_SETCURSEL, select, 0);
    }
    updatePreview();
}

void fitView() {
    double minx = 0.0;
    double miny = 0.0;
    double maxx = 0.0;
    double maxy = 0.0;
    for (const std::optional<rm::Box>& b : S.boxes) {
        if (b) {
            minx = std::min(minx, b->x0);
            miny = std::min(miny, b->y0);
            maxx = std::max(maxx, b->x1);
            maxy = std::max(maxy, b->y1);
        }
    }
    std::optional<double> w = paperUnits();
    if (w) {
        maxy = std::max(maxy, *w);
    }
    double padx = (maxx - minx) * 0.03 + 1;
    double pady = (maxy - miny) * 0.05 + 1;
    minx -= padx;
    miny -= pady;
    maxx += padx;
    maxy += pady;
    double m = dp(10);
    double sc = std::min((S.cw - 2 * m) / std::max(maxx - minx, 1.0), (S.ch - 2 * m) / std::max(maxy - miny, 1.0));
    S.vx = minx;
    S.vy = miny;
    S.vs = clampScale(sc);
}

void ensureView() {
    RECT rc;
    GetClientRect(S.preview, &rc);
    S.cw = rc.right;
    S.ch = rc.bottom;
    if (S.shapes.empty() || S.cw < 30 || S.ch < 30) {
        return;
    }
    if (S.autoFit && S.holding < 0) {
        fitView();
    }
}

std::pair<double, double> toPaper(int x, int y) {
    double m = dp(10);
    return {S.vx + (x - m) / S.vs, S.vy + (S.ch - m - y) / S.vs};
}

void zoomAt(double f, int x, int y) {
    ensureView();
    if (S.shapes.empty()) {
        return;
    }
    double m = dp(10);
    std::pair<double, double> p = toPaper(x, y);
    double ns = clampScale(S.vs * f);
    S.vx = p.first - (x - m) / ns;
    S.vy = p.second - (S.ch - m - y) / ns;
    S.vs = ns;
    S.autoFit = false;
    InvalidateRect(S.preview, nullptr, FALSE);
}

void zoomCenter(double f) {
    RECT rc;
    GetClientRect(S.preview, &rc);
    zoomAt(f, rc.right / 2, rc.bottom / 2);
}

void fitAll() {
    S.autoFit = true;
    InvalidateRect(S.preview, nullptr, FALSE);
}

std::pair<double, double> turnedSize(const Layer& l, double deg) {
    std::optional<rm::Box> b = rm::boxOf(rm::loadRotated(S.cache, l.path, deg, kUnitMm[l.unit], l.noHeader).ops);
    if (!b) {
        return {0.0, 0.0};
    }
    return {b->x1 - b->x0, b->y1 - b->y0};
}

void turnLayer(int i, double delta) {
    Layer& l = S.layers[static_cast<size_t>(i)];
    double next = rm::mod360(l.deg + delta);
    if (l.pos && i < static_cast<int>(S.boxes.size()) && S.boxes[static_cast<size_t>(i)]) {
        const rm::Box& b = *S.boxes[static_cast<size_t>(i)];
        std::pair<double, double> sz = turnedSize(l, next);
        l.pos = rm::Pt{(b.x0 + b.x1) / 2 - sz.first / 2, (b.y0 + b.y1) / 2 - sz.second / 2};
    }
    l.deg = next;
}

void dropPiece() {
    S.holding = -1;
    fillList();
    updatePreview();
}

void cancelHold() {
    if (S.holding < 0) {
        return;
    }
    for (size_t n = 0; n < S.layers.size() && n < S.snapDeg.size(); ++n) {
        S.layers[n].deg = S.snapDeg[n];
        S.layers[n].pos = S.snapPos[n];
    }
    S.holding = -1;
    fillList();
    updatePreview();
}

void startPan(int x, int y) {
    ensureView();
    if (S.shapes.empty()) {
        return;
    }
    S.autoFit = false;
    S.panning = true;
    S.panLast = POINT{x, y};
    SetCapture(S.preview);
}

void onLeftDown(int x, int y) {
    ensureView();
    if (S.shapes.empty()) {
        return;
    }
    if (S.holding >= 0) {
        dropPiece();
        return;
    }
    std::pair<double, double> m = toPaper(x, y);
    int hit = -1;
    double best = -1.0;
    for (size_t n = 0; n < S.boxes.size(); ++n) {
        if (!S.boxes[n]) {
            continue;
        }
        const rm::Box& b = *S.boxes[n];
        if (b.x0 <= m.first && m.first <= b.x1 && b.y0 <= m.second && m.second <= b.y1) {
            double area = (b.x1 - b.x0) * (b.y1 - b.y0);
            if (best < 0 || area <= best) {
                hit = static_cast<int>(n);
                best = area;
            }
        }
    }
    if (hit < 0) {
        startPan(x, y);
        return;
    }
    S.snapDeg.clear();
    S.snapPos.clear();
    for (size_t n = 0; n < S.layers.size(); ++n) {
        S.snapDeg.push_back(S.layers[n].deg);
        S.snapPos.push_back(S.layers[n].pos);
        if (n < S.boxes.size() && S.boxes[n]) {
            S.layers[n].pos = rm::Pt{S.boxes[n]->x0, S.boxes[n]->y0};
        }
    }
    S.holding = hit;
    const rm::Box& b = *S.boxes[static_cast<size_t>(hit)];
    S.grabX = m.first - b.x0;
    S.grabY = m.second - b.y0;
    SendMessageW(S.list, LB_SETCURSEL, hit, 0);
    InvalidateRect(S.preview, nullptr, FALSE);
    showStatus();
}

void onMove(int x, int y) {
    if (S.panning) {
        S.vx -= (x - S.panLast.x) / S.vs;
        S.vy += (y - S.panLast.y) / S.vs;
        S.panLast = POINT{x, y};
        InvalidateRect(S.preview, nullptr, FALSE);
        return;
    }
    if (S.holding < 0) {
        return;
    }
    ensureView();
    std::pair<double, double> m = toPaper(x, y);
    size_t i = static_cast<size_t>(S.holding);
    double nx = m.first - S.grabX;
    double ny = m.second - S.grabY;
    rm::Box b = *S.boxes[i];
    double dx = nx - b.x0;
    double dy = ny - b.y0;
    S.layers[i].pos = rm::Pt{nx, ny};
    S.boxes[i] = rm::Box{b.x0 + dx, b.y0 + dy, b.x1 + dx, b.y1 + dy};
    S.holdOffX += dx;
    S.holdOffY += dy;
    InvalidateRect(S.preview, nullptr, FALSE);
}

void onRightDown(int x, int y) {
    if (S.holding < 0) {
        return;
    }
    ensureView();
    std::pair<double, double> m = toPaper(x, y);
    int i = S.holding;
    turnLayer(i, 90.0);
    Layer& l = S.layers[static_cast<size_t>(i)];
    std::pair<double, double> sz = turnedSize(l, l.deg);
    l.pos = rm::Pt{m.first - sz.first / 2, m.second - sz.second / 2};
    S.grabX = sz.first / 2;
    S.grabY = sz.second / 2;
    fillList();
    updatePreview();
}

void drawScene(HDC dc, int w, int h) {
    ensureView();
    if (S.shapes.empty()) {
        if (!S.message.empty()) {
            SelectObject(dc, S.font);
            SetBkMode(dc, TRANSPARENT);
            SetTextColor(dc, RGB(0x66, 0x66, 0x66));
            RECT r{dp(20), h / 3, w - dp(20), h};
            std::wstring text = wide(S.message);
            DrawTextW(dc, text.c_str(), -1, &r, DT_CENTER | DT_WORDBREAK | DT_NOPREFIX);
        }
        return;
    }
    const double m = dp(10);
    const double sc = S.vs;
    auto X = [&](double x) { return clampInt(m + (x - S.vx) * sc); };
    auto Y = [&](double y) { return clampInt(h - m - (y - S.vy) * sc); };

    std::optional<double> pw = paperUnits();
    if (pw) {
        int yt = Y(*pw);
        int yb = Y(0.0);
        RECT band{-1, yt, w + 1, yb};
        HBRUSH br = CreateSolidBrush(RGB(0xf3, 0xf3, 0xf3));
        FillRect(dc, &band, br);
        DeleteObject(br);
        HPEN dash = CreatePen(PS_DASH, 1, RGB(0x88, 0x88, 0x88));
        HGDIOBJ old = SelectObject(dc, dash);
        SetBkMode(dc, TRANSPARENT);
        MoveToEx(dc, 0, yt, nullptr);
        LineTo(dc, w, yt);
        MoveToEx(dc, 0, yb, nullptr);
        LineTo(dc, w, yb);
        SelectObject(dc, old);
        DeleteObject(dash);
    }

    std::map<std::pair<int, int>, HFONT> fonts;
    HGDIOBJ nullBrush = GetStockObject(NULL_BRUSH);
    SetBkMode(dc, TRANSPARENT);
    SetTextAlign(dc, TA_BASELINE | TA_LEFT);

    for (size_t n = 0; n < S.shapes.size(); ++n) {
        COLORREF color = kColors[n % kColorCount];
        bool held = static_cast<int>(n) == S.holding;
        double ox = held ? S.holdOffX : 0.0;
        double oy = held ? S.holdOffY : 0.0;
        HPEN pen = CreatePen(PS_SOLID, held ? dp(2) + 1 : 1, color);
        HGDIOBJ oldPen = SelectObject(dc, pen);
        HGDIOBJ oldBrush = SelectObject(dc, nullBrush);
        SetTextColor(dc, color);
        for (const rm::Item& it : S.shapes[n]) {
            if (it.type == 0) {
                std::vector<POINT> pts;
                pts.reserve(it.pts.size());
                for (const rm::Pt& p : it.pts) {
                    pts.push_back(POINT{X(p.x + ox), Y(p.y + oy)});
                }
                if (pts.size() > 1) {
                    Polyline(dc, pts.data(), static_cast<int>(pts.size()));
                }
            } else if (it.type == 1) {
                double r = it.r;
                Ellipse(dc, X(it.c.x + ox - r), Y(it.c.y + oy + r), X(it.c.x + ox + r), Y(it.c.y + oy - r));
            } else {
                int size = std::max(7, std::min(400, static_cast<int>(it.height * sc)));
                int angle = static_cast<int>(std::lround(std::atan2(it.dy, it.dx) * 180.0 / 3.14159265358979323846 * 10.0));
                std::pair<int, int> key{size, angle};
                auto found = fonts.find(key);
                if (found == fonts.end()) {
                    HFONT f = CreateFontW(-size, 0, angle, angle, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                          OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY,
                                          DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
                    found = fonts.emplace(key, f).first;
                }
                HGDIOBJ oldFont = SelectObject(dc, found->second);
                TextOutA(dc, X(it.c.x + ox), Y(it.c.y + oy), it.text.c_str(), static_cast<int>(it.text.size()));
                SelectObject(dc, oldFont);
            }
        }
        SelectObject(dc, oldBrush);
        SelectObject(dc, oldPen);
        DeleteObject(pen);

        if (S.boxes[n]) {
            HGDIOBJ oldFont = SelectObject(dc, S.bold);
            std::wstring label = std::to_wstring(n + 1);
            TextOutW(dc, X(S.boxes[n]->x0) + dp(6), Y(S.boxes[n]->y1) + dp(22), label.c_str(),
                     static_cast<int>(label.size()));
            SelectObject(dc, oldFont);
        }
    }
    for (auto& f : fonts) {
        DeleteObject(f.second);
    }
}

LRESULT CALLBACK PreviewProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(hwnd, &ps);
            RECT rc;
            GetClientRect(hwnd, &rc);
            int w = rc.right;
            int h = rc.bottom;
            if (w > 0 && h > 0) {
                HDC mem = CreateCompatibleDC(dc);
                HBITMAP bmp = CreateCompatibleBitmap(dc, w, h);
                HGDIOBJ old = SelectObject(mem, bmp);
                FillRect(mem, &rc, static_cast<HBRUSH>(GetStockObject(WHITE_BRUSH)));
                drawScene(mem, w, h);
                BitBlt(dc, 0, 0, w, h, mem, 0, 0, SRCCOPY);
                SelectObject(mem, old);
                DeleteObject(bmp);
                DeleteDC(mem);
            }
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_LBUTTONDOWN:
            SetFocus(hwnd);
            onLeftDown(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
            return 0;
        case WM_MBUTTONDOWN:
            SetFocus(hwnd);
            startPan(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
            return 0;
        case WM_RBUTTONDOWN:
            onRightDown(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
            return 0;
        case WM_LBUTTONUP:
        case WM_MBUTTONUP:
            if (S.panning) {
                S.panning = false;
                ReleaseCapture();
            }
            return 0;
        case WM_CAPTURECHANGED:
            S.panning = false;
            return 0;
        case WM_MOUSEMOVE:
            onMove(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
            return 0;
        case WM_MOUSEWHEEL: {
            POINT p{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            ScreenToClient(hwnd, &p);
            zoomAt(std::pow(1.2, GET_WHEEL_DELTA_WPARAM(wParam) / 120.0), p.x, p.y);
            return 0;
        }
        case WM_SETCURSOR:
            if (LOWORD(lParam) == HTCLIENT) {
                SetCursor(LoadCursorW(nullptr, (S.holding >= 0 || S.panning) ? kCursorMove : kCursorArrow));
                return TRUE;
            }
            break;
        case WM_SIZE:
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        default:
            break;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

std::vector<std::wstring> pickFiles() {
    std::vector<wchar_t> buf(65536, 0);
    static const wchar_t filter[] = L"Plotter files\0*.plt;*.hpgl;*.hpg;*.hp;*.prn\0All files\0*.*\0";
    OPENFILENAMEW ofn;
    ZeroMemory(&ofn, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = S.hwnd;
    ofn.lpstrFilter = filter;
    ofn.lpstrFile = buf.data();
    ofn.nMaxFile = static_cast<DWORD>(buf.size());
    ofn.lpstrTitle = L"Choose HPGL files";
    ofn.Flags = OFN_EXPLORER | OFN_ALLOWMULTISELECT | OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY;
    if (!S.lastDir.empty()) {
        ofn.lpstrInitialDir = S.lastDir.c_str();
    }
    std::vector<std::wstring> files;
    if (!GetOpenFileNameW(&ofn)) {
        return files;
    }
    std::vector<std::wstring> parts;
    const wchar_t* p = buf.data();
    while (*p) {
        parts.emplace_back(p);
        p += wcslen(p) + 1;
    }
    if (parts.size() == 1) {
        files.push_back(parts[0]);
    } else {
        for (size_t i = 1; i < parts.size(); ++i) {
            std::wstring dir = parts[0];
            if (!dir.empty() && dir.back() != L'\\') {
                dir += L'\\';
            }
            files.push_back(dir + parts[i]);
        }
    }
    return files;
}

void addFiles() {
    std::vector<std::wstring> files = pickFiles();
    if (files.empty()) {
        return;
    }
    for (const std::wstring& f : files) {
        Layer l;
        l.path = utf8(f);
        S.layers.push_back(l);
        size_t slash = f.find_last_of(L"\\/");
        if (slash != std::wstring::npos) {
            S.lastDir = f.substr(0, slash);
        }
    }
    S.autoFit = true;
    refreshList(static_cast<int>(S.layers.size()) - 1);
}

int selectedIndex() {
    int i = static_cast<int>(SendMessageW(S.list, LB_GETCURSEL, 0, 0));
    return (i == LB_ERR || i >= static_cast<int>(S.layers.size())) ? -1 : i;
}

void removeSelected() {
    int i = selectedIndex();
    if (i < 0) {
        return;
    }
    S.layers.erase(S.layers.begin() + i);
    S.holding = -1;
    S.autoFit = true;
    SendMessageW(S.list, LB_SETCURSEL, static_cast<WPARAM>(-1), 0);
    refreshList(std::min(i, static_cast<int>(S.layers.size()) - 1));
}

void turnSelected(double delta) {
    int i = selectedIndex();
    if (i < 0) {
        return;
    }
    turnLayer(i, delta);
    fillList();
    updatePreview();
}

void saveFile() {
    if (S.layers.empty()) {
        return;
    }
    if (S.holding >= 0) {
        dropPiece();
    }
    std::optional<double> width = paperUnits();
    if (!width) {
        message(L"Type the paper width in cm first.", L"Paper width needed", MB_ICONINFORMATION);
        SetFocus(S.edit);
        return;
    }
    std::vector<int> bad = tooWide();
    if (!bad.empty()) {
        std::wstring list;
        for (size_t i = 0; i < bad.size(); ++i) {
            if (i > 0) {
                list += L", ";
            }
            list += std::to_wstring(bad[i] + 1);
        }
        message(L"Piece " + list + L" goes past the paper width. Move or turn it first.", L"Outside the paper",
                MB_ICONERROR);
        return;
    }

    std::wstring first = baseName(S.layers[0].path);
    size_t dot = first.find_last_of(L'.');
    std::wstring stem = (dot == std::wstring::npos || dot == 0) ? first : first.substr(0, dot);
    std::wstring ext = (dot == std::wstring::npos || dot == 0) ? L".plt" : first.substr(dot);
    std::vector<wchar_t> buf(32768, 0);
    std::wstring initial = stem + L" merged" + ext;
    wcsncpy(buf.data(), initial.c_str(), buf.size() - 1);
    static const wchar_t filter[] = L"Plotter files\0*.plt;*.hpgl;*.hpg;*.hp;*.prn\0All files\0*.*\0";
    OPENFILENAMEW ofn;
    ZeroMemory(&ofn, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = S.hwnd;
    ofn.lpstrFilter = filter;
    ofn.lpstrFile = buf.data();
    ofn.nMaxFile = static_cast<DWORD>(buf.size());
    ofn.lpstrTitle = L"Save merged file as";
    std::wstring defExt = ext.substr(1);
    ofn.lpstrDefExt = defExt.c_str();
    std::wstring dir = S.lastDir;
    if (dir.empty()) {
        std::wstring firstFull = wide(S.layers[0].path);
        size_t slash = firstFull.find_last_of(L"\\/");
        if (slash != std::wstring::npos) {
            dir = firstFull.substr(0, slash);
        }
    }
    if (!dir.empty()) {
        ofn.lpstrInitialDir = dir.c_str();
    }
    ofn.Flags = OFN_EXPLORER | OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY;
    if (!GetSaveFileNameW(&ofn)) {
        return;
    }
    std::wstring out(buf.data());

    wchar_t outFull[MAX_PATH * 2] = {0};
    GetFullPathNameW(out.c_str(), MAX_PATH * 2, outFull, nullptr);
    CharLowerW(outFull);
    for (const Layer& l : S.layers) {
        wchar_t inFull[MAX_PATH * 2] = {0};
        GetFullPathNameW(wide(l.path).c_str(), MAX_PATH * 2, inFull, nullptr);
        CharLowerW(inFull);
        if (wcscmp(inFull, outFull) == 0) {
            message(L"That would overwrite one of your original files.", L"Choose another name", MB_ICONERROR);
            return;
        }
    }

    rm::warnings().clear();
    std::string text;
    try {
        text = rm::merge(rm::placeLayers(S.cache, requests(), width), false, kUnitMm[S.outIdx]);
    } catch (const rm::HpglError& e) {
        message(wide(e.what()), L"Could not merge", MB_ICONERROR);
        return;
    } catch (const std::exception& e) {
        message(wide(e.what()), L"Something went wrong", MB_ICONERROR);
        return;
    }
    FILE* f = _wfopen(out.c_str(), L"wb");
    if (!f) {
        message(L"The file could not be written.", L"Something went wrong", MB_ICONERROR);
        return;
    }
    size_t written = fwrite(text.data(), 1, text.size(), f);
    fclose(f);
    if (written != text.size()) {
        message(L"The file could not be written completely.", L"Something went wrong", MB_ICONERROR);
        return;
    }
    size_t slash = out.find_last_of(L"\\/");
    std::wstring note = L"Saved " + (slash == std::wstring::npos ? out : out.substr(slash + 1));
    if (!rm::warnings().empty()) {
        note += L"\n\nNotes:";
        for (const std::string& w : rm::warnings()) {
            note += L"\n" + wide(w);
        }
    }
    rm::warnings().clear();
    message(note, L"Done", MB_ICONINFORMATION);
}

HWND makeButton(HWND parent, int id, const wchar_t* text) {
    HWND h = CreateWindowExW(0, L"BUTTON", text, WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, 0, 0, 10, 10,
                             parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), S.inst, nullptr);
    SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(S.font), TRUE);
    return h;
}

void addSlot(HWND h, int w, int ht, int gapAfter, bool glue, int extra) {
    S.slots.push_back(Slot{h, w, ht, gapAfter, glue, extra});
}

void addButton(HWND parent, int id, const wchar_t* text, int pad, int gapAfter, bool useBold) {
    HWND h = makeButton(parent, id, text);
    if (useBold) {
        SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(S.bold), TRUE);
    }
    int w = textWidth(text, useBold ? S.bold : S.font) + dp(pad);
    addSlot(h, w, dp(32), gapAfter, false, 0);
}

void addLabel(HWND parent, const wchar_t* text, int gapAfter) {
    HWND label = CreateWindowExW(0, L"STATIC", text, WS_CHILD | WS_VISIBLE | SS_LEFT | SS_CENTERIMAGE, 0, 0, 10, 10,
                                 parent, nullptr, S.inst, nullptr);
    SendMessageW(label, WM_SETFONT, reinterpret_cast<WPARAM>(S.font), TRUE);
    addSlot(label, textWidth(text, S.font) + dp(6), dp(32), gapAfter, true, 0);
}

HWND addCombo(HWND parent, int id, int gapAfter, bool glue) {
    HWND h = CreateWindowExW(0, L"COMBOBOX", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL | CBS_DROPDOWNLIST,
                             0, 0, 10, 10, parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), S.inst,
                             nullptr);
    SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(S.font), TRUE);
    int widest = 0;
    for (int k = 0; k < 3; ++k) {
        SendMessageW(h, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(kUnitText[k]));
        widest = std::max(widest, textWidth(kUnitText[k], S.font));
    }
    SendMessageW(h, CB_SETCURSEL, 0, 0);
    addSlot(h, widest + dp(44), dp(26), gapAfter, glue, dp(140));
    return h;
}

void groupExtents(int& largest) {
    largest = 0;
    size_t i = 0;
    while (i < S.slots.size()) {
        size_t j = i;
        int gw = 0;
        while (true) {
            gw += S.slots[j].width + (S.slots[j].glue ? S.slots[j].gapAfter : 0);
            if (S.slots[j].glue && j + 1 < S.slots.size()) {
                ++j;
            } else {
                break;
            }
        }
        largest = std::max(largest, gw);
        i = j + 1;
    }
}

WNDPROC g_listProc = nullptr;

RECT headerChipRect(const RECT& item) {
    RECT r;
    r.left = item.left + dp(8);
    r.right = r.left + dp(104);
    r.top = item.top + dp(30);
    r.bottom = r.top + dp(20);
    return r;
}

RECT unitChipRect(const RECT& item) {
    RECT h = headerChipRect(item);
    RECT r;
    r.left = h.right + dp(8);
    r.right = r.left + dp(96);
    r.top = h.top;
    r.bottom = h.bottom;
    return r;
}

void chooseUnit(int index, const RECT& chip) {
    HMENU menu = CreatePopupMenu();
    for (int k = 0; k < 3; ++k) {
        UINT flags = MF_STRING | (S.layers[static_cast<size_t>(index)].unit == k ? MF_CHECKED : 0);
        AppendMenuW(menu, flags, static_cast<UINT_PTR>(k + 1), kUnitText[k]);
    }
    POINT pt{chip.left, chip.bottom};
    ClientToScreen(S.list, &pt);
    int cmd = static_cast<int>(TrackPopupMenu(menu, TPM_RETURNCMD | TPM_NONOTIFY | TPM_LEFTALIGN | TPM_TOPALIGN,
                                              pt.x, pt.y, 0, S.hwnd, nullptr));
    DestroyMenu(menu);
    if (cmd >= 1 && cmd <= 3 && S.layers[static_cast<size_t>(index)].unit != cmd - 1) {
        S.layers[static_cast<size_t>(index)].unit = cmd - 1;
        S.autoFit = true;
        fillList();
        updatePreview();
    }
}

void toggleHeader(int index) {
    Layer& l = S.layers[static_cast<size_t>(index)];
    if (!l.noHeader) {
        bool found = false;
        try {
            std::vector<rm::Op> ops = rm::loadRotated(S.cache, l.path, 0.0, kUnitMm[l.unit], false).ops;
            found = rm::removeHeaderText(ops);
        } catch (const std::exception&) {
            found = false;
        }
        if (!found) {
            message(L"No header text was found in this file.", L"Header text", MB_ICONINFORMATION);
            return;
        }
    }
    l.noHeader = !l.noHeader;
    fillList();
    updatePreview();
}

void drawChip(HDC dc, const RECT& chip, const wchar_t* text, bool sel, bool arrow, COLORREF textColor) {
    HPEN pen = CreatePen(PS_SOLID, 1, sel ? RGB(0xff, 0xff, 0xff) : RGB(0x80, 0x80, 0x80));
    HBRUSH fill = CreateSolidBrush(RGB(0xff, 0xff, 0xff));
    HGDIOBJ oldPen = SelectObject(dc, pen);
    HGDIOBJ oldBrush = SelectObject(dc, fill);
    RoundRect(dc, chip.left, chip.top, chip.right, chip.bottom, dp(6), dp(6));
    SelectObject(dc, oldBrush);
    SelectObject(dc, oldPen);
    DeleteObject(fill);
    DeleteObject(pen);
    SetTextColor(dc, textColor);
    RECT label = chip;
    label.left += dp(8);
    label.right -= arrow ? dp(20) : dp(6);
    DrawTextW(dc, text, -1, &label, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
    if (arrow) {
        int cy = (chip.top + chip.bottom) / 2;
        POINT tri[3] = {{chip.right - dp(16), cy - dp(2)}, {chip.right - dp(8), cy - dp(2)},
                        {chip.right - dp(12), cy + dp(3)}};
        HGDIOBJ blackBrush = SelectObject(dc, GetStockObject(BLACK_BRUSH));
        HGDIOBJ nullPen = SelectObject(dc, GetStockObject(NULL_PEN));
        Polygon(dc, tri, 3);
        SelectObject(dc, nullPen);
        SelectObject(dc, blackBrush);
    }
}

LRESULT CALLBACK ListProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_LBUTTONDOWN) {
        int x = GET_X_LPARAM(lParam);
        int y = GET_Y_LPARAM(lParam);
        LRESULT r = SendMessageW(hwnd, LB_ITEMFROMPOINT, 0, MAKELPARAM(x, y));
        int index = static_cast<int>(LOWORD(r));
        if (HIWORD(r) == 0 && index < static_cast<int>(S.layers.size())) {
            RECT item;
            SendMessageW(hwnd, LB_GETITEMRECT, static_cast<WPARAM>(index), reinterpret_cast<LPARAM>(&item));
            RECT header = headerChipRect(item);
            RECT unit = unitChipRect(item);
            POINT p{x, y};
            if (PtInRect(&header, p)) {
                SendMessageW(hwnd, LB_SETCURSEL, static_cast<WPARAM>(index), 0);
                SetFocus(hwnd);
                toggleHeader(index);
                return 0;
            }
            if (PtInRect(&unit, p)) {
                SendMessageW(hwnd, LB_SETCURSEL, static_cast<WPARAM>(index), 0);
                SetFocus(hwnd);
                chooseUnit(index, unit);
                return 0;
            }
        }
    }
    return CallWindowProcW(g_listProc, hwnd, msg, wParam, lParam);
}

void createControls(HWND hwnd) {
    addButton(hwnd, ID_ADD, L"Add files", 28, dp(4), false);
    addButton(hwnd, ID_REMOVE, L"Remove", 28, dp(4), false);
    addButton(hwnd, ID_LEFT, L"Turn left", 28, dp(4), false);
    addButton(hwnd, ID_RIGHT, L"Turn right", 28, dp(22), false);

    addLabel(hwnd, L"Paper width (cm)", dp(2));
    S.edit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL, 0,
                             0, 10, 10, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_WIDTH)), S.inst,
                             nullptr);
    SendMessageW(S.edit, WM_SETFONT, reinterpret_cast<WPARAM>(S.font), TRUE);
    addSlot(S.edit, dp(70), dp(28), dp(22), false, 0);

    addButton(hwnd, ID_ZOOMOUT, L"\u2212", 22, dp(2), false);
    addButton(hwnd, ID_ZOOMIN, L"+", 22, dp(2), false);
    addButton(hwnd, ID_FIT, L"Fit", 22, dp(22), false);

    addLabel(hwnd, L"Save as", dp(4));
    S.cbOut = addCombo(hwnd, ID_UNITS_OUT, dp(8), true);
    addButton(hwnd, ID_SAVE, L"Save merged file...", 30, 0, true);

    int largest = 0;
    groupExtents(largest);
    S.toolbarWidth = largest + dp(20);

    S.list = CreateWindowExW(WS_EX_CLIENTEDGE, L"LISTBOX", L"",
                             WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_TABSTOP | LBS_NOTIFY | LBS_OWNERDRAWFIXED |
                                 LBS_HASSTRINGS | LBS_NOINTEGRALHEIGHT,
                             0, 0, 10, 10, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_LIST)), S.inst,
                             nullptr);
    SendMessageW(S.list, WM_SETFONT, reinterpret_cast<WPARAM>(S.font), TRUE);
    g_listProc = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(S.list, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(ListProc)));

    S.status = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_LEFT, 0, 0, 10, 10, hwnd,
                               reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_STATUS)), S.inst, nullptr);
    SendMessageW(S.status, WM_SETFONT, reinterpret_cast<WPARAM>(S.font), TRUE);

    S.preview = CreateWindowExW(WS_EX_CLIENTEDGE, L"RalvioliPreview", L"", WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS, 0,
                                0, 10, 10, hwnd, nullptr, S.inst, nullptr);
}

void layoutAll(int w, int h) {
    const int margin = dp(10);
    const int rowH = dp(32);
    const int rowGap = dp(6);
    std::vector<std::pair<int, int>> pos(S.slots.size());
    int x = margin;
    int row = 0;
    size_t i = 0;
    while (i < S.slots.size()) {
        size_t j = i;
        int gw = 0;
        while (true) {
            gw += S.slots[j].width + (S.slots[j].glue ? S.slots[j].gapAfter : 0);
            if (S.slots[j].glue && j + 1 < S.slots.size()) {
                ++j;
            } else {
                break;
            }
        }
        if (x + gw > w - margin && x > margin) {
            x = margin;
            ++row;
        }
        for (size_t k = i; k <= j; ++k) {
            pos[k] = {x, row};
            x += S.slots[k].width + S.slots[k].gapAfter;
        }
        i = j + 1;
    }
    int rows = row + 1;
    int barH = rows * rowH + (rows - 1) * rowGap + 2 * dp(8);
    int barTop = h - barH;
    for (size_t k = 0; k < S.slots.size(); ++k) {
        const Slot& s = S.slots[k];
        int y = barTop + dp(8) + pos[k].second * (rowH + rowGap) + (rowH - s.height) / 2;
        MoveWindow(s.hwnd, pos[k].first, y, s.width, s.height + s.extra, TRUE);
    }
    int bodyTop = dp(10);
    int listW = dp(300);
    int bodyH = std::max(barTop - dp(6) - bodyTop, dp(50));
    int statusH = dp(44);
    MoveWindow(S.list, dp(10), bodyTop, listW, std::max(bodyH - statusH - dp(6), dp(40)), TRUE);
    MoveWindow(S.status, dp(10), bodyTop + std::max(bodyH - statusH, dp(40)) + dp(2), listW, statusH - dp(2), TRUE);
    int px = dp(10) + listW + dp(10);
    MoveWindow(S.preview, px, bodyTop, std::max(w - px - dp(10), dp(50)), bodyH, TRUE);
}

void unitsOutChanged() {
    int sel = static_cast<int>(SendMessageW(S.cbOut, CB_GETCURSEL, 0, 0));
    if (sel >= 0 && sel <= 2) {
        S.outIdx = sel;
    }
}

LRESULT CALLBACK MainProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_CREATE:
            S.hwnd = hwnd;
            createControls(hwnd);
            return 0;
        case WM_SIZE:
            if (S.preview) {
                layoutAll(LOWORD(lParam), HIWORD(lParam));
            }
            return 0;
        case WM_GETMINMAXINFO: {
            MINMAXINFO* mm = reinterpret_cast<MINMAXINFO*>(lParam);
            RECT r{0, 0, S.toolbarWidth, dp(420)};
            AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
            mm->ptMinTrackSize.x = r.right - r.left;
            mm->ptMinTrackSize.y = r.bottom - r.top;
            return 0;
        }
        case WM_MEASUREITEM: {
            MEASUREITEMSTRUCT* mi = reinterpret_cast<MEASUREITEMSTRUCT*>(lParam);
            if (mi->CtlID == ID_LIST) {
                mi->itemHeight = static_cast<UINT>(dp(58));
                return TRUE;
            }
            break;
        }
        case WM_DRAWITEM: {
            DRAWITEMSTRUCT* di = reinterpret_cast<DRAWITEMSTRUCT*>(lParam);
            if (di->CtlID == ID_LIST) {
                if (di->itemID == static_cast<UINT>(-1)) {
                    return TRUE;
                }
                bool sel = (di->itemState & ODS_SELECTED) != 0;
                FillRect(di->hDC, &di->rcItem, GetSysColorBrush(sel ? COLOR_HIGHLIGHT : COLOR_WINDOW));
                int len = static_cast<int>(SendMessageW(di->hwndItem, LB_GETTEXTLEN, di->itemID, 0));
                std::vector<wchar_t> text(static_cast<size_t>(std::max(len, 0)) + 1, 0);
                SendMessageW(di->hwndItem, LB_GETTEXT, di->itemID, reinterpret_cast<LPARAM>(text.data()));
                SetBkMode(di->hDC, TRANSPARENT);
                SetTextColor(di->hDC, sel ? GetSysColor(COLOR_HIGHLIGHTTEXT) : kColors[di->itemID % kColorCount]);
                HGDIOBJ old = SelectObject(di->hDC, S.font);
                RECT r = di->rcItem;
                r.left += dp(6);
                r.right -= dp(6);
                r.top += dp(4);
                r.bottom = r.top + dp(24);
                DrawTextW(di->hDC, text.data(), -1, &r, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);

                bool has = di->itemID < S.layers.size();
                int unit = has ? S.layers[di->itemID].unit : 0;
                bool noHeader = has && S.layers[di->itemID].noHeader;
                drawChip(di->hDC, headerChipRect(di->rcItem), noHeader ? L"Header: off" : L"Header: on", sel, false,
                         noHeader ? RGB(0xc6, 0x28, 0x28) : RGB(0x20, 0x20, 0x20));
                drawChip(di->hDC, unitChipRect(di->rcItem), kUnitShort[unit], sel, true, RGB(0x20, 0x20, 0x20));
                SelectObject(di->hDC, old);
                return TRUE;
            }
            break;
        }
        case WM_CTLCOLORSTATIC:
            if (reinterpret_cast<HWND>(lParam) == S.status) {
                HDC dc = reinterpret_cast<HDC>(wParam);
                SetTextColor(dc, RGB(0xc6, 0x28, 0x28));
                SetBkColor(dc, GetSysColor(COLOR_BTNFACE));
                return reinterpret_cast<LRESULT>(GetSysColorBrush(COLOR_BTNFACE));
            }
            break;
        case WM_COMMAND:
            switch (LOWORD(wParam)) {
                case ID_ADD:
                    addFiles();
                    break;
                case ID_REMOVE:
                    removeSelected();
                    break;
                case ID_LEFT:
                    turnSelected(90.0);
                    break;
                case ID_RIGHT:
                    turnSelected(-90.0);
                    break;
                case ID_ZOOMOUT:
                    zoomCenter(1.0 / 1.25);
                    break;
                case ID_ZOOMIN:
                    zoomCenter(1.25);
                    break;
                case ID_FIT:
                    fitAll();
                    break;
                case ID_SAVE:
                    saveFile();
                    break;
                case ID_CANCEL:
                    cancelHold();
                    break;
                case ID_WIDTH:
                    if (HIWORD(wParam) == EN_CHANGE && S.preview) {
                        updatePreview();
                    }
                    break;
                case ID_UNITS_OUT:
                    if (HIWORD(wParam) == CBN_SELCHANGE) {
                        unitsOutChanged();
                    }
                    break;
                default:
                    break;
            }
            return 0;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
        default:
            break;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

}

int WINAPI WinMain(HINSTANCE inst, HINSTANCE, LPSTR, int show) {
    S.inst = inst;
    INITCOMMONCONTROLSEX icc;
    icc.dwSize = sizeof(icc);
    icc.dwICC = ICC_STANDARD_CLASSES;
    InitCommonControlsEx(&icc);

    HDC screen = GetDC(nullptr);
    S.dpi = GetDeviceCaps(screen, LOGPIXELSX);
    ReleaseDC(nullptr, screen);

    S.font = CreateFontW(-MulDiv(9, S.dpi, 72), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                         OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
                         L"Segoe UI");
    S.bold = CreateFontW(-MulDiv(9, S.dpi, 72), 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                         OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
                         L"Segoe UI");

    HICON iconBig = static_cast<HICON>(LoadImageW(inst, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXICON),
                                                  GetSystemMetrics(SM_CYICON), LR_DEFAULTCOLOR));
    HICON iconSmall = static_cast<HICON>(LoadImageW(inst, MAKEINTRESOURCEW(1), IMAGE_ICON,
                                                    GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON),
                                                    LR_DEFAULTCOLOR));

    WNDCLASSEXW pc;
    ZeroMemory(&pc, sizeof(pc));
    pc.cbSize = sizeof(pc);
    pc.lpfnWndProc = PreviewProc;
    pc.hInstance = inst;
    pc.hCursor = LoadCursorW(nullptr, kCursorArrow);
    pc.lpszClassName = L"RalvioliPreview";
    RegisterClassExW(&pc);

    WNDCLASSEXW wc;
    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = MainProc;
    wc.hInstance = inst;
    wc.hIcon = iconBig;
    wc.hIconSm = iconSmall;
    wc.hCursor = LoadCursorW(nullptr, kCursorArrow);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(static_cast<INT_PTR>(COLOR_BTNFACE + 1));
    wc.lpszClassName = L"RalvioliMerger";
    RegisterClassExW(&wc);

    HWND hwnd = CreateWindowExW(0, L"RalvioliMerger", L"Ralvioli Merger", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                                CW_USEDEFAULT, CW_USEDEFAULT, dp(1250), dp(780), nullptr, nullptr, inst, nullptr);
    if (!hwnd) {
        return 1;
    }
    ShowWindow(hwnd, show);
    UpdateWindow(hwnd);

    ACCEL esc;
    esc.fVirt = FVIRTKEY;
    esc.key = VK_ESCAPE;
    esc.cmd = ID_CANCEL;
    HACCEL accel = CreateAcceleratorTableW(&esc, 1);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (!TranslateAcceleratorW(hwnd, accel, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    return static_cast<int>(msg.wParam);
}
