// Manages the print queue, preview, and worker lifecycle, and records progress for interrupted jobs.
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <commdlg.h>
#include <stdio.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cwchar>
#include <cwctype>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "RalvioliCore.hpp"
#include "PlotStream.hpp"
#include "RalvioliUsb.hpp"
#include "RalvioliWinUsb.hpp"
#include "PlotterResource.h"
#include "PlotPrint.hpp"
#include "Protocol.hpp"
#include <filesystem>

namespace {

enum {
    ID_ADD = 101,
    ID_REMOVE,
    ID_WIDTH,
    ID_ZOOMOUT,
    ID_ZOOMIN,
    ID_FIT,
    ID_FROM,
    ID_TO,
    ID_CALIBRATE,
    ID_START,
    ID_STOP,
    ID_TEST_USB,
    ID_LIST,
    ID_STATUS
};

const UINT kMsgProg = WM_APP + 1;      // wParam layer, lParam bytes sent
const UINT kMsgDone = WM_APP + 2;      // wParam layer, lParam 0 done / 1 stopped / 2 error
const UINT kMsgError = WM_APP + 3;     // lParam new std::string*
const UINT kMsgFinished = WM_APP + 4;
const UINT kMsgUsbResult = WM_APP + 5;
const UINT_PTR kUsbStatusTimer = 1;

const COLORREF kColors[] = {RGB(0x15, 0x65, 0xc0), RGB(0xc6, 0x28, 0x28), RGB(0x2e, 0x7d, 0x32),
                            RGB(0xef, 0x6c, 0x00), RGB(0x6a, 0x1b, 0x9a), RGB(0x00, 0x83, 0x8f)};
const int kColorCount = 6;
const COLORREF kInk = RGB(0x20, 0x20, 0x20);
const COLORREF kFaint = RGB(0xd8, 0xd8, 0xd8);
const COLORREF kBarGreen = RGB(0x2e, 0x7d, 0x32);
const double kUnitMm[3] = {0.025, 0.01, 0.0254};
const double kHpglOutputStepMm = 0.025;
const wchar_t* const kUnitText[3] = {L"0.025 mm (HP-GL/2)", L"0.01 mm", L"0.0254 mm"};
const wchar_t* const kUnitShort[3] = {L"0.025 mm", L"0.01 mm", L"0.0254 mm"};
const LPCWSTR kCursorArrow = MAKEINTRESOURCEW(32512);
const LPCWSTR kCursorMove = MAKEINTRESOURCEW(32646);

struct Layer {
    std::string path;
    int unit = 0;
    bool noHeader = false;
    double from = 0.0;
    double to = 100.0;
    std::wstring status;
    int pct = 0;
    size_t sent = 0;
    size_t total = 0;
};

struct Slot {
    HWND hwnd;
    int width;
    int height;
    int gapAfter;
    bool glue;
    int extra;
};

struct Settings {
    UINT vid = 0x135D;
    UINT pid = 0xF001;
    int pipe = 1;
    int pipeIn = 129;
    double lineWidthMm = 0.4;
    int timeoutMs = 2000;
    bool libusbK = false;
    std::wstring ini;
};

struct State {
    HINSTANCE inst = nullptr;
    HWND hwnd = nullptr;
    HWND list = nullptr;
    HWND preview = nullptr;
    HWND edit = nullptr;
    HWND editFrom = nullptr;
    HWND editTo = nullptr;
    HWND status = nullptr;
    HWND usbStatus = nullptr;
    HWND btnCalibrate = nullptr;
    HWND calibrationWindow = nullptr;
    HWND btnAdd = nullptr;
    HWND btnRemove = nullptr;
    HWND btnStart = nullptr;
    HWND btnStop = nullptr;
    HWND btnTestUsb = nullptr;
    cal::Factors calibration;
    HFONT font = nullptr;
    HFONT bold = nullptr;
    int dpi = 96;
    std::vector<Slot> slots;
    int toolbarWidth = 0;
    Settings cfg;
    std::vector<Layer> layers;
    std::vector<std::optional<rm::Box>> boxes;
    rm::Cache cache;
    // the layer shown in the preview
    int shown = -1;
    bool viewOk = false;
    std::optional<rm::Box> box;
    rp::Stream stream;
    rp::Plan plan;
    std::vector<rp::DItem> items;
    std::string message;
    std::wstring statusText;
    std::wstring queueWarning;
    bool statusError = false;
    bool updating = false;
    bool autoFit = true;
    double vx = 0.0;
    double vy = 0.0;
    double vs = 1.0;
    int cw = 0;
    int ch = 0;
    bool panning = false;
    POINT panLast{0, 0};
    std::wstring lastDir;
    bool running = false;
    HANDLE thread = nullptr;
};

State S;
std::atomic<bool> g_stop(false);

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

int bottomPad() {
    return dp(26);
}

void message(const std::wstring& text, const wchar_t* title, UINT icon) {
    MessageBoxW(S.hwnd, text.c_str(), title, MB_OK | icon);
}

double readField(HWND h) {
    wchar_t buf[64] = {0};
    GetWindowTextW(h, buf, 64);
    std::wstring t(buf);
    for (wchar_t& c : t) {
        if (c == L',') {
            c = L'.';
        }
    }
    return std::wcstod(t.c_str(), nullptr);
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

// ---------------------------------------------------------------- settings
std::wstring iniText(const wchar_t* key, const wchar_t* def) {
    wchar_t buf[512] = {0};
    GetPrivateProfileStringW(L"Plotter", key, def, buf, 512, S.cfg.ini.c_str());
    return buf;
}

std::optional<double> positiveNumber(std::wstring text) {
    std::replace(text.begin(), text.end(), L',', L'.');
    wchar_t* end = nullptr;
    double value = std::wcstod(text.c_str(), &end);
    if (end == text.c_str() || !std::isfinite(value) || value <= 0.0) {
        return std::nullopt;
    }
    while (std::iswspace(*end)) {
        ++end;
    }
    return *end == L'\0' ? std::optional<double>(value) : std::nullopt;
}

void loadSettings() {
    wchar_t path[MAX_PATH * 2] = {0};
    GetModuleFileNameW(nullptr, path, MAX_PATH * 2);
    std::wstring p(path);
    size_t dot = p.find_last_of(L'.');
    S.cfg.ini = (dot == std::wstring::npos ? p : p.substr(0, dot)) + L".ini";
    if (GetFileAttributesW(S.cfg.ini.c_str()) == INVALID_FILE_ATTRIBUTES) {
        wchar_t local[MAX_PATH * 2]{};
        DWORD length = GetEnvironmentVariableW(L"LOCALAPPDATA", local, static_cast<DWORD>(std::size(local)));
        if (length && length < std::size(local)) {
            std::wstring directory = std::wstring(local) + L"\\Ralvioli Suite";
            if (CreateDirectoryW(directory.c_str(), nullptr) || GetLastError() == ERROR_ALREADY_EXISTS) {
                S.cfg.ini = directory + L"\\Ralvioli Plotter.ini";
            }
        }
    }
    S.cfg.vid = static_cast<UINT>(std::wcstoul(iniText(L"Vid", L"135D").c_str(), nullptr, 16));
    S.cfg.pid = static_cast<UINT>(std::wcstoul(iniText(L"Pid", L"F001").c_str(), nullptr, 16));
    S.cfg.pipe = static_cast<int>(std::wcstol(iniText(L"Pipe", L"1").c_str(), nullptr, 10));
    S.cfg.pipeIn = GetPrivateProfileIntW(L"Plotter", L"PipeIn", 129, S.cfg.ini.c_str());
    S.cfg.lineWidthMm = std::clamp(positiveNumber(iniText(L"LineWidthMm", L"0.4")).value_or(0.4), 0.01, 5.0);
    S.cfg.timeoutMs = std::clamp(std::wcstol(iniText(L"TimeoutMs", L"2000").c_str(), nullptr, 10), 100L, 60000L);
    S.cfg.libusbK = _wcsicmp(iniText(L"Backend", L"WinUSB").c_str(), L"libusbK") == 0;
    std::wstring calibration = iniText(L"Calibration", L"1|1");
    size_t separator = calibration.find(L'|');
    if (separator != std::wstring::npos) {
        auto feed = positiveNumber(calibration.substr(0, separator));
        auto across = positiveNumber(calibration.substr(separator + 1));
        if (feed && across && *feed >= 0.000001 && *feed <= 1000000.0 &&
            *across >= 0.000001 && *across <= 1000000.0) {
            S.calibration = {*feed, *across};
        }
    }
}

void saveSettings() {
    wchar_t buf[64] = {0};
    GetWindowTextW(S.edit, buf, 64);
    WritePrivateProfileStringW(L"Plotter", L"WidthCm", buf, S.cfg.ini.c_str());
}

// ---------------------------------------------------------------- status and boxes
void renderStatus() {
    std::wstring text = S.statusText;
    if (!S.queueWarning.empty()) {
        if (!text.empty()) text += L"  |  ";
        text += S.queueWarning;
    }
    SetWindowTextW(S.status, text.empty() ? L"Idle" : text.c_str());
    InvalidateRect(S.status, nullptr, TRUE);
}

void setStatus(const std::wstring& t, bool error) {
    if (t != S.statusText || error != S.statusError) {
        S.statusText = t;
        S.statusError = error;
        renderStatus();
    }
}

void updateUsbStatus() {
    const wchar_t* text;
    switch (plotterUsbPresence(S.cfg.vid, S.cfg.pid)) {
        case UsbPresence::Present: text = L"Printer: detected by Windows"; break;
        case UsbPresence::Missing: text = L"Printer: USB device not detected"; break;
        default: text = L"Printer: USB status unavailable"; break;
    }
    SetWindowTextW(S.usbStatus, text);
}

std::optional<rm::Box> layerBox(const Layer& l, std::string& error) {
    try {
        const rm::Piece& piece = rm::loadRotated(S.cache, l.path, 0.0, kUnitMm[l.unit], l.noHeader);
        std::optional<rm::Box> b = rm::boxOf(piece.ops);
        if (!b) {
            error = "Nothing to draw in this file.";
            return std::nullopt;
        }
        double dx = std::max(0.0, -b->x0);
        double dy = std::max(0.0, -b->y0);
        return rm::Box{b->x0 + dx, b->y0 + dy, b->x1 + dx, b->y1 + dy};
    } catch (const std::exception& e) {
        error = e.what();
        return std::nullopt;
    }
}

void computeBox(size_t i) {
    if (S.boxes.size() < S.layers.size()) {
        S.boxes.resize(S.layers.size());
    }
    std::string error;
    S.boxes[i] = layerBox(S.layers[i], error);
    if (!S.boxes[i]) {
        S.layers[i].status = L"error";
    } else if (S.layers[i].status == L"error") {
        S.layers[i].status.clear();
    }
}

std::vector<int> tooWide(int from) {
    std::vector<int> bad;
    std::optional<double> w = paperUnits();
    if (!w) {
        return bad;
    }
    for (size_t n = static_cast<size_t>(std::max(from, 0)); n < S.boxes.size(); ++n) {
        if (S.boxes[n] && (S.boxes[n]->y1 * S.calibration.across > *w + 0.5 ||
                           S.boxes[n]->y0 * S.calibration.across < -0.5)) {
            bad.push_back(static_cast<int>(n));
        }
    }
    return bad;
}

int selectedIndex() {
    int i = static_cast<int>(SendMessageW(S.list, LB_GETCURSEL, 0, 0));
    return (i == LB_ERR || i >= static_cast<int>(S.layers.size())) ? -1 : i;
}

void showStatus() {
    if (S.running) {
        return;
    }
    std::wstring t;
    std::vector<int> bad = tooWide(0);
    if (!bad.empty()) {
        t = L"Outside the paper: file ";
        for (size_t i = 0; i < bad.size(); ++i) {
            if (i > 0) {
                t += L", ";
            }
            t += std::to_wstring(bad[i] + 1);
        }
    }
    if (t != S.queueWarning) {
        S.queueWarning = std::move(t);
        renderStatus();
    }
}

void fillList() {
    int sel = static_cast<int>(SendMessageW(S.list, LB_GETCURSEL, 0, 0));
    SendMessageW(S.list, LB_RESETCONTENT, 0, 0);
    for (size_t n = 0; n < S.layers.size(); ++n) {
        std::wstring line = std::to_wstring(n + 1) + L". " + baseName(S.layers[n].path);
        SendMessageW(S.list, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(line.c_str()));
    }
    if (sel >= 0 && sel < static_cast<int>(S.layers.size())) {
        SendMessageW(S.list, LB_SETCURSEL, sel, 0);
    }
}

void invalidateRow(int i) {
    RECT r;
    if (SendMessageW(S.list, LB_GETITEMRECT, static_cast<WPARAM>(i), reinterpret_cast<LPARAM>(&r)) != LB_ERR) {
        InvalidateRect(S.list, &r, FALSE);
    }
}

// ---------------------------------------------------------------- preview data
void rebuildView() {
    S.items.clear();
    S.plan = rp::Plan();
    S.stream = rp::Stream();
    S.box.reset();
    S.viewOk = false;
    S.message.clear();
    S.shown = selectedIndex();
    if (S.shown >= 0) {
        const Layer& l = S.layers[static_cast<size_t>(S.shown)];
        try {
            rm::Req q;
            q.path = l.path;
            q.unit = kUnitMm[l.unit];
            q.noHeader = l.noHeader;
            std::vector<rm::Piece> placed = rm::placeLayers(S.cache, {q}, paperUnits());
            const rm::Piece& piece = placed[0];
            S.box = rm::boxOf(piece.ops);
            if (S.box) {
                S.box->x0 *= S.calibration.feed;
                S.box->x1 *= S.calibration.feed;
                S.box->y0 *= S.calibration.across;
                S.box->y1 *= S.calibration.across;
            }
            S.stream = rp::makeStream(piece, kHpglOutputStepMm, S.calibration);
            S.plan = rp::makePlan(S.stream, l.from, l.to);
            S.items = rp::buildDrawing(piece, S.stream);
            S.viewOk = S.box.has_value() && !S.items.empty();
            if (!S.viewOk) {
                S.message = "Nothing to draw in this file.";
            }
        } catch (const rm::HpglError& e) {
            S.message = std::string("Cannot show this file:\n") + e.what();
        } catch (const std::exception& e) {
            S.message = std::string("Cannot show this file: ") + e.what();
        }
        rm::warnings().clear();
    }
    InvalidateRect(S.preview, nullptr, FALSE);
    showStatus();
}

void fitView() {
    if (!S.box) {
        return;
    }
    double minx = S.box->x0;
    double miny = S.box->y0;
    double maxx = S.box->x1;
    double maxy = S.box->y1;
    double padx = (maxx - minx) * 0.03 + 1;
    double pady = (maxy - miny) * 0.05 + 1;
    minx -= padx;
    miny -= pady;
    maxx += padx;
    maxy += pady;
    double m = dp(10);
    double sc = std::min((S.cw - 2 * m) / std::max(maxx - minx, 1.0),
                         (S.ch - 2 * m - bottomPad()) / std::max(maxy - miny, 1.0));
    S.vx = minx;
    S.vy = miny;
    S.vs = clampScale(sc);
}

void ensureView() {
    RECT rc;
    GetClientRect(S.preview, &rc);
    S.cw = rc.right;
    S.ch = rc.bottom;
    if (!S.viewOk || S.cw < 30 || S.ch < 30) {
        return;
    }
    if (S.autoFit) {
        fitView();
    }
}

std::pair<double, double> toPaper(int x, int y) {
    double m = dp(10);
    return {S.vx + (x - m) / S.vs, S.vy + (S.ch - m - bottomPad() - y) / S.vs};
}

void zoomAt(double f, int x, int y) {
    ensureView();
    if (!S.viewOk) {
        return;
    }
    double m = dp(10);
    std::pair<double, double> p = toPaper(x, y);
    double ns = clampScale(S.vs * f);
    S.vx = p.first - (x - m) / ns;
    S.vy = p.second - (S.ch - m - bottomPad() - y) / ns;
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

void startPan(int x, int y) {
    ensureView();
    if (!S.viewOk) {
        return;
    }
    S.autoFit = false;
    S.panning = true;
    S.panLast = POINT{x, y};
    SetCapture(S.preview);
}

void onMove(int x, int y) {
    if (S.panning) {
        S.vx -= (x - S.panLast.x) / S.vs;
        S.vy += (y - S.panLast.y) / S.vs;
        S.panLast = POINT{x, y};
        InvalidateRect(S.preview, nullptr, FALSE);
    }
}

std::wstring calibrationText(double factor, bool forDisplay = false) {
    char text[64];
    auto result = forDisplay ? std::to_chars(text, text + sizeof(text), factor, std::chars_format::general, 7)
                             : std::to_chars(text, text + sizeof(text), factor, std::chars_format::general);
    return std::wstring(text, result.ptr);
}

struct Job {
    int index = -1;
    rm::Piece piece;
    rp::Stream stream;
    rp::Plan plan;
    std::wstring title;
};

bool startWindowsPrint(HWND owner, std::vector<Job> jobs, double paperWidth, cal::Factors factors);

void printCalibrationRectangle(HWND dialog, cal::Factors factors) {
    if (S.running) return;
    wchar_t widthText[64]{}, lengthText[64]{};
    GetDlgItemTextW(dialog, IDC_TEST_RECT_WIDTH, widthText, 64);
    GetDlgItemTextW(dialog, IDC_TEST_RECT_LENGTH, lengthText, 64);
    const auto widthCm = positiveNumber(widthText);
    const auto lengthCm = positiveNumber(lengthText);
    if (!widthCm || !lengthCm) {
        MessageBoxW(dialog, L"Enter a positive width and length in centimeters.", L"Test rectangle",
                    MB_OK | MB_ICONWARNING);
        SetFocus(GetDlgItem(dialog, widthCm ? IDC_TEST_RECT_LENGTH : IDC_TEST_RECT_WIDTH));
        return;
    }

    const double width = *widthCm * rm::kUnitsPerCm;
    const double length = *lengthCm * rm::kUnitsPerCm;
    const double across = width * factors.across;
    const double feed = length * factors.feed;
    if (!(across >= 1.0 && across <= 10000000.0 && feed >= 1.0 && feed <= 10000000.0)) {
        MessageBoxW(dialog, L"The calibrated rectangle is outside the supported size range.",
                    L"Test rectangle", MB_OK | MB_ICONWARNING);
        return;
    }

    // Leave a small margin inside the printer's printable area around the stroke.
    const double margin = std::max(2.0, S.cfg.lineWidthMm) / kHpglOutputStepMm;
    Job job;
    job.title = L"Calibration test rectangle";
    rm::Op move(rm::K::Pts);
    move.verb = "PU";
    move.pts = {{margin, margin}};
    job.piece.ops.push_back(std::move(move));
    rm::Op draw(rm::K::Pts);
    draw.verb = "PD";
    draw.pts = {{margin + feed, margin}, {margin + feed, margin + across},
                {margin, margin + across}, {margin, margin}};
    job.piece.ops.push_back(std::move(draw));
    job.stream = rp::makeStream(job.piece, kHpglOutputStepMm);
    job.plan = rp::makePlan(job.stream, 0, 100);
    std::vector<Job> jobs;
    jobs.push_back(std::move(job));
    if (startWindowsPrint(dialog, std::move(jobs), across + 2 * margin, {})) {
        SetDlgItemTextW(dialog, IDC_TEST_RECT_STATUS, L"Submitting test rectangle...");
        SetDlgItemTextW(dialog, IDC_FEED_EXPECTED, calibrationText(*lengthCm * 10.0).c_str());
        SetDlgItemTextW(dialog, IDC_ACROSS_EXPECTED, calibrationText(*widthCm * 10.0).c_str());
    }
}
INT_PTR CALLBACK calibrationDialog(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* factors = reinterpret_cast<cal::Factors*>(GetWindowLongPtrW(hwnd, DWLP_USER));
    if (msg == WM_DESTROY) {
        S.calibrationWindow = nullptr;
        return FALSE;
    }
    if (msg == WM_INITDIALOG) {
        S.calibrationWindow = hwnd;
        factors = reinterpret_cast<cal::Factors*>(lp);
        SetWindowLongPtrW(hwnd, DWLP_USER, lp);
        for (int edit : {IDC_TEST_RECT_WIDTH, IDC_TEST_RECT_LENGTH}) {
            SendDlgItemMessageW(hwnd, edit, EM_SETLIMITTEXT, 63, 0);
        }
        SetDlgItemTextW(hwnd, IDC_TEST_RECT_WIDTH, L"10");
        SetDlgItemTextW(hwnd, IDC_TEST_RECT_LENGTH, L"15");
    } else if (msg != WM_COMMAND) {
        return FALSE;
    }

    const int id = LOWORD(wp);
    if (msg == WM_COMMAND) {
        switch (id) {
            case IDOK:
            case IDCANCEL:
                EndDialog(hwnd, id);
                return TRUE;
            case IDC_TEST_RECT_PRINT:
                printCalibrationRectangle(hwnd, *factors);
                return TRUE;
            case IDC_CALIBRATION_RESET:
                *factors = {};
                break;
            case IDC_FEED_CALCULATE:
            case IDC_ACROSS_CALCULATE: {
                const bool feed = id == IDC_FEED_CALCULATE;
                const int expectedId = feed ? IDC_FEED_EXPECTED : IDC_ACROSS_EXPECTED;
                const int actualId = feed ? IDC_FEED_ACTUAL : IDC_ACROSS_ACTUAL;
                wchar_t expectedText[64]{};
                wchar_t actualText[64]{};
                GetDlgItemTextW(hwnd, expectedId, expectedText, 64);
                GetDlgItemTextW(hwnd, actualId, actualText, 64);
                auto expected = positiveNumber(expectedText);
                auto actual = positiveNumber(actualText);
                double& factor = feed ? factors->feed : factors->across;
                double next = 1.0;
                auto result = cal::calculate(factor, expected.value_or(0), actual.value_or(0), false, next);
                if (result == cal::Result::NeedsConfirm) {
                    if (MessageBoxW(hwnd, L"The correction exceeds 1%. Apply it?", L"Paper calibration",
                                    MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) != IDYES) {
                        return TRUE;
                    }
                    result = cal::calculate(factor, *expected, *actual, true, next);
                }
                if (result != cal::Result::Applied || next < 0.000001 || next > 1000000.0) {
                    MessageBoxW(hwnd, L"Enter positive lengths in millimeters that give a valid correction.",
                                L"Paper calibration", MB_OK | MB_ICONWARNING);
                    return TRUE;
                }
                factor = next;
                SetDlgItemTextW(hwnd, expectedId, L"1");
                SetDlgItemTextW(hwnd, actualId, L"1");
                break;
            }
            default:
                return FALSE;
        }
    }
    if (msg == WM_INITDIALOG || id == IDC_CALIBRATION_RESET) {
        for (int edit : {IDC_FEED_EXPECTED, IDC_FEED_ACTUAL, IDC_ACROSS_EXPECTED, IDC_ACROSS_ACTUAL}) {
            SendDlgItemMessageW(hwnd, edit, EM_SETLIMITTEXT, 63, 0);
            SetDlgItemTextW(hwnd, edit, L"1");
        }
    }
    SetDlgItemTextW(hwnd, IDC_FEED_FACTOR, calibrationText(factors->feed, true).c_str());
    SetDlgItemTextW(hwnd, IDC_ACROSS_FACTOR, calibrationText(factors->across, true).c_str());
    return TRUE;
}

void showCalibration() {
    if (S.running) {
        return;
    }
    cal::Factors factors = S.calibration;
    INT_PTR result = DialogBoxParamW(S.inst, MAKEINTRESOURCEW(IDD_CALIBRATION), S.hwnd,
                                    calibrationDialog, reinterpret_cast<LPARAM>(&factors));
    if (result == -1) {
        message(L"Could not open the calibration dialog.", L"Paper calibration", MB_ICONERROR);
    } else if (result == IDOK) {
        std::wstring text = calibrationText(factors.feed) + L"|" + calibrationText(factors.across);
        if (!WritePrivateProfileStringW(L"Plotter", L"Calibration", text.c_str(), S.cfg.ini.c_str())) {
            message(L"Could not save the calibration settings.", L"Paper calibration", MB_ICONERROR);
            return;
        }
        S.calibration = factors;
        rebuildView();
        fitAll();
    }
}

void drawScene(HDC dc, int w, int h) {
    ensureView();
    if (!S.viewOk) {
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
    const int bp = bottomPad();
    auto X = [&](double x) { return clampInt(m + (x - S.vx) * sc); };
    auto Y = [&](double y) { return clampInt(h - m - bp - (y - S.vy) * sc); };
    auto plotX = [&](double x) { return X(x * S.calibration.feed); };
    auto plotY = [&](double y) { return Y(y * S.calibration.across); };

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

    const Layer& layer = S.layers[static_cast<size_t>(S.shown)];
    const size_t sent = layer.sent;
    COLORREF color = kColors[static_cast<size_t>(S.shown) % kColorCount];
    auto stateOf = [&](int ln) -> int {
        if (ln < 0 || ln >= static_cast<int>(S.plan.lineIn.size()) || !S.plan.lineIn[static_cast<size_t>(ln)]) {
            return 0;
        }
        return S.plan.lineEnd[static_cast<size_t>(ln)] <= sent ? 2 : 1;
    };
    HPEN pens[3] = {CreatePen(PS_SOLID, 1, kFaint), CreatePen(PS_SOLID, 1, color),
                    CreatePen(PS_SOLID, dp(2), kInk)};
    COLORREF cols[3] = {kFaint, color, kInk};
    HGDIOBJ nullBrush = GetStockObject(NULL_BRUSH);
    HGDIOBJ oldPen = SelectObject(dc, pens[1]);
    HGDIOBJ oldBrush = SelectObject(dc, nullBrush);
    SetBkMode(dc, TRANSPARENT);
    SetTextAlign(dc, TA_BASELINE | TA_LEFT);
    std::map<std::pair<int, int>, HFONT> fonts;
    std::vector<POINT> run;

    for (const rp::DItem& it : S.items) {
        if (it.type == 0) {
            run.clear();
            int cur = -1;
            for (size_t k = 0; k < it.pts.size(); ++k) {
                POINT p{plotX(it.pts[k].x), plotY(it.pts[k].y)};
                if (k == 0) {
                    run.push_back(p);
                    continue;
                }
                int st = stateOf(it.ln[k]);
                if (cur == -1) {
                    cur = st;
                } else if (st != cur) {
                    if (run.size() > 1) {
                        SelectObject(dc, pens[cur]);
                        Polyline(dc, run.data(), static_cast<int>(run.size()));
                    }
                    POINT last = run.back();
                    run.clear();
                    run.push_back(last);
                    cur = st;
                }
                run.push_back(p);
            }
            if (run.size() > 1 && cur >= 0) {
                SelectObject(dc, pens[cur]);
                Polyline(dc, run.data(), static_cast<int>(run.size()));
            }
        } else if (it.type == 1) {
            int st = stateOf(it.line);
            SelectObject(dc, pens[st]);
            double r = it.r;
            Ellipse(dc, plotX(it.c.x - r), plotY(it.c.y + r), plotX(it.c.x + r), plotY(it.c.y - r));
        } else {
            int st = stateOf(it.line);
            SetTextColor(dc, cols[st]);
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
            TextOutA(dc, plotX(it.c.x), plotY(it.c.y), it.text.c_str(), static_cast<int>(it.text.size()));
            SelectObject(dc, oldFont);
        }
    }
    SelectObject(dc, oldBrush);
    SelectObject(dc, oldPen);
    for (HPEN p : pens) {
        DeleteObject(p);
    }
    for (auto& f : fonts) {
        DeleteObject(f.second);
    }

    // percentage and progress bar
    if (layer.total > 0) {
        std::wstring t = std::to_wstring(layer.pct) + L"%";
        HGDIOBJ oldFont = SelectObject(dc, S.bold);
        HFONT big = CreateFontW(-dp(34), 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                                CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
        SelectObject(dc, big);
        SetTextAlign(dc, TA_TOP | TA_LEFT);
        SetTextColor(dc, kBarGreen);
        TextOutW(dc, dp(12), dp(6), t.c_str(), static_cast<int>(t.size()));
        SelectObject(dc, oldFont);
        DeleteObject(big);
    }
    int bx = dp(10);
    int bw = w - 2 * dp(10);
    int bh = dp(12);
    int by = h - dp(8) - bh;
    if (bw > 10) {
        RECT back{bx, by, bx + bw, by + bh};
        HBRUSH bg = CreateSolidBrush(RGB(0xe4, 0xe4, 0xe4));
        FillRect(dc, &back, bg);
        DeleteObject(bg);
        RECT fill{bx, by, bx + static_cast<int>(static_cast<double>(bw) * layer.pct / 100.0), by + bh};
        HBRUSH fg = CreateSolidBrush(kBarGreen);
        FillRect(dc, &fill, fg);
        DeleteObject(fg);
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
        case WM_MBUTTONDOWN:
            SetFocus(hwnd);
            startPan(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
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
                SetCursor(LoadCursorW(nullptr, S.panning ? kCursorMove : kCursorArrow));
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

// ---------------------------------------------------------------- files
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

void onSelect() {
    S.updating = true;
    int i = selectedIndex();
    if (i >= 0) {
        SetWindowTextW(S.editFrom, wide(rm::num(S.layers[static_cast<size_t>(i)].from)).c_str());
        SetWindowTextW(S.editTo, wide(rm::num(S.layers[static_cast<size_t>(i)].to)).c_str());
    } else {
        SetWindowTextW(S.editFrom, L"");
        SetWindowTextW(S.editTo, L"");
    }
    S.updating = false;
    S.autoFit = true;
    rebuildView();
}

void addFiles() {
    std::vector<std::wstring> files = pickFiles();
    if (files.empty()) {
        return;
    }
    size_t first = S.layers.size();
    for (const std::wstring& f : files) {
        Layer l;
        l.path = utf8(f);
        l.unit = rp::guessUnit(l.path);
        S.layers.push_back(l);
        computeBox(S.layers.size() - 1);
        size_t slash = f.find_last_of(L"\\/");
        if (slash != std::wstring::npos) {
            S.lastDir = f.substr(0, slash);
        }
    }
    rm::warnings().clear();
    fillList();
    SendMessageW(S.list, LB_SETCURSEL, static_cast<WPARAM>(first), 0);
    onSelect();
}

void removeSelected() {
    int i = selectedIndex();
    if (i < 0) {
        return;
    }
    S.layers.erase(S.layers.begin() + i);
    if (static_cast<size_t>(i) < S.boxes.size()) {
        S.boxes.erase(S.boxes.begin() + i);
    }
    fillList();
    int next = S.layers.empty() ? -1 : std::min(i, static_cast<int>(S.layers.size()) - 1);
    SendMessageW(S.list, LB_SETCURSEL, static_cast<WPARAM>(next), 0);
    onSelect();
}

void layerChanged(int i) {
    Layer& l = S.layers[static_cast<size_t>(i)];
    l.pct = 0;
    l.sent = 0;
    l.total = 0;
    l.status.clear();
    computeBox(static_cast<size_t>(i));
    invalidateRow(i);
    if (i == selectedIndex()) {
        S.autoFit = true;
        rebuildView();
    } else {
        showStatus();
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
    layerChanged(index);
}

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
        layerChanged(index);
    }
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

WNDPROC g_listProc = nullptr;

LRESULT CALLBACK ListProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_LBUTTONDOWN && !S.running) {
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
                if (index != S.shown) {
                    onSelect();
                }
                toggleHeader(index);
                return 0;
            }
            if (PtInRect(&unit, p)) {
                SendMessageW(hwnd, LB_SETCURSEL, static_cast<WPARAM>(index), 0);
                SetFocus(hwnd);
                if (index != S.shown) {
                    onSelect();
                }
                chooseUnit(index, unit);
                return 0;
            }
        }
    }
    return CallWindowProcW(g_listProc, hwnd, msg, wParam, lParam);
}

// ---------------------------------------------------------------- printing
struct ThreadArg {
    std::vector<Job> jobs;
    Settings cfg;
    HWND hwnd;
    HDC printer = nullptr;
    ~ThreadArg() { if (printer) DeleteDC(printer); }
    double paperWidth = 0;
    cal::Factors calibration;
};

void postError(HWND h, const std::string& s) {
    auto detail = std::make_unique<std::string>(s);
    if (PostMessageW(h, kMsgError, 0, reinterpret_cast<LPARAM>(detail.get()))) {
        detail.release();
    }
}

std::string hexBytes(const std::string& bytes) {
    std::string hex;
    for (unsigned char b : bytes) {
        char text[4];
        snprintf(text, sizeof(text), "%02X ", b);
        hex += text;
    }
    return hex;
}

bool checkVega(Transport& usb, std::string& log, std::string& result) {
    if (!usb.open(result)) return false;
    log += usb.description() + "\n";
    // AlgotexOcx 0x1000f746 -> 0x1001a0a0 -> 0x1001a020 stores the command big-endian.
    const std::string request = proto::frame(0x3F, proto::be16(0x1A));
    auto read = [&](std::string& value, const char* label) {
        size_t offset = 0;
        while (offset < value.size()) {
            size_t got = 0;
            const bool ok = usb.read(reinterpret_cast<unsigned char*>(value.data()) + offset,
                                     value.size() - offset, got, result);
            offset += got;
            if (!ok || !got) {
                log += std::string(label) + ": " + hexBytes(value.substr(0, offset)) + "\n";
                if (ok) result = std::string("Empty read while receiving ") + label;
                return false;
            }
        }
        log += std::string(label) + ": " + hexBytes(value) + "\n";
        return true;
    };
    std::string ack(1, '\0');
    for (int attempt = 0; attempt < 3; ++attempt) {
        size_t sent = 0;
        log += "Status request: " + hexBytes(request) + "\n";
        if (usb.write(reinterpret_cast<const unsigned char*>(request.data()), request.size(), sent, result) ||
            sent != request.size()) {
            if (result.empty()) result = "Incomplete status request";
            return false;
        }
        if (!read(ack, "ACK")) return false;
        if (ack[0] != 1) break;
    }
    if (ack[0] != 0) {
        result = "Plotter did not accept the status request";
        return false;
    }
    std::string header(8, '\0');
    size_t length = 0;
    if (!read(header, "Reply header") || !proto::checkHeader(header, 0x1A, length, result)) return false;
    std::string body(length + 2, '\0');
    if (!read(body, "Reply body") || !proto::checkBody(header, body, result)) return false;
    result = "VEGA status reply verified (" + std::to_string(length) + " data bytes)";
    return true;
}

DWORD WINAPI printThread(LPVOID p) {
    std::unique_ptr<ThreadArg> a(static_cast<ThreadArg*>(p));
    if (!a->printer) {
        std::unique_ptr<Transport> usb;
        if (a->cfg.libusbK) {
            auto backend = std::make_unique<LibUsbKTransport>(a->cfg.vid, a->cfg.pid, a->cfg.pipe, a->cfg.timeoutMs);
            backend->forceIn = a->cfg.pipeIn;
            usb = std::move(backend);
        } else {
            auto backend = std::make_unique<WinUsbTransport>(a->cfg.vid, a->cfg.pid, a->cfg.pipe, a->cfg.timeoutMs);
            backend->forceIn = a->cfg.pipeIn;
            usb = std::move(backend);
        }
        std::string log = "Algotex VEGA status check\n";
        std::string result;
        const bool passed = checkVega(*usb, log, result);
        usb->close();
        log += result + "\n";
        auto path = std::filesystem::path(a->cfg.ini);
        path.replace_extension(L".usb.txt");
        if (FILE* file = _wfopen(path.c_str(), L"wb")) {
            const bool written = fwrite(log.data(), 1, log.size(), file) == log.size();
            if (fclose(file) == 0 && written) result += " | Log: " + utf8(path.wstring());
            else result += " | Could not write the USB log";
        } else {
            result += " | Could not open the USB log";
        }
        auto detail = std::make_unique<std::string>(std::move(result));
        if (PostMessageW(a->hwnd, kMsgUsbResult, passed, reinterpret_cast<LPARAM>(detail.get()))) {
            detail.release();
        }
    } else {
        SetAbortProc(a->printer, [](HDC, int) -> BOOL { return !g_stop; });
        for (const Job& job : a->jobs) {
            if (g_stop) break;
            std::string error;
            PostMessageW(a->hwnd, kMsgProg, job.index, 0);
            bool submitted = false;
            try {
                const int dpi = std::clamp(std::max(GetDeviceCaps(a->printer, LOGPIXELSX),
                                                   GetDeviceCaps(a->printer, LOGPIXELSY)), 75, 600);
                submitted = printPlot(job.piece, job.stream, job.plan, a->calibration, a->paperWidth,
                                      dpi, a->cfg.lineWidthMm, a->printer, job.title, g_stop, error);
            } catch (const std::exception& e) {
                error = e.what();
            }
            if (!submitted && !g_stop) postError(a->hwnd, utf8(job.title) + ": " + error);
            PostMessageW(a->hwnd, kMsgDone, job.index, submitted ? 0 : g_stop ? 1 : 2);
            if (!submitted) break;
        }
    }
    PostMessageW(a->hwnd, kMsgFinished, 0, 0);
    return 0;
}
void setRunning(bool running) {
    S.running = running;
    EnableWindow(S.btnAdd, !running);
    EnableWindow(S.btnRemove, !running);
    EnableWindow(S.edit, !running);
    EnableWindow(S.editFrom, !running);
    EnableWindow(S.editTo, !running);
    EnableWindow(S.btnCalibrate, !running);
    EnableWindow(S.btnStart, !running);
    EnableWindow(S.btnTestUsb, !running);
    EnableWindow(S.btnStop, running);
}

void testUsb() {
    if (S.running) {
        return;
    }
    auto a = std::make_unique<ThreadArg>();
    a->cfg = S.cfg;
    a->hwnd = S.hwnd;
    S.thread = CreateThread(nullptr, 0, printThread, a.get(), 0, nullptr);
    if (!S.thread) {
        setStatus(L"Could not start the USB test: " + wide(winErrorText(GetLastError())), true);
        return;
    }
    a.release();
    setRunning(true);
    EnableWindow(S.btnStop, FALSE);
    setStatus(L"Checking VEGA status...", false);
}

void startPrint() {
    if (S.running || S.layers.empty()) {
        return;
    }
    std::optional<double> width = paperUnits();
    if (!width) {
        message(L"Type the paper width in cm first.", L"Paper width needed", MB_ICONINFORMATION);
        SetFocus(S.edit);
        return;
    }
    int first = std::max(selectedIndex(), 0);
    std::vector<int> bad = tooWide(first);
    if (!bad.empty()) {
        std::wstring list;
        for (size_t i = 0; i < bad.size(); ++i) {
            if (i > 0) {
                list += L", ";
            }
            list += std::to_wstring(bad[i] + 1);
        }
        message(L"File " + list + L" goes past the paper width.", L"Outside the paper", MB_ICONERROR);
        return;
    }
    std::vector<Job> jobs;
    std::wstring problems;
    for (int i = first; i < static_cast<int>(S.layers.size()); ++i) {
        Layer& l = S.layers[static_cast<size_t>(i)];
        l.sent = 0;
        l.pct = 0;
        l.total = 0;
        try {
            rm::Req q;
            q.path = l.path;
            q.unit = kUnitMm[l.unit];
            q.noHeader = l.noHeader;
            std::vector<rm::Piece> placed = rm::placeLayers(S.cache, {q}, width);
            rp::Stream st = rp::makeStream(placed[0], kHpglOutputStepMm, S.calibration);
            rp::Plan plan = rp::makePlan(st, l.from, l.to);
            if (plan.bytes.empty()) {
                l.status = L"empty";
                invalidateRow(i);
                continue;
            }
            l.total = plan.bytes.size();
            l.status = L"queued";
            Job j;
            j.index = i;
            j.piece = std::move(placed[0]);
            j.stream = std::move(st);
            j.plan = std::move(plan);
            j.title = baseName(l.path);
            jobs.push_back(std::move(j));
        } catch (const std::exception& e) {
            l.status = L"error";
            problems += baseName(l.path) + L": " + wide(e.what()) + L"\n";
        }
        invalidateRow(i);
    }
    rm::warnings().clear();
    if (!problems.empty()) {
        setStatus(L"File error: " + problems, true);
        message(problems, L"Some files cannot be printed", MB_ICONERROR);
    }
    if (!jobs.empty() && !startWindowsPrint(S.hwnd, std::move(jobs), *width, S.calibration)) {
        for (Layer& layer : S.layers) {
            if (layer.status == L"queued") {
                layer.status.clear();
                layer.total = 0;
            }
        }
    }
    rebuildView();
    InvalidateRect(S.list, nullptr, FALSE);
}

bool startWindowsPrint(HWND owner, std::vector<Job> jobs, double paperWidth, cal::Factors factors) {
    PRINTDLGW dlg{};
    dlg.lStructSize = sizeof(dlg);
    dlg.hwndOwner = owner;
    dlg.Flags = PD_RETURNDC | PD_NOSELECTION | PD_NOPAGENUMS | PD_DISABLEPRINTTOFILE;
    dlg.nCopies = 1;
    const bool selected = PrintDlgW(&dlg) != FALSE;
    const DWORD dialogError = selected ? 0 : CommDlgExtendedError();
    if (dlg.hDevMode) GlobalFree(dlg.hDevMode);
    if (dlg.hDevNames) GlobalFree(dlg.hDevNames);
    auto args = std::make_unique<ThreadArg>();
    args->printer = dlg.hDC;
    if (!selected || !args->printer) {
        if (dialogError || selected) {
            MessageBoxW(owner, L"Windows could not open the selected printer.", L"Print", MB_OK | MB_ICONERROR);
        }
        return false;
    }
    args->cfg = S.cfg;
    args->hwnd = S.hwnd;
    args->jobs = std::move(jobs);
    args->paperWidth = paperWidth;
    args->calibration = factors;
    g_stop = false;
    S.thread = CreateThread(nullptr, 0, printThread, args.get(), 0, nullptr);
    if (!S.thread) {
        MessageBoxW(owner, wide(winErrorText(GetLastError())).c_str(), L"Print", MB_OK | MB_ICONERROR);
        return false;
    }
    args.release();
    setRunning(true);
    setStatus(L"Submitting to Windows printer...", false);
    return true;
}
// ---------------------------------------------------------------- controls
HWND makeButton(HWND parent, int id, const wchar_t* text) {
    HWND h = CreateWindowExW(0, L"BUTTON", text, WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, 0, 0, 10, 10,
                             parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), S.inst, nullptr);
    SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(S.font), TRUE);
    return h;
}

void addSlot(HWND h, int w, int ht, int gapAfter, bool glue, int extra) {
    S.slots.push_back(Slot{h, w, ht, gapAfter, glue, extra});
}

HWND addButton(HWND parent, int id, const wchar_t* text, int pad, int gapAfter, bool useBold) {
    HWND h = makeButton(parent, id, text);
    if (useBold) {
        SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(S.bold), TRUE);
    }
    int w = textWidth(text, useBold ? S.bold : S.font) + dp(pad);
    addSlot(h, w, dp(32), gapAfter, false, 0);
    return h;
}

void addLabel(HWND parent, const wchar_t* text, int gapAfter) {
    HWND label = CreateWindowExW(0, L"STATIC", text, WS_CHILD | WS_VISIBLE | SS_LEFT | SS_CENTERIMAGE, 0, 0, 10, 10,
                                 parent, nullptr, S.inst, nullptr);
    SendMessageW(label, WM_SETFONT, reinterpret_cast<WPARAM>(S.font), TRUE);
    addSlot(label, textWidth(text, S.font) + dp(6), dp(32), gapAfter, true, 0);
}

HWND addEdit(HWND parent, int id, int width, int gapAfter) {
    HWND h = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL, 0, 0,
                             10, 10, parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), S.inst, nullptr);
    SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(S.font), TRUE);
    addSlot(h, dp(width), dp(28), gapAfter, false, 0);
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

