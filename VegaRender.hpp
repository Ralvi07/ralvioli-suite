#pragma once

#include "VegaRaster.hpp"
#include "PlotStream.hpp"
#include "PlotText.hpp"
#include <atomic>
#include <functional>
#include <memory>

namespace vega {

struct RasterSurface {
    HDC dc = CreateCompatibleDC(nullptr);
    HBITMAP bitmap = nullptr;
    HPEN pen = nullptr;
    HPEN labelPen = nullptr;
    ~RasterSurface() {
        if (dc) DeleteDC(dc);
        if (bitmap) DeleteObject(bitmap);
        if (pen) DeleteObject(pen);
        if (labelPen) DeleteObject(labelPen);
    }
};

struct FeedRange {
    double first;
    double last;
};

inline FeedRange feedRange(const rm::Box& box, double feedCalibration, double from, double to,
                           int bandRows = rowsPerBand) {
    const double rowsPerUnit = 0.025 * feedCalibration * rowsPerMm;
    const double length = (box.x1 - box.x0) * rowsPerUnit;
    const auto boundary = [&](double pct) {
        return std::clamp(std::round(length * pct / (100 * bandRows)) * bandRows, 0.0, length);
    };
    return {box.x0 + (from <= 0 ? 0 : boundary(from)) / rowsPerUnit,
            box.x0 + (to >= 100 ? length : boundary(to)) / rowsPerUnit};
}

struct RasterJob {
    std::string record;
    unsigned bandCount = 0;
    std::function<std::string(unsigned)> encodeBand;
};

inline RasterJob rasterize(const rm::Piece& piece, const rp::Stream& stream, const rp::Plan& plan,
                           cal::Factors calibration, double paperMm, double rightMarginMm,
                           double spacingMm, double lineMm,
                           const std::string& title, const std::atomic_bool& stop, bool bidi = false,
                           std::optional<FeedRange> range = std::nullopt, double returnShiftMm = 0,
                           HeadLayout heads = {}, double textWidthMm = 0, double labelScale = 1) {
    const int bandRows = heads.bandRows();
    if (!bandRows || heads.mask >= (1u << headSlots)) throw std::runtime_error("Select at least one VEGA print head.");
    int firstHeadColumn = 0;
    int lastHeadColumn = 0;
    for (unsigned slot = 0; slot < headSlots; ++slot) {
        if (!heads.enabled(slot)) continue;
        if (!std::isfinite(heads.offsetMm[slot]) || std::abs(heads.offsetMm[slot]) > 327.67)
            throw std::runtime_error("Invalid VEGA head offset.");
        firstHeadColumn = std::min(firstHeadColumn, heads.columnOffset(slot));
        lastHeadColumn = std::max(lastHeadColumn, heads.columnOffset(slot));
    }
    auto drawing = rp::buildDrawing(piece, stream);
    const auto selected = [&](int line) {
        return line >= 0 && static_cast<size_t>(line) < plan.lineIn.size() && plan.lineIn[line];
    };

    // The Plotter's preview uses HP-GL X for feed and Y for across.
    struct Mark {
        const rp::DItem* item;
        size_t end;
        double x0, y0, x1, y1;
    };
    const double sx = 0.025 * calibration.across * columnsPerMm;
    const double sy = 0.025 * calibration.feed * rowsPerMm;
    const int stroke = std::max(1, static_cast<int>(std::lround(lineMm * columnsPerMm)));
    const double textMm = plotTextWidth(lineMm, textWidthMm);
    const int labelStroke = labelScale > 0 && textMm > 0 ? std::max(1, static_cast<int>(std::lround(textMm * columnsPerMm))) : 0;
    int padding = stroke;
    std::vector<Mark> marks;
    double left = INFINITY, top = INFINITY, right = -INFINITY, bottom = -INFINITY;
    const auto gdi = std::make_shared<RasterSurface>();
    if (!gdi->dc) throw std::runtime_error("Could not create the VEGA raster surface.");
    const auto include = [&](const rp::DItem& item, size_t end, double x0, double y0, double x1, double y1) {
        Mark m{&item, end, x0, y0, x1, y1};
        left = std::min(left, m.x0);
        top = std::min(top, m.y0);
        right = std::max(right, m.x1);
        bottom = std::max(bottom, m.y1);
        marks.push_back(m);
    };
    for (const auto& item : drawing) {
        if (item.type == 0) {
            for (size_t i = 1; i < item.pts.size(); ++i) {
                if (!selected(item.ln[i])) continue;
                const auto a = item.pts[i - 1];
                const auto b = item.pts[i];
                include(item, i, std::min(a.y, b.y) * sx, std::min(a.x, b.x) * sy,
                        std::max(a.y, b.y) * sx, std::max(a.x, b.x) * sy);
            }
        } else if (selected(item.line) && item.type == 1) {
            const double radius = std::abs(item.r);
            include(item, 0, (item.c.y - radius) * sx, (item.c.x - radius) * sy,
                    (item.c.y + radius) * sx, (item.c.x + radius) * sy);
        } else if (labelStroke && selected(item.line) && !item.text.empty()) {
            padding = std::max(padding, labelStroke);
            const double height = std::clamp(std::abs(item.height) * sy * labelScale, 1.0, 4096.0);
            const double angle = std::atan2(item.dx * sy, item.dy * sx);
            const double x = item.c.y * sx;
            const double y = item.c.x * sy;
            const double width = item.text.size() * (5.0 / 6) * height;
            const double wX = width * std::cos(angle), wY = width * std::sin(angle);
            const double hX = height * std::sin(angle), hY = -height * std::cos(angle);
            const double descX = -hX / 3, descY = -hY / 3;
            include(item, 0, x + std::min(0.0, wX) + std::min(hX, descX),
                    y + std::min(0.0, wY) + std::min(hY, descY),
                    x + std::max(0.0, wX) + std::max(hX, descX),
                    y + std::max(0.0, wY) + std::max(hY, descY));
        }
    }
    if (marks.empty()) throw std::runtime_error("The selected range contains no drawing.");
    if (range) {
        top = range->first * sy;
        bottom = range->last * sy;
        if (bottom <= top) throw std::runtime_error("The selected percentages cover no complete VEGA band.");
    }
    const double width = right - left;
    const double length = bottom - top;
    constexpr double maxLengthMm = 100000;
    if (!std::isfinite(width) || !std::isfinite(length))
        throw std::runtime_error("The selected drawing has invalid coordinates.");
    const double checkedWidth = width + (padding - stroke);
    if (checkedWidth > paperMm * columnsPerMm + 1.0 || checkedWidth > 24000) {
        char detail[128];
        snprintf(detail, sizeof(detail), "Raster width %.1f mm exceeds the %.1f mm printable width.",
                 checkedWidth / columnsPerMm, std::min(paperMm, 24000 / columnsPerMm));
        throw std::runtime_error(detail);
    }
    if (length > maxLengthMm * rowsPerMm + 0.01)
        throw std::runtime_error("The selected drawing exceeds the 100 m VEGA print length limit.");
    if (spacingMm < 0 || !std::isfinite(spacingMm) ||
        rightMarginMm < 0 || !std::isfinite(rightMarginMm) || rightMarginMm > 10000)
        throw std::runtime_error("The VEGA spacing or right margin is invalid.");

    const int inkColumns = static_cast<int>(std::ceil(width)) + padding + 1;
    const int inkRows = static_cast<int>(std::ceil(length - 1e-7)) + (range ? 0 : padding + 1);
    const int bandCount = (inkRows + bandRows - 1) / bandRows;
    // Columns increase away from the configured right margin. Add room on
    // the right before reversing the return band's column order.
    const int shift = bidi && bandCount > 1
        ? static_cast<int>(std::lround(-returnShiftMm * columnsPerMm)) : 0;
    const int leadingColumns = std::max(0, -shift) - firstHeadColumn;
    const double margin = rightMarginMm - leadingColumns / columnsPerMm;
    const int extraLeftColumns = std::max(0, shift);
    if (margin < 0 || (extraLeftColumns > 0 && checkedWidth + extraLeftColumns > paperMm * columnsPerMm + 1.0) ||
        (shift != 0 && checkedWidth + std::abs(shift) > 24000))
        throw std::runtime_error("The bidirectional return shift exceeds the printable width.");
    // Bidi scans use the carriage origin. Encode the right margin as blank
    // raster columns so it is part of both the forward and return passes.
    const int marginColumns = bidi ? static_cast<int>(std::lround(margin * columnsPerMm)) : 0;
    const int overrun = std::max(scanOverrunColumns, lastHeadColumn + nozzleBankOffset + 3);
    const int scanColumns = std::max(minScanColumns, marginColumns + inkColumns + std::abs(shift) - firstHeadColumn + overrun);
    const int lastRows = inkRows - (bandCount - 1) * bandRows;
    const unsigned advance = finalBandAdvance(lastRows * (static_cast<double>(heads.bandFeed()) / bandRows), spacingMm);
    RasterJob job;
    job.record = jobRecord(title, bandCount, scanColumns, bidi ? 0 : margin,
                           advance, bidi, heads.bandFeed());
    job.bandCount = static_cast<unsigned>(bandCount);
    struct {
        BITMAPINFOHEADER header{};
        RGBQUAD palette[2]{{255, 255, 255, 0}, {0, 0, 0, 0}};
    } info;
    info.header.biSize = sizeof(info.header);
    info.header.biWidth = inkColumns;
    info.header.biHeight = -bandRows;
    info.header.biPlanes = 1;
    info.header.biBitCount = 1;
    info.header.biClrUsed = 2;
    void* pixels = nullptr;
    if (!gdi->dc) throw std::runtime_error("Could not create the VEGA raster surface.");
    gdi->bitmap = CreateDIBSection(gdi->dc, reinterpret_cast<BITMAPINFO*>(&info), DIB_RGB_COLORS, &pixels, nullptr, 0);
    gdi->pen = CreatePen(PS_SOLID, stroke, RGB(0, 0, 0));
    if (labelStroke) gdi->labelPen = CreatePen(PS_SOLID, labelStroke, RGB(0, 0, 0));
    if (!gdi->bitmap || !gdi->pen || (labelStroke && !gdi->labelPen))
        throw std::runtime_error("Could not allocate a VEGA raster band.");
    SelectObject(gdi->dc, gdi->bitmap);
    SelectObject(gdi->dc, gdi->pen);
    SelectObject(gdi->dc, GetStockObject(NULL_BRUSH));
    SetBkMode(gdi->dc, TRANSPARENT);
    SetTextColor(gdi->dc, RGB(0, 0, 0));
    const size_t stride = (static_cast<size_t>(inkColumns) + 31) / 32 * 4;
    // The drawing owns the points referenced by each mark. Only one encoded band is held at a time.
    job.encodeBand = [drawing = std::move(drawing), marks = std::move(marks), gdi, pixels, stride,
                      sx, sy, labelScale, left, top, padding, inkColumns, inkRows, scanColumns, bandCount, bidi, shift,
                      firstColumn = leadingColumns + marginColumns, bandRows, heads, &stop](unsigned band) {
        if (band >= static_cast<unsigned>(bandCount)) throw std::out_of_range("VEGA band index");
        if (stop) return std::string();
        const auto col = [&](double y) { return static_cast<LONG>(std::lround(y * sx - left + padding / 2)); };
        const int start = static_cast<int>(band) * bandRows;
        const auto row = [&](double x) {
            return static_cast<LONG>(std::lround(x * sy - top + padding / 2) - start);
        };
        ZeroMemory(pixels, stride * bandRows);
        for (const auto& mark : marks) {
            if (mark.y1 - top + padding < start || mark.y0 - top - padding >= start + bandRows) continue;
            const auto& item = *mark.item;
            if (item.type == 0) {
                const auto a = item.pts[mark.end - 1];
                const auto b = item.pts[mark.end];
                const POINT points[]{{col(a.y), row(a.x)}, {col(b.y), row(b.x)}};
                if (!Polyline(gdi->dc, points, 2)) throw std::runtime_error("Could not rasterize a line.");
                SetPixelV(gdi->dc, points[1].x, points[1].y, RGB(0, 0, 0));
            } else if (item.type == 1) {
                if (!Ellipse(gdi->dc, col(item.c.y - item.r), row(item.c.x - item.r),
                             col(item.c.y + item.r) + 1, row(item.c.x + item.r) + 1))
                    throw std::runtime_error("Could not rasterize a circle.");
            } else {
                const double height = std::clamp(std::abs(item.height) * sy * labelScale, 1.0, 4096.0);
                const double angle = std::atan2(item.dx * sy, item.dy * sx);
                if (!drawPlotText(gdi->dc, col(item.c.y), row(item.c.x), item.text, height, angle, gdi->labelPen))
                    throw std::runtime_error("Could not rasterize a plot label.");
            }
        }
        GdiFlush();
        const bool returning = bidi && band % 2 == 1;
        const int offset = firstColumn + (returning ? shift : 0);
        Band raster(scanColumns);
        const auto* bits = static_cast<const unsigned char*>(pixels);
        int headStart = 0;
        for (unsigned slot = 0; slot < headSlots; ++slot) {
            if (!heads.enabled(slot)) continue;
            const int headColumn = offset + heads.columnOffset(slot);
            const int rows = std::min(rowsPerBand, inkRows - start - headStart);
            for (int row = 0; row < rows; ++row) {
                const auto* rowBits = bits + static_cast<size_t>(headStart + row) * stride;
                for (int byte = 0; byte < (inkColumns + 7) / 8; ++byte) {
                    if (rowBits[byte] == 0) continue;
                    for (int bit = 0; bit < 8 && byte * 8 + bit < inkColumns; ++bit)
                        if (rowBits[byte] & (0x80 >> bit)) raster.dot(headColumn + byte * 8 + bit, row, slot);
                }
            }
            headStart += rowsPerBand;
        }
        return raster.encode(returning);
    };
    return job;
}

}  // namespace vega
