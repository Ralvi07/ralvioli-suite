#pragma once

#include "PlotStream.hpp"

// Diagnostic PBM: columns run across the paper, rows run along the feed.
inline bool writePlotRaster(const rm::Piece& piece, const rp::Stream& stream, const rp::Plan& plan,
                            cal::Factors calibration, double paperUnits, int dpi, double lineWidthMm,
                            const std::wstring& path, std::string& error) {
    const auto drawing = rp::buildDrawing(piece, stream);
    const auto selected = [&](int line) {
        return line >= 0 && static_cast<size_t>(line) < plan.lineIn.size() && plan.lineIn[line];
    };
    const double scale = (dpi / 25.4) * 0.025;
    const double across = calibration.across * scale;
    const double feed = calibration.feed * scale;
    const int penWidth = std::max(1, static_cast<int>(std::lround(lineWidthMm * dpi / 25.4)));
    const double columns = std::ceil(paperUnits * scale - 1e-9);
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
        } else if (selected(item.line)) {
            const double margin = item.type == 1 ? std::abs(item.r) :
                std::abs(item.height) * (static_cast<double>(item.text.size()) + 1.0);
            include(item.c.x - margin, item.c.x + margin);
        }
    }
    if (!drawable) {
        error = "The selected range contains no supported drawing commands.";
        return false;
    }
    const double origin = plan.lineIn.front() ? -stream.dx : first;
    const double rows = std::ceil((last - origin) * feed - 1e-9) + penWidth + 1;
    if (!(columns >= 1 && columns <= 100000 && rows >= 1 && rows <= 1000000)) {
        error = "Raster dimensions exceed the export limits.";
        return false;
    }
    const int width = static_cast<int>(columns);
    const int height = static_cast<int>(rows);
    const size_t rowBytes = (width + 7) / 8;
    const size_t stride = (rowBytes + 3) & ~size_t{3};
    if (stride * height > 128 * 1024 * 1024) {
        error = "Raster exceeds 128 MiB. Reduce DPI or select a shorter range.";
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
    bool ok = dc && bitmap && pen;
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
            } else if (selected(item.line)) {
                const int angle = static_cast<int>(std::lround(std::atan2(-item.dx * feed, item.dy * across)
                                                               * 1800 / 3.14159265358979323846));
                const int size = static_cast<int>(std::clamp(std::round(std::abs(item.height) * scale), 1.0, 4096.0));
                HFONT font = CreateFontW(-size, 0, angle, angle, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                         OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS, NONANTIALIASED_QUALITY,
                                         DEFAULT_PITCH, L"Segoe UI");
                if (font) {
                    HGDIOBJ old = SelectObject(dc, font);
                    ok = TextOutA(dc, deviceX(item.c.y), deviceY(item.c.x), item.text.c_str(),
                                  static_cast<int>(item.text.size())) && ok;
                    SelectObject(dc, old);
                    DeleteObject(font);
                } else {
                    ok = false;
                }
            }
        }
        ok = GdiFlush() && ok;
    }
    if (!ok) {
        error = "Windows could not render the raster.";
    } else {
        FILE* out = _wfopen(path.c_str(), L"wbx");
        if (!out) {
            error = "Could not create the PBM file. Check permissions or choose a new output name.";
            ok = false;
        } else {
            ok = std::fprintf(out, "P4\n%d %d\n", width, height) > 0;
            const auto* row = static_cast<const unsigned char*>(pixels);
            for (int y = 0; ok && y < height; ++y, row += stride) {
                ok = std::fwrite(row, 1, rowBytes, out) == rowBytes;
            }
            ok = std::fclose(out) == 0 && ok;
            if (!ok) {
                DeleteFileW(path.c_str());
                error = "Could not finish writing the PBM file.";
            }
        }
    }
    if (dc) DeleteDC(dc);
    if (bitmap) DeleteObject(bitmap);
    if (pen) DeleteObject(pen);
    return ok;
}
