#include <cassert>
#include <iostream>
#include <set>
#include <tuple>
#include "PlotStream.hpp"

static void put(const std::string& path, const std::string& data) {
    FILE* f = fopen(path.c_str(), "wb");
    fwrite(data.data(), 1, data.size(), f);
    fclose(f);
}

static std::string sample(double step, bool rel) {
    double W = 0.600 / (step / 1000.0), L = 1.529 / (step / 1000.0);
    auto u = [&](double v) { return (long long)std::llround(v); };
    std::string f = "IN;SP1;LT;\n";
    f += "PU" + std::to_string(u(W * 0.1)) + "," + std::to_string(u(W + 400)) + ";SI0.3,0.3;LB" +
         "Sweatshirt Female Riban x2 60Cm Long : 1,529 m Larg : 0,600 m Eff : 89,29 %" + "\x03;\n";
    f += "PU0,0;PD" + std::to_string(u(L)) + ",0," + std::to_string(u(L)) + "," + std::to_string(u(W)) + ",0," +
         std::to_string(u(W)) + ",0,0;\n";
    if (rel) f += "PU1000,1000;PR;PD500,0,0,500,-500,0,0,-500;PA;PU;\n";
    else     f += "PU1000,1000;PD1500,1000,1500,1500,1000,1500,1000,1000;PU;\n";
    f += "PU3000,3000;AA3000,3500,180;PU;CI200;LT2;\nPU4000,4000;LB" "A1" "\x03;\n";
    for (int i = 0; i < 40; ++i)
        f += "PU" + std::to_string(5000 + i * 100) + ",2000;PD" + std::to_string(5050 + i * 100) + ",3000," +
             std::to_string(5100 + i * 100) + ",2000;\n";
    f += "PU0,0;SP0;";
    return f;
}

static std::multiset<std::tuple<long, long, long, long>> segs(const std::vector<rp::DItem>& items,
                                                              const rp::Plan* plan, double scale) {
    std::multiset<std::tuple<long, long, long, long>> out;
    for (const auto& it : items) {
        if (it.type != 0) continue;
        for (size_t k = 1; k < it.pts.size(); ++k) {
            if (plan && !(it.ln[k] >= 0 && plan->lineIn[it.ln[k]])) continue;
            out.insert({std::lround(it.pts[k - 1].x * scale), std::lround(it.pts[k - 1].y * scale),
                        std::lround(it.pts[k].x * scale), std::lround(it.pts[k].y * scale)});
        }
    }
    return out;
}

int main() {
    double factor = 1.0;
    assert(cal::calculate(1.0, 100.0, 99.5, false, factor) == cal::Result::Applied);
    assert(std::fabs(factor - 100.0 / 99.5) < 1e-12);
    assert(cal::calculate(factor, 100.0, 105.0, false, factor) == cal::Result::NeedsConfirm);
    assert(cal::calculate(factor, 100.0, 105.0, true, factor) == cal::Result::Applied);
    assert(std::fabs(factor - (100.0 / 99.5) * (100.0 / 105.0)) < 1e-12);
    assert(cal::calculate(1.0, 1.0, 0.0, true, factor) == cal::Result::Invalid);

    const double units[3] = {0.025, 0.01, 0.0254};
    int checked = 0;
    for (int ui = 0; ui < 3; ++ui)
        for (int rel = 0; rel < 2; ++rel) {
            std::string path = "./rp_in.plt";
            put(path, sample(units[ui], rel));
            int guess = rp::guessUnit(path);
            std::cout << "input step " << units[ui] << " rel " << rel << " guessed index " << guess << std::endl;
            assert(guess == ui);
            for (int nh = 0; nh < 2; ++nh) {
                rm::Cache cache;
                rm::Req rq;
                rq.path = path; rq.unit = units[ui]; rq.noHeader = nh;
                std::vector<rm::Piece> placed = rm::placeLayers(cache, {rq}, 60.0 * rm::kUnitsPerCm);
                const rm::Piece& piece = placed[0];
                for (int oi = 0; oi < 3; ++oi) {
                    rp::Stream st = rp::makeStream(piece, units[oi]);
                    std::string want = rm::merge({piece}, false, units[oi]);
                    assert(rp::streamText(st) == want);
                    rp::Plan full = rp::makePlan(st, 0, 100);
                    assert(full.bytes == want);
                    rp::Stream calibrated = rp::makeStream(piece, units[oi], {1.005, 0.995});
                    const std::string setup = "SC0,1.005,0,0.995,2;\n";
                    for (double start : {0.0, 40.0}) {
                        rp::Plan adjusted = rp::makePlan(calibrated, start, 90.0);
                        size_t scale = adjusted.bytes.find(setup);
                        assert(scale != std::string::npos && adjusted.bytes.find(setup, scale + 1) == std::string::npos);
                        assert(adjusted.bytes.find("IN;") < scale);
                        assert(adjusted.bytes.substr(adjusted.bytes.size() - 4) == "SC;\n");
                    }
                    ++checked;
                }
                // range prints, output 0.025 so coordinates stay integral
                rp::Stream st = rp::makeStream(piece, 0.025);
                std::vector<rp::DItem> items = rp::buildDrawing(piece, st);
                assert(nh == 0 || rp::streamText(st).find("Larg") == std::string::npos);
                assert(nh == 1 || rp::streamText(st).find("Larg") != std::string::npos);
                double r[][2] = {{0, 100}, {0, 50}, {30, 100}, {40, 70}, {99, 100}, {10, 11}};
                for (auto& rg : r) {
                    rp::Plan p = rp::makePlan(st, rg[0], rg[1]);
                    if (p.bytes.empty()) { std::cout << "  empty range " << rg[0] << "-" << rg[1] << "\n"; continue; }
                    put("./rp_out.plt", p.bytes);
                    rm::Cache c2;
                    const rm::Piece& back = rm::loadRotated(c2, "./rp_out.plt", 0.0, 0.025, false);
                    std::vector<rm::Item> ri = rm::renderLayer(back);
                    std::multiset<std::tuple<long, long, long, long>> got;
                    for (auto& it : ri) {
                        if (it.type != 0) continue;
                        for (size_t k = 1; k < it.pts.size(); ++k)
                            got.insert({std::lround(it.pts[k - 1].x), std::lround(it.pts[k - 1].y),
                                        std::lround(it.pts[k].x), std::lround(it.pts[k].y)});
                    }
                    auto want = segs(items, &p, 1.0);
                    if (got != want)
                        std::cout << "MISMATCH step " << units[ui] << " rel " << rel << " range " << rg[0] << "-" << rg[1]
                                  << " got " << got.size() << " want " << want.size() << "\n";
                    assert(got == want);
                    // progress map: lineEnd is monotonic and ends at (or before) the byte total
                    size_t prev = 0;
                    for (size_t j = 0; j < p.lineIn.size(); ++j) if (p.lineIn[j]) { assert(p.lineEnd[j] >= prev); prev = p.lineEnd[j]; }
                    assert(prev <= p.bytes.size());
                }
            }
        }
    std::cout << "stream checks: " << checked << "  ALL OK\n";
}

