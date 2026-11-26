// Manages the print queue, preview, and worker lifecycle, and records progress for interrupted jobs.
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>
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
#include "VegaRender.hpp"
#include <array>
#include <filesystem>

namespace {

enum {
    ID_ADD = 101,
    ID_REMOVE,
    ID_FIT,
    ID_FILL,
    ID_FROM,
    ID_TO,
    ID_START = ID_TO + 2,
    ID_STOP,
    ID_TEST_USB,
    ID_SERVER_CONFIG,
    ID_LIST,
    ID_STATUS,
    ID_PRINT_AUTO,
    ID_CLEAN_HEADS,
    ID_ADVANCE_PAPER,
    ID_PAUSE,
    ID_SAVE_PRINT,
    ID_LOAD_PRINT,
    ID_RESET_VEGA,
    ID_PREVIOUS_BAND,
    ID_NEXT_BAND
};

const UINT kMsgProg = WM_APP + 1;      // wParam layer, lParam bytes sent
const UINT kMsgDone = WM_APP + 2;      // wParam layer, lParam 0 done / 1 stopped / 2 error
const UINT kMsgError = WM_APP + 3;     // lParam new std::string*
const UINT kMsgFinished = WM_APP + 4;
const UINT kMsgUsbResult = WM_APP + 5;
const UINT kMsgVegaRecovery = WM_APP + 6;
const UINT kMsgPrintStatus = WM_APP + 7;
const UINT kMsgHeadCleanDone = WM_APP + 8;
const UINT kMsgPauseState = WM_APP + 9;
const UINT kMsgReconnect = WM_APP + 10;
const UINT kMsgSaved = WM_APP + 11;
const UINT kMsgBandCursor = WM_APP + 12;
const UINT_PTR kUsbStatusTimer = 1;
const UINT_PTR kPreviewPanTimer = 2;

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
    bool mergerExport = false;
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

struct QueueDrawing {
    size_t index;
    std::vector<rp::DItem> items;
    double offset;
    double first;
    double last;
};

struct Slot {
    HWND hwnd;
    int width;
    int height;
    int gapAfter;
};

struct Settings {
    UINT vid = 0x135D;
    UINT pid = 0xF001;
    int pipe = 1;
    int pipeIn = 129;
    double lineWidthMm = 2.0;
    double textWidthMm = 0.0;
    double labelSizePercent = 100.0;
    double rightMarginMm = 55;
    double paperWidthCm = 195;
    double spacingMm = 10;
    double shift = 0;
    vega::HeadLayout heads;
    int rotation = 0;
    bool bidi = false;
    int timeoutMs = 2000;
    std::wstring backend;
    std::wstring ini;
};

struct ThreadArg;

// The captured firing lane (slot 0) drives the physical head labeled 2 on this plotter.
constexpr std::array<unsigned, vega::headSlots> kPhysicalHeadForSlot{2, 1, 3, 4};

enum class IndicatorTone { Neutral, Good, Fault };

using VegaStatus = int64_t;
constexpr VegaStatus kVegaPaperSensor = 1LL << 16;
constexpr VegaStatus kVegaInkUnexpected = 1LL << 17;
constexpr int kVegaErrorShift = 18;
constexpr int kVegaInkWordShift = kVegaErrorShift + 16;
constexpr unsigned kNoBand = UINT_MAX;

struct BandCursor {
    int layer;
    unsigned first, count, natural, selected;
    bool needsOrigin;
};

struct State {
    HINSTANCE inst = nullptr;
    HWND hwnd = nullptr;
    HWND list = nullptr;
    HWND preview = nullptr;
    HWND editFrom = nullptr;
    HWND editTo = nullptr;
    HWND status = nullptr;
    HWND usbStatus = nullptr;
    HWND totalLength = nullptr;
    HWND toolsLabel = nullptr;
    HWND calibrationWindow = nullptr;
    HWND btnAdd = nullptr;
    HWND btnRemove = nullptr;
    HWND btnStart = nullptr;
    HWND printAuto = nullptr;
    HWND btnAdvancePaper = nullptr;
    HWND btnPause = nullptr;
    HWND btnSavePrint = nullptr;
    HWND btnLoadPrint = nullptr;
    HWND btnPreviousBand = nullptr;
    HWND btnNextBand = nullptr;
    std::optional<BandCursor> bandCursor;
    HWND btnResetVega = nullptr;
    bool resetPending = false;
    bool waitingForReconnect = false;
    bool vegaSession = false;
    bool savingPrint = false;
    HWND btnCleanHeads = nullptr;
    bool maintenanceRunning = false;
    HWND btnStop = nullptr;
    HWND btnTestUsb = nullptr;
    HWND btnServerConfig = nullptr;
    HWND indicators[7]{};
    IndicatorTone indicatorTones[7]{};
    HBRUSH goodIndicator = nullptr;
    HBRUSH faultIndicator = nullptr;
    HANDLE connectionThread = nullptr;
    std::unique_ptr<ThreadArg> pendingPrint;
    bool reconnectingAfterPrint = false;
    HANDLE disconnect = nullptr;
    VegaStatus statusWord = -1;
    bool recoveryRequired = false;
    cal::Factors calibration;
    HFONT font = nullptr;
    HFONT bold = nullptr;
    int dpi = 96;
    std::array<std::vector<Slot>, 3> controlRows;
    size_t controlRow = 0;
    int toolbarWidth = 0;
    Settings cfg;
    std::vector<Layer> layers;
    std::vector<std::optional<rm::Box>> boxes;
    rm::Cache cache;
    // the layer shown in the preview
    int shown = -1;
    bool viewOk = false;
    std::optional<rm::Box> box;
    std::vector<QueueDrawing> queueDrawings;
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
    UINT panButton = 0;
    int rangeDrag = 0;
    int rangeDragOffset = 0;
    POINT panLast{0, 0};
    std::wstring lastDir;
    bool running = false;
    HANDLE thread = nullptr;
};

State S;
std::atomic<bool> g_stop(false);
std::atomic<bool> g_resetOnDisconnect(false);
std::atomic<bool> g_pause(false);
std::atomic<unsigned> g_seekBand{kNoBand};
std::atomic<bool> g_reconnect(false);
std::atomic<bool> g_savePrint(false);
std::filesystem::path g_savePrintPath; // Published to the worker by g_savePrint.
std::atomic<bool> g_advancePaper(false);
std::atomic<bool> g_paperStopPending(false);
enum class MaintenanceAction { None, CleanHeads, AdvancePaper };
std::atomic<MaintenanceAction> g_maintenance{MaintenanceAction::None};

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

vega::FeedRange configuredFeedRange(const rm::Box& bounds, double calibration, double from, double to) {
    return vega::feedRange(bounds, calibration, from, to, S.cfg.heads.bandRows());
}

std::optional<double> paperUnits() {
    return std::max(0.0, S.cfg.paperWidthCm - S.cfg.rightMarginMm / 10) * rm::kUnitsPerCm;
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
    S.cfg.paperWidthCm = positiveNumber(iniText(L"WidthCm", L"195")).value_or(195);
    S.cfg.vid = static_cast<UINT>(std::wcstoul(iniText(L"Vid", L"135D").c_str(), nullptr, 16));
    S.cfg.pid = static_cast<UINT>(std::wcstoul(iniText(L"Pid", L"F001").c_str(), nullptr, 16));
    S.cfg.pipe = static_cast<int>(std::wcstol(iniText(L"Pipe", L"1").c_str(), nullptr, 10));
    S.cfg.pipeIn = GetPrivateProfileIntW(L"Plotter", L"PipeIn", 129, S.cfg.ini.c_str());
    S.cfg.lineWidthMm = std::clamp(positiveNumber(iniText(L"LineWidthMm", L"2")).value_or(2.0), 0.01, 5.0);
    const auto textOffset = iniText(L"TextStrokeOffsetMm", L"0");
    wchar_t* textEnd = nullptr;
    const double offset = std::wcstod(textOffset.c_str(), &textEnd);
    if (textEnd != textOffset.c_str() && *textEnd == L'\0' && offset >= -5 && offset <= 5)
        S.cfg.textWidthMm = offset;
    const auto labelSize = iniText(L"LabelSizePercent", L"100");
    wchar_t* labelEnd = nullptr;
    const double percent = std::wcstod(labelSize.c_str(), &labelEnd);
    if (labelEnd != labelSize.c_str() && *labelEnd == L'\0' && percent >= 0 && percent <= 1000)
        S.cfg.labelSizePercent = percent;
    S.cfg.timeoutMs = std::clamp(std::wcstol(iniText(L"TimeoutMs", L"2000").c_str(), nullptr, 10), 100L, 60000L);
    S.cfg.backend = iniText(L"Backend", L"Auto");
    S.cfg.rightMarginMm = std::max(0.0, std::wcstod(iniText(L"RightMarginMm", L"55").c_str(), nullptr));
    S.cfg.spacingMm = std::max(0.0, std::wcstod(iniText(L"SpacingMm", L"10").c_str(), nullptr));
    const double shift = std::wcstod(iniText(L"ShiftMm", L"0").c_str(), nullptr);
    S.cfg.shift = std::isfinite(shift) && shift >= -32768 && shift <= 32767 ? shift : 0;
    const auto headMask = GetPrivateProfileIntW(L"Plotter", L"HeadMask", 1, S.cfg.ini.c_str());
    if (headMask > 0 && headMask < (1u << vega::headSlots)) S.cfg.heads.mask = headMask;
    for (unsigned slot = 0; slot < vega::headSlots; ++slot) {
        const auto key = L"Head" + std::to_wstring(slot + 1) + L"OffsetMm";
        const auto value = iniText(key.c_str(), L"");
        wchar_t* end = nullptr;
        const double offset = std::wcstod(value.c_str(), &end);
        if (end != value.c_str() && *end == L'\0' && std::isfinite(offset) && std::abs(offset) <= 327.67)
            S.cfg.heads.offsetMm[slot] = offset;
    }
    const UINT rotation = GetPrivateProfileIntW(L"Plotter", L"Rotation", 0, S.cfg.ini.c_str());
    S.cfg.rotation = rotation == 90 || rotation == 180 ? rotation : 0;
    S.cfg.bidi = GetPrivateProfileIntW(L"Plotter", L"Bidi", 0, S.cfg.ini.c_str()) != 0;
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

// ---------------------------------------------------------------- status and boxes
std::wstring plotterFault(VegaStatus word) {
    if (word < 0) return {};
    const auto code = static_cast<unsigned>((word >> kVegaErrorShift) & 0xffff);
    switch (code) {
        case 0: return (word & 0xffff) == 0xffff ? L"VEGA cover open." : L"";
        case 1:
        case 2: return L"VEGA paper tension roller fault.";
        case 3: return L"VEGA paper encoder timeout.";
        case 5: return L"VEGA paper jam sensor fault.";
        case 7: return L"VEGA carriage fault.";
        case 9: return L"VEGA paper encoder mismatch.";
        default: return L"VEGA controller error " + std::to_wstring(code) + L".";
    }
}

std::wstring inkStatusWarning(VegaStatus word) {
    if (word < 0 || !(word & kVegaInkUnexpected)) return {};
    wchar_t message[80];
    std::swprintf(message, std::size(message), L"VEGA sent an unexpected ink signal (0x%04X).",
                  static_cast<unsigned>((word >> kVegaInkWordShift) & 0xffff));
    return message;
}

std::wstring paperStatusWarning(VegaStatus word) {
    return word >= 0 && (word & kVegaPaperSensor) ? L"VEGA paper sensor tripped." : L"";
}

void renderStatus() {
    std::wstring text = S.statusText;
    for (const auto& fault : {plotterFault(S.statusWord), inkStatusWarning(S.statusWord),
                             paperStatusWarning(S.statusWord)}) {
        if (fault.empty() || text.find(fault) != std::wstring::npos) continue;
        if (!text.empty()) text += L"  |  ";
        text += fault;
    }
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
        case UsbPresence::Present: text = L"Algotex USB device detected"; break;
        case UsbPresence::Missing: text = L"Printer: USB device not detected"; break;
        default: text = L"Printer: USB status unavailable"; break;
    }
    SetWindowTextW(S.usbStatus, text);
}

double drawingRotation(const Layer& layer) {
    if (S.cfg.rotation != 90) return S.cfg.rotation;
    const auto width = paperUnits();
    if (!width || *width <= 0) return 0;
    const auto& piece = rm::loadRotated(S.cache, layer.path, 0, kUnitMm[layer.unit], layer.noHeader);
    const auto box = rm::boxOf(piece.ops);
    return box && box->y1 * S.calibration.across > *width &&
                   (box->x1 - box->x0) * S.calibration.across <= *width ? 90 : 0;
}

std::optional<rm::Box> layerBox(const Layer& l, std::string& error) {
    try {
        const rm::Piece& piece = rm::loadRotated(S.cache, l.path, drawingRotation(l), kUnitMm[l.unit], l.noHeader);
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

std::wstring formatLength(double units) {
    wchar_t text[48];
    std::swprintf(text, std::size(text), L"%.2f m", units * S.calibration.feed * (kHpglOutputStepMm / 1000));
    return text;
}

double selectedLength(size_t index) {
    if (index >= S.layers.size() || index >= S.boxes.size() || !S.boxes[index]) return 0;
    const auto& layer = S.layers[index];
    if (layer.to <= layer.from) return 0;
    const auto range = configuredFeedRange(*S.boxes[index], S.calibration.feed, layer.from, layer.to);
    return std::max(0.0, range.last - range.first);
}

void layoutAll(int w, int h);

void updateTotalLength() {
    double total = 0;
    for (size_t i = 0; i < S.layers.size(); ++i) total += selectedLength(i);
    const auto text = L"Total length  " + formatLength(total);
    auto& slot = S.controlRows[0][2];
    const int width = textWidth(text.c_str(), S.bold);
    SetWindowTextW(S.totalLength, text.c_str());
    if (slot.width != width) {
        slot.width = width;
        RECT client{};
        GetClientRect(S.hwnd, &client);
        layoutAll(client.right, client.bottom);
    }
}

void showStatus() {
    updateTotalLength();
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
        const Layer& layer = S.layers[n];
        std::wstring line = std::to_wstring(n + 1) + L". " +
            (layer.mergerExport ? L"Ralvioli Merger Export" : baseName(layer.path));
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
bool showingQueue() {
    return SendMessageW(S.printAuto, BM_GETCHECK, 0, 0) == BST_CHECKED;
}

void rebuildView() {
    S.items.clear();
    S.queueDrawings.clear();
    S.box.reset();
    S.viewOk = false;
    S.message.clear();
    S.shown = selectedIndex();
    const bool queue = showingQueue();
    double nextStart = 0;
    for (size_t i = 0; i < S.layers.size(); ++i) {
        if (!queue && static_cast<int>(i) != S.shown) continue;
        const Layer& layer = S.layers[i];
        if (queue && (layer.status == L"done" || layer.to <= layer.from)) continue;
        try {
            const rm::Req request{layer.path, drawingRotation(layer), kUnitMm[layer.unit], layer.noHeader};
            const auto placed = rm::placeLayers(S.cache, {request}, paperUnits());
            const rm::Piece& piece = placed.front();
            const auto box = rm::boxOf(piece.ops);
            const auto stream = rp::makeStream(piece, kHpglOutputStepMm, S.calibration);
            auto items = rp::buildDrawing(piece, stream);
            if (!box || items.empty()) continue;
            const double low = box->y0 * S.calibration.across;
            const double high = box->y1 * S.calibration.across;
            if (queue) {
                const auto range = configuredFeedRange(*box, S.calibration.feed, layer.from, layer.to);
                if (range.last <= range.first) continue;
                const double offset = nextStart - range.first * S.calibration.feed;
                const double end = offset + range.last * S.calibration.feed;
                if (S.box) {
                    S.box->x1 = end;
                    S.box->y0 = std::min(S.box->y0, low);
                    S.box->y1 = std::max(S.box->y1, high);
                } else {
                    S.box = rm::Box{nextStart, low, end, high};
                }
                S.queueDrawings.push_back({i, std::move(items), offset, nextStart, end});
                nextStart = end + S.cfg.spacingMm / kHpglOutputStepMm;
            } else {
                S.box = rm::Box{box->x0 * S.calibration.feed, low, box->x1 * S.calibration.feed, high};
                S.items = std::move(items);
            }
        } catch (const std::exception& e) {
            S.message = std::string("Cannot show file ") + std::to_string(i + 1) + ": " + e.what();
        }
        rm::warnings().clear();
    }
    S.viewOk = S.box.has_value();
    if (!S.viewOk && S.message.empty() && !S.layers.empty())
        S.message = queue ? "No printable ranges in the queue." : "Nothing to draw in this file.";
    InvalidateRect(S.preview, nullptr, FALSE);
    showStatus();
}

double fittedScale() {
    const double width = S.box->x1 - S.box->x0;
    const double height = S.box->y1 - S.box->y0;
    const int margin = dp(10);
    return clampScale(std::min((S.cw - 2 * margin) / std::max(width * 1.06 + 2, 1.0),
                               (S.ch - 2 * margin - bottomPad()) / std::max(height * 1.10 + 2, 1.0)));
}

void fitView() {
    if (!S.box) return;
    S.vx = S.box->x0 - ((S.box->x1 - S.box->x0) * 0.03 + 1);
    S.vy = S.box->y0 - ((S.box->y1 - S.box->y0) * 0.05 + 1);
    S.vs = fittedScale();
}

bool previewZoomed() {
    return S.viewOk && S.box && !S.autoFit && S.vs > fittedScale() * 1.001;
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

void fitAll() {
    S.autoFit = true;
    InvalidateRect(S.preview, nullptr, FALSE);
}

void fillView() {
    ensureView();
    if (!S.viewOk || !S.box) return;
    const int width = S.cw - 2 * dp(10);
    const int height = S.ch - 2 * dp(10) - bottomPad();
    if (width <= 0 || height <= 0) return;

    const auto& box = *S.box;
    const double spanX = box.x1 - box.x0;
    const double spanY = box.y1 - box.y0;
    const double scaleX = spanX > 0 ? width / spanX : 0;
    const double scaleY = spanY > 0 ? height / spanY : 0;
    S.vs = clampScale(std::max(scaleX, scaleY));
    S.vx = (box.x0 + box.x1 - width / S.vs) / 2;
    S.vy = (box.y0 + box.y1 - height / S.vs) / 2;
    S.autoFit = false;
    InvalidateRect(S.preview, nullptr, FALSE);
}

void startPan(int x, int y, UINT button) {
    ensureView();
    if (!S.viewOk || S.rangeDrag || S.panButton) return;
    S.autoFit = false;
    S.panButton = button;
    KillTimer(S.preview, kPreviewPanTimer);
    S.panLast = POINT{x, y};
    SetCapture(S.preview);
}

void onMove(int x, int y) {
    if (S.panButton) {
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

struct PrintSource {
    std::string bytes;
    int unit;
    bool noHeader;
};

struct Job {
    int index = -1;
    rm::Piece piece;
    rp::Stream stream;
    rp::Plan plan;
    std::wstring title;
    std::optional<vega::FeedRange> range;
    std::optional<PrintSource> source;
};

bool startVegaJobs(std::vector<Job> jobs, double paperWidth, cal::Factors calibration, HWND owner);

void printCalibrationRectangle(HWND dialog) {
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

    // Replacement factors use measurements from a rectangle printed at unity scale.
    const double across = *widthCm * rm::kUnitsPerCm;
    const double feed = *lengthCm * rm::kUnitsPerCm;
    if (!(across >= 1.0 && across <= 10000000.0 && feed >= 1.0 && feed <= 10000000.0)) {
        MessageBoxW(dialog, L"The test rectangle is outside the supported size range.",
                    L"Test rectangle", MB_OK | MB_ICONWARNING);
        return;
    }

    const double printableWidth = paperUnits().value_or(0);
    if (across + S.cfg.lineWidthMm / kHpglOutputStepMm > printableWidth) {
        SetDlgItemTextW(dialog, IDC_TEST_RECT_STATUS, L"The test rectangle exceeds the VEGA printable width.");
        return;
    }
    Job job;
    job.index = -1;
    job.title = L"Calibration test rectangle";
    rm::Op move(rm::K::Pts);
    move.verb = "PU";
    move.pts = {{0, 0}};
    job.piece.ops.push_back(std::move(move));
    rm::Op draw(rm::K::Pts);
    draw.verb = "PD";
    draw.pts = {{feed, 0}, {feed, across}, {0, across}, {0, 0}};
    job.piece.ops.push_back(std::move(draw));
    job.stream = rp::makeStream(job.piece, kHpglOutputStepMm);
    job.plan = rp::makePlan(job.stream, 0, 100);
    std::vector<Job> jobs;
    jobs.push_back(std::move(job));
    if (startVegaJobs(std::move(jobs), printableWidth, {}, dialog)) {
        SetDlgItemTextW(dialog, IDC_TEST_RECT_STATUS, L"Preparing an uncalibrated VEGA test rectangle...");
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
                printCalibrationRectangle(hwnd);
                return TRUE;
            case IDC_FEED_RESET:
                factors->feed = 1.0;
                SetDlgItemTextW(hwnd, IDC_FEED_EXPECTED, L"1");
                SetDlgItemTextW(hwnd, IDC_FEED_ACTUAL, L"1");
                break;
            case IDC_ACROSS_RESET:
                factors->across = 1.0;
                SetDlgItemTextW(hwnd, IDC_ACROSS_EXPECTED, L"1");
                SetDlgItemTextW(hwnd, IDC_ACROSS_ACTUAL, L"1");
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
                auto result = cal::calculate(1.0, expected.value_or(0), actual.value_or(0), false, next);
                if (result == cal::Result::NeedsConfirm) {
                    if (MessageBoxW(hwnd, L"The correction exceeds 1%. Apply it?", L"Paper calibration",
                                    MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) != IDYES) {
                        return TRUE;
                    }
                    result = cal::calculate(1.0, *expected, *actual, true, next);
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
    if (msg == WM_INITDIALOG) {
        for (int edit : {IDC_FEED_EXPECTED, IDC_FEED_ACTUAL, IDC_ACROSS_EXPECTED, IDC_ACROSS_ACTUAL}) {
            SendDlgItemMessageW(hwnd, edit, EM_SETLIMITTEXT, 63, 0);
            SetDlgItemTextW(hwnd, edit, L"1");
        }
    }
    SetDlgItemTextW(hwnd, IDC_FEED_FACTOR, calibrationText(factors->feed, true).c_str());
    SetDlgItemTextW(hwnd, IDC_ACROSS_FACTOR, calibrationText(factors->across, true).c_str());
    return TRUE;
}

void showCalibration(HWND owner) {
    if (S.running) {
        return;
    }
    cal::Factors factors = S.calibration;
    INT_PTR result = DialogBoxParamW(S.inst, MAKEINTRESOURCEW(IDD_CALIBRATION), owner,
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
        for (size_t i = 0; i < S.layers.size(); ++i) computeBox(i);
        InvalidateRect(S.list, nullptr, FALSE);
        rebuildView();
        fitAll();
    }
}

rm::Box previewFeedBox() {
    const auto& box = *S.box;
    return {box.x0 / S.calibration.feed, box.y0, box.x1 / S.calibration.feed, box.y1};
}

int rangeMarkerX(double feed) {
    return clampInt(dp(10) + (feed * S.calibration.feed - S.vx) * S.vs);
}

RECT rangeHandle(int marker, int x) {
    const int y = dp(marker == 1 ? 42 : 64);
    return {x - dp(7), y, x + dp(7), y + dp(18)};
}

int hitRangeHandle(POINT mouse) {
    ensureView();
    if (showingQueue() || S.running || !S.viewOk || !S.box || S.shown < 0) return 0;
    const auto& layer = S.layers[static_cast<size_t>(S.shown)];
    const auto box = previewFeedBox();
    if (box.x1 <= box.x0) return 0;
    const auto range = configuredFeedRange(box, S.calibration.feed, layer.from, layer.to);
    const RECT from = rangeHandle(1, rangeMarkerX(range.first));
    const RECT to = rangeHandle(2, rangeMarkerX(range.last));
    if (PtInRect(&from, mouse)) return 1;
    if (PtInRect(&to, mouse)) return 2;
    return 0;
}

void dragRangeHandle(int mouseX) {
    if (S.running || !S.box || S.shown < 0) return;
    auto& layer = S.layers[static_cast<size_t>(S.shown)];
    const auto box = previewFeedBox();
    const double span = box.x1 - box.x0;
    if (span <= 0) return;
    const double feed = (S.vx + (mouseX - S.rangeDragOffset - dp(10)) / S.vs) / S.calibration.feed;
    const double requested = std::clamp(100 * (feed - box.x0) / span, 0.0, 100.0);
    const double edge = requested == 100 ? box.x1 :
        configuredFeedRange(box, S.calibration.feed, requested, requested).first;
    const double snapped = std::clamp(100 * (edge - box.x0) / span, 0.0, 100.0);
    double& value = S.rangeDrag == 1 ? layer.from : layer.to;
    value = S.rangeDrag == 1 ? std::min(snapped, layer.to) : std::max(snapped, layer.from);
    S.updating = true;
    SetWindowTextW(S.rangeDrag == 1 ? S.editFrom : S.editTo, wide(rm::num(value)).c_str());
    S.updating = false;
    invalidateRow(S.shown);
    updateTotalLength();
    InvalidateRect(S.preview, nullptr, FALSE);
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
    double feedOffset = 0;
    auto plotX = [&](double x) { return X(feedOffset + x * S.calibration.feed); };
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

    HGDIOBJ oldBrush = SelectObject(dc, GetStockObject(NULL_BRUSH));
    HGDIOBJ oldPen = GetCurrentObject(dc, OBJ_PEN);
    SetBkMode(dc, TRANSPARENT);
    SetTextAlign(dc, TA_BASELINE | TA_LEFT);
    std::vector<POINT> run;
    const auto drawRange = [&](const auto& items, int x0, int x1, COLORREF ink, int penWidth) {
        if (x1 <= x0) return;
        const int saved = SaveDC(dc);
        if (!saved) return;
        IntersectClipRect(dc, x0, 0, x1, h);
        HPEN pen = CreatePen(PS_SOLID, penWidth, ink);
        SelectObject(dc, pen);
        SetTextColor(dc, ink);
        const double textMm = plotTextWidth(S.cfg.lineWidthMm, S.cfg.textWidthMm);
        const int textPixels = std::max(1, static_cast<int>(std::lround(penWidth * textMm / S.cfg.lineWidthMm)));
        HPEN textPen = S.cfg.labelSizePercent > 0 && textMm > 0 ? CreatePen(PS_SOLID, textPixels, ink) : nullptr;
        for (const auto& item : items) {
            if (item.type == 0) {
                run.clear();
                for (const auto& p : item.pts) run.push_back({plotX(p.x), plotY(p.y)});
                if (run.size() > 1) Polyline(dc, run.data(), static_cast<int>(run.size()));
            } else if (item.type == 1) {
                const double r = item.r;
                Ellipse(dc, plotX(item.c.x-r), plotY(item.c.y+r), plotX(item.c.x+r), plotY(item.c.y-r));
            } else if (textPen) {
                const double height = std::clamp(std::abs(item.height) * sc * (S.cfg.labelSizePercent / 100), 1.0, 4096.0);
                drawPlotText(dc, plotX(item.c.x), plotY(item.c.y), item.text, height,
                             -std::atan2(item.dy, item.dx), textPen);
            }
        }
        RestoreDC(dc, saved);
        DeleteObject(pen);
        if (textPen) DeleteObject(textPen);
    };
    const auto restoreDrawing = [&] {
        SelectObject(dc, oldBrush);
        SelectObject(dc, oldPen);
    };
    if (showingQueue()) {
        for (const auto& drawing : S.queueDrawings) {
            const Layer& layer = S.layers[drawing.index];
            const int left = std::clamp(X(drawing.first), 0, w);
            const int right = std::clamp(X(drawing.last) + 1, left, w);
            const int printed = layer.pct >= 100 ? right : std::clamp(
                X(drawing.first + (drawing.last - drawing.first) * layer.pct / 100.0), left, right);
            feedOffset = drawing.offset;
            drawRange(drawing.items, left, printed, kBarGreen, dp(2));
            drawRange(drawing.items, printed, right, kColors[drawing.index % kColorCount], 1);
        }
        restoreDrawing();
        return;
    }

    const Layer& layer = S.layers[static_cast<size_t>(S.shown)];
    const COLORREF color = kColors[static_cast<size_t>(S.shown) % kColorCount];
    const auto geometry = previewFeedBox();
    const auto range = configuredFeedRange(geometry, S.calibration.feed, layer.from, layer.to);
    const int left = std::clamp(plotX(range.first), 0, w);
    const int right = layer.to <= layer.from ? left : std::clamp(plotX(range.last), left, w);
    const int printed = std::clamp(plotX(range.first + (range.last - range.first) * layer.pct / 100.0),
                                   left, right);
    const bool partial = layer.from > 0 || layer.to < 100;
    drawRange(S.items, 0, partial ? left : 0, kFaint, 1);
    drawRange(S.items, partial ? left : 0, printed, kBarGreen, dp(2));
    drawRange(S.items, printed, partial ? right : w, color, 1);
    drawRange(S.items, partial ? right : w, w, kFaint, 1);
    HPEN guide = CreatePen(PS_DASH, 1, RGB(95, 102, 111));
    HGDIOBJ previous = SelectObject(dc, guide);
    for (const int x : {rangeMarkerX(range.first), rangeMarkerX(range.last)}) {
        if (x < 0 || x >= w) continue;
        MoveToEx(dc, x, 0, nullptr);
        LineTo(dc, x, h - bp);
    }
    SelectObject(dc, previous);
    DeleteObject(guide);
    for (int marker : {1, 2}) {
        const int x = rangeMarkerX(marker == 1 ? range.first : range.last);
        if (x < 0 || x >= w) continue;
        const RECT handle = rangeHandle(marker, x);
        HBRUSH fill = CreateSolidBrush(marker == S.rangeDrag ? kBarGreen : RGB(32, 83, 146));
        FillRect(dc, &handle, fill);
        DeleteObject(fill);
        RECT arrows = handle;
        SetTextColor(dc, RGB(255, 255, 255));
        SetTextAlign(dc, TA_TOP | TA_LEFT);
        DrawTextW(dc, marker == 1 ? L"▸" : L"◂", -1, &arrows,
                  DT_CENTER | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
    }
    restoreDrawing();

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

constexpr POINT previewDirections[]{{-1, 0}, {1, 0}, {0, -1}, {0, 1}};

RECT previewArrow(int direction) {
    const int inset = dp(8);
    const int size = dp(30);
    const int bottom = S.ch - bottomPad();
    const POINT delta = previewDirections[direction];
    const int x = delta.x < 0 ? inset : delta.x > 0 ? S.cw - size - inset : (S.cw - size) / 2;
    const int y = delta.y < 0 ? inset : delta.y > 0 ? bottom - size - inset : (bottom - size) / 2;
    return {x, y, x + size, y + size};
}

int hitPreviewArrow(POINT mouse) {
    if (!previewZoomed() || S.rangeDrag || S.panButton) return -1;
    for (int i = 0; i < static_cast<int>(std::size(previewDirections)); ++i) {
        const RECT arrow = previewArrow(i);
        if (PtInRect(&arrow, mouse)) return i;
    }
    return -1;
}

void drawPreviewArrows(HDC dc) {
    if (!previewZoomed()) return;
    const int saved = SaveDC(dc);
    if (!saved) return;
    SelectObject(dc, GetStockObject(DC_BRUSH));
    SelectObject(dc, GetStockObject(NULL_PEN));
    for (int i = 0; i < static_cast<int>(std::size(previewDirections)); ++i) {
        const RECT arrow = previewArrow(i);
        SetDCBrushColor(dc, RGB(42, 58, 75));
        RoundRect(dc, arrow.left, arrow.top, arrow.right, arrow.bottom, dp(7), dp(7));
        const POINT center{(arrow.left + arrow.right) / 2, (arrow.top + arrow.bottom) / 2};
        const POINT d = previewDirections[i];
        const int tip = dp(7), base = dp(5), half = dp(6);
        const POINT triangle[]{{center.x + d.x * tip, center.y + d.y * tip},
                               {center.x - d.x * base - d.y * half, center.y - d.y * base + d.x * half},
                               {center.x - d.x * base + d.y * half, center.y - d.y * base - d.x * half}};
        SetDCBrushColor(dc, RGB(255, 255, 255));
        Polygon(dc, triangle, 3);
    }
    RestoreDC(dc, saved);
}

void drawBandCursor(HDC dc, int width, int height) {
    if (!S.bandCursor || !g_pause || width < dp(140)) return;
    const auto& cursor = *S.bandCursor;
    const int saved = SaveDC(dc);
    if (!saved) return;
    const COLORREF blue = RGB(32, 83, 146);
    const auto fraction = static_cast<double>(cursor.selected) / cursor.count;
    if (S.viewOk && cursor.layer >= 0) {
        std::optional<double> marker;
        if (showingQueue()) {
            for (const auto& drawing : S.queueDrawings) {
                if (drawing.index == static_cast<size_t>(cursor.layer)) {
                    marker = drawing.first + (drawing.last - drawing.first) * fraction;
                    break;
                }
            }
        } else if (S.shown == cursor.layer) {
            const auto& layer = S.layers[static_cast<size_t>(cursor.layer)];
            const auto range = configuredFeedRange(previewFeedBox(), S.calibration.feed, layer.from, layer.to);
            marker = (range.first + (range.last - range.first) * fraction) * S.calibration.feed;
        }
        if (marker) {
            const int x = clampInt(dp(10) + (*marker - S.vx) * S.vs);
            HPEN pen = CreatePen(PS_SOLID, dp(2), blue);
            const auto old = SelectObject(dc, pen);
            MoveToEx(dc, x, 0, nullptr);
            LineTo(dc, x, height - bottomPad());
            SelectObject(dc, old);
            DeleteObject(pen);
        }
    }
    const int panelWidth = std::min(width - dp(16), dp(220));
    const int left = (width - panelWidth) / 2;
    RECT panel{left, dp(8), left + panelWidth, dp(46)};
    FillRect(dc, &panel, static_cast<HBRUSH>(GetStockObject(WHITE_BRUSH)));
    SelectObject(dc, S.bold);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, blue);
    SetTextAlign(dc, TA_TOP | TA_LEFT);
    RECT text{panel.left, panel.top, panel.right, dp(31)};
    const auto label = cursor.selected == cursor.count
        ? L"End of print" : L"Resume at band " + std::to_wstring(cursor.selected + 1) +
                           L" of " + std::to_wstring(cursor.count);
    DrawTextW(dc, label.c_str(), -1, &text, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    RECT track{left + dp(8), dp(34), left + panelWidth - dp(8), dp(40)};
    FillRect(dc, &track, GetSysColorBrush(COLOR_3DFACE));
    const int x = track.left + static_cast<int>(std::lround((track.right - track.left) * fraction));
    RECT pointer{x - dp(2), track.top - dp(2), x + dp(2), track.bottom + dp(2)};
    HBRUSH marker = CreateSolidBrush(blue);
    FillRect(dc, &pointer, marker);
    DeleteObject(marker);
    RestoreDC(dc, saved);
}

LRESULT CALLBACK PreviewProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_DROPFILES:
            return SendMessageW(S.hwnd, msg, wParam, lParam);
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
                drawBandCursor(mem, w, h);
                drawPreviewArrows(mem);
                BitBlt(dc, 0, 0, w, h, mem, 0, 0, SRCCOPY);
                SelectObject(mem, old);
                DeleteObject(bmp);
                DeleteDC(mem);
            }
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_LBUTTONDOWN: {
            const POINT mouse{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            if (S.panButton || hitPreviewArrow(mouse) >= 0) return 0;
            S.rangeDrag = hitRangeHandle(mouse);
            if (S.rangeDrag) {
                const auto& layer = S.layers[static_cast<size_t>(S.shown)];
                const auto range = configuredFeedRange(previewFeedBox(), S.calibration.feed, layer.from, layer.to);
                S.rangeDragOffset = mouse.x - rangeMarkerX(S.rangeDrag == 1 ? range.first : range.last);
                SetFocus(hwnd);
                SetCapture(hwnd);
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            SetFocus(hwnd);
            startPan(mouse.x, mouse.y, WM_LBUTTONUP);
            return 0;
        }
        case WM_MBUTTONDOWN:
            SetFocus(hwnd);
            startPan(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam), WM_MBUTTONUP);
            return 0;
        case WM_LBUTTONUP:
        case WM_MBUTTONUP:
            if (S.rangeDrag && msg == WM_LBUTTONUP) {
                dragRangeHandle(GET_X_LPARAM(lParam));
                S.rangeDrag = 0;
                ReleaseCapture();
                InvalidateRect(hwnd, nullptr, FALSE);
            } else if (S.panButton == msg) {
                S.panButton = 0;
                ReleaseCapture();
            }
            return 0;
        case WM_CAPTURECHANGED:
            S.panButton = 0;
            S.rangeDrag = 0;
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        case WM_MOUSEMOVE:
            if (S.rangeDrag) dragRangeHandle(GET_X_LPARAM(lParam));
            else if (S.panButton) onMove(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
            else if (hitPreviewArrow({GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)}) >= 0)
                SetTimer(hwnd, kPreviewPanTimer, 25, nullptr);
            else KillTimer(hwnd, kPreviewPanTimer);
            return 0;
        case WM_TIMER: {
            if (wParam != kPreviewPanTimer) break;
            POINT mouse{};
            int arrow = -1;
            if (GetCursorPos(&mouse) && WindowFromPoint(mouse) == hwnd && ScreenToClient(hwnd, &mouse))
                arrow = hitPreviewArrow(mouse);
            if (arrow < 0) {
                KillTimer(hwnd, kPreviewPanTimer);
                return 0;
            }
            const POINT direction = previewDirections[arrow];
            S.vx += direction.x * dp(10) / S.vs;
            S.vy -= direction.y * dp(10) / S.vs;
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        case WM_MOUSEWHEEL: {
            POINT p{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            ScreenToClient(hwnd, &p);
            zoomAt(std::pow(1.2, GET_WHEEL_DELTA_WPARAM(wParam) / 120.0), p.x, p.y);
            return 0;
        }
        case WM_SETCURSOR:
            if (LOWORD(lParam) == HTCLIENT) {
                POINT pt{};
                GetCursorPos(&pt);
                ScreenToClient(hwnd, &pt);
                const auto resize = MAKEINTRESOURCEW(32644);
                SetCursor(LoadCursorW(nullptr, S.panButton ? kCursorMove :
                                       hitPreviewArrow(pt) >= 0 ? MAKEINTRESOURCEW(32649) :
                                       S.rangeDrag || hitRangeHandle(pt) ? resize : kCursorArrow));
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

void addFiles(const std::vector<std::wstring>& files, std::optional<int> mergerUnit = std::nullopt) {
    if (files.empty()) {
        return;
    }
    size_t first = S.layers.size();
    for (const std::wstring& f : files) {
        Layer l;
        l.path = utf8(f);
        l.mergerExport = mergerUnit.has_value();
        l.unit = mergerUnit ? *mergerUnit : rp::guessUnit(l.path);
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

void addDroppedFiles(HDROP drop) {
    struct DropHandle {
        HDROP handle;
        ~DropHandle() { DragFinish(handle); }
    } owned{drop};
    if (S.running) return;

    std::vector<std::wstring> files;
    const UINT count = DragQueryFileW(drop, 0xffffffffu, nullptr, 0);
    files.reserve(count);
    for (UINT i = 0; i < count; ++i) {
        const UINT length = DragQueryFileW(drop, i, nullptr, 0);
        if (!length) continue;
        std::wstring path(length + 1, L'\0');
        if (DragQueryFileW(drop, i, path.data(), length + 1) != length) continue;
        path.resize(length);
        const DWORD attributes = GetFileAttributesW(path.c_str());
        if (attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_DIRECTORY))
            files.push_back(std::move(path));
    }
    if (!files.empty()) addFiles(files);
    else setStatus(L"Drop one or more plot files into the queue.", true);
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
    if (msg == WM_DROPFILES) return SendMessageW(S.hwnd, msg, wParam, lParam);
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
    std::filesystem::path loadSaved;
    std::vector<Job> jobs;
    Settings cfg;
    HWND hwnd;
    HDC printer = nullptr;
    ~ThreadArg() { if (printer) DeleteDC(printer); }
    double paperWidth = 0;
    bool useCapturedSquare = false;
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

bool checkVega(Transport& usb, std::string& log, std::string& result, VegaStatus& statusWord,
               unsigned* bandsConsumed = nullptr, unsigned* lastCommand = nullptr) {
    log += usb.description() + "\n";
    // Capture frames 43, 46 and 48 show a direct status response on the command pipe.
    const std::string request = proto::frame(0x3F, proto::be16(0x1A));
    log += "Status request: " + hexBytes(request) + "\n";
    constexpr DWORD busyDelays[]{100, 200, 300, 500, 750, 1000};
    for (size_t attempt = 0;; ++attempt) {
        size_t sent = 0;
        const int code = usb.write(reinterpret_cast<const unsigned char*>(request.data()), request.size(), sent, result);
        if (code == 0 && sent == request.size()) break;
        if (code != 2 || sent != 0 || attempt == std::size(busyDelays)) {
            if (result.empty()) result = "Incomplete status request";
            return false;
        }
        log += "Status endpoint busy. Retrying status query.\n";
        Sleep(busyDelays[attempt]);
    }
    // A previous host can leave unread status bytes in the device queue.
    constexpr size_t payloadSize = 64;
    constexpr size_t frameSize = 8 + payloadSize + 2;
    std::string pending;
    std::string body;
    const ULONGLONG deadline = GetTickCount64() + 2500;
    for (int packet = 0; packet < 8 && GetTickCount64() < deadline; ++packet) {
        unsigned char buffer[512];
        size_t got = 0;
        if (!usb.read(buffer, sizeof(buffer), got, result)) return false;
        if (!got) continue;
        const std::string bytes(reinterpret_cast<const char*>(buffer), got);
        log += "Bulk IN: " + hexBytes(bytes) + "\n";
        pending += bytes;
        while (pending.size() >= 8) {
            size_t length = 0;
            if (!proto::checkHeader(pending.substr(0, 8), 0x1A, length, result) ||
                length != payloadSize) {
                pending.erase(0, 1);
                continue;
            }
            if (pending.size() < frameSize) break;
            body = pending.substr(8, payloadSize + 2);
            if (proto::checkBody(pending.substr(0, 8), body, result)) break;
            body.clear();
            pending.erase(0, 1);
        }
        if (!body.empty()) break;
    }
    if (body.empty()) {
        result = "No complete VEGA status reply received";
        return false;
    }
    const auto word = [&](size_t offset) {
        return (static_cast<unsigned char>(body[offset]) << 8) |
                static_cast<unsigned char>(body[offset + 1]);
    };
    const int state = word(0);
    if (bandsConsumed) *bandsConsumed = (static_cast<unsigned>(word(10)) << 16) | word(12);
    if (lastCommand) *lastCommand = word(40);
    // AlgoServer 0x421310 reads paper at byte 24 and the fault code in the high word at byte 48.
    const VegaStatus ink = word(8);
    // On this VEGA, bit 0x10 stayed clear with fresh ink and during successful prints.
    statusWord = state | ((ink & 0x10) ? kVegaInkUnexpected : 0) |
                 (word(24) ? kVegaPaperSensor : 0) | (VegaStatus(word(48)) << kVegaErrorShift) |
                 (ink << kVegaInkWordShift);
    result = "VEGA status reply verified (" + std::to_string(payloadSize) + " data bytes)";
    return true;
}

struct VegaPrintData {
    std::string record;
    std::string header;
    unsigned bandCount = 0;
    std::function<std::string(unsigned)> encodeBand;
    unsigned firstBand = 0;
};

VegaPrintData capturedSquare(double spacingMm) {
    constexpr unsigned packets[]{46, 34, 34, 34, 34, 34, 34, 48};
    const HMODULE module = GetModuleHandleW(nullptr);
    const HRSRC resource = FindResourceW(module, MAKEINTRESOURCEW(IDR_VEGA_SQUARE), RT_RCDATA);
    if (!resource || SizeofResource(module, resource) != 64 + 468 + 298 * vega::packetSize)
        throw std::runtime_error("The embedded XP square raster is missing or invalid.");
    const auto* cursor = static_cast<const char*>(LockResource(LoadResource(module, resource)));
    if (!cursor) throw std::runtime_error("Could not load the embedded XP square raster.");
    VegaPrintData data{vega::capturedSquareRecord(std::string_view(cursor, 64), spacingMm), std::string(cursor + 64, 468),
                       static_cast<unsigned>(std::size(packets)), {}};
    std::vector<std::string_view> bands;
    cursor += 532;
    for (const auto count : packets) {
        bands.emplace_back(cursor, count * vega::packetSize);
        cursor += bands.back().size();
    }
    data.encodeBand = [bands = std::move(bands)](unsigned index) { return std::string(bands.at(index)); };
    return data;
}

struct VegaPosition {
    // Setup and a partially transferred band cannot safely be replayed.
    enum Phase : unsigned { New, Setup, Bands, Finish };
    unsigned phase = New;
    unsigned next = 0;
    unsigned confirmed = 0;
    unsigned inFlight = 0;
    unsigned blankBefore = 0;
    unsigned selected = kNoBand;

    unsigned nextArtwork() const { return std::max(next, blankBefore); }
    unsigned selectedArtwork() const { return selected == kNoBand ? nextArtwork() : selected; }
    unsigned packedPhase() const { return phase | (selected == kNoBand ? 0 : (selected + 1) << 3); }
    unsigned packedFlight() const { return inFlight | (blankBefore << 16); }

    void unpackSelection() {
        selected = (phase >> 3) ? (phase >> 3) - 1 : kNoBand;
        phase &= 7;
        blankBefore = inFlight >> 16;
        inFlight &= 0xffff;
    }
};

struct SpoolSource {
    std::filesystem::path path;
    ~SpoolSource() { DeleteFileW(path.c_str()); }
};

struct PreparedVegaJob {
    VegaPrintData data;
    std::wstring title;
    std::optional<PrintSource> source;
};

constexpr uint32_t kMaxSavedSource = 512 * 1024 * 1024;

template<class T>
void spoolWrite(std::ostream& out, T number) {
    out.write(reinterpret_cast<const char*>(&number), sizeof(number));
}

template<class T>
void spoolRead(std::istream& in, T& number) {
    if (!in.read(reinterpret_cast<char*>(&number), sizeof(number)))
        throw std::runtime_error("Incomplete saved print.");
}

void writeSpoolBytes(std::ostream& out, std::string_view bytes) {
    spoolWrite(out, static_cast<uint32_t>(bytes.size()));
    out.write(bytes.data(), bytes.size());
}

std::string readSpoolBytes(std::istream& in, uint32_t limit) {
    uint32_t size = 0;
    spoolRead(in, size);
    if (size > limit) throw std::runtime_error("Invalid saved print record.");
    std::string bytes(size, '\0');
    if (!in.read(bytes.data(), size)) throw std::runtime_error("Incomplete saved print.");
    return bytes;
}

std::filesystem::path autoRecoveryPath(const Settings& cfg) {
    return std::filesystem::path(cfg.ini).parent_path() / L"autorecovery.pltsave";
}

using RecoveryEntry = std::array<uint32_t, 6>;

uint32_t recoveryChecksum(const RecoveryEntry& entry) {
    uint32_t hash = 2166136261u;
    for (size_t i = 0; i + 1 < entry.size(); ++i) hash = (hash ^ entry[i]) * 16777619u;
    return hash;
}

struct RecoveryJournal {
    HANDLE file = INVALID_HANDLE_VALUE;
    ~RecoveryJournal() { if (file != INVALID_HANDLE_VALUE) CloseHandle(file); }

    void append(unsigned job, const VegaPosition& position) const {
        RecoveryEntry entry{job, position.packedPhase(), position.next, position.confirmed, position.packedFlight(), 0};
        entry.back() = recoveryChecksum(entry);
        DWORD bytes = 0;
        if (!WriteFile(file, entry.data(), sizeof(entry), &bytes, nullptr) ||
            bytes != sizeof(entry) || !FlushFileBuffers(file))
            throw std::runtime_error("Could not update autorecovery.pltsave. Print stopped.");
    }

    bool removeOnClose() const {
        FILE_DISPOSITION_INFO disposition{TRUE};
        return SetFileInformationByHandle(file, FileDispositionInfo, &disposition, sizeof(disposition)) != FALSE;
    }
};

void saveVegaSpool(const std::filesystem::path& path, const Settings& cfg,
                   const std::vector<PreparedVegaJob>& jobs, size_t current,
                   const VegaPosition& position, bool journal = false) {
    auto temporary = path;
    temporary += L".tmp-" + std::to_wstring(GetCurrentProcessId());
    try {
        std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
        out.exceptions(std::ios::badbit | std::ios::failbit);
        out.write(journal ? "RVEGA05" : "RVEGA04", 7);
        spoolWrite(out, cfg.vid);
        spoolWrite(out, cfg.pid);
        spoolWrite(out, static_cast<unsigned>(cfg.bidi));
        spoolWrite(out, position.packedPhase());
        spoolWrite(out, position.next);
        spoolWrite(out, position.confirmed);
        spoolWrite(out, position.packedFlight());
        spoolWrite(out, static_cast<unsigned>(jobs.size() - current));
        for (size_t i = current; i < jobs.size(); ++i) {
            const auto& job = jobs[i];
            writeSpoolBytes(out, utf8(job.title));
            spoolWrite(out, job.source ? job.source->unit : -1);
            if (job.source) {
                if (job.source->bytes.size() > kMaxSavedSource)
                    throw std::runtime_error("The original plot file is too large to save.");
                spoolWrite(out, static_cast<unsigned>(job.source->noHeader));
                writeSpoolBytes(out, job.source->bytes);
            }
            writeSpoolBytes(out, job.data.record);
            writeSpoolBytes(out, job.data.header);
            spoolWrite(out, job.data.bandCount);
            const unsigned first = job.data.firstBand;
            spoolWrite(out, first);
            for (unsigned band = first; band < job.data.bandCount; ++band) {
                if (g_stop) throw std::runtime_error("Save canceled.");
                writeSpoolBytes(out, job.data.encodeBand(band));
            }
        }
        out.close();
        if (journal) {
            const HANDLE staging = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr,
                                               OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (staging == INVALID_HANDLE_VALUE)
                throw std::runtime_error("Could not open the automatic recovery file for flushing.");
            const bool flushed = FlushFileBuffers(staging) != FALSE;
            CloseHandle(staging);
            if (!flushed) throw std::runtime_error("Could not flush the automatic recovery file.");
        }
        if (g_stop || !MoveFileExW(temporary.c_str(), path.c_str(),
                                    MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
            throw std::runtime_error("Could not commit the saved print.");
    } catch (...) {
        DeleteFileW(temporary.c_str());
        throw;
    }
}

void loadVegaSpool(const std::filesystem::path& path, Settings& cfg,
                   std::vector<PreparedVegaJob>& jobs, VegaPosition& position) {
    auto source = std::make_shared<SpoolSource>();
    wchar_t snapshot[MAX_PATH]{};
    if (!GetTempFileNameW(std::filesystem::temp_directory_path().c_str(), L"plt", 0, snapshot))
        throw std::runtime_error("Could not create a temporary copy of the saved print.");
    source->path = snapshot;
    if (!CopyFileW(path.c_str(), source->path.c_str(), FALSE))
        throw std::runtime_error("Could not open the saved print.");
    std::ifstream in(source->path, std::ios::binary);
    char magic[7]{};
    if (!in.read(magic, sizeof(magic)))
        throw std::runtime_error("Could not open a valid saved print.");
    const std::string_view version(magic, sizeof(magic));
    const bool editable = version == "RVEGA04" || version == "RVEGA05";
    const bool journal = version == "RVEGA03" || version == "RVEGA05";
    if (version != "RVEGA01" && version != "RVEGA02" && !editable && !journal)
        throw std::runtime_error("Could not open a valid saved print.");
    unsigned vid, pid, bidi, count;
    spoolRead(in, vid);
    spoolRead(in, pid);
    spoolRead(in, bidi);
    spoolRead(in, position.phase);
    spoolRead(in, position.next);
    spoolRead(in, position.confirmed);
    spoolRead(in, position.inFlight);
    if (editable) position.unpackSelection();
    spoolRead(in, count);
    if (vid != cfg.vid || pid != cfg.pid)
        throw std::runtime_error("The saved print uses a different USB device configuration.");
    if (bidi > 1 || count == 0 || count > 65535 || position.phase > VegaPosition::Finish)
        throw std::runtime_error("Invalid saved print state.");
    cfg.bidi = bidi != 0;
    in.seekg(0, std::ios::end);
    const auto end = in.tellg();
    in.seekg(7 + 8 * sizeof(unsigned));
    jobs.reserve(count);
    for (unsigned i = 0; i < count; ++i) {
        PreparedVegaJob job;
        job.title = wide(readSpoolBytes(in, 65536));
        if (version != "RVEGA01") {
            int unit;
            spoolRead(in, unit);
            if (unit < -1 || unit >= static_cast<int>(std::size(kUnitMm)))
                throw std::runtime_error("Invalid saved data step.");
            if (unit >= 0) {
                unsigned noHeader;
                spoolRead(in, noHeader);
                if (noHeader > 1) throw std::runtime_error("Invalid saved header setting.");
                job.source = PrintSource{readSpoolBytes(in, kMaxSavedSource), unit, noHeader != 0};
            }
        }
        job.data.record = readSpoolBytes(in, 4096);
        job.data.header = readSpoolBytes(in, 65536);
        spoolRead(in, job.data.bandCount);
        unsigned first = i == 0 ? position.confirmed : 0;
        if (editable) spoolRead(in, first);
        job.data.firstBand = first;
        if (job.data.bandCount == 0 || job.data.bandCount > 65535 || first > job.data.bandCount ||
            (i == 0 && (position.confirmed > position.next || position.next > job.data.bandCount ||
                         position.inFlight > job.data.bandCount || position.nextArtwork() < first ||
                         position.blankBefore >= job.data.bandCount ||
                         (position.selected != kNoBand && (position.selected < first || position.selected > job.data.bandCount)))))
            throw std::runtime_error("Invalid saved VEGA band count.");
        std::vector<std::streamoff> offsets;
        offsets.reserve(job.data.bandCount - first);
        for (unsigned band = first; band < job.data.bandCount; ++band) {
            offsets.push_back(in.tellg());
            uint32_t size;
            spoolRead(in, size);
            if (!size || size % vega::packetSize || size > 8 * 1024 * 1024 ||
                in.tellg() + static_cast<std::streamoff>(size) > end)
                throw std::runtime_error("Invalid saved VEGA band.");
            in.seekg(size, std::ios::cur);
        }
        job.data.encodeBand = [source, first, offsets = std::move(offsets)](unsigned band) {
            std::ifstream input(source->path, std::ios::binary);
            input.seekg(offsets.at(band - first));
            return readSpoolBytes(input, 8 * 1024 * 1024);
        };
        jobs.push_back(std::move(job));
    }
    if (!journal) {
        if (in.tellg() != end) throw std::runtime_error("Unexpected data in the saved print.");
        return;
    }

    unsigned current = 0;
    RecoveryEntry entry{};
    // The last write may have been interrupted. Earlier flushed entries are still usable.
    while (in.read(reinterpret_cast<char*>(entry.data()), sizeof(entry)) &&
           entry.back() == recoveryChecksum(entry)) {
        const auto [job, phase, next, confirmed, inFlight, checksum] = entry;
        VegaPosition updated{phase, next, confirmed, inFlight};
        if (editable) updated.unpackSelection();
        if (job < current || job >= jobs.size() || updated.phase > VegaPosition::Finish ||
            confirmed > next || next > jobs[job].data.bandCount || updated.inFlight > jobs[job].data.bandCount ||
            updated.nextArtwork() < jobs[job].data.firstBand || updated.blankBefore >= jobs[job].data.bandCount ||
            (updated.selected != kNoBand &&
             (updated.selected < jobs[job].data.firstBand || updated.selected > jobs[job].data.bandCount)) ||
            (job == current && confirmed < position.confirmed && (!editable || updated.phase != VegaPosition::New)))
            throw std::runtime_error("Invalid automatic recovery position.");
        current = job;
        position = updated;
    }
    jobs.erase(jobs.begin(), jobs.begin() + current);
}

template <class Usb>
bool printVega(Usb& usb, const VegaPrintData& data, const Job& job, const Settings& cfg,
               HWND hwnd, std::string& result, VegaPosition& position,
               bool reconnecting, const std::function<void()>& saveIfRequested,
               bool* transportFailed = nullptr, const std::function<void()>& checkpoint = {},
               bool* restartRequested = nullptr, bool* needsOrigin = nullptr) {
    if (data.bandCount == 0 || !data.encodeBand) {
        result = "The job has no VEGA raster bands.";
        return false;
    }
    const unsigned bandCount = data.bandCount;
    const auto checkBandSelection = [&] {
        if (!restartRequested || g_stop) return true;
        const unsigned selected = position.selectedArtwork();
        if (!(needsOrigin && *needsOrigin)) {
            if (selected == position.nextArtwork()) return true;
            if (selected > position.nextArtwork() && selected < bandCount) {
                position.blankBefore = selected;
                position.selected = kNoBand;
                if (checkpoint) checkpoint();
                return true;
            }
        }
        *restartRequested = true;
        return false;
    };
    const auto waitPaused = [&] {
        do {
            saveIfRequested();
            if (!g_pause || g_stop) break;
            Sleep(100);
        } while (true);
        return !g_stop && checkBandSelection();
    };
    const auto progress = [&](unsigned consumed) {
        const size_t confirmed = job.plan.bytes.size() * consumed / bandCount;
        const size_t display = job.index < 0 ? 100ULL * consumed / bandCount : confirmed;
        PostMessageW(hwnd, kMsgProg, job.index, static_cast<LPARAM>(display));
    };
    const auto send = [&](UCHAR pipe, std::string_view data) {
        result.clear();
        size_t sent = 0;
        if (usb.writeEndpoint(pipe, reinterpret_cast<const unsigned char*>(data.data()), data.size(), sent, result) ||
            sent != data.size()) {
            if (transportFailed) *transportFailed = true;
            if (result.empty()) result = "Short VEGA USB write.";
            return false;
        }
        return true;
    };
    int state = -1;
    unsigned consumed = 0;
    unsigned command = 0;
    bool trackBands = false;
    VegaStatus lastIndicatorWord = -1;
    const auto publish = [&](VegaStatus word) {
        if (word == lastIndicatorWord) return;
        lastIndicatorWord = word;
        PostMessageW(hwnd, kMsgPrintStatus, 0, word);
    };
    const auto poll = [&] {
        std::string log;
        VegaStatus word;
        if (!checkVega(usb, log, result, word, &consumed, &command)) {
            if (transportFailed) *transportFailed = true;
            publish(-2);
            const auto path = std::filesystem::path(cfg.ini).replace_extension(L".usb.txt");
            if (FILE* file = _wfopen(path.c_str(), L"wb")) {
                log += result + "\n";
                fwrite(log.data(), 1, log.size(), file);
                fclose(file);
            }
            return false;
        }
        // Both vendor versions poll here between bands. Share the verified reply with the UI.
        publish(word);
        const auto fault = plotterFault(word);
        if (!fault.empty()) {
            result = utf8(fault);
            return false;
        }
        state = static_cast<int>(word & 0xffff);
        if (trackBands && consumed > position.confirmed && consumed <= bandCount) {
            position.confirmed = consumed;
            if (checkpoint) checkpoint();
            progress(consumed);
        }
        return true;
    };
    const auto waitFor = [&](const char* stage, ULONGLONG timeout, auto ready) {
        const auto limit = GetTickCount64() + timeout;
        while (!g_stop && GetTickCount64() < limit) {
            if (!poll()) {
                result = std::string(stage) + ": " + result;
                return false;
            }
            if (ready()) return true;
            Sleep(100);
        }
        if (!g_stop) {
            char detail[120];
            snprintf(detail, sizeof(detail), "%s timed out (VEGA state 0x%04X, bands consumed %u).", stage, state, consumed);
            result = detail;
        }
        return false;
    };
    if (g_stop || !poll()) return false;
    if (reconnecting && state == 0 && position.phase != VegaPosition::New && needsOrigin) {
        *needsOrigin = true;
        if (position.selected == kNoBand)
            position.selected = std::max({position.confirmed, position.blankBefore, data.firstBand});
        PostMessageW(hwnd, kMsgReconnect, 0, 0);
        PostMessageW(hwnd, kMsgPauseState, TRUE, 0);
        return waitPaused();
    }
    if (position.phase == VegaPosition::New) {
        if (state != 0) {
            result = "The VEGA is not idle. Clear or finish its current job first.";
            return false;
        }
        if (reconnecting || g_pause) {
            if (reconnecting) PostMessageW(hwnd, kMsgReconnect, 0, 0);
            PostMessageW(hwnd, kMsgPauseState, TRUE, 0);
            if (!waitPaused()) return false;
            PostMessageW(hwnd, kMsgPauseState, FALSE, 0);
        }
        position.phase = VegaPosition::Setup;
        if (checkpoint) checkpoint();
        // XP trace frames 1307 to 1347. The two job records include the XP-generated title and header raster.
        if (!send(0x01, proto::frame(0x3F, proto::be16(0x1B)))) return false;
        Sleep(400);
        if (g_stop || !send(0x01, proto::frame(0x1E, proto::be16(0x62)))) return false;
        if (!waitFor("Job setup", 3000, [&] { return command == 0x62 && state == 0; })) return false;
        if (!send(0x01, data.record)) return false;
        Sleep(260);
        if (g_stop || !send(0x01, proto::frame(0x3F, proto::be16(0x03)))) return false;
        if (!waitFor("Job header", 5000, [&] { return state == 0x02; })) return false;
        if (!data.header.empty() && position.blankBefore == 0 && !send(0x01, data.header)) return false;
        Sleep(100);
        if (!send(0x01, proto::frame(0x3F, proto::be16(0x04)))) return false;
        position.phase = VegaPosition::Bands;
        if (checkpoint) checkpoint();
    } else {
        if (position.phase == VegaPosition::Setup || state == 0 ||
            consumed < position.confirmed || consumed > std::max(position.next, position.inFlight)) {
            result = "VEGA cannot verify the saved job and band position.";
            return false;
        }
        if (position.inFlight) {
            if (consumed != position.inFlight) {
                result = "VEGA cannot confirm the last USB band. Resume would risk duplicating or skipping a band.";
                return false;
            }
            position.next = position.inFlight;
            position.inFlight = 0;
            position.confirmed = consumed;
            if (checkpoint) checkpoint();
        }
        if (state != 0x1E && (state < 0x14 || state > 0x17)) {
            result = "VEGA is in an unsupported state for resuming this job.";
            return false;
        }
        if (state == 0x1E && (consumed != bandCount || position.next != bandCount)) {
            result = "The VEGA has an unexpected completion position.";
            return false;
        }
        if (position.next == bandCount) {
            position.confirmed = consumed;
            if (checkpoint) checkpoint();
            // There are no more raster bands to send. Wait for the user's resume
            // before acknowledging firmware completion.
            PostMessageW(hwnd, kMsgReconnect, 0, 0);
            if (!waitPaused()) return false;
        }
    }
    trackBands = true;

    bool announceReconnect = reconnecting && position.phase == VegaPosition::Bands;
    std::string blankBand;
    const auto sendBand = [&](unsigned band, const char* stage) {
        auto deadline = GetTickCount64() + 20000;
        bool paused = false;
        for (;;) {
            if (g_stop) return false;
            if (!poll()) {
                result = std::string(stage) + ": " + result;
                return false;
            }
            const bool pauseRequested = g_pause;
            if (pauseRequested && !paused && state >= 0x14 && state <= 0x17) {
                // VEGA finishes its current motion and reports 0x17 when paused.
                if (state != 0x17 || !announceReconnect) {
                    if (!send(0x01, proto::frame(0x3F, proto::be16(0x05)))) return false;
                    if (!waitFor("Pause", 20000, [&] { return state == 0x17; })) return false;
                }
                paused = true;
                if (announceReconnect) {
                    PostMessageW(hwnd, kMsgReconnect, 0, 0);
                    announceReconnect = false;
                }
                PostMessageW(hwnd, kMsgPauseState, TRUE, 0);
            }
            if (paused) {
                saveIfRequested();
                if (state != 0x17) {
                    result = "VEGA left its paused state.";
                    return false;
                }
                if (!g_pause) {
                    if (!checkBandSelection()) return false;
                    if (!send(0x01, proto::frame(0x3F, proto::be16(0x09)))) return false;
                    paused = false;
                    PostMessageW(hwnd, kMsgPauseState, FALSE, 0);
                    deadline = GetTickCount64() + 20000;
                }
            } else {
                const bool ready = cfg.bidi ? (state >= 0x15 && state <= 0x17) :
                                   state == (band == 0 ? 0x15 : 0x16);
                if (!pauseRequested && ready && consumed == band) break;
                if (GetTickCount64() >= deadline) {
                    result = std::string(stage) + " timed out (VEGA state " +
                             std::to_string(state) + ", bands consumed " + std::to_string(consumed) + ").";
                    return false;
                }
            }
            Sleep(100);
        }
        // Complete the transfer once the band has been accepted.
        if (g_stop) return false;
        if (band < position.blankBefore && blankBand.empty()) {
            size_t length = 0;
            if (data.record.size() != 64 || !proto::checkHeader(data.record.substr(0, 8), 0x0D, length, result) ||
                length != 54 || !proto::checkBody(data.record.substr(0, 8), data.record.substr(8), result))
                return false;
            unsigned columns = 0;
            for (size_t i = 50; i < 54; ++i) columns = (columns << 8) | static_cast<unsigned char>(data.record[i]);
            if (!columns || columns > 150000) {
                result = "Invalid VEGA scan width in the saved print.";
                return false;
            }
            for (unsigned col = 0; col < columns; ++col) blankBand.append("\0\xC0\0\xC0", 4);
            blankBand.resize((blankBand.size() + vega::packetSize - 1) / vega::packetSize * vega::packetSize, '\0');
        }
        const auto payload = band < position.blankBefore ? blankBand : data.encodeBand(band);
        if (payload.empty() || payload.size() % vega::packetSize != 0) {
            result = "Invalid VEGA raster band.";
            return false;
        }
        const std::string_view wire(payload);
        const auto packets = wire.size() / vega::packetSize;
        const unsigned timing = data.header.empty() ? vega::bandTiming(packets, band == 0) :
                               band == 0 ? 0x18 : 0x2D;
        const auto begin = proto::frame(0x1C, proto::be16(0) + proto::be16(timing));
        position.inFlight = band + 1;
        if (checkpoint) checkpoint();
        if (!send(0x01, begin)) return false;
        unsigned char ack[2]{};
        size_t got = 0;
        if (!usb.readEndpoint(0x82, ack, sizeof(ack), got, result)) {
            if (transportFailed) *transportFailed = true;
            return false;
        }
        if (got != sizeof(ack) || ack[0] != 0 || ack[1] != 0x0F) {
            result = "VEGA refused a raster band.";
            return false;
        }
        for (size_t offset = 0; offset < payload.size(); offset += vega::packetSize) {
            if (!send(0x02, wire.substr(offset, vega::packetSize))) return false;
        }
        if (!send(0x01, proto::frame(0x1D, proto::be16(0) + proto::be16(band + 1)))) return false;
        position.next = band + 1;
        position.inFlight = 0;
        if (checkpoint) checkpoint();
        return true;
    };
    const unsigned finalBand = bandCount - 1;
    for (unsigned band = position.next; band < finalBand; ++band) {
        const auto stage = "Ready for band " + std::to_string(band + 1) + " of " + std::to_string(finalBand);
        if (!sendBand(band, stage.c_str())) return false;
    }
    // The job record schedules final paper advance after the last raster band.
    if (position.next <= finalBand && !sendBand(finalBand, "Ready for final paper advance")) return false;
    if (!waitFor("Paper advance completion", 30000, [&] {
        return state == 0x1E && consumed == bandCount;
    })) return false;
    position.phase = VegaPosition::Finish;
    if (checkpoint) checkpoint();
    if (g_pause) {
        PostMessageW(hwnd, kMsgPauseState, TRUE, 0);
        if (!waitPaused()) return false;
    }
    if (g_stop || !send(0x01, proto::frame(0x3F, proto::be16(0x23)))) return false;
    return waitFor("Return to idle", 6000, [&] { return state == 0; });
}

bool resetBeforeDisconnect(const Settings& cfg, std::string& result, Transport* existing = nullptr);

DWORD WINAPI vegaPrintThread(LPVOID p) {
    std::unique_ptr<ThreadArg> args(static_cast<ThreadArg*>(p));
    std::vector<PreparedVegaJob> jobs;
    VegaPosition position;
    bool allDone = false;
    try {
        if (!args->loadSaved.empty()) {
            loadVegaSpool(args->loadSaved, args->cfg, jobs, position);
            args->jobs.resize(jobs.size());
            for (size_t i = 0; i < jobs.size(); ++i) {
                args->jobs[i].index = -2;
                args->jobs[i].title = jobs[i].title;
            }
        } else {
            jobs.reserve(args->jobs.size());
            for (auto& job : args->jobs) {
                if (g_stop) break;
                PreparedVegaJob prepared;
                prepared.title = job.title;
                prepared.source = std::move(job.source);
                if (args->useCapturedSquare) {
                    prepared.data = capturedSquare(args->cfg.spacingMm);
                } else {
                    auto raster = vega::rasterize(job.piece, job.stream, job.plan, args->calibration,
                        args->paperWidth * 0.025, args->cfg.rightMarginMm, args->cfg.spacingMm,
                        args->cfg.lineWidthMm, utf8(job.title), g_stop, args->cfg.bidi, job.range, args->cfg.shift, args->cfg.heads,
                        args->cfg.textWidthMm, args->cfg.labelSizePercent / 100);
                    prepared.data.record = std::move(raster.record);
                    prepared.data.bandCount = raster.bandCount;
                    prepared.data.encodeBand = std::move(raster.encodeBand);
                }
                jobs.push_back(std::move(prepared));
            }
        }

        if (g_stop || jobs.empty()) throw std::runtime_error("Print canceled.");
        const auto recovery = autoRecoveryPath(args->cfg);
        saveVegaSpool(recovery, args->cfg, jobs, 0, position, true);
        RecoveryJournal journal;
        journal.file = CreateFileW(recovery.c_str(), GENERIC_WRITE | DELETE, FILE_SHARE_READ,
                                  nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        LARGE_INTEGER end{};
        if (journal.file == INVALID_HANDLE_VALUE || !SetFilePointerEx(journal.file, end, nullptr, FILE_END))
            throw std::runtime_error("Could not open autorecovery.pltsave for band updates.");

        bool reconnecting = !args->loadSaved.empty();
        for (size_t current = 0; current < jobs.size() && !g_stop; ++current) {
            const auto& prepared = jobs[current];
            const auto& job = args->jobs[current];
            const auto checkpoint = [&] { journal.append(static_cast<unsigned>(current), position); };
            unsigned shownBand = kNoBand;
            bool needsOrigin = reconnecting && position.phase == VegaPosition::New && position.blankBefore > 0;
            bool shownOrigin = false;
            const auto saveIfRequested = [&] {
                const unsigned seek = g_seekBand.exchange(kNoBand);
                if (seek >= prepared.data.firstBand && seek <= prepared.data.bandCount) {
                    position.selected = !needsOrigin && seek == position.nextArtwork() ? kNoBand : seek;
                    checkpoint();
                }
                if (g_pause && (shownBand != position.nextArtwork() || shownOrigin != needsOrigin)) {
                    shownBand = position.nextArtwork();
                    shownOrigin = needsOrigin;
                    auto cursor = std::make_unique<BandCursor>(BandCursor{job.index, prepared.data.firstBand,
                        prepared.data.bandCount, shownBand, position.selectedArtwork(), needsOrigin});
                    if (PostMessageW(args->hwnd, kMsgBandCursor, 0, reinterpret_cast<LPARAM>(cursor.get())))
                        cursor.release();
                }
                if (!g_pause) shownBand = kNoBand;
                if (!g_savePrint.exchange(false)) return;
                try {
                    const auto path = std::move(g_savePrintPath);
                    saveVegaSpool(path, args->cfg, jobs, current, position);
                    PostMessageW(args->hwnd, kMsgSaved, 0, position.selectedArtwork());
                } catch (const std::exception& e) {
                    PostMessageW(args->hwnd, kMsgSaved, 1, 0);
                    postError(args->hwnd, e.what());
                }
            };
            bool searching = false;
            while (!g_stop) {
                if (g_pause) saveIfRequested();
                LibUsbKTransport usb(args->cfg.vid, args->cfg.pid, 0x01, std::min(args->cfg.timeoutMs, 2000));
                std::string result;
                bool done = false;
                bool restart = false;
                const bool opened = usb.open(result) && usb.enableRasterEndpoints(result);
                bool transportFailed = !opened;
                if (opened)
                    done = printVega(usb, prepared.data, job, args->cfg, args->hwnd, result,
                                     position, reconnecting, saveIfRequested, &transportFailed, checkpoint,
                                     &restart, &needsOrigin);
                if (restart && !g_stop) {
                    const unsigned selected = position.selectedArtwork();
                    if (selected >= prepared.data.bandCount) {
                        done = true;
                    } else if (resetBeforeDisconnect(args->cfg, result, &usb)) {
                        position = {};
                        position.blankBefore = selected;
                        checkpoint();
                        shownBand = kNoBand;
                        needsOrigin = false;
                        reconnecting = false;
                        continue;
                    }
                }
                usb.close();
                if (done) {
                    PostMessageW(args->hwnd, kMsgDone, job.index, 0);
                    position = {};
                    reconnecting = false;
                    allDone = current + 1 == jobs.size();
                    if (!allDone) journal.append(static_cast<unsigned>(current + 1), position);
                    break;
                }
                if (g_stop) break;
                g_pause = true;
                shownBand = kNoBand;
                g_reconnect = false;
                if (!searching || !transportFailed || opened) {
                    PostMessageW(args->hwnd, kMsgReconnect, transportFailed ? 1 : 2, job.index);
                    postError(args->hwnd, transportFailed
                        ? "VEGA connection interrupted. Band checkpoint saved in autorecovery.pltsave. Searching..."
                        : "Print paused. " + utf8(job.title) + ": " + result);
                }
                searching = transportFailed;
                const auto retryAt = GetTickCount64() + 1000;
                while (!g_stop && !g_reconnect) {
                    saveIfRequested();
                    if (transportFailed && GetTickCount64() >= retryAt) break;
                    Sleep(100);
                }
                reconnecting = true;
            }
            if (g_stop) PostMessageW(args->hwnd, kMsgDone, job.index, 1);
        }
        if (allDone && !journal.removeOnClose())
            postError(args->hwnd, "Print completed, but autorecovery.pltsave could not be deleted.");
    } catch (const std::exception& e) {
        if (!g_stop) postError(args->hwnd, e.what());
    }
    if (g_resetOnDisconnect.exchange(false)) {
        std::string result;
        const bool reset = resetBeforeDisconnect(args->cfg, result);
        PostMessageW(args->hwnd, kMsgVegaRecovery, reset ? 1 : 0, 0);
        if (!reset && !result.empty()) postError(args->hwnd, "Could not reset VEGA: " + result);
    } else if (!allDone && position.phase != VegaPosition::New) {
        PostMessageW(args->hwnd, kMsgVegaRecovery, 0, 0);
    }
    PostMessageW(args->hwnd, kMsgFinished, allDone, 0);
    return 0;
}

DWORD WINAPI printThread(LPVOID p) {
    std::unique_ptr<ThreadArg> a(static_cast<ThreadArg*>(p));
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
                                  dpi, a->cfg.lineWidthMm, a->printer, job.title, g_stop, error,
                                  a->cfg.textWidthMm, a->cfg.labelSizePercent / 100);
        } catch (const std::exception& e) {
            error = e.what();
        }
        if (!submitted && !g_stop) postError(a->hwnd, utf8(job.title) + ": " + error);
        PostMessageW(a->hwnd, kMsgDone, job.index, submitted ? 0 : g_stop ? 1 : 2);
        if (!submitted) break;
    }
    PostMessageW(a->hwnd, kMsgFinished, 0, 0);
    return 0;
}
bool canPrint() {
    return !S.running && !S.maintenanceRunning && S.connectionThread && S.statusWord >= 0 &&
           (S.statusWord & 0xffff) == 0 && plotterFault(S.statusWord).empty() &&
           !S.recoveryRequired && !g_paperStopPending &&
           WaitForSingleObject(S.disconnect, 0) == WAIT_TIMEOUT;
}

void updateBandButtons() {
    const bool editable = S.bandCursor && S.vegaSession && S.running && g_pause && !g_stop && !S.savingPrint;
    EnableWindow(S.btnPreviousBand, editable && S.bandCursor->selected > S.bandCursor->first);
    EnableWindow(S.btnNextBand, editable && S.bandCursor->selected <
                 std::max(S.bandCursor->count - 1, S.bandCursor->natural));
}

void stepBand(int direction) {
    const HWND button = direction < 0 ? S.btnPreviousBand : S.btnNextBand;
    if (!S.bandCursor || !IsWindowEnabled(button)) return;
    auto& cursor = *S.bandCursor;
    if (direction < 0) --cursor.selected;
    else ++cursor.selected;
    g_seekBand = cursor.selected;
    updateBandButtons();
    InvalidateRect(S.preview, nullptr, FALSE);
}

void updatePrintButton() {
    const bool ready = canPrint();
    EnableWindow(S.btnStart, ready);
    EnableWindow(S.btnCleanHeads, ready);
    EnableWindow(S.btnAdvancePaper, ready || g_advancePaper);
    EnableWindow(S.btnLoadPrint, !S.running && !S.maintenanceRunning && !g_paperStopPending);
    EnableWindow(S.btnResetVega, !S.maintenanceRunning && !S.resetPending &&
                 !S.savingPrint && (!S.running || (S.vegaSession && !g_stop)));
    updateBandButtons();
}

void setRunning(bool running) {
    S.running = running;
    EnableWindow(S.btnAdd, !running);
    EnableWindow(S.btnRemove, !running);
    EnableWindow(S.editFrom, !running);
    EnableWindow(S.editTo, !running);
    updatePrintButton();
    EnableWindow(S.printAuto, !running);
    EnableWindow(S.btnTestUsb, !running);
    EnableWindow(S.btnServerConfig, !running);
    EnableWindow(S.btnStop, running);
    EnableWindow(S.btnPause, running && S.vegaSession);
    EnableWindow(S.btnSavePrint, running && S.vegaSession && !g_savePrint);
    SetWindowTextW(S.btnPause, running && g_pause ? L"Resume" : L"Pause");
}

std::array<IndicatorTone, 7> indicatorTones(VegaStatus word) {
    using Tone = IndicatorTone;
    const bool online = word >= 0;
    const int state = word & 0xffff;
    return {
        online ? Tone::Good : Tone::Fault,
        online ? (state == 0xffff ? Tone::Fault : Tone::Good) : Tone::Neutral,
        online ? (word & kVegaPaperSensor ? Tone::Fault : Tone::Good) : Tone::Neutral,
        word == -2 || !plotterFault(word).empty() ? Tone::Fault : Tone::Good,
        online && (state == 0 || (state >= 0x14 && state <= 0x17)) ? Tone::Good : Tone::Neutral,
        online ? (word & kVegaInkUnexpected ? Tone::Fault : Tone::Good) : Tone::Neutral,
        Tone::Neutral
    };
}

void showIndicators(VegaStatus word) {
    S.statusWord = word;
    const wchar_t* labels[]{L"Online", L"Cover", L"Paper", L"Error", L"Plot", L"Ink", L"Man"};
    const int state = word & 0xffff;
    const bool online = word >= 0;
    const wchar_t* values[]{online ? L"Yes" : L"No",
        online ? (state == 0xffff ? L"Open" : L"Closed") : L"—",
        online ? (word & kVegaPaperSensor ? L"Check" : L"Ready") : L"—",
        L"",
        online ? (state == 0x17 ? L"Paused" : state == 0 ? L"Idle" : state == 0xffff ? L"Blocked" : L"Active") : L"—",
        online ? (word & kVegaInkUnexpected ? L"Check" : L"Clear") : L"—",
        SendMessageW(S.printAuto, BM_GETCHECK, 0, 0) == BST_CHECKED ? L"Auto" : L"Manual"};
    const auto tones = indicatorTones(word);
    for (size_t i = 0; i < std::size(labels); ++i) {
        S.indicatorTones[i] = tones[i];
        const wchar_t* value = values[i];
        SetWindowTextW(S.indicators[i], (*value ? std::wstring(labels[i]) + L"  " + value : labels[i]).c_str());
        InvalidateRect(S.indicators[i], nullptr, TRUE);
    }
    if (S.status) renderStatus();
}

struct Connection {
    Settings cfg;
    HWND hwnd;
    HANDLE disconnect;
    bool resetOnConnect;
};

bool resetVega(Transport& usb, HANDLE disconnect, std::string& log, std::string& result,
               VegaStatus* verifiedStatus = nullptr) {
    // AlgoServer's resetError and print-abort handlers send request 0x1B.
    const auto command = proto::frame(0x3F, proto::be16(0x1B));
    log += "Reset request: " + hexBytes(command) + "\n";
    size_t sent = 0;
    if (usb.write(reinterpret_cast<const unsigned char*>(command.data()), command.size(), sent, result) ||
        sent != command.size()) {
        if (result.empty()) result = "Incomplete VEGA reset request.";
        return false;
    }
    const auto limit = GetTickCount64() + 8000;
    VegaStatus status = -1;
    while (GetTickCount64() < limit && WaitForSingleObject(disconnect, 200) == WAIT_TIMEOUT) {
        if (checkVega(usb, log, result, status) && (status & 0xffff) == 0 && plotterFault(status).empty()) {
            if (verifiedStatus) *verifiedStatus = status;
            return true;
        }
    }
    if (WaitForSingleObject(disconnect, 0) == WAIT_TIMEOUT) {
        result = status < 0 ? "VEGA stopped responding after the reset request. " + result :
                              "VEGA did not return to idle after the reset request.";
    }
    return false;
}

bool resetBeforeDisconnect(const Settings& cfg, std::string& result, Transport* existing) {
    const HANDLE cancel = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!cancel) {
        result = "Could not create the VEGA reset event.";
        return false;
    }
    std::string log;
    if (existing && resetVega(*existing, cancel, log, result)) {
        CloseHandle(cancel);
        return true;
    }
    if (existing) existing->close();
    const int timeout = std::min(cfg.timeoutMs, 2000);
    LibUsbKTransport libusb(cfg.vid, cfg.pid, cfg.pipe, timeout);
    WinUsbTransport winusb(cfg.vid, cfg.pid, cfg.pipe, timeout);
    libusb.forceIn = winusb.forceIn = cfg.pipeIn;
    const bool useWinUsb = _wcsicmp(cfg.backend.c_str(), L"WinUSB") == 0;
    const bool useLibUsb = _wcsicmp(cfg.backend.c_str(), L"libusbK") == 0;
    Transport* usb = !useWinUsb && libusb.open(result) ? static_cast<Transport*>(&libusb) : nullptr;
    if (!usb && !useLibUsb && winusb.open(result)) usb = &winusb;
    if (usb) result.clear();
    const bool reset = usb && resetVega(*usb, cancel, log, result);
    CloseHandle(cancel);
    return reset;
}

bool cleanVegaHeads(Transport& usb, std::string& log, std::string& result) {
    VegaStatus status = -1;
    if (!checkVega(usb, log, result, status)) return false;
    if ((status & 0xffff) != 0) {
        result = "Wait for the VEGA to go idle before cleaning the heads.";
        return false;
    }

    const auto send = [&](unsigned code) {
        const auto request = proto::frame(0x3F, proto::be16(code));
        log += "Head cleaning request: " + hexBytes(request) + "\n";
        size_t sent = 0;
        result.clear();
        if (usb.write(reinterpret_cast<const unsigned char*>(request.data()), request.size(), sent, result) == 0 &&
            sent == request.size()) return true;
        if (result.empty()) result = "Incomplete VEGA head cleaning request.";
        return false;
    };

    // AlgoServer 0x4221d0 and AlgotexOcx 0x1000fd29 send the same sequence.
    if (!send(0x2B)) return false;
    if (!send(0x2A)) {
        const auto error = result;
        send(0x2D);
        result = error;
        return false;
    }
    Sleep(1000);
    if (!send(0x2D)) return false;
    const auto deadline = GetTickCount64() + 8000;
    do {
        if (!checkVega(usb, log, result, status)) return false;
        if ((status & 0xffff) == 0) return true;
        Sleep(200);
    } while (GetTickCount64() < deadline);
    result = "The VEGA did not return to idle after head cleaning.";
    return false;
}

bool sendPaperCommand(Transport& usb, unsigned code, std::string& error) {
    const auto frame = proto::frame(0x3F, proto::be16(code));
    size_t sent = 0;
    return usb.write(reinterpret_cast<const unsigned char*>(frame.data()), frame.size(), sent, error) == 0 &&
           sent == frame.size();
}

bool advanceVegaPaper(Transport& usb, HANDLE disconnect, std::string& log, std::string& result) {
    VegaStatus status = -1;
    if (!checkVega(usb, log, result, status)) return false;
    if ((status & 0xffff) != 0) {
        result = "Wait for the VEGA to go idle before advancing paper.";
        return false;
    }
    // AlgoServer advancePaper sends 0x1D for forward motion and 0x1C on release.
    result.clear();
    if (!g_advancePaper || WaitForSingleObject(disconnect, 0) != WAIT_TIMEOUT) return true;
    g_paperStopPending = true;
    const bool started = sendPaperCommand(usb, 0x1D, result);
    while (started && g_advancePaper && WaitForSingleObject(disconnect, 100) == WAIT_TIMEOUT) {}
    std::string stopError;
    if (!sendPaperCommand(usb, 0x1C, stopError)) {
        result = "Could not confirm paper advance stop. " + stopError;
        return false;
    }
    g_paperStopPending = false;
    if (!started && result.empty()) result = "Could not start paper advance.";
    return started;
}

DWORD WINAPI monitorVega(LPVOID p) {
    std::unique_ptr<Connection> connection(static_cast<Connection*>(p));
    const auto& cfg = connection->cfg;
    std::unique_ptr<Transport> usb;
    std::string result;
    const auto open = [&](auto candidate) {
        candidate->forceIn = cfg.pipeIn;
        if (candidate->open(result)) usb = std::move(candidate);
    };
    const int timeout = std::min(cfg.timeoutMs, 2000);
    const auto connect = [&] {
        if (_wcsicmp(cfg.backend.c_str(), L"WinUSB") != 0)
            open(std::make_unique<LibUsbKTransport>(cfg.vid, cfg.pid, cfg.pipe, timeout));
        if (!usb && _wcsicmp(cfg.backend.c_str(), L"libusbK") != 0)
            open(std::make_unique<WinUsbTransport>(cfg.vid, cfg.pid, cfg.pipe, timeout));
    };
    const auto deadline = GetTickCount64() + 8000;
    std::string log;
    VegaStatus word = -1;
    while (WaitForSingleObject(connection->disconnect, 0) == WAIT_TIMEOUT) {
        connect();
        if (usb && g_paperStopPending && sendPaperCommand(*usb, 0x1C, result)) g_paperStopPending = false;
        if (usb && !g_paperStopPending && checkVega(*usb, log, result, word)) break;
        usb.reset();
        if (!connection->resetOnConnect || GetTickCount64() >= deadline ||
            WaitForSingleObject(connection->disconnect, 250) != WAIT_TIMEOUT) break;
    }
    if (usb && connection->resetOnConnect &&
        WaitForSingleObject(connection->disconnect, 0) == WAIT_TIMEOUT) {
        result.clear();
        if (resetVega(*usb, connection->disconnect, log, result, &word)) {
            PostMessageW(connection->hwnd, kMsgVegaRecovery, 1, 0);
        } else {
            usb.reset();
        }
    }
    while (usb && WaitForSingleObject(connection->disconnect, 0) == WAIT_TIMEOUT) {
        PostMessageW(connection->hwnd, kMsgUsbResult, static_cast<WPARAM>(word + 1), 0);
        if (WaitForSingleObject(connection->disconnect, 500) != WAIT_TIMEOUT) break;
        log.clear();
        const auto maintenance = g_maintenance.exchange(MaintenanceAction::None);
        if (maintenance != MaintenanceAction::None) {
            auto detail = std::make_unique<std::string>();
            const bool cleaning = maintenance == MaintenanceAction::CleanHeads;
            const bool ok = cleaning ? cleanVegaHeads(*usb, log, *detail) :
                                       advanceVegaPaper(*usb, connection->disconnect, log, *detail);
            if (g_paperStopPending) result = *detail;
            if (PostMessageW(connection->hwnd, kMsgHeadCleanDone, (cleaning ? 2 : 0) | (ok ? 1 : 0),
                             reinterpret_cast<LPARAM>(detail.get()))) detail.release();
            if (g_paperStopPending) break;
        }
        result.clear();
        if (!checkVega(*usb, log, result, word)) break;
    }
    if (g_resetOnDisconnect.exchange(false)) {
        result.clear();
        if (resetBeforeDisconnect(cfg, result, usb.get()))
            PostMessageW(connection->hwnd, kMsgVegaRecovery, 1, 0);
        else if (result.empty()) result = "Could not reset VEGA before disconnecting.";
    } else if (WaitForSingleObject(connection->disconnect, 0) == WAIT_OBJECT_0) result.clear();
    else {
        auto path = std::filesystem::path(cfg.ini);
        path.replace_extension(L".usb.txt");
        if (FILE* file = _wfopen(path.c_str(), L"wb")) {
            log += result + "\n";
            fwrite(log.data(), 1, log.size(), file);
            fclose(file);
        }
    }
    usb.reset();
    auto detail = std::make_unique<std::string>(std::move(result));
    if (PostMessageW(connection->hwnd, kMsgUsbResult, 0, reinterpret_cast<LPARAM>(detail.get()))) detail.release();
    return 0;
}

void testUsb(bool afterPrint = false, bool reset = false) {
    if (S.connectionThread) {
        g_resetOnDisconnect = true;
        SetEvent(S.disconnect);
        EnableWindow(S.btnTestUsb, FALSE);
        updatePrintButton();
        return;
    }
    S.disconnect = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!S.disconnect) {
        setStatus(L"Could not create the USB connection event.", true);
        return;
    }
    auto args = std::make_unique<Connection>(Connection{S.cfg, S.hwnd, S.disconnect, reset});
    auto* raw = args.release();
    S.connectionThread = CreateThread(nullptr, 0, monitorVega, raw, 0, nullptr);
    if (!S.connectionThread) {
        delete raw;
        CloseHandle(S.disconnect);
        S.disconnect = nullptr;
        setStatus(L"Could not start the USB connection.", true);
        return;
    }
    S.reconnectingAfterPrint = afterPrint;
    S.maintenanceRunning = reset;
    SetWindowTextW(S.btnTestUsb, L"Disconnect");
    if (reset) EnableWindow(S.btnTestUsb, FALSE);
    if (!afterPrint) setStatus(reset ? L"Finding and resetting VEGA..." : L"Connecting to VEGA...", false);
}

void resetPlotter() {
    if (S.resetPending || S.maintenanceRunning || S.savingPrint ||
        (S.running && (!S.vegaSession || g_stop))) return;
    if (MessageBoxW(S.hwnd, L"Reset VEGA? The plotter will clear any active print.",
                    L"Reset VEGA", MB_OKCANCEL | MB_ICONWARNING | MB_DEFBUTTON2) != IDOK) return;
    if (S.running || S.connectionThread) {
        S.resetPending = true;
        if (S.running) g_stop = true;
        if (S.connectionThread) SetEvent(S.disconnect);
        EnableWindow(S.btnTestUsb, FALSE);
        EnableWindow(S.btnPause, FALSE);
        EnableWindow(S.btnSavePrint, FALSE);
        setStatus(L"Preparing to reset VEGA...", false);
    } else {
        testUsb(false, true);
    }
    updatePrintButton();
}

bool startVegaJobs(std::vector<Job> jobs, double paperWidth, cal::Factors calibration = S.calibration,
                   HWND owner = nullptr) {
    if (!canPrint()) {
        setStatus(L"Connect to an idle VEGA before printing.", true);
        if (owner) SetDlgItemTextW(owner, IDC_TEST_RECT_STATUS, L"Connect to an idle VEGA before printing.");
        return false;
    }
    constexpr char square[] = "IN;SP1;PA;PU0,0;PD4000,0,4000,4000,0,4000,0,0;PU250,250;PD650,450;PU;SP0;";
    bool captured = false;
    if (jobs.size() == 1 && jobs.front().index >= 0) {
        const Layer& layer = S.layers[static_cast<size_t>(jobs.front().index)];
        if (layer.unit == 0 && !layer.noHeader && layer.from == 0 &&
            layer.to == 100 && !S.cfg.bidi && S.cfg.rotation == 0 && S.cfg.lineWidthMm == 0.4 &&
            S.cfg.heads.mask == 1 && S.cfg.heads.columnOffset(0) == 0 &&
            S.calibration.feed == 1 && S.calibration.across == 1) {
            try {
                std::string_view hpgl = jobs.front().source->bytes;
                while (!hpgl.empty() && (hpgl.back() == '\r' || hpgl.back() == '\n')) hpgl.remove_suffix(1);
                captured = hpgl == square;
            } catch (const std::exception& e) {
                setStatus(wide(e.what()), true);
                return false;
            }
        }
    }
    if (MessageBoxW(owner ? owner : S.hwnd, L"Are you sure you are ready to print ?",
                    L"Print to Algotex VEGA", MB_OKCANCEL | MB_ICONQUESTION) != IDOK) return false;
    if (!canPrint()) {
        setStatus(L"The VEGA connection is no longer ready.", true);
        return false;
    }
    auto args = std::make_unique<ThreadArg>();
    args->jobs = std::move(jobs);
    args->cfg = S.cfg;
    args->hwnd = S.hwnd;
    args->paperWidth = paperWidth;
    args->calibration = calibration;
    args->useCapturedSquare = captured;
    g_stop = false;
    g_pause = false;
    g_seekBand = kNoBand;
    g_reconnect = false;
    g_savePrint = false;
    S.vegaSession = true;
    S.pendingPrint = std::move(args);
    setRunning(true);
    setStatus(captured ? L"Sending XP test square to VEGA..." : L"Preparing VEGA raster...", false);
    SetEvent(S.disconnect);
    return true;
}

void startPrint() {
    if (S.running || S.layers.empty()) return;
    if (!canPrint()) {
        setStatus(L"Connect to an idle VEGA before printing.", true);
        return;
    }
    std::vector<int> pending;
    if (SendMessageW(S.printAuto, BM_GETCHECK, 0, 0) == BST_CHECKED) {
        for (size_t i = 0; i < S.layers.size(); ++i) {
            if (S.layers[i].status != L"done") pending.push_back(static_cast<int>(i));
        }
        if (pending.empty()) {
            setStatus(L"No waiting files to print.", false);
            return;
        }
    } else {
        const int selected = selectedIndex();
        if (selected < 0) {
            setStatus(L"Select a file to print.", false);
            return;
        }
        pending.push_back(selected);
    }
    std::optional<double> width = paperUnits();
    if (!width || *width <= 0) {
        message(L"Set a right margin smaller than the configured paper width.", L"Printable width", MB_ICONINFORMATION);
        return;
    }
    std::vector<int> bad = tooWide(pending.front());
    bad.erase(std::remove_if(bad.begin(), bad.end(), [&](int index) {
        return !std::binary_search(pending.begin(), pending.end(), index);
    }), bad.end());
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
    for (const int i : pending) {
        Layer& l = S.layers[static_cast<size_t>(i)];
        l.sent = 0;
        l.pct = 0;
        l.total = 0;
        try {
            rm::Req q;
            q.path = l.path;
            q.unit = kUnitMm[l.unit];
            q.deg = drawingRotation(l);
            q.noHeader = l.noHeader;
            PrintSource source{rm::readFile(l.path), l.unit, l.noHeader};
            rm::Cache printCache;
            printCache.parsed.emplace(std::make_pair(q.path, q.unit),
                                      rm::parseContents(q.path, source.bytes, q.unit));
            std::vector<rm::Piece> placed = rm::placeLayers(printCache, {q}, width);
            rp::Stream st = rp::makeStream(placed[0], kHpglOutputStepMm, S.calibration);
            rp::Plan plan = rp::makePlan(st, 0, 100);
            if (plan.bytes.empty()) {
                l.status = L"empty";
                invalidateRow(i);
                continue;
            }
            l.total = plan.bytes.size();
            l.status = L"queued";
            Job j;
            j.index = i;
            j.source = std::move(source);
            j.piece = std::move(placed[0]);
            j.stream = std::move(st);
            j.plan = std::move(plan);
            if (l.from > 0 || l.to < 100) {
                const auto bounds = rm::boxOf(j.piece.ops);
                if (!bounds) throw std::runtime_error("The drawing has no feed bounds.");
                j.range = configuredFeedRange(*bounds, S.calibration.feed, l.from, l.to);
                if (l.to <= l.from || j.range->last <= j.range->first)
                    throw std::runtime_error("The selected percentages cover no complete VEGA band.");
            }
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
    if (!jobs.empty() && !startVegaJobs(std::move(jobs), *width)) {
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

// ---------------------------------------------------------------- controls
HWND makeButton(HWND parent, int id, const wchar_t* text) {
    HWND h = CreateWindowExW(0, L"BUTTON", text, WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, 0, 0, 10, 10,
                             parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), S.inst, nullptr);
    SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(S.font), TRUE);
    return h;
}

void addSlot(HWND h, int w, int ht, int gapAfter) {
    S.controlRows[S.controlRow].push_back(Slot{h, w, ht, gapAfter});
}

HWND addButton(HWND parent, int id, const wchar_t* text, int pad, int gapAfter, bool useBold) {
    HWND h = makeButton(parent, id, text);
    if (useBold) {
        SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(S.bold), TRUE);
    }
    int w = textWidth(text, useBold ? S.bold : S.font) + dp(pad);
    addSlot(h, w, dp(32), gapAfter);
    return h;
}

void addLabel(HWND parent, const wchar_t* text, int gapAfter) {
    HWND label = CreateWindowExW(0, L"STATIC", text, WS_CHILD | WS_VISIBLE | SS_LEFT | SS_CENTERIMAGE, 0, 0, 10, 10,
                                 parent, nullptr, S.inst, nullptr);
    SendMessageW(label, WM_SETFONT, reinterpret_cast<WPARAM>(S.font), TRUE);
    addSlot(label, textWidth(text, S.font) + dp(6), dp(32), gapAfter);
}

HWND addEdit(HWND parent, int id, int width, int gapAfter) {
    HWND h = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL, 0, 0,
                             10, 10, parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), S.inst, nullptr);
    SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(S.font), TRUE);
    addSlot(h, dp(width), dp(28), gapAfter);
    return h;
}

void createControls(HWND hwnd) {
    S.btnAdd = addButton(hwnd, ID_ADD, L"Add", 28, dp(4), false);
    S.btnRemove = addButton(hwnd, ID_REMOVE, L"Remove", 28, dp(12), false);
    S.totalLength = CreateWindowExW(0, L"STATIC", L"Total length  0.00 m", WS_CHILD | WS_VISIBLE | SS_CENTERIMAGE,
                                    0, 0, 10, 10, hwnd, nullptr, S.inst, nullptr);
    SendMessageW(S.totalLength, WM_SETFONT, reinterpret_cast<WPARAM>(S.bold), TRUE);
    addSlot(S.totalLength, textWidth(L"Total length  0.00 m", S.bold), dp(32), dp(6));
    S.btnPreviousBand = addButton(hwnd, ID_PREVIOUS_BAND, L"\u21B6", 18, dp(3), false);
    S.btnSavePrint = addButton(hwnd, ID_SAVE_PRINT, L"Save State", 24, dp(4), false);
    S.btnLoadPrint = addButton(hwnd, ID_LOAD_PRINT, L"Load State", 24, dp(3), false);
    S.btnNextBand = addButton(hwnd, ID_NEXT_BAND, L"\u21B7", 18, 0, false);
    EnableWindow(S.btnSavePrint, FALSE);

    S.controlRow = 1;
    addButton(hwnd, ID_FIT, L"Fit", 22, dp(2), false);
    addButton(hwnd, ID_FILL, L"Fill", 22, dp(12), false);

    addLabel(hwnd, L"From %", dp(2));
    S.editFrom = addEdit(hwnd, ID_FROM, 52, dp(6));
    addLabel(hwnd, L"To %", dp(2));
    S.editTo = addEdit(hwnd, ID_TO, 52, dp(12));

    S.btnStart = addButton(hwnd, ID_START, L"Print", 40, dp(4), true);
    S.printAuto = CreateWindowExW(0, L"BUTTON", L"Print auto",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX, 0, 0, 10, 10,
        hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_PRINT_AUTO)), S.inst, nullptr);
    SendMessageW(S.printAuto, WM_SETFONT, reinterpret_cast<WPARAM>(S.font), TRUE);
    addSlot(S.printAuto, textWidth(L"Print auto", S.font) + dp(23), dp(32), dp(4));
    S.btnPause = addButton(hwnd, ID_PAUSE, L"Pause", 48, dp(4), false);
    EnableWindow(S.btnPause, FALSE);
    S.btnStop = addButton(hwnd, ID_STOP, L"Stop", 40, dp(12), false);
    S.btnTestUsb = addButton(hwnd, ID_TEST_USB, L"Connect", 76, dp(4), false);
    S.btnResetVega = addButton(hwnd, ID_RESET_VEGA, L"Reset", 24, 0, false);

    S.controlRow = 2;
    S.toolsLabel = CreateWindowExW(0, L"STATIC", L"Tools", WS_CHILD | WS_VISIBLE | SS_CENTERIMAGE,
                                   0, 0, 10, 10, hwnd, nullptr, S.inst, nullptr);
    SendMessageW(S.toolsLabel, WM_SETFONT, reinterpret_cast<WPARAM>(S.bold), TRUE);
    S.btnAdvancePaper = addButton(hwnd, ID_ADVANCE_PAPER, L"Advance paper", 20, dp(6), false);
    S.btnCleanHeads = addButton(hwnd, ID_CLEAN_HEADS, L"Clean heads", 20, dp(6), false);
    S.btnServerConfig = addButton(hwnd, ID_SERVER_CONFIG, L"Configuration", 24, 0, false);
    updatePrintButton();
    EnableWindow(S.btnStop, FALSE);

    for (const auto& row : S.controlRows) {
        int width = dp(20);
        for (const auto& slot : row) width += slot.width + slot.gapAfter;
        S.toolbarWidth = std::max(S.toolbarWidth, width);
    }

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

    S.goodIndicator = CreateSolidBrush(RGB(0xd2, 0xf0, 0xd7));
    S.faultIndicator = CreateSolidBrush(RGB(0xff, 0xd4, 0xd4));
    for (HWND& indicator : S.indicators) {
        indicator = CreateWindowExW(WS_EX_STATICEDGE, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_CENTER | SS_CENTERIMAGE,
                                    0, 0, 10, 10, hwnd, nullptr, S.inst, nullptr);
        SendMessageW(indicator, WM_SETFONT, reinterpret_cast<WPARAM>(S.font), TRUE);
    }
    showIndicators(-1);
    S.preview = CreateWindowExW(WS_EX_CLIENTEDGE, L"RalvioliPlotPreview", L"", WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS,
                                0, 0, 10, 10, hwnd, nullptr, S.inst, nullptr);
}

void layoutAll(int w, int h) {
    const int margin = dp(10);
    const int rowH = dp(32);
    const int rowGap = dp(6);
    const int toolsLabelH = dp(20);
    const int statusTop = margin;
    const int bodyTop = statusTop + dp(27);
    const int statusHeight = dp(32);
    const int usbHeight = dp(22);
    const int statusFooter = h - margin - statusHeight;
    const int usbFooter = statusFooter - dp(4) - usbHeight;
    const int barTop = usbFooter - rowGap - 3 * rowH - 2 * rowGap - toolsLabelH;
    struct Bounds {
        HWND window;
        int x, y, width, height;
    };
    std::vector<Bounds> children;
    int rowTop = barTop;
    for (size_t i = 0; i < S.controlRows.size(); ++i) {
        if (i == 2) {
            children.push_back({S.toolsLabel, margin, rowTop, dp(100), toolsLabelH});
            rowTop += toolsLabelH;
        }
        int x = margin;
        for (const auto& slot : S.controlRows[i]) {
            children.push_back({slot.hwnd, x, rowTop + (rowH - slot.height) / 2, slot.width, slot.height});
            x += slot.width + slot.gapAfter;
        }
        rowTop += rowH + rowGap;
    }
    const int listW = dp(365);
    const int bodyH = std::max(barTop - rowGap - bodyTop, dp(50));
    children.push_back({S.list, dp(10), bodyTop, listW, bodyH});
    int px = dp(10) + listW + dp(10);
    children.push_back({S.preview, px, bodyTop, std::max(w - px - dp(10), dp(50)), bodyH});
    int statusWidth = std::max(w - 2 * margin, 1);
    children.push_back({S.status, margin, statusFooter, statusWidth, statusHeight});
    children.push_back({S.usbStatus, margin, usbFooter, statusWidth, usbHeight});
    for (int i = 0; i < 7; ++i) {
        const int left = statusWidth * i / 7;
        const int right = statusWidth * (i + 1) / 7;
        children.push_back({S.indicators[i], margin + left, statusTop,
                            std::max(right - left - (i < 6 ? dp(4) : 0), 1), dp(21)});
    }

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
    if (layer && layer->pct > 0) {
        RECT filled = di->rcItem;
        filled.right = filled.left + MulDiv(filled.right - filled.left, layer->pct, 100);
        HBRUSH green = CreateSolidBrush(sel ? RGB(20, 111, 51) : RGB(201, 237, 207));
        FillRect(di->hDC, &filled, green);
        DeleteObject(green);
    }
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
    RECT unitRect = unitChipRect(di->rcItem);
    drawChip(di->hDC, unitRect, kUnitShort[unit], sel, true, RGB(0x20, 0x20, 0x20));
    if (di->itemID < S.boxes.size() && S.boxes[di->itemID]) {
        const auto& box = *S.boxes[di->itemID];
        wchar_t width[48]{};
        std::swprintf(width, std::size(width), L"%.1f cm",
                      (box.y1 - box.y0) * S.calibration.across / rm::kUnitsPerCm);
        const auto length = formatLength(selectedLength(di->itemID)) + L"  ×  " + width;
        RECT lengthRect{unitRect.right + dp(6), unitRect.top, di->rcItem.right - dp(8), unitRect.bottom};
        SetTextColor(di->hDC, sel ? GetSysColor(COLOR_HIGHLIGHTTEXT) : kInk);
        DrawTextW(di->hDC, length.c_str(), -1, &lengthRect,
                  DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
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
    invalidateRow(i);
    rebuildView();
}

INT_PTR CALLBACK historyDialog(HWND dialog, UINT msg, WPARAM wParam, LPARAM lParam);

INT_PTR CALLBACK ServerConfigProc(HWND dialog, UINT msg, WPARAM wParam, LPARAM) {
    if (msg == WM_INITDIALOG) {
        const auto set = [&](int id, double value) {
            wchar_t number[48];
            swprintf(number, std::size(number), L"%.8g", value);
            SetDlgItemTextW(dialog, id, number);
        };
        set(IDC_RIGHT_MARGIN, S.cfg.rightMarginMm);
        set(IDC_SPACING, S.cfg.spacingMm);
        set(IDC_SHIFT, S.cfg.shift);
        set(IDC_LINE_WIDTH, S.cfg.lineWidthMm);
        const bool labelsOff = S.cfg.labelSizePercent == 0 ||
                               plotTextWidth(S.cfg.lineWidthMm, S.cfg.textWidthMm) == 0;
        set(IDC_LABEL_SIZE, labelsOff ? 0 : S.cfg.labelSizePercent);
        if (labelsOff)
            SetDlgItemTextW(dialog, IDC_TEXT_WIDTH, L"Off");
        else
            set(IDC_TEXT_WIDTH, S.cfg.textWidthMm);
        for (unsigned slot = 0; slot < vega::headSlots; ++slot) {
            CheckDlgButton(dialog, IDC_HEAD_1 + kPhysicalHeadForSlot[slot] - 1, S.cfg.heads.enabled(slot) ? BST_CHECKED : BST_UNCHECKED);
            set(IDC_HEAD_1_OFFSET + kPhysicalHeadForSlot[slot] - 1, S.cfg.heads.offsetMm[slot]);
        }
        CheckRadioButton(dialog, IDC_MONO, IDC_BIDI, S.cfg.bidi ? IDC_BIDI : IDC_MONO);
        CheckRadioButton(dialog, IDC_ROTATE_0, IDC_ROTATE_180,
            S.cfg.rotation == 90 ? IDC_ROTATE_AUTO : S.cfg.rotation == 180 ? IDC_ROTATE_180 : IDC_ROTATE_0);
        for (const int id : {IDC_TEST_LINES, IDC_TEST_SQUARES})
            EnableWindow(GetDlgItem(dialog, id), canPrint());
        return TRUE;
    }
    if (msg != WM_COMMAND) return FALSE;
    if (LOWORD(wParam) == IDCANCEL) {
        EndDialog(dialog, IDCANCEL);
        return TRUE;
    }
    if (LOWORD(wParam) == IDC_CONFIG_CALIBRATE) {
        showCalibration(dialog);
        return TRUE;
    }
    if (LOWORD(wParam) == IDC_PLOT_HISTORY) {
        DialogBoxParamW(S.inst, MAKEINTRESOURCEW(IDD_PLOT_HISTORY), dialog, historyDialog, 0);
        return TRUE;
    }
    const auto action = LOWORD(wParam);
    const auto read = [&](int id) {
        wchar_t field[48]{};
        GetDlgItemTextW(dialog, id, field, static_cast<int>(std::size(field)));
        std::replace(field, field + std::wcslen(field), L',', L'.');
        wchar_t* end = nullptr;
        const double value = std::wcstod(field, &end);
        return end != field && *end == L'\0' && std::isfinite(value) ? value : std::numeric_limits<double>::quiet_NaN();
    };
    const auto readOffset = [&] {
        wchar_t field[48]{};
        GetDlgItemTextW(dialog, IDC_TEXT_WIDTH, field, static_cast<int>(std::size(field)));
        return _wcsicmp(field, L"Off") == 0 ? -5.0 : read(IDC_TEXT_WIDTH);
    };
    const auto synchronizeLabels = [&](int changed) {
        const double line = read(IDC_LINE_WIDTH);
        const double offset = readOffset();
        const double size = read(IDC_LABEL_SIZE);
        if (!(line >= 0.01 && line <= 5 && offset >= -5 && offset <= 5 &&
              size >= 0 && size <= 1000)) return;
        const bool strokeOff = plotTextWidth(line, offset) == 0;
        if ((changed == IDC_LABEL_SIZE && size == 0) ||
            (changed != IDC_LABEL_SIZE && strokeOff)) {
            SetDlgItemTextW(dialog, IDC_TEXT_WIDTH, L"Off");
            SetDlgItemTextW(dialog, IDC_LABEL_SIZE, L"0");
        } else if (changed == IDC_LABEL_SIZE && strokeOff) {
            SetDlgItemTextW(dialog, IDC_TEXT_WIDTH, L"0");
        } else if (changed == IDC_TEXT_WIDTH && size == 0) {
            SetDlgItemTextW(dialog, IDC_LABEL_SIZE, L"100");
        }
    };
    if (HIWORD(wParam) == EN_KILLFOCUS &&
        (action == IDC_LABEL_SIZE || action == IDC_TEXT_WIDTH || action == IDC_LINE_WIDTH)) {
        const HWND edit = GetDlgItem(dialog, action);
        if (SendMessageW(edit, EM_GETMODIFY, 0, 0)) {
            synchronizeLabels(action);
            SendMessageW(edit, EM_SETMODIFY, FALSE, 0);
        }
        return TRUE;
    }
    if (action != IDOK && action != IDC_TEST_LINES && action != IDC_TEST_SQUARES) return FALSE;
    const int focused = GetDlgCtrlID(GetFocus());
    if (focused == IDC_LABEL_SIZE || focused == IDC_TEXT_WIDTH || focused == IDC_LINE_WIDTH) {
        if (SendMessageW(GetFocus(), EM_GETMODIFY, 0, 0)) synchronizeLabels(focused);
    }

    const double width = S.cfg.paperWidthCm * 10;
    const double margin = read(IDC_RIGHT_MARGIN);
    const double spacing = read(IDC_SPACING);
    const double shift = read(IDC_SHIFT);
    const double lineWidth = read(IDC_LINE_WIDTH);
    if (!(lineWidth >= 0.01 && lineWidth <= 5.0)) {
        MessageBoxW(dialog, L"Enter a line width between 0.01 and 5 mm.",
                    L"Configuration", MB_OK | MB_ICONWARNING);
        SetFocus(GetDlgItem(dialog, IDC_LINE_WIDTH));
        return TRUE;
    }
    double textWidth = readOffset();
    double labelSize = read(IDC_LABEL_SIZE);
    if (!(textWidth >= -5 && textWidth <= 5)) {
        MessageBoxW(dialog, L"Enter a text stroke offset from -5 to 5 mm, or Off.",
                    L"Configuration", MB_OK | MB_ICONWARNING);
        SetFocus(GetDlgItem(dialog, IDC_TEXT_WIDTH));
        return TRUE;
    }
    if (!(labelSize >= 0 && labelSize <= 1000)) {
        MessageBoxW(dialog, L"Enter a label size from 0 to 1000 percent.",
                    L"Configuration", MB_OK | MB_ICONWARNING);
        SetFocus(GetDlgItem(dialog, IDC_LABEL_SIZE));
        return TRUE;
    }
    if (!(margin >= 0 && margin < width &&
          spacing >= 0 && spacing <= 10000 && shift >= -32768 && shift <= 32767)) {
        MessageBoxW(dialog, L"Enter valid millimeter values, a right margin smaller than the paper width, and a shift between -32768 and 32767 mm.",
                    L"Configuration", MB_OK | MB_ICONWARNING);
        return TRUE;
    }
    vega::HeadLayout heads;
    heads.mask = 0;
    for (unsigned slot = 0; slot < vega::headSlots; ++slot) {
        if (IsDlgButtonChecked(dialog, IDC_HEAD_1 + kPhysicalHeadForSlot[slot] - 1) == BST_CHECKED) heads.mask |= 1u << slot;
        const auto offset = read(IDC_HEAD_1_OFFSET + kPhysicalHeadForSlot[slot] - 1);
        if (!std::isfinite(offset) || std::abs(offset) > 327.67) {
            MessageBoxW(dialog, L"Enter head offsets between -327.67 and 327.67 mm.",
                        L"Configuration", MB_OK | MB_ICONWARNING);
            SetFocus(GetDlgItem(dialog, IDC_HEAD_1_OFFSET + kPhysicalHeadForSlot[slot] - 1));
            return TRUE;
        }
        heads.offsetMm[slot] = offset;
    }
    if (!heads.mask) {
        MessageBoxW(dialog, L"Select at least one print head.", L"Configuration", MB_OK | MB_ICONWARNING);
        return TRUE;
    }
    S.cfg.heads = heads;
    S.cfg.rightMarginMm = margin;
    S.cfg.spacingMm = spacing;
    S.cfg.shift = shift;
    S.cfg.lineWidthMm = lineWidth;
    if (labelSize == 0 || plotTextWidth(lineWidth, textWidth) == 0) {
        labelSize = 0;
        textWidth = -5.0;
        SetDlgItemTextW(dialog, IDC_TEXT_WIDTH, L"Off");
    }
    S.cfg.textWidthMm = textWidth;
    S.cfg.labelSizePercent = labelSize;
    S.cfg.bidi = IsDlgButtonChecked(dialog, IDC_BIDI) == BST_CHECKED;
    S.cfg.rotation = IsDlgButtonChecked(dialog, IDC_ROTATE_AUTO) == BST_CHECKED ? 90 :
                     IsDlgButtonChecked(dialog, IDC_ROTATE_180) == BST_CHECKED ? 180 : 0;
    const auto save = [&](const wchar_t* key, double value) {
        wchar_t number[48];
        swprintf(number, std::size(number), L"%.8g", value);
        WritePrivateProfileStringW(L"Plotter", key, number, S.cfg.ini.c_str());
    };
    save(L"RightMarginMm", margin);
    save(L"SpacingMm", spacing);
    save(L"ShiftMm", shift);
    save(L"LineWidthMm", lineWidth);
    save(L"TextStrokeOffsetMm", textWidth);
    save(L"LabelSizePercent", labelSize);
    save(L"HeadMask", heads.mask);
    for (unsigned slot = 0; slot < vega::headSlots; ++slot) {
        const auto key = L"Head" + std::to_wstring(slot + 1) + L"OffsetMm";
        save(key.c_str(), heads.offsetMm[slot]);
    }
    save(L"Bidi", S.cfg.bidi);
    save(L"Rotation", S.cfg.rotation);
    for (size_t i = 0; i < S.layers.size(); ++i) computeBox(i);
    InvalidateRect(S.list, nullptr, FALSE);
    rebuildView();
    EndDialog(dialog, action);
    return TRUE;
}

void requestMaintenance(MaintenanceAction action) {
    if (!canPrint()) return;
    S.maintenanceRunning = true;
    g_advancePaper = action == MaintenanceAction::AdvancePaper;
    updatePrintButton();
    if (g_advancePaper) {
        SetWindowTextW(S.btnAdvancePaper, L"Stop paper");
        EnableWindow(S.btnAdvancePaper, TRUE);
    }
    EnableWindow(S.btnTestUsb, FALSE);
    setStatus(action == MaintenanceAction::CleanHeads ? L"Cleaning VEGA heads..." : L"Advancing paper...", false);
    g_maintenance.store(action);
}

std::filesystem::path printHistoryPath() {
    return std::filesystem::path(S.cfg.ini).parent_path() / L"Ralvioli Plotter.history.txt";
}

void appendPrintHistory(const std::wstring& title, const std::wstring& outcome, int pct) {
    SYSTEMTIME now{};
    GetLocalTime(&now);
    wchar_t date[32];
    std::swprintf(date, std::size(date), L"%04u-%02u-%02u %02u:%02u",
                  static_cast<unsigned>(now.wYear), static_cast<unsigned>(now.wMonth),
                  static_cast<unsigned>(now.wDay), static_cast<unsigned>(now.wHour), static_cast<unsigned>(now.wMinute));
    std::ofstream stream(printHistoryPath(), std::ios::binary | std::ios::app);
    stream << utf8(std::wstring(date) + L"  " + title + L"  " + outcome + L"  " + std::to_wstring(pct) + L"%") << "\r\n";
}

INT_PTR CALLBACK historyDialog(HWND dialog, UINT msg, WPARAM wParam, LPARAM) {
    if (msg == WM_INITDIALOG) {
        std::ifstream input(printHistoryPath(), std::ios::binary);
        if (input) {
            SetDlgItemTextW(dialog, IDC_HISTORY_TEXT, wide(std::string(std::istreambuf_iterator<char>(input), {})).c_str());
        } else {
            SetDlgItemTextW(dialog, IDC_HISTORY_TEXT, L"No Ralvioli print history yet.");
        }
        return TRUE;
    }
    if (msg == WM_COMMAND && LOWORD(wParam) == IDCANCEL) {
        EndDialog(dialog, IDCANCEL);
        return TRUE;
    }
    return FALSE;
}

double maintenanceSpanMm(double paperMm, const Settings& cfg) {
    const double stroke = std::max(1.0, std::round(cfg.lineWidthMm * vega::columnsPerMm));
    const double shift = cfg.bidi ? std::round(-cfg.shift * vega::columnsPerMm) : 0;
    const double available = paperMm * vega::columnsPerMm - std::max(0.0, shift);
    return (std::min(available, 24000.0 - std::abs(shift)) - stroke - 1) / vega::columnsPerMm;
}

rm::Piece maintenancePattern(bool squares, double spanMm, const cal::Factors& calibration) {
    rm::Piece piece;
    const double across = kHpglOutputStepMm * calibration.across;
    const double feed = kHpglOutputStepMm * calibration.feed;
    const auto path = [&](std::initializer_list<rm::Pt> points) {
        rm::Op move(rm::K::Pts);
        move.verb = "PU";
        move.pts = {*points.begin()};
        piece.ops.push_back(std::move(move));
        rm::Op draw(rm::K::Pts);
        draw.verb = "PD";
        draw.pts.assign(points.begin() + 1, points.end());
        piece.ops.push_back(std::move(draw));
    };
    if (squares) {
        constexpr double squareMm = 15;
        constexpr double gapMm = 5;
        const int count = std::max(1, static_cast<int>(std::round((spanMm + gapMm) / (squareMm + gapMm))));
        const double gap = std::min(gapMm, spanMm / (2 * count));
        const double side = (spanMm - gap * (count - 1)) / count;
        for (int i = 0; i < count; ++i) {
            const double first = i * (side + gap) / across;
            const double last = (i + 1 == count ? spanMm : i * (side + gap) + side) / across;
            path({{0, first}, {side / feed, first}, {side / feed, last}, {0, last}, {0, first}});
        }
    } else {
        for (int i = 0; i < 5; ++i) {
            const double x = i * 20.0 / feed;
            path({{x, 0}, {x, spanMm / across}});
        }
    }
    return piece;
}

void startMaintenancePattern(bool squares) {
    if (!canPrint()) return;
    const auto width = paperUnits();
    const double span = maintenanceSpanMm(width.value_or(0) * kHpglOutputStepMm, S.cfg);
    if (!(span > 0)) {
        setStatus(L"Enter a paper width that leaves room for the test pattern.", true);
        return;
    }
    Job job;
    job.index = -2;
    job.title = squares ? L"VEGA test squares" : L"VEGA test lines";
    job.piece = maintenancePattern(squares, span, S.calibration);
    job.stream = rp::makeStream(job.piece, kHpglOutputStepMm, S.calibration);
    job.plan = rp::makePlan(job.stream, 0, 100);
    std::vector<Job> jobs;
    jobs.push_back(std::move(job));
    startVegaJobs(std::move(jobs), *width);
}

std::filesystem::path choosePrintState(bool saving) {
    wchar_t path[32768]{};
    if (saving) wcscpy_s(path, L"VEGA Print.pltsave");
    OPENFILENAMEW dialog{};
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner = S.hwnd;
    dialog.lpstrFilter = saving ? L"Plotter saved state (*.pltsave)\0*.pltsave\0" :
        L"Plotter saved state (*.pltsave)\0*.pltsave\0Legacy VEGA checkpoint (*.vega-job)\0*.vega-job\0";
    dialog.lpstrFile = path;
    dialog.nMaxFile = static_cast<DWORD>(std::size(path));
    dialog.lpstrDefExt = saving ? L"pltsave" : nullptr;
    dialog.lpstrTitle = saving ? L"Save Print State" : L"Load Print State";
    dialog.Flags = OFN_EXPLORER | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR |
                   (saving ? OFN_OVERWRITEPROMPT : OFN_FILEMUSTEXIST);
    if (!(saving ? GetSaveFileNameW(&dialog) : GetOpenFileNameW(&dialog))) return {};
    std::filesystem::path selected(path);
    if (saving && _wcsicmp(selected.extension().c_str(), L".pltsave") != 0) {
        setStatus(L"Use the .pltsave extension to save the print state.", true);
        return {};
    }
    return selected;
}

void startSavedPrint(std::filesystem::path path) {
    if (path.empty() || S.running || S.maintenanceRunning || g_paperStopPending) return;
    auto args = std::make_unique<ThreadArg>();
    args->cfg = S.cfg;
    args->hwnd = S.hwnd;
    args->loadSaved = std::move(path);
    g_stop = false;
    g_pause = true;
    g_seekBand = kNoBand;
    g_savePrint = false;
    g_reconnect = false;
    S.vegaSession = true;
    setRunning(true);
    EnableWindow(S.btnPause, FALSE);
    SetWindowTextW(S.btnTestUsb, L"Reconnect");
    setStatus(L"Loading the saved VEGA print...", false);
    if (S.connectionThread) {
        S.pendingPrint = std::move(args);
        SetEvent(S.disconnect);
        return;
    }
    auto* raw = args.release();
    S.thread = CreateThread(nullptr, 0, vegaPrintThread, raw, 0, nullptr);
    if (!S.thread) {
        delete raw;
        setStatus(L"Could not start the VEGA print worker.", true);
        PostMessageW(S.hwnd, kMsgFinished, 0, 0);
    }
}

LRESULT CALLBACK MainProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_DROPFILES:
            addDroppedFiles(reinterpret_cast<HDROP>(wParam));
            return 0;
        case WM_CREATE:
            S.hwnd = hwnd;
            S.updating = true;
            createControls(hwnd);
            DragAcceptFiles(hwnd, TRUE);
            DragAcceptFiles(S.list, TRUE);
            DragAcceptFiles(S.preview, TRUE);
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
            for (size_t i = 0; i < std::size(S.indicators); ++i) {
                if (reinterpret_cast<HWND>(lParam) != S.indicators[i]) continue;
                const auto tone = S.indicatorTones[i];
                if (tone == IndicatorTone::Neutral) break;
                const bool good = tone == IndicatorTone::Good;
                const HBRUSH brush = good ? S.goodIndicator : S.faultIndicator;
                if (!brush) break;
                const HDC dc = reinterpret_cast<HDC>(wParam);
                SetTextColor(dc, good ? RGB(0x15, 0x4d, 0x26) : RGB(0x65, 0x1b, 0x1b));
                SetBkMode(dc, TRANSPARENT);
                return reinterpret_cast<LRESULT>(brush);
            }
            if (reinterpret_cast<HWND>(lParam) == S.status) {
                HDC dc = reinterpret_cast<HDC>(wParam);
                const bool error = S.statusError || !S.queueWarning.empty() || !plotterFault(S.statusWord).empty() ||
                                   !inkStatusWarning(S.statusWord).empty() || !paperStatusWarning(S.statusWord).empty();
                SetTextColor(dc, error ? RGB(0xc6, 0x28, 0x28) : RGB(0x20, 0x20, 0x20));
                SetBkColor(dc, GetSysColor(COLOR_BTNFACE));
                return reinterpret_cast<LRESULT>(GetSysColorBrush(COLOR_BTNFACE));
            }
            break;
        case WM_COMMAND:
            switch (LOWORD(wParam)) {
                case ID_ADD:
                    addFiles(pickFiles());
                    break;
                case ID_REMOVE:
                    removeSelected();
                    break;
                case ID_FIT:
                    fitAll();
                    break;
                case ID_FILL:
                    fillView();
                    break;
                case ID_PRINT_AUTO:
                    if (HIWORD(wParam) == BN_CLICKED) {
                        fitAll();
                        rebuildView();
                        showIndicators(S.statusWord);
                    }
                    break;
                case ID_START:
                    startPrint();
                    break;
                case ID_SERVER_CONFIG: {
                    const auto action = DialogBoxParamW(S.inst, MAKEINTRESOURCEW(IDD_SERVER_CONFIG),
                                                        hwnd, ServerConfigProc, 0);
                    if (action == IDC_TEST_LINES || action == IDC_TEST_SQUARES)
                        startMaintenancePattern(action == IDC_TEST_SQUARES);
                    break;
                }
                case ID_ADVANCE_PAPER:
                    if (g_advancePaper.exchange(false)) {
                        EnableWindow(S.btnAdvancePaper, FALSE);
                        setStatus(L"Stopping paper advance...", false);
                    } else {
                        requestMaintenance(MaintenanceAction::AdvancePaper);
                    }
                    break;
                case ID_CLEAN_HEADS:
                    requestMaintenance(MaintenanceAction::CleanHeads);
                    break;
                case ID_TEST_USB:
                    if (S.waitingForReconnect) {
                        g_reconnect = true;
                        EnableWindow(S.btnTestUsb, FALSE);
                        setStatus(L"Reconnecting to the paused VEGA job...", false);
                    } else if (!S.running && !S.maintenanceRunning) testUsb();
                    break;
                case ID_PAUSE:
                    if (S.vegaSession && S.running && !g_stop && !S.waitingForReconnect && !S.savingPrint) {
                        const bool pause = !g_pause;
                        if (!pause && S.bandCursor &&
                            (S.bandCursor->needsOrigin || S.bandCursor->selected < S.bandCursor->natural) &&
                            S.bandCursor->selected < S.bandCursor->count &&
                            MessageBoxW(hwnd, L"Align the paper at the original start of this job before restarting. "
                                               L"VEGA will advance past earlier bands without printing them. Continue?",
                                        L"Resume at selected band", MB_OKCANCEL | MB_ICONQUESTION | MB_DEFBUTTON2) != IDOK)
                            break;
                        g_pause = pause;
                        if (!pause) S.bandCursor.reset();
                        updateBandButtons();
                        InvalidateRect(S.preview, nullptr, FALSE);
                        SetWindowTextW(S.btnPause, pause ? L"Resume" : L"Pause");
                        setStatus(pause ? L"Pause requested." : L"Resume requested.", false);
                    }
                    break;
                case ID_PREVIOUS_BAND:
                    stepBand(-1);
                    break;
                case ID_NEXT_BAND:
                    stepBand(1);
                    break;
                case ID_SAVE_PRINT:
                    if (S.vegaSession && S.running && !g_stop && !S.savingPrint) {
                        g_pause = true;
                        SetWindowTextW(S.btnPause, L"Resume");
                        auto path = choosePrintState(true);
                        if (path.empty() || !S.running || g_stop) break;
                        g_savePrintPath = std::move(path);
                        S.savingPrint = true;
                        updatePrintButton();
                        EnableWindow(S.btnPause, FALSE);
                        EnableWindow(S.btnSavePrint, FALSE);
                        EnableWindow(S.btnTestUsb, FALSE);
                        setStatus(L"Saving the print at its confirmed VEGA band...", false);
                        g_savePrint = true;
                    }
                    break;
                case ID_LOAD_PRINT:
                    if (!S.running && !S.maintenanceRunning && !g_paperStopPending)
                        startSavedPrint(choosePrintState(false));
                    break;
                case ID_STOP:
                    g_resetOnDisconnect = true;
                    g_stop = true;
                    EnableWindow(S.btnPause, FALSE);
                    updatePrintButton();
                    setStatus(L"Stopping...", false);
                    break;
                case ID_RESET_VEGA:
                    resetPlotter();
                    break;
                case ID_LIST:
                    if (HIWORD(wParam) == LBN_SELCHANGE) {
                        onSelect();
                    }
                    break;
                case ID_FROM:
                case ID_TO:
                    if (HIWORD(wParam) == EN_CHANGE) {
                        applyRange();
                    }
                    break;
                default:
                    break;
            }
            return 0;
        case kMsgProg: {
            if (static_cast<int>(wParam) == -1) {
                if (!g_stop && !g_pause) {
                    const auto progress = L"Printing test rectangle on VEGA — " + std::to_wstring(lParam) + L"%";
                    setStatus(progress, false);
                    if (S.calibrationWindow)
                        SetDlgItemTextW(S.calibrationWindow, IDC_TEST_RECT_STATUS, progress.c_str());
                }
                return 0;
            }
            if (static_cast<int>(wParam) == -2) {
                if (!g_stop && !g_pause) setStatus(L"Printing on VEGA \u2014 " + std::to_wstring(lParam) + L"%", false);
                return 0;
            }
            size_t i = static_cast<size_t>(wParam);
            if (i >= S.layers.size()) {
                return 0;
            }
            Layer& l = S.layers[i];
            l.sent = static_cast<size_t>(lParam);
            l.pct = l.total ? static_cast<int>(std::min<size_t>(100, l.sent * 100 / l.total)) : 0;
            l.status = std::to_wstring(l.pct) + L"%";
            invalidateRow(static_cast<int>(i));
            if (!g_stop && !g_pause) {
                setStatus(std::wstring(L"Printing ") + baseName(l.path) +
                          L" — " + std::to_wstring(l.pct) + L"%", false);
            }
            if (static_cast<int>(i) == S.shown) {
                InvalidateRect(S.preview, nullptr, FALSE);
            }
            return 0;
        }
        case kMsgDone: {
            if (static_cast<int>(wParam) == -2) {
                if (lParam == 0) setStatus(L"VEGA print finished.", false);
                if (lParam == 1) setStatus(L"VEGA print stopped.", false);
                return 0;
            }
            if (static_cast<int>(wParam) == -1) {
                const wchar_t* status = lParam == 0 ? L"Test rectangle printed on VEGA." :
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
                l.status = L"done";
                setStatus(std::wstring(L"Finished ") + baseName(l.path), false);
            } else if (lParam == 1) {
                l.status = L"stopped " + std::to_wstring(l.pct) + L"%";
                setStatus(L"Stopped " + baseName(l.path) + L" at " + std::to_wstring(l.pct) + L"%", false);
            } else {
                l.status = L"error";
            }
            appendPrintHistory(baseName(l.path), l.status, l.pct);
            invalidateRow(static_cast<int>(i));
            if (static_cast<int>(i) == S.shown) {
                InvalidateRect(S.preview, nullptr, FALSE);
            }
            return 0;
        }
        case kMsgError: {
            std::unique_ptr<std::string> s(reinterpret_cast<std::string*>(lParam));
            const auto detail = wide(*s);
            setStatus(detail, true);
            if (S.running && S.calibrationWindow)
                SetDlgItemTextW(S.calibrationWindow, IDC_TEST_RECT_STATUS, detail.c_str());
            return 0;
        }
        case kMsgVegaRecovery:
            S.recoveryRequired = wParam == 0;
            if (!S.recoveryRequired && S.connectionThread &&
                WaitForSingleObject(S.disconnect, 0) == WAIT_TIMEOUT) {
                S.maintenanceRunning = false;
                EnableWindow(S.btnTestUsb, TRUE);
                updatePrintButton();
                setStatus(L"VEGA reset and connected", false);
            }
            return 0;
        case kMsgReconnect:
            if (S.running && !g_stop) {
                S.waitingForReconnect = wParam != 0;
                EnableWindow(S.btnTestUsb, S.waitingForReconnect && !S.savingPrint);
                EnableWindow(S.btnPause, !S.waitingForReconnect && !S.savingPrint);
                EnableWindow(S.btnSavePrint, !S.savingPrint);
                SetWindowTextW(S.btnTestUsb, S.waitingForReconnect ? L"Reconnect" : L"Connected");
                SetWindowTextW(S.btnPause, L"Resume");
                if (S.waitingForReconnect) {
                    if (wParam == 1) showIndicators(-2);
                    const size_t row = static_cast<size_t>(lParam);
                    if (row < S.layers.size()) {
                        auto& layer = S.layers[row];
                        layer.status = L"paused " + std::to_wstring(layer.pct) + L"%";
                        invalidateRow(static_cast<int>(row));
                    }
                } else {
                    setStatus(L"VEGA connected. Press Resume to continue.", false);
                }
            }
            return 0;
        case kMsgSaved:
            S.savingPrint = false;
            updatePrintButton();
            if (S.running && !g_stop) {
                EnableWindow(S.btnSavePrint, TRUE);
                EnableWindow(S.btnPause, !S.waitingForReconnect);
                EnableWindow(S.btnTestUsb, S.waitingForReconnect);
                if (!wParam) setStatus(L"Print state saved.", false);
            }
            return 0;
        case kMsgPauseState:
            if (S.running && !g_stop && g_pause == (wParam != 0))
                setStatus(wParam ? L"Print paused." : L"Print resumed.", false);
            return 0;
        case kMsgBandCursor: {
            std::unique_ptr<BandCursor> cursor(reinterpret_cast<BandCursor*>(lParam));
            if (S.running && g_pause && !g_stop && cursor->count) {
                if (S.bandCursor && S.bandCursor->layer == cursor->layer &&
                    S.bandCursor->natural == cursor->natural &&
                    (S.bandCursor->needsOrigin == cursor->needsOrigin ||
                     S.bandCursor->selected != S.bandCursor->natural))
                    cursor->selected = S.bandCursor->selected;
                S.bandCursor = *cursor;
                updateBandButtons();
                InvalidateRect(S.preview, nullptr, FALSE);
            }
            return 0;
        }
        case kMsgPrintStatus:
            if (S.running) showIndicators(static_cast<VegaStatus>(lParam));
            return 0;
        case kMsgHeadCleanDone: {
            std::unique_ptr<std::string> detail(reinterpret_cast<std::string*>(lParam));
            S.maintenanceRunning = false;
            g_advancePaper = false;
            SetWindowTextW(S.btnAdvancePaper, L"Advance paper");
            EnableWindow(S.btnTestUsb, !S.running);
            updatePrintButton();
            const bool cleaning = (wParam & 2) != 0;
            const bool ok = (wParam & 1) != 0;
            if (ok) setStatus(cleaning ? L"VEGA head cleaning finished" : L"Paper advance finished", false);
            else setStatus((cleaning ? L"VEGA head cleaning failed: " : L"Paper advance failed: ") + wide(*detail), true);
            return 0;
        }
        case kMsgUsbResult: {
            if (wParam) {
                if (S.connectionThread && WaitForSingleObject(S.disconnect, 0) == WAIT_TIMEOUT) {
                    if (S.statusWord < 0 && !S.reconnectingAfterPrint) setStatus(L"VEGA connected", false);
                    S.reconnectingAfterPrint = false;
                    const auto word = static_cast<VegaStatus>(wParam - 1);
                    if (word != S.statusWord) showIndicators(word);
                    updatePrintButton();
                }
            } else {
                std::unique_ptr<std::string> result(reinterpret_cast<std::string*>(lParam));
                if (S.connectionThread) {
                    WaitForSingleObject(S.connectionThread, INFINITE);
                    CloseHandle(S.connectionThread);
                    CloseHandle(S.disconnect);
                    S.connectionThread = nullptr;
                    S.disconnect = nullptr;
                }
                if (S.pendingPrint) {
                    auto args = std::move(S.pendingPrint);
                    if (!g_stop) {
                        auto* raw = args.release();
                        S.thread = CreateThread(nullptr, 0, vegaPrintThread, raw, 0, nullptr);
                        if (S.thread) return 0;
                        delete raw;
                        setStatus(L"Could not start the VEGA print worker.", true);
                    }
                    PostMessageW(hwnd, kMsgFinished, 0, 0);
                    return 0;
                }
                if (S.resetPending && !S.running) {
                    S.resetPending = false;
                    testUsb(false, true);
                    updatePrintButton();
                    return 0;
                }
                S.reconnectingAfterPrint = false;
                S.maintenanceRunning = false;
                g_maintenance.store(MaintenanceAction::None);
                showIndicators(result && !result->empty() ? -2 : -1);
                updatePrintButton();
                SetWindowTextW(S.btnTestUsb, g_paperStopPending ? L"Reconnect" : L"Connect");
                EnableWindow(S.btnTestUsb, !S.running);
                if (result && !result->empty()) {
                    setStatus((S.recoveryRequired ? L"VEGA recovery failed: " : L"VEGA connection lost: ") +
                              wide(*result), true);
                }
                else setStatus(L"VEGA disconnected", false);
            }
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
            showIndicators(S.statusWord == -2 ? -2 : -1);
            S.vegaSession = false;
            S.savingPrint = false;
            S.waitingForReconnect = false;
            S.bandCursor.reset();
            g_pause = false;
            g_seekBand = kNoBand;
            g_savePrint = false;
            setRunning(false);
            SetWindowTextW(S.btnTestUsb, L"Connect");
            if (S.resetPending) {
                S.resetPending = false;
                testUsb(false, true);
                updatePrintButton();
            } else if (wParam && !g_stop) testUsb(true);
            updateUsbStatus();
            showStatus();
            return 0;
        case WM_CLOSE:
            S.resetPending = false;
            g_resetOnDisconnect = true;
            g_stop = true;
            if (S.connectionThread) {
                SetEvent(S.disconnect);
                WaitForSingleObject(S.connectionThread, INFINITE);
                MSG pending{};
                while (PeekMessageW(&pending, hwnd, kMsgUsbResult, kMsgHeadCleanDone, PM_REMOVE))
                    DispatchMessageW(&pending);
            }
            if (S.running) {
                g_stop = true;
                if (S.thread != nullptr) {
                    WaitForSingleObject(S.thread, INFINITE);
                }
            }
            if (g_resetOnDisconnect.exchange(false)) {
                std::string detail;
                resetBeforeDisconnect(S.cfg, detail);
            }
            DestroyWindow(hwnd);
            return 0;
        case WM_DESTROY:
            if (S.goodIndicator) DeleteObject(S.goodIndicator);
            if (S.faultIndicator) DeleteObject(S.faultIndicator);
            S.goodIndicator = S.faultIndicator = nullptr;
            KillTimer(hwnd, kUsbStatusTimer);
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
    bool openedMergerExport = false;
    int argc = 0;
    if (LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc)) {
        if (argc == 4 && wcscmp(argv[1], L"--merger-export") == 0 &&
            wcslen(argv[2]) == 1 && argv[2][0] >= L'0' && argv[2][0] <= L'2') {
            addFiles({argv[3]}, argv[2][0] - L'0');
            openedMergerExport = true;
        }
        LocalFree(argv);
    }
    ShowWindow(hwnd, show);
    UpdateWindow(hwnd);
    const auto recovery = autoRecoveryPath(S.cfg);
    const DWORD recoveryAttributes = GetFileAttributesW(recovery.c_str());
    if (!openedMergerExport && recoveryAttributes != INVALID_FILE_ATTRIBUTES && !(recoveryAttributes & FILE_ATTRIBUTE_DIRECTORY) &&
        MessageBoxW(hwnd, L"An unfinished print was saved in autorecovery.pltsave. Load it?",
                    L"Recover Print", MB_YESNO | MB_ICONQUESTION) == IDYES) {
        startSavedPrint(recovery);
    }

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return static_cast<int>(msg.wParam);
}
