// X10 semantic transforms (generic; mirror civ6x10/transforms.py).
// Full precision internally; formatting matches destination conventions:
// ADDITIVE/PROBABILITY/DISCOUNT use %.6g, COMBAT uses %.2f.
#pragma once
#include <cmath>
#include <cstdio>

namespace X10Transforms {
    enum Kind { ADDITIVE = 0, COMBAT = 1, PROBABILITY = 2, DISCOUNT = 3 };

    // Signed finite sanity bound (negative penalties/discounts are valid).
    // Family-specific validity (probability/discount ranges, combat domain,
    // count integrality) is enforced per branch below.
    inline bool FiniteSane(double v) {
        return v == v && std::abs(v) <= 1000000.0;
    }

    // Returns false when the transform is undefined for the input.
    // countLike: result must be integral; fractional outcomes are refused
    // (DECISION_REQUIRED), never floored. kErr is the source quantization
    // half-ULP of k (0 for INT32 multipliers): a count-like result may round
    // to integer n only when |v-n| <= |official|*kErr + double-rounding
    // allowance, so stored-FLOAT32 k (e.g. 7.300000190734863) still accepts
    // exact 10->73 while refusing genuine fractions like 3->21.9.
    inline bool Apply(int kind, double official, double k, double kErr,
                      bool countLike, char* out, size_t cap) {
        if (!(k == k) || k < 0 || k > 100) return false;
        double v = 0;
        const char* fmt = "%.6g";
        switch (kind) {
        case ADDITIVE:
            v = official * k;
            break;
        case COMBAT: {
            double inner = k * (exp(official / 25.0) - 1.0) + 1.0;
            if (inner <= 0) return false;
            v = 25.0 * log(inner);
            fmt = "%.2f";
            break;
        }
        case PROBABILITY: {
            double p = official;
            bool percent = (official > 1.0 || official < -1.0);
            if (percent) {
                if (official > 100.0 || official < -100.0) return false;
                p = official / 100.0;
            }
            if (p < 0 || p > 1) return false;
            v = 1.0 - pow(1.0 - p, k);
            if (percent) v *= 100.0;
            break;
        }
        case DISCOUNT: {
            double d = official < 0 ? -official / 100.0 : official / 100.0;
            if (d < 0 || d > 1) return false;
            v = (1.0 - pow(1.0 - d, k)) * 100.0;
            if (official < 0) v = -v;
            break;
        }
        default:
            return false;
        }
        if (!FiniteSane(v)) return false;
        if (countLike) {
            double r = round(v);
            // FLOAT32-quantization-aware exactness: tolerate only the
            // propagated source error of k, never a coarse epsilon.
            double tol = fabs(official) * kErr + 1e-9 * fmax(1.0, fabs(v));
            if (!(tol < 0.5) || fabs(v - r) > tol) return false;
            v = r;
        }
        snprintf(out, cap, fmt, v);
        return true;
    }
}