void createControls(HWND hwnd) {
    S.btnAdd = addButton(hwnd, ID_ADD, L"Add files", 28, dp(4), false);
    S.btnRemove = addButton(hwnd, ID_REMOVE, L"Remove", 28, dp(22), false);

    addLabel(hwnd, L"Paper width (cm)", dp(2));
    S.edit = addEdit(hwnd, ID_WIDTH, 70, dp(22));
    SetWindowTextW(S.edit, iniText(L"WidthCm", L"").c_str());

    addButton(hwnd, ID_ZOOMOUT, L"\u2212", 22, dp(2), false);
    addButton(hwnd, ID_ZOOMIN, L"+", 22, dp(2), false);
    addButton(hwnd, ID_FIT, L"Fit", 22, dp(22), false);

    addLabel(hwnd, L"From %", dp(2));
    S.editFrom = addEdit(hwnd, ID_FROM, 52, dp(10));
    addLabel(hwnd, L"To %", dp(2));
    S.editTo = addEdit(hwnd, ID_TO, 52, dp(22));

    S.btnCalibrate = addButton(hwnd, ID_CALIBRATE, L"Calibrate", 74, dp(22), false);
    S.btnStart = addButton(hwnd, ID_START, L"Print", 40, dp(4), true);
    S.btnStop = addButton(hwnd, ID_STOP, L"Stop", 40, 0, false);
    S.btnTestUsb = addButton(hwnd, ID_TEST_USB, L"Check plotter", 70, 0, false);
    EnableWindow(S.btnStop, FALSE);

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

    S.usbStatus = CreateWindowExW(WS_EX_STATICEDGE, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_LEFT | SS_CENTERIMAGE,
                                  0, 0, 10, 10, hwnd, nullptr, S.inst, nullptr);
    SendMessageW(S.usbStatus, WM_SETFONT, reinterpret_cast<WPARAM>(S.font), TRUE);
    S.status = CreateWindowExW(WS_EX_STATICEDGE, L"STATIC", L"Idle", WS_CHILD | WS_VISIBLE | SS_LEFT | SS_NOPREFIX,
                               0, 0, 10, 10, hwnd,
                               reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_STATUS)), S.inst, nullptr);
    SendMessageW(S.status, WM_SETFONT, reinterpret_cast<WPARAM>(S.font), TRUE);

    S.preview = CreateWindowExW(WS_EX_CLIENTEDGE, L"RalvioliPlotPreview", L"", WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS,
                                0, 0, 10, 10, hwnd, nullptr, S.inst, nullptr);
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
    int statusH = dp(58);
    int barTop = h - barH - statusH;
    struct Bounds {
        HWND window;
        int x, y, width, height;
    };
    std::vector<Bounds> children;
    children.reserve(S.slots.size() + 4);
    for (size_t k = 0; k < S.slots.size(); ++k) {
        const Slot& slot = S.slots[k];
        int y = barTop + dp(8) + pos[k].second * (rowH + rowGap) + (rowH - slot.height) / 2;
        children.push_back({slot.hwnd, pos[k].first, y, slot.width, slot.height + slot.extra});
    }
    int bodyTop = dp(10);
    int listW = dp(300);
    int bodyH = std::max(barTop - dp(6) - bodyTop, dp(50));
    children.push_back({S.list, dp(10), bodyTop, listW, bodyH});
    int px = dp(10) + listW + dp(10);
    children.push_back({S.preview, px, bodyTop, std::max(w - px - dp(10), dp(50)), bodyH});
    int statusTop = h - statusH;
    int statusWidth = std::max(w - 2 * margin, 1);
    children.push_back({S.usbStatus, margin, statusTop, statusWidth, dp(22)});
    children.push_back({S.status, margin, statusTop + dp(23), statusWidth, dp(32)});

    constexpr UINT flags = SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOREDRAW | SWP_NOCOPYBITS;
    HDWP batch = BeginDeferWindowPos(static_cast<int>(children.size()));
    for (const Bounds& child : children) {
        if (!batch) break;
        batch = DeferWindowPos(batch, child.window, nullptr, child.x, child.y, child.width, child.height, flags);
    }
    if (!batch || !EndDeferWindowPos(batch)) {
        for (const Bounds& child : children) {
            SetWindowPos(child.window, nullptr, child.x, child.y, child.width, child.height, flags);
        }
    }
    // Erase the old toolbar and status positions after every child has moved.
    RedrawWindow(S.hwnd, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_FRAME);
}

