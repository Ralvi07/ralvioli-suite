// Manages the Win32 viewer and its per-file preview, rotation, cropping, and export controls.
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>
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
#include "PreviewNavigation.hpp"

namespace {

enum {
    ID_OPEN = 101,
    ID_LEFT,
    ID_RIGHT,
    ID_CROP,
    ID_APPLY,
    ID_HEADER,
    ID_UNDO,
    ID_ZOOMOUT,
    ID_ZOOMIN,
    ID_FIT,
    ID_UNITS_IN,
    ID_UNITS_OUT,
    ID_SAVE,
    ID_ESCAPE,
    ID_ENTER
};

const double kUnitMm[3] = {0.025, 0.01, 0.0254};
const wchar_t* const kUnitText[3] = {L"0.025 mm (HP-GL/2)", L"0.01 mm", L"0.0254 mm"};
const LPCWSTR kCursorArrow = MAKEINTRESOURCEW(32512);
const LPCWSTR kCursorCross = MAKEINTRESOURCEW(32515);
const LPCWSTR kCursorMove = MAKEINTRESOURCEW(32646);
const COLORREF kDrawColor = RGB(0x15, 0x65, 0xc0);

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
    HWND preview = nullptr;
    HWND cbIn = nullptr;
    HWND cbOut = nullptr;
    HWND btnCrop = nullptr;
    HWND btnApply = nullptr;
    HWND btnUndo = nullptr;
    std::vector<HWND> needsFile;
    HFONT font = nullptr;
    HFONT bold = nullptr;
    int dpi = 96;
    std::vector<Slot> slots;
    int minWidth = 0;
    std::string path;
    bool has = false;
    int inIdx = 0;
    int outIdx = 0;
    rm::Piece doc;
    std::vector<rm::Piece> undo;
    std::vector<rm::Item> shapes;
    std::optional<rm::Box> box;
    bool cropMode = false;
    bool dragging = false;
    bool hasCrop = false;
    rm::Pt cropA;
    rm::Pt cropB;
    bool autoFit = true;
    double vx = 0.0;
    double vy = 0.0;
    double vs = 1.0;
    int cw = 0;
    int ch = 0;
    UINT panButton = 0;
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

std::wstring baseName(const std::wstring& path) {
    size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? path : path.substr(slash + 1);
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

void updateTitle() {
    std::wstring t = L"Ralvioli Viewer";
    if (S.has) {
        t += L" - " + baseName(wide(S.path));
    }
    SetWindowTextW(S.hwnd, t.c_str());
}

void updateButtons() {
    for (HWND h : S.needsFile) {
        EnableWindow(h, S.has ? TRUE : FALSE);
    }
    EnableWindow(S.btnApply, (S.has && S.hasCrop) ? TRUE : FALSE);
    EnableWindow(S.btnUndo, (S.has && !S.undo.empty()) ? TRUE : FALSE);
    SendMessageW(S.btnCrop, BM_SETCHECK, S.cropMode ? BST_CHECKED : BST_UNCHECKED, 0);
}

void refreshShapes() {
    S.shapes = rm::renderLayer(S.doc);
    S.box = rm::boxOf(S.doc.ops);
    InvalidateRect(S.preview, nullptr, FALSE);
    updateButtons();
}

bool fitView(bool apply = true) {
    if (!S.box) return false;
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
    double sc = std::min((S.cw - 2 * m) / std::max(maxx - minx, 1.0), (S.ch - 2 * m) / std::max(maxy - miny, 1.0));
    const double fitted = clampScale(sc);
    if (apply) {
        S.vx = minx;
        S.vy = miny;
        S.vs = fitted;
    }
    return S.vs > fitted * 1.001;
}

void ensureView() {
    RECT rc;
    GetClientRect(S.preview, &rc);
    S.cw = rc.right;
    S.ch = rc.bottom;
    if (!S.has || S.cw < 30 || S.ch < 30) {
        return;
    }
    if (S.autoFit) {
        fitView();
    }
}

std::pair<double, double> toPaper(int x, int y) {
    double m = dp(10);
    return {S.vx + (x - m) / S.vs, S.vy + (S.ch - m - y) / S.vs};
}

void zoomAt(double f, int x, int y) {
    ensureView();
    if (!S.has) {
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

void loadFile(const std::wstring& wpath) {
    std::string path = utf8(wpath);
    try {
        rm::Parsed parsed = rm::parse(path, kUnitMm[S.inIdx]);
        rm::warnings().clear();
        S.doc = rm::Piece{parsed.ops, rm::Pt{0.0, 0.0}};
    } catch (const std::exception& e) {
        message(wide(e.what()), L"Could not open the file", MB_ICONERROR);
        return;
    }
    S.path = path;
    S.has = true;
    S.undo.clear();
    S.cropMode = false;
    S.dragging = false;
    S.hasCrop = false;
    S.autoFit = true;
    size_t slash = wpath.find_last_of(L"\\/");
    if (slash != std::wstring::npos) {
        S.lastDir = wpath.substr(0, slash);
    }
    updateTitle();
    refreshShapes();
    if (!S.box) {
        message(L"There is nothing to draw in this file.", L"Empty file", MB_ICONINFORMATION);
    }
}

std::wstring pickOpen() {
    std::vector<wchar_t> buf(32768, 0);
    static const wchar_t filter[] = L"Plotter files\0*.plt;*.hpgl;*.hpg;*.hp;*.prn\0All files\0*.*\0";
    OPENFILENAMEW ofn;
    ZeroMemory(&ofn, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = S.hwnd;
    ofn.lpstrFilter = filter;
    ofn.lpstrFile = buf.data();
    ofn.nMaxFile = static_cast<DWORD>(buf.size());
    ofn.lpstrTitle = L"Open an HPGL file";
    ofn.Flags = OFN_EXPLORER | OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY;
    if (!S.lastDir.empty()) {
        ofn.lpstrInitialDir = S.lastDir.c_str();
    }
    if (!GetOpenFileNameW(&ofn)) {
        return std::wstring();
    }
    return std::wstring(buf.data());
}

void openFile() {
    std::wstring f = pickOpen();
    if (!f.empty()) {
        loadFile(f);
    }
}

void pushUndo() {
    S.undo.push_back(S.doc);
    if (S.undo.size() > 30) {
        S.undo.erase(S.undo.begin());
    }
}

void editDone() {
    S.hasCrop = false;
    S.dragging = false;
    S.autoFit = true;
    refreshShapes();
}

void turn(double delta) {
    if (!S.has) {
        return;
    }
    pushUndo();
    S.doc = rm::rotatePiece(S.doc, delta);
    editDone();
}

void removeHeader() {
    if (!S.has) {
        return;
    }
    std::vector<rm::Op> ops = S.doc.ops;
    if (!rm::removeHeaderText(ops)) {
        message(L"No header text was found in this file.", L"Header text", MB_ICONINFORMATION);
        return;
    }
    pushUndo();
    S.doc.ops = ops;
    editDone();
}

void undoEdit() {
    if (S.undo.empty()) {
        return;
    }
    S.doc = S.undo.back();
    S.undo.pop_back();
    S.hasCrop = false;
    S.dragging = false;
    refreshShapes();
}

void setCropMode(bool on) {
    S.cropMode = on;
    S.hasCrop = false;
    S.dragging = false;
    updateButtons();
    InvalidateRect(S.preview, nullptr, FALSE);
}

void applyCrop() {
    if (!S.has || !S.hasCrop) {
        return;
    }
    rm::Box r{std::min(S.cropA.x, S.cropB.x), std::min(S.cropA.y, S.cropB.y), std::max(S.cropA.x, S.cropB.x),
              std::max(S.cropA.y, S.cropB.y)};
    rm::Piece cropped = rm::cropPiece(S.doc, r);
    if (!rm::boxOf(cropped.ops)) {
        message(L"There is nothing inside the crop area.", L"Crop", MB_ICONINFORMATION);
        return;
    }
    pushUndo();
    S.doc = cropped;
    S.cropMode = false;
    editDone();
}

void escapeKey() {
    if (S.cropMode) {
        setCropMode(false);
    }
}

void saveFile() {
    if (!S.has) {
        return;
    }
    std::wstring first = baseName(wide(S.path));
    size_t dot = first.find_last_of(L'.');
    std::wstring stem = (dot == std::wstring::npos || dot == 0) ? first : first.substr(0, dot);
    std::wstring ext = (dot == std::wstring::npos || dot == 0) ? L".plt" : first.substr(dot);
    std::vector<wchar_t> buf(32768, 0);
    std::wstring initial = stem + L" edited" + ext;
    wcsncpy(buf.data(), initial.c_str(), buf.size() - 1);
    static const wchar_t filter[] = L"Plotter files\0*.plt;*.hpgl;*.hpg;*.hp;*.prn\0All files\0*.*\0";
    OPENFILENAMEW ofn;
    ZeroMemory(&ofn, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = S.hwnd;
    ofn.lpstrFilter = filter;
    ofn.lpstrFile = buf.data();
    ofn.nMaxFile = static_cast<DWORD>(buf.size());
    ofn.lpstrTitle = L"Save as";
    std::wstring defExt = ext.substr(1);
    ofn.lpstrDefExt = defExt.c_str();
    if (!S.lastDir.empty()) {
        ofn.lpstrInitialDir = S.lastDir.c_str();
    }
    ofn.Flags = OFN_EXPLORER | OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY;
    if (!GetSaveFileNameW(&ofn)) {
        return;
    }
    std::wstring out(buf.data());

    wchar_t outFull[MAX_PATH * 2] = {0};
    wchar_t inFull[MAX_PATH * 2] = {0};
    GetFullPathNameW(out.c_str(), MAX_PATH * 2, outFull, nullptr);
    GetFullPathNameW(wide(S.path).c_str(), MAX_PATH * 2, inFull, nullptr);
    CharLowerW(outFull);
    CharLowerW(inFull);
    if (wcscmp(inFull, outFull) == 0) {
        message(L"That would overwrite the original file.", L"Choose another name", MB_ICONERROR);
        return;
    }

    std::string text;
    try {
        text = rm::merge(std::vector<rm::Piece>{S.doc}, false, kUnitMm[S.outIdx]);
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
    message(L"Saved " + baseName(out), L"Done", MB_ICONINFORMATION);
}

void unitsInChanged() {
    int sel = static_cast<int>(SendMessageW(S.cbIn, CB_GETCURSEL, 0, 0));
    if (sel < 0 || sel > 2 || sel == S.inIdx) {
        return;
    }
    if (S.has) {
        if (!S.undo.empty()) {
            int answer = MessageBoxW(S.hwnd, L"Changing the file units reloads the file and discards your edits. Continue?",
                                     L"File units", MB_YESNO | MB_ICONQUESTION);
            if (answer != IDYES) {
                SendMessageW(S.cbIn, CB_SETCURSEL, S.inIdx, 0);
                return;
            }
        }
        S.inIdx = sel;
        loadFile(wide(S.path));
    } else {
        S.inIdx = sel;
    }
}

void unitsOutChanged() {
    int sel = static_cast<int>(SendMessageW(S.cbOut, CB_GETCURSEL, 0, 0));
    if (sel >= 0 && sel <= 2) {
        S.outIdx = sel;
    }
}

void startPan(int x, int y, UINT releaseButton) {
    ensureView();
    if (!S.has || S.dragging || S.panButton) return;
    // Middle-button panning works at every zoom level and preserves edit mode.
    S.autoFit = false;
    S.panButton = releaseButton;
    KillTimer(S.preview, preview::hoverTimer);
    S.panLast = POINT{x, y};
    SetCapture(S.preview);
}

void onLeftDown(int x, int y) {
    ensureView();
    if (!S.has) {
        return;
    }
    if (S.cropMode) {
        std::pair<double, double> p = toPaper(x, y);
        S.cropA = rm::Pt{p.first, p.second};
        S.cropB = S.cropA;
        S.dragging = true;
        S.hasCrop = false;
        SetCapture(S.preview);
        InvalidateRect(S.preview, nullptr, FALSE);
        updateButtons();
        return;
    }
    startPan(x, y, WM_LBUTTONUP);
}

void onMove(int x, int y) {
    if (S.panButton) {
        S.vx -= (x - S.panLast.x) / S.vs;
        S.vy += (y - S.panLast.y) / S.vs;
        S.panLast = POINT{x, y};
        InvalidateRect(S.preview, nullptr, FALSE);
        return;
    }
    if (S.dragging) {
        std::pair<double, double> p = toPaper(x, y);
        S.cropB = rm::Pt{p.first, p.second};
        InvalidateRect(S.preview, nullptr, FALSE);
    }
}

void onLeftUp() {
    if (S.panButton) {
        S.panButton = 0;
        ReleaseCapture();
    }
    if (S.dragging) {
        S.dragging = false;
        ReleaseCapture();
        S.hasCrop = std::fabs(S.cropB.x - S.cropA.x) * S.vs > 4 && std::fabs(S.cropB.y - S.cropA.y) * S.vs > 4;
        updateButtons();
        InvalidateRect(S.preview, nullptr, FALSE);
    }
}

void drawScene(HDC dc, int, int h) {
    ensureView();
    if (!S.has || S.shapes.empty()) {
        return;
    }
    const double m = dp(10);
    const double sc = S.vs;
    auto X = [&](double x) { return clampInt(m + (x - S.vx) * sc); };
    auto Y = [&](double y) { return clampInt(h - m - (y - S.vy) * sc); };

    std::map<std::pair<int, int>, HFONT> fonts;
    HGDIOBJ nullBrush = GetStockObject(NULL_BRUSH);
    SetBkMode(dc, TRANSPARENT);
    SetTextAlign(dc, TA_BASELINE | TA_LEFT);
    HPEN pen = CreatePen(PS_SOLID, 1, kDrawColor);
    HGDIOBJ oldPen = SelectObject(dc, pen);
    HGDIOBJ oldBrush = SelectObject(dc, nullBrush);
    SetTextColor(dc, kDrawColor);
    for (const rm::Item& it : S.shapes) {
        if (it.type == 0) {
            std::vector<POINT> pts;
            pts.reserve(it.pts.size());
            for (const rm::Pt& p : it.pts) {
                pts.push_back(POINT{X(p.x), Y(p.y)});
            }
            if (pts.size() > 1) {
                Polyline(dc, pts.data(), static_cast<int>(pts.size()));
            }
        } else if (it.type == 1) {
            Ellipse(dc, X(it.c.x - it.r), Y(it.c.y + it.r), X(it.c.x + it.r), Y(it.c.y - it.r));
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
            TextOutA(dc, X(it.c.x), Y(it.c.y), it.text.c_str(), static_cast<int>(it.text.size()));
            SelectObject(dc, oldFont);
        }
    }
    SelectObject(dc, oldBrush);
    SelectObject(dc, oldPen);
    DeleteObject(pen);
    for (auto& f : fonts) {
        DeleteObject(f.second);
    }

    if (S.cropMode && (S.dragging || S.hasCrop)) {
        int x0 = X(std::min(S.cropA.x, S.cropB.x));
        int x1 = X(std::max(S.cropA.x, S.cropB.x));
        int y0 = Y(std::max(S.cropA.y, S.cropB.y));
        int y1 = Y(std::min(S.cropA.y, S.cropB.y));
        HPEN dash = CreatePen(PS_DASH, 1, RGB(0xc6, 0x28, 0x28));
        HGDIOBJ op = SelectObject(dc, dash);
        HGDIOBJ ob = SelectObject(dc, nullBrush);
        Rectangle(dc, x0, y0, x1, y1);
        SelectObject(dc, ob);
        SelectObject(dc, op);
        DeleteObject(dash);
        double wcm = std::fabs(S.cropB.x - S.cropA.x) / rm::kUnitsPerCm;
        double hcm = std::fabs(S.cropB.y - S.cropA.y) / rm::kUnitsPerCm;
        wchar_t label[64];
        swprintf(label, 64, L"%.1f x %.1f cm", wcm, hcm);
        SelectObject(dc, S.font);
        SetTextAlign(dc, TA_TOP | TA_LEFT);
        SetTextColor(dc, RGB(0xc6, 0x28, 0x28));
        TextOutW(dc, x0 + dp(4), y0 + dp(4), label, static_cast<int>(wcslen(label)));
    }
}

bool previewZoomed() {
    return S.has && !S.autoFit && fitView(false);
}

int hitPreviewArrow(POINT mouse) {
    if (S.panButton || S.dragging || !previewZoomed()) return -1;
    return preview::hitArrow(mouse, S.cw, S.ch, dp);
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
                if (previewZoomed()) preview::drawArrows(mem, w, h, dp);
                BitBlt(dc, 0, 0, w, h, mem, 0, 0, SRCCOPY);
                SelectObject(mem, old);
                DeleteObject(bmp);
                DeleteDC(mem);
            }
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_LBUTTONDOWN:
            if (S.panButton || hitPreviewArrow({GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)}) >= 0) return 0;
            SetFocus(hwnd);
            onLeftDown(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
            return 0;
        case WM_MBUTTONDOWN:
            SetFocus(hwnd);
            startPan(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam), WM_MBUTTONUP);
            return 0;
        case WM_LBUTTONUP:
            if (S.panButton != WM_MBUTTONUP) onLeftUp();
            return 0;
        case WM_MBUTTONUP:
            if (S.panButton == msg) onLeftUp();
            return 0;
        case WM_CAPTURECHANGED:
            S.panButton = 0;
            return 0;
        case WM_MOUSEMOVE: {
            const POINT mouse{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            if (hitPreviewArrow(mouse) >= 0) SetTimer(hwnd, preview::hoverTimer, 25, nullptr);
            else {
                KillTimer(hwnd, preview::hoverTimer);
                onMove(mouse.x, mouse.y);
            }
            return 0;
        }
        case WM_TIMER: {
            if (wParam != preview::hoverTimer) break;
            const int arrow = preview::hoveredArrow(hwnd, hitPreviewArrow);
            if (arrow < 0) {
                KillTimer(hwnd, preview::hoverTimer);
                return 0;
            }
            const POINT direction = preview::previewDirections[arrow];
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
                if (preview::hoveredArrow(hwnd, hitPreviewArrow) >= 0) {
                    SetCursor(LoadCursorW(nullptr, IDC_HAND));
                    return TRUE;
                }
                LPCWSTR cursor = S.panButton ? kCursorMove : (S.cropMode ? kCursorCross : kCursorArrow);
                SetCursor(LoadCursorW(nullptr, cursor));
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

HWND makeButton(HWND parent, int id, const wchar_t* text, DWORD style) {
    HWND h = CreateWindowExW(0, L"BUTTON", text, WS_CHILD | WS_VISIBLE | WS_TABSTOP | style, 0, 0, 10, 10, parent,
                             reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), S.inst, nullptr);
    SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(S.font), TRUE);
    return h;
}

void addSlot(HWND h, int w, int ht, int gapAfter, bool glue, int extra) {
    S.slots.push_back(Slot{h, w, ht, gapAfter, glue, extra});
}

HWND addButton(HWND parent, int id, const wchar_t* text, int pad, int gapAfter, bool useBold, DWORD style,
               bool needsFile) {
    HWND h = makeButton(parent, id, text, style);
    if (useBold) {
        SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(S.bold), TRUE);
    }
    int w = textWidth(text, useBold ? S.bold : S.font) + dp(pad);
    addSlot(h, w, dp(32), gapAfter, false, 0);
    if (needsFile) {
        S.needsFile.push_back(h);
    }
    return h;
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

void createControls(HWND hwnd) {
    addButton(hwnd, ID_OPEN, L"Open file", 28, dp(22), false, BS_PUSHBUTTON, false);
    addButton(hwnd, ID_LEFT, L"Turn left", 28, dp(4), false, BS_PUSHBUTTON, true);
    addButton(hwnd, ID_RIGHT, L"Turn right", 28, dp(22), false, BS_PUSHBUTTON, true);
    S.btnCrop = addButton(hwnd, ID_CROP, L"Crop", 28, dp(4), false, BS_AUTOCHECKBOX | BS_PUSHLIKE, true);
    S.btnApply = addButton(hwnd, ID_APPLY, L"Apply crop", 28, dp(22), false, BS_PUSHBUTTON, false);
    addButton(hwnd, ID_HEADER, L"Remove header text", 28, dp(4), false, BS_PUSHBUTTON, true);
    S.btnUndo = addButton(hwnd, ID_UNDO, L"Undo", 28, dp(22), false, BS_PUSHBUTTON, false);
    addButton(hwnd, ID_ZOOMOUT, L"\u2212", 22, dp(2), false, BS_PUSHBUTTON, false);
    addButton(hwnd, ID_ZOOMIN, L"+", 22, dp(2), false, BS_PUSHBUTTON, false);
    addButton(hwnd, ID_FIT, L"Fit", 22, dp(22), false, BS_PUSHBUTTON, false);
    addLabel(hwnd, L"File units", dp(4));
    S.cbIn = addCombo(hwnd, ID_UNITS_IN, dp(22), false);
    addLabel(hwnd, L"Save as", dp(4));
    S.cbOut = addCombo(hwnd, ID_UNITS_OUT, dp(8), true);
    addButton(hwnd, ID_SAVE, L"Save file...", 30, 0, true, BS_PUSHBUTTON, true);

    int largest = 0;
    groupExtents(largest);
    S.minWidth = largest + dp(20);

    S.preview = CreateWindowExW(WS_EX_CLIENTEDGE, L"RalvioliViewerPreview", L"", WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS,
                                0, 0, 10, 10, hwnd, nullptr, S.inst, nullptr);
    DragAcceptFiles(hwnd, TRUE);
    updateButtons();
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
    int bodyH = std::max(barTop - dp(6) - bodyTop, dp(50));
    MoveWindow(S.preview, margin, bodyTop, std::max(w - 2 * margin, dp(50)), bodyH, TRUE);
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
            RECT r{0, 0, std::max(S.minWidth, dp(640)), dp(420)};
            AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
            mm->ptMinTrackSize.x = r.right - r.left;
            mm->ptMinTrackSize.y = r.bottom - r.top;
            return 0;
        }
        case WM_DROPFILES: {
            HDROP drop = reinterpret_cast<HDROP>(wParam);
            wchar_t buf[MAX_PATH * 4] = {0};
            if (DragQueryFileW(drop, 0, buf, MAX_PATH * 4) > 0) {
                loadFile(buf);
            }
            DragFinish(drop);
            return 0;
        }
        case WM_COMMAND:
            switch (LOWORD(wParam)) {
                case ID_OPEN:
                    openFile();
                    break;
                case ID_LEFT:
                    turn(90.0);
                    break;
                case ID_RIGHT:
                    turn(-90.0);
                    break;
                case ID_CROP:
                    setCropMode(SendMessageW(S.btnCrop, BM_GETCHECK, 0, 0) == BST_CHECKED);
                    break;
                case ID_APPLY:
                case ID_ENTER:
                    applyCrop();
                    break;
                case ID_HEADER:
                    removeHeader();
                    break;
                case ID_UNDO:
                    undoEdit();
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
                case ID_ESCAPE:
                    escapeKey();
                    break;
                case ID_UNITS_IN:
                    if (HIWORD(wParam) == CBN_SELCHANGE) {
                        unitsInChanged();
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
    pc.lpszClassName = L"RalvioliViewerPreview";
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
    wc.lpszClassName = L"RalvioliViewer";
    RegisterClassExW(&wc);

    HWND hwnd = CreateWindowExW(0, L"RalvioliViewer", L"Ralvioli Viewer", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                                CW_USEDEFAULT, CW_USEDEFAULT, dp(1250), dp(780), nullptr, nullptr, inst, nullptr);
    if (!hwnd) {
        return 1;
    }
    ShowWindow(hwnd, show);
    UpdateWindow(hwnd);

    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argv != nullptr) {
        if (argc > 1) {
            loadFile(argv[1]);
        }
        LocalFree(argv);
    }

    ACCEL keys[2];
    keys[0].fVirt = FVIRTKEY;
    keys[0].key = VK_ESCAPE;
    keys[0].cmd = ID_ESCAPE;
    keys[1].fVirt = FVIRTKEY;
    keys[1].key = VK_RETURN;
    keys[1].cmd = ID_ENTER;
    HACCEL accel = CreateAcceleratorTableW(keys, 2);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (!TranslateAcceleratorW(hwnd, accel, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    return static_cast<int>(msg.wParam);
}
