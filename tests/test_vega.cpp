#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cassert>
#include <iostream>
#include "VegaRender.hpp"

unsigned word(std::string_view bytes, size_t offset) {
    return static_cast<unsigned char>(bytes[offset]) |
           (static_cast<unsigned char>(bytes[offset + 1]) << 8);
}
unsigned be32(std::string_view bytes, size_t offset) {
    unsigned result = 0;
    for (size_t i = 0; i < 4; ++i) result = (result << 8) | static_cast<unsigned char>(bytes[offset + i]);
    return result;
}

std::vector<std::string_view> columns(std::string_view bytes, size_t width) {
    std::vector<std::string_view> result;
    size_t start = 0;
    for (size_t offset = 0; offset + 4 <= bytes.size() && result.size() < width; offset += 4) {
        if (word(bytes, offset) != 0xc000) continue;
        result.push_back(bytes.substr(start, offset + 4 - start));
        start = offset + 4;
    }
    assert(result.size() == width);
    return result;
}

int main(int argc, char** argv) {
    const auto fixture = rm::readFile("VegaSquareRaster.bin");
    const auto record = vega::jobRecord("VEGA-square-10cm-0025mm.hpg", 8, 8330, 100, 334);
    assert(record == fixture.substr(0, 64));
    assert(vega::capturedSquareRecord(record, 10) == record);
    const auto spaced = vega::capturedSquareRecord(record, 30);
    assert(spaced.substr(56, 2) == proto::be16(647));
    assert(spaced.substr(0, 56) == record.substr(0, 56));
    assert(spaced.substr(58, 4) == record.substr(58, 4));
    std::string checksumError;
    assert(proto::checkBody(spaced.substr(0, 8), spaced.substr(8), checksumError));
    assert(vega::finalBandAdvance(192, 30) - vega::finalBandAdvance(192, 10) == 313);

    int rowBySlot[22][14];
    for (auto& phase : rowBySlot) std::fill(std::begin(phase), std::end(phase), -1);
    for (int row = 0; row < 300; ++row) {
        const unsigned slot = vega::nozzleForRow[row];
        assert(rowBySlot[slot >> 4][slot & 15] == -1);
        rowBySlot[slot >> 4][slot & 15] = row;
    }
    size_t offset = 532;
    unsigned number = 0;
    bool recoveredTick = false;
    for (const unsigned packetCount : {46, 34, 34, 34, 34, 34, 34, 48}) {
        const std::string_view bytes(fixture.data() + offset, packetCount * vega::packetSize);
        int x = 0;
        vega::Band encoded(8330);
        for (size_t i = 0; i + 4 <= bytes.size() && x < 8330; i += 4) {
            const auto position = word(bytes, i);
            const auto mask = word(bytes, i + 2);
            const unsigned phase = (position & 0x3fff) / 4;
            assert(phase < 22);
            for (unsigned bit = 0; bit < 14; ++bit) {
                if (!(mask & (1u << bit))) continue;
                const int row = rowBySlot[phase][bit];
                assert(row >= 0);
                if (row % 2 == 1) {
                    encoded.dot(x, row / 2);
                    if (number == 1 && x == 100 && row / 2 < 70) recoveredTick = true;
                }
            }
            if (position & 0xc000) ++x;
        }
        assert(x == 8330 && encoded.encode() == bytes);
        offset += bytes.size();
        ++number;
    }
    assert(recoveredTick && offset == fixture.size());
    std::cout << "XP job record and all 8 raster bands match byte for byte.\n";

    const std::atomic_bool keepGoing{false};
    const auto longJob = [&](double lengthMm, double from = 0, double to = 100) {
        rm::Piece piece;
        rm::Op move(rm::K::Pts);
        move.verb = "PU";
        move.pts = {{0, 0}};
        rm::Op draw = move;
        draw.verb = "PD";
        draw.pts = {{lengthMm / 0.025, 400}};
        piece.ops = {move, draw};
        const auto stream = rp::makeStream(piece, 0.025);
        const auto plan = rp::makePlan(stream, 0, 100);
        const auto bounds = rm::boxOf(piece.ops);
        assert(bounds);
        std::optional<vega::FeedRange> range;
        if (from > 0 || to < 100) range = vega::feedRange(*bounds, 1, from, to);
        return vega::rasterize(piece, stream, plan, {}, 1895, 55, 10, 0.4, "length check", keepGoing, false, range);
    };
    const auto middle = longJob(100, 25, 75);
    assert(middle.bandCount == 4);
    assert(!middle.encodeBand(0).empty() && !middle.encodeBand(3).empty());
    assert(longJob(20000).bandCount > 1500);
    const auto maximum = longJob(100000);
    assert(maximum.bandCount > 7900);
    assert(!maximum.encodeBand(0).empty());
    assert(!maximum.encodeBand(maximum.bandCount - 1).empty());
    bool rejected = false;
    try { longJob(100001); }
    catch (const std::runtime_error& error) {
        rejected = std::string(error.what()).find("100 m") != std::string::npos;
    }
    assert(rejected);
    std::cout << "20 m and 100 m jobs accepted. 100.001 m rejected.\n";

    const auto textJob = [&](std::string name, double textWidthMm, double labelScale = 1) {
        rm::Piece piece;
        rm::Op move(rm::K::Pts);
        move.verb = "PU";
        move.pts = {{4000, 500}};
        rm::Op label(rm::K::Label);
        label.text = std::move(name);
        piece.ops = {move, label};
        const auto stream = rp::makeStream(piece, 0.025);
        const auto plan = rp::makePlan(stream, 0, 100);
        return vega::rasterize(piece, stream, plan, {}, 1895, 55, 10, 0.4, "Text stroke", keepGoing,
                               false, std::nullopt, 0, {}, textWidthMm, labelScale);
    };
    const auto countDots = [](const vega::RasterJob& job) {
        unsigned ink = 0;
        for (unsigned band = 0; band < job.bandCount; ++band) {
            const auto bytes = job.encodeBand(band);
            for (size_t i = 0; i + 4 <= bytes.size(); i += 4)
                ink += __builtin_popcount(word(bytes, i + 2) & 0x3fff);
        }
        return ink;
    };
    assert(countDots(textJob("QEMER.7", 2)) > countDots(textJob("QEMER.7", 0.4)));
    const std::string fileHeader = "File: Marker 1    Modified: Sun 14 Dec 25    La.: 1140.0 mm    Lu.: 1049.7 mm";
    assert(countDots(textJob(fileHeader, 2)) > countDots(textJob(fileHeader, 0)));
    assert(countDots(textJob(fileHeader, 0)) > countDots(textJob(fileHeader, -0.2)));
    assert(countDots(textJob("SELANIK", 0, 1.5)) > countDots(textJob("SELANIK", 0)));
    assert(countDots(textJob("SELANIK", 0)) > countDots(textJob("SELANIK", 0, 0.5)));
    assert(plotTextWidth(0.4, -0.4) == 0);
    bool textOmitted = false;
    try { textJob("SELANIK", -0.4); }
    catch (const std::runtime_error& error) {
        textOmitted = std::string(error.what()).find("no drawing") != std::string::npos;
    }
    assert(textOmitted);
    textOmitted = false;
    try { textJob(fileHeader, 0, 0); }
    catch (const std::runtime_error& error) {
        textOmitted = std::string(error.what()).find("no drawing") != std::string::npos;
    }
    assert(textOmitted);

    HDC display = GetDC(nullptr);
    HDC canvas = CreateCompatibleDC(display);
    HBITMAP bitmap = CreateCompatibleBitmap(display, 32, 32);
    ReleaseDC(nullptr, display);
    HPEN strokePen = CreatePen(PS_SOLID, 5, RGB(0, 0, 0));
    assert(canvas && bitmap && strokePen);
    const HGDIOBJ originalBitmap = SelectObject(canvas, bitmap);
    assert(PatBlt(canvas, 0, 0, 32, 32, WHITENESS));
    assert(drawPlotText(canvas, 12, 22, "I", 12, 0, strokePen));
    const HGDIOBJ originalPen = SelectObject(canvas, strokePen);
    const POINT reference[]{{24, 10}, {24, 22}};
    assert(Polyline(canvas, reference, 2));
    for (int y = 5; y < 28; ++y)
        for (int x = 11; x <= 21; ++x)
            assert(GetPixel(canvas, x, y) == GetPixel(canvas, x + 8, y));
    SelectObject(canvas, originalPen);
    SelectObject(canvas, originalBitmap);
    DeleteObject(strokePen);
    DeleteObject(bitmap);
    DeleteDC(canvas);
    std::cout << "Text offsets, label scaling, Off, and matching capital-I and line strokes verified.\n";

    if (argc < 2) return 0;
    if (argc == 3) {
        rm::Cache cache;
        const auto& piece = rm::loadRotated(cache, argv[2], 0, 0.025, false);
        const auto stream = rp::makeStream(piece, 0.025);
        const auto plan = rp::makePlan(stream, 0, 100);
        const auto job = vega::rasterize(piece, stream, plan, {}, 1895, 55, 10, 0.4, "Olsi 178 cm", keepGoing);
        assert(!job.encodeBand(0).empty() && !job.encodeBand(job.bandCount - 1).empty());
        std::cout << "Olsi file accepted and first and last bands encoded. " << job.bandCount << " bands.\n";
    }
    rm::Cache cache;
    const auto& piece = rm::loadRotated(cache, argv[1], 0, 0.01, false);
    const auto stream = rp::makeStream(piece, 0.025);
    const auto plan = rp::makePlan(stream, 0, 100);
    const std::atomic_bool stop{false};
    const auto generated = vega::rasterize(piece, stream, plan, {}, 1895, 55, 10, 0.4, "PANT LAND", stop);
    assert(generated.bandCount > 8);
    const unsigned scanWidth = be32(generated.record, 50);
    assert(scanWidth > 13000 && scanWidth < 15000);
    unsigned ink = 0;
    for (unsigned index = 0; index < generated.bandCount; ++index) {
        const auto band = generated.encodeBand(index);
        assert(band.size() % vega::packetSize == 0);
        for (size_t i = 0; i + 4 <= band.size(); i += 4)
            if (word(band, i + 2) & 0x3fff) ++ink;
    }
    assert(ink > 100);
    const auto bidi = vega::rasterize(piece, stream, plan, {}, 1895, 55, 10, 0.4, "PANT LAND", stop, true);
    assert(bidi.record.substr(40, 2) == proto::be16(1));
    assert(generated.record.substr(40, 2) == proto::be16(0));
    assert(bidi.bandCount == generated.bandCount);
    for (unsigned index = 0; index < bidi.bandCount; ++index) {
        const auto normal = generated.encodeBand(index);
        const auto alternating = bidi.encodeBand(index);
        if (index % 2 == 0) {
            assert(alternating == normal);
        } else {
            auto forward = columns(normal, scanWidth);
            std::reverse(forward.begin(), forward.end());
            assert(columns(alternating, scanWidth) == forward);
        }
    }
    std::cout << "Bidi alternates scan direction and preserves the nozzle firing masks.\n";
    std::cout << "Generated " << generated.bandCount << " bands, " << scanWidth
              << " scan columns and " << ink << " nonempty firing records from the independent HP-GL file.\n";
}