void drawListItem(DRAWITEMSTRUCT* di) {
    if (di->itemID == static_cast<UINT>(-1)) {
        return;
    }
    bool sel = (di->itemState & ODS_SELECTED) != 0;
    FillRect(di->hDC, &di->rcItem, GetSysColorBrush(sel ? COLOR_HIGHLIGHT : COLOR_WINDOW));
    int len = static_cast<int>(SendMessageW(di->hwndItem, LB_GETTEXTLEN, di->itemID, 0));
    std::vector<wchar_t> text(static_cast<size_t>(std::max(len, 0)) + 1, 0);
    SendMessageW(di->hwndItem, LB_GETTEXT, di->itemID, reinterpret_cast<LPARAM>(text.data()));
    bool has = di->itemID < S.layers.size();
    const Layer* layer = has ? &S.layers[di->itemID] : nullptr;
    SetBkMode(di->hDC, TRANSPARENT);
    HGDIOBJ old = SelectObject(di->hDC, S.font);
    RECT r = di->rcItem;
    r.left += dp(6);
    r.right -= dp(6);
    r.top += dp(4);
    r.bottom = r.top + dp(24);
    if (layer != nullptr && !layer->status.empty()) {
        int sw = textWidth(layer->status.c_str(), S.font);
        RECT sr = r;
        sr.left = r.right - sw;
        COLORREF sc = sel ? GetSysColor(COLOR_HIGHLIGHTTEXT) : kInk;
        if (!sel && (layer->status == L"error" || layer->status == L"too wide")) {
            sc = RGB(0xc6, 0x28, 0x28);
        } else if (!sel && layer->status == L"done") {
            sc = kBarGreen;
        }
        SetTextColor(di->hDC, sc);
        DrawTextW(di->hDC, layer->status.c_str(), -1, &sr, DT_SINGLELINE | DT_VCENTER | DT_RIGHT | DT_NOPREFIX);
        r.right -= sw + dp(8);
    }
    SetTextColor(di->hDC, sel ? GetSysColor(COLOR_HIGHLIGHTTEXT) : kColors[di->itemID % kColorCount]);
    DrawTextW(di->hDC, text.data(), -1, &r, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);

    int unit = has ? layer->unit : 0;
    bool noHeader = has && layer->noHeader;
    drawChip(di->hDC, headerChipRect(di->rcItem), noHeader ? L"Header: off" : L"Header: on", sel, false,
             noHeader ? RGB(0xc6, 0x28, 0x28) : RGB(0x20, 0x20, 0x20));
    drawChip(di->hDC, unitChipRect(di->rcItem), kUnitShort[unit], sel, true, RGB(0x20, 0x20, 0x20));
    if (has && layer->total > 0) {
        RECT back{di->rcItem.left + dp(8), di->rcItem.top + dp(52), di->rcItem.right - dp(8), di->rcItem.top + dp(56)};
        HBRUSH bg = CreateSolidBrush(RGB(0xe0, 0xe0, 0xe0));
        FillRect(di->hDC, &back, bg);
        DeleteObject(bg);
        RECT fill = back;
        fill.right = back.left + static_cast<int>(static_cast<double>(back.right - back.left) * layer->pct / 100.0);
        HBRUSH fg = CreateSolidBrush(kBarGreen);
        FillRect(di->hDC, &fill, fg);
        DeleteObject(fg);
    }
    SelectObject(di->hDC, old);
}

