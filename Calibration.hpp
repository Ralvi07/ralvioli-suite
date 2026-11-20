// Validates measured print dimensions and computes the correction factors for each plotter axis.
#pragma once
#include <cmath>
// Calibration as done by the old Algotex engine ("Kalibrierung" dialog in AlgotexOcx.ocx):
// for each direction there is a current factor ("Wert"), the size the print should have ("Soll")
// and the size that was measured on the printed sheet ("Ist"). "Rechnen" multiplies the factor by
// Soll / Ist and then resets Soll and Ist to 1. A correction outside 0.99 .. 1.01 has to be confirmed.
// Ralvioli stores normalized corrections, so 1 means no scaling on either axis.

namespace cal {

enum class Result { Applied, NeedsConfirm, Invalid };
struct Factors {
    double feed = 1.0;
    double across = 1.0;

    bool active() const { return feed != 1.0 || across != 1.0; }
};

inline Result calculate(double current, double soll, double ist, bool confirmed, double& out) {
    if (!(ist > 0.0) || !(soll > 0.0) || !(current > 0.0) ||
        !std::isfinite(ist) || !std::isfinite(soll) || !std::isfinite(current)) {
        return Result::Invalid;
    }
    double ratio = soll / ist;
    double next = current * ratio;
    if (!std::isfinite(next) || !(next > 0.0)) {
        return Result::Invalid;
    }
    bool small = ratio >= 0.99 && ratio <= 1.01;
    if (!small && !confirmed) {
        return Result::NeedsConfirm;
    }
    out = next;
    return Result::Applied;
}

}  // namespace cal
