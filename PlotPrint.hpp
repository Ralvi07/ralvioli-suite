#pragma once

#include "PlotStream.hpp"
#include "PlotText.hpp"
#include <atomic>

// The monochrome bitmap stays in memory. The Windows printer driver receives it at physical size.
inline bool printPlot(const rm::Piece& piece, const rp::Stream& stream, const rp::Plan& plan,
                            cal::Factors calibration, double paperUnits, int dpi, double lineWidthMm,
                            HDC printer, const std::wstring& title, const std::atomic_bool& stop, std::string& error,
                            double textWidthMm = 0, double labelScale = 1) {
    if (stop) return false;
    const auto drawing = rp::buildDrawing(piece, stream);
    const auto selected = [&](int line) {
        return line >= 0 && static_cast<size_t>(line) < plan.lineIn.size() && plan.lineIn[line];
    };
    const double scale = (dpi / 25.4) * 0.025;
    const double across = calibration.across * scale;
    const double feed = calibration.feed * scale;
    const int penWidth = std::max(1, static_cast<int>(std::lround(lineWidthMm * dpi / 25.4)));
    const double columns = std::ceil(paperUnits * scale - 1e-9);
    const int textPixels = std::max(1, static_cast<int>(std::lround(plotTextWidth(lineWidthMm, textWidthMm) * dpi / 25.4)));
    const bool printText = labelScale > 0 && plotTextWidth(lineWidthMm, textWidthMm) > 0;
    bool hasTextStroke = false;
    double first = 0, last = 0;
    bool drawable = false;
    const auto include = [&](double start, double end) {
        first = drawable ? std::min(first, start) : start;
        last = drawable ? std::max(last, end) : end;
        drawable = true;
    };
    for (const auto& item : drawing) {
        if (item.type == 0) {
            for (size_t i = 1; i < item.pts.size(); ++i) {
                if (selected(item.ln[i])) {
                    include(std::min(item.pts[i - 1].x, item.pts[i].x),
                            std::max(item.pts[i - 1].x, item.pts[i].x));
                }
            }
        } else if (selected(item.line) && (item.type == 1 || (printText && !item.text.empty()))) {
            hasTextStroke |= item.type == 2;
            const double margin = item.type == 1 ? std::abs(item.r) :
                std::abs(item.height) * labelScale * (static_cast<double>(item.text.size()) + 1.0);
            include(item.c.x - margin, item.c.x + margin);
        }
    }
    if (!drawable) {
        error = "The selected range contains no supported drawing commands.";
        return false;
    }
    const double origin = plan.lineIn.front() ? -stream.dx : first;
    const double rows = std::ceil((last - origin) * feed - 1e-9) + std::max(penWidth, hasTextStroke ? textPixels : 0) + 1;
    if (!(columns >= 1 && columns <= 100000 && rows >= 1 && rows <= 1000000)) {
        error = "Raster dimensions exceed the export limits.";
        return false;
    }
    const int width = static_cast<int>(columns);
    const int height = static_cast<int>(rows);
    const size_t rowBytes = (width + 7) / 8;
    const size_t stride = (rowBytes + 3) & ~size_t{3};
    if (stride * height > 128 * 1024 * 1024) {
        error = "Raster exceeds 128 MiB. Select a shorter range.";
        return false;
    }

    const double printWidth = std::round(static_cast<double>(width) * GetDeviceCaps(printer, LOGPIXELSX) / dpi);
    const double printHeight = std::round(static_cast<double>(height) * GetDeviceCaps(printer, LOGPIXELSY) / dpi);
    if (!(printWidth >= 1 && printWidth <= GetDeviceCaps(printer, HORZRES) &&
          printHeight >= 1 && printHeight <= GetDeviceCaps(printer, VERTRES))) {
        error = "The job does not fit the printer's printable area. Select a larger paper size or a shorter range.";
        return false;
    }
    struct {
        BITMAPINFOHEADER header{};
        RGBQUAD palette[2]{{255, 255, 255, 0}, {0, 0, 0, 0}};
    } info;
    info.header.biSize = sizeof(info.header);
    info.header.biWidth = width;
    info.header.biHeight = -height;
    info.header.biPlanes = 1;
    info.header.biBitCount = 1;
    info.header.biClrUsed = 2;

    void* pixels = nullptr;
    HDC dc = CreateCompatibleDC(nullptr);
    HBITMAP bitmap = CreateDIBSection(dc, reinterpret_cast<BITMAPINFO*>(&info), DIB_RGB_COLORS, &pixels, nullptr, 0);
    HPEN pen = CreatePen(PS_SOLID, penWidth, RGB(0, 0, 0));
    HPEN textPen = hasTextStroke ? CreatePen(PS_SOLID, textPixels, RGB(0, 0, 0)) : nullptr;
    bool ok = dc && bitmap && pen && (!hasTextStroke || textPen);
    if (ok) {
        SelectObject(dc, bitmap);
        SelectObject(dc, pen);
        SelectObject(dc, GetStockObject(NULL_BRUSH));
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, RGB(0, 0, 0));
        ZeroMemory(pixels, stride * height);
        const auto deviceX = [&](double y) {
            return static_cast<LONG>(std::clamp(std::round((y + stream.dy) * across), -1e8, 1e8));
        };
        const auto deviceY = [&](double x) {
            return static_cast<LONG>(std::clamp(std::round((x - origin) * feed), -1e8, 1e8));
        };
        for (const auto& item : drawing) {
            if (item.type == 0) {
                for (size_t i = 1; i < item.pts.size(); ++i) {
                    if (!selected(item.ln[i])) continue;
                    const POINT points[]{{deviceX(item.pts[i - 1].y), deviceY(item.pts[i - 1].x)},
                                         {deviceX(item.pts[i].y), deviceY(item.pts[i].x)}};
                    ok = Polyline(dc, points, 2) && ok;
                    SetPixelV(dc, points[1].x, points[1].y, RGB(0, 0, 0));
                }
            } else if (selected(item.line) && item.type == 1) {
                ok = Ellipse(dc, deviceX(item.c.y - item.r), deviceY(item.c.x - item.r),
                             deviceX(item.c.y + item.r) + 1, deviceY(item.c.x + item.r) + 1) && ok;
            } else if (textPen && selected(item.line)) {
                const double angle = std::atan2(item.dx * feed, item.dy * across);
                const double height = std::clamp(std::abs(item.height) * labelScale * scale, 1.0, 4096.0);
                ok = drawPlotText(dc, deviceX(item.c.y), deviceY(item.c.x), item.text,
                                  height, angle, textPen) && ok;
            }
        }
        ok = GdiFlush() && ok;
    }
    if (!ok) {
        error = "Windows could not render the raster.";
    } else {
        DOCINFOW doc{};
        doc.cbSize = sizeof(doc);
        doc.lpszDocName = title.c_str();
        if (StartDocW(printer, &doc) <= 0) {
            error = "Windows could not start the print job.";
            ok = false;
        } else {
            ok = StartPage(printer) > 0;
            if (ok) {
                SetStretchBltMode(printer, BLACKONWHITE);
                const int lines = StretchDIBits(printer, 0, 0, static_cast<int>(printWidth),
                                                static_cast<int>(printHeight), 0, 0, width, height, pixels,
                                                reinterpret_cast<BITMAPINFO*>(&info), DIB_RGB_COLORS, SRCCOPY);
                ok = lines != GDI_ERROR && lines != 0 && EndPage(printer) > 0;
            }
            if (ok) ok = EndDoc(printer) > 0;
            if (!ok) {
                AbortDoc(printer);
                error = "Windows could not submit the printed page.";
            }
        }
    }
    if (dc) DeleteDC(dc);
    if (bitmap) DeleteObject(bitmap);
    if (pen) DeleteObject(pen);
    if (textPen) DeleteObject(textPen);
    return ok;
}