void applyRange() {
    int i = selectedIndex();
    if (i < 0 || S.updating || S.running) {
        return;
    }
    Layer& l = S.layers[static_cast<size_t>(i)];
    l.from = std::max(0.0, std::min(100.0, readField(S.editFrom)));
    l.to = GetWindowTextLengthW(S.editTo) == 0 ? 100.0 : std::max(0.0, std::min(100.0, readField(S.editTo)));
    rebuildView();
}

LRESULT CALLBACK MainProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_CREATE:
            S.hwnd = hwnd;
            S.updating = true;
            createControls(hwnd);
            S.updating = false;
            updateUsbStatus();
            SetTimer(hwnd, kUsbStatusTimer, 2000, nullptr);
            return 0;
        case WM_TIMER:
            if (wParam == kUsbStatusTimer && !S.running) updateUsbStatus();
            return 0;
        case WM_SIZE:
            if (S.preview && wParam != SIZE_MINIMIZED) {
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
                mi->itemHeight = static_cast<UINT>(dp(60));
                return TRUE;
            }
            break;
        }
        case WM_DRAWITEM: {
            DRAWITEMSTRUCT* di = reinterpret_cast<DRAWITEMSTRUCT*>(lParam);
            if (di->CtlID == ID_LIST) {
                drawListItem(di);
                return TRUE;
            }
            break;
        }
        case WM_CTLCOLORSTATIC:
            if (reinterpret_cast<HWND>(lParam) == S.status) {
                HDC dc = reinterpret_cast<HDC>(wParam);
                SetTextColor(dc, S.statusError || !S.queueWarning.empty() ? RGB(0xc6, 0x28, 0x28) : RGB(0x20, 0x20, 0x20));
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
                case ID_ZOOMOUT:
                    zoomCenter(1.0 / 1.25);
                    break;
                case ID_ZOOMIN:
                    zoomCenter(1.25);
                    break;
                case ID_FIT:
                    fitAll();
                    break;
                case ID_START:
                    startPrint();
                    break;
                case ID_TEST_USB:
                    testUsb();
                    break;
                case ID_STOP:
                    g_stop = true;
                    setStatus(L"Stopping...", false);
                    break;
                case ID_LIST:
                    if (HIWORD(wParam) == LBN_SELCHANGE) {
                        onSelect();
                    }
                    break;
                case ID_WIDTH:
                    if (HIWORD(wParam) == EN_CHANGE && S.preview && !S.updating) {
                        saveSettings();
                        InvalidateRect(S.preview, nullptr, FALSE);
                        showStatus();
                    }
                    break;
                case ID_FROM:
                case ID_TO:
                    if (HIWORD(wParam) == EN_CHANGE) {
                        applyRange();
                    }
                    break;
                case ID_CALIBRATE:
                    showCalibration();
                    break;
                default:
                    break;
            }
            return 0;
        case kMsgProg: {
            size_t i = static_cast<size_t>(wParam);
            if (i >= S.layers.size()) {
                return 0;
            }
            Layer& l = S.layers[i];
            l.sent = static_cast<size_t>(lParam);
            l.pct = l.total ? static_cast<int>(std::min<size_t>(100, l.sent * 100 / l.total)) : 0;
            l.status = std::to_wstring(l.pct) + L"%";
            invalidateRow(static_cast<int>(i));
            if (!g_stop) {
                setStatus(std::wstring(L"Rendering ") + baseName(l.path) +
                          L" — " + std::to_wstring(l.pct) + L"%", false);
            }
            if (static_cast<int>(i) == S.shown) {
                InvalidateRect(S.preview, nullptr, FALSE);
            }
            return 0;
        }
        case kMsgDone: {
            if (static_cast<int>(wParam) == -1) {
                const wchar_t* status = lParam == 0 ? L"Test rectangle submitted to Windows printer." :
                    lParam == 1 ? L"Test rectangle stopped." : L"Test rectangle failed. See Plotter status for details.";
                if (lParam != 2) setStatus(status, false);
                if (S.calibrationWindow) SetDlgItemTextW(S.calibrationWindow, IDC_TEST_RECT_STATUS, status);
                return 0;
            }
            size_t i = static_cast<size_t>(wParam);
            if (i >= S.layers.size()) {
                return 0;
            }
            Layer& l = S.layers[i];
            if (lParam == 0) {
                l.pct = 100;
                l.sent = l.total;
                l.status = L"submitted";
                setStatus(std::wstring(L"Submitted ") + baseName(l.path), false);
            } else if (lParam == 1) {
                l.status = L"stopped " + std::to_wstring(l.pct) + L"%";
                setStatus(L"Stopped " + baseName(l.path) + L" at " + std::to_wstring(l.pct) + L"%", false);
            } else {
                l.status = L"error";
            }
            invalidateRow(static_cast<int>(i));
            if (static_cast<int>(i) == S.shown) {
                InvalidateRect(S.preview, nullptr, FALSE);
            }
            return 0;
        }
        case kMsgError: {
            std::unique_ptr<std::string> s(reinterpret_cast<std::string*>(lParam));
            setStatus(wide(*s), true);
            return 0;
        }
        case kMsgUsbResult: {
            std::unique_ptr<std::string> result(reinterpret_cast<std::string*>(lParam));
            setStatus((wParam ? L"Plotter check passed: " : L"Plotter check failed: ") + wide(*result), !wParam);
            return 0;
        }
        case kMsgFinished:
            if (S.thread != nullptr) {
                WaitForSingleObject(S.thread, 2000);
                CloseHandle(S.thread);
                S.thread = nullptr;
            }
            for (size_t i = 0; i < S.layers.size(); ++i) {
                if (S.layers[i].status == L"queued") {
                    S.layers[i].status.clear();
                    S.layers[i].total = 0;
                    invalidateRow(static_cast<int>(i));
                }
            }
            setRunning(false);
            updateUsbStatus();
            showStatus();
            return 0;
        case WM_CLOSE:
            if (S.running) {
                g_stop = true;
                if (S.thread != nullptr) {
                    WaitForSingleObject(S.thread, 5000);
                }
            }
            DestroyWindow(hwnd);
            return 0;
        case WM_DESTROY:
            KillTimer(hwnd, kUsbStatusTimer);
            saveSettings();
            PostQuitMessage(0);
            return 0;
        default:
            break;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

}  // namespace

int WINAPI WinMain(HINSTANCE inst, HINSTANCE, LPSTR, int show) {
    S.inst = inst;
    INITCOMMONCONTROLSEX icc;
    icc.dwSize = sizeof(icc);
    icc.dwICC = ICC_STANDARD_CLASSES;
    InitCommonControlsEx(&icc);

    HDC screen = GetDC(nullptr);
    S.dpi = GetDeviceCaps(screen, LOGPIXELSX);
    ReleaseDC(nullptr, screen);
    loadSettings();

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
    pc.lpszClassName = L"RalvioliPlotPreview";
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
    wc.lpszClassName = L"RalvioliPlotter";
    RegisterClassExW(&wc);

    HWND hwnd = CreateWindowExW(0, L"RalvioliPlotter", L"Ralvioli Plotter", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                                CW_USEDEFAULT, CW_USEDEFAULT, dp(1250), dp(780), nullptr, nullptr, inst, nullptr);
    if (!hwnd) {
        return 1;
    }
    ShowWindow(hwnd, show);
    UpdateWindow(hwnd);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return static_cast<int>(msg.wParam);
}
