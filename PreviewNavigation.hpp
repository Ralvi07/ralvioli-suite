#pragma once
#include <windows.h>
#include <iterator>

namespace preview {
inline constexpr UINT_PTR hoverTimer = 2;

inline constexpr POINT previewDirections[]{{-1, 0}, {1, 0}, {0, -1}, {0, 1}};

inline RECT arrowRect(int direction, int width, int bottom, int size, int inset) {
    const POINT delta = previewDirections[direction];
    const int x = delta.x < 0 ? inset : delta.x > 0 ? width - size - inset : (width - size) / 2;
    const int y = delta.y < 0 ? inset : delta.y > 0 ? bottom - size - inset : (bottom - size) / 2;
    return {x, y, x + size, y + size};
}

// Keep drawing and hit testing on the same DPI-scaled rectangles.
template <class Scale>
int hitArrow(POINT mouse, int width, int bottom, Scale dp) {
    for (int i = 0; i < 4; ++i) {
        const RECT arrow = arrowRect(i, width, bottom, dp(30), dp(8));
        if (PtInRect(&arrow, mouse)) return i;
    }
    return -1;
}

template <class Scale>
void drawArrows(HDC dc, int width, int bottom, Scale dp) {
    const int saved = SaveDC(dc);
    if (!saved) return;
    SelectObject(dc, GetStockObject(DC_BRUSH));
    SelectObject(dc, GetStockObject(NULL_PEN));
    for (int i = 0; i < static_cast<int>(std::size(previewDirections)); ++i) {
        const RECT arrow = arrowRect(i, width, bottom, dp(30), dp(8));
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

// Recheck the actual cursor position so panning stops when it leaves the window.
template <class HitTest>
int hoveredArrow(HWND hwnd, HitTest hit) {
    POINT mouse{};
    if (GetCursorPos(&mouse) && WindowFromPoint(mouse) == hwnd && ScreenToClient(hwnd, &mouse))
        return hit(mouse);
    return -1;
}
}  // namespace preview
