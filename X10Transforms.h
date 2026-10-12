// X10 semantic transforms (generic; mirror civ6x10/transforms.py).
// Full precision internally; formatting matches destination conventions:
// ADDITIVE/PROBABILITY/DISCOUNT use %.6g, COMBAT uses %.2f.
#pragma once
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace X10Transforms {
    enum Kind { ADDITIVE = 0, COMBAT = 1, PROBABILITY = 2, DISCOUNT = 3,
                VECTOR = 4, DELTA = 5, REPEAT = 6 };

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
    //
    // Negative combat officials are debuffs, not bonuses: the tenfold
    // formula is undefined for them (inner<=0), so they scale flat
    // (Phase 8B: symmetrical magnitude scaling, e.g. -5 -> -50 at x10).
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
            if (official < 0) {
                // Debuff: flat magnitude (formula is bonus-only).
                v = official * k;
                break;
            }
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
        case DELTA: {
            // Non-destructive bridge helper: the direct table keeps V,
            // this definition contributes V*(k-1) for a total of V*k.
            // At k=1 the delta is exactly 0 (helper inert).
            v = official * (k - 1.0);
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

    // Vector magnitudes (Phase 8B: Petra-style positional Amount lists).
    // Scales each comma-separated element additively (same semantics as
    // ADDITIVE), preserving element count, order, and compact "a,b,c"
    // formatting (%.6g per element, no spaces). Fails closed on any
    // malformed input: fewer than 2 elements, empty elements, non-numeric
    // or non-finite elements, overlong input. Overflow of `out` fails
    // closed. Mirrors civ6x10.transforms.vector_multiply exactly.
    inline bool ApplyVector(const char* officialStr, double k,
                            char* out, size_t cap) {
        if (!officialStr || !out || cap == 0) return false;
        if (!(k == k) || k < 0 || k > 100) return false;
        // Count elements first (strict: at least one comma).
        size_t len = 0;
        int commas = 0;
        while (officialStr[len] && len < 256) {
            if (officialStr[len] == ',') commas++;
            len++;
        }
        if (officialStr[len] != '\0' || commas < 1) return false;
        size_t pos = 0;   // read position in officialStr
        size_t wpos = 0;  // write position in out
        int index = 0;
        char num[64];
        while (true) {
            size_t nlen = 0;
            while (officialStr[pos] == ' ' || officialStr[pos] == '\t') pos++;
            while (officialStr[pos] && officialStr[pos] != ',' &&
                   nlen + 1 < sizeof(num)) {
                num[nlen++] = officialStr[pos++];
            }
            while (nlen > 0 && (num[nlen - 1] == ' ' || num[nlen - 1] == '\t')) nlen--;
            num[nlen] = '\0';
            if (nlen == 0) return false;
            char* end = nullptr;
            double v = strtod(num, &end);
            if (!end || *end != '\0') return false;
            if (!(v == v) || std::abs(v) > 1000000.0) return false;
            double scaled = v * k;
            if (!(scaled == scaled) || std::abs(scaled) > 1000000.0) return false;
            char elem[64];
            int n = snprintf(elem, sizeof(elem), "%.6g", scaled);
            if (n <= 0 || (size_t)n >= sizeof(elem)) return false;
            if (index > 0) {
                if (wpos + 1 >= cap) return false;
                out[wpos++] = ',';
            }
            if (wpos + (size_t)n >= cap) return false;
            memcpy(out + wpos, elem, (size_t)n);
            wpos += (size_t)n;
            index++;
            if (!officialStr[pos]) break;
            pos++;  // skip comma
        }
        if (index < 2) return false;
        if (wpos >= cap) return false;
        out[wpos] = '\0';
        return true;
    }

    // Integral-k copy count for implicit-+1 slot grants (Phase 8B REPEAT).
    // Returns the number of inert copies to activate (N-1 for integral k
    // in [1, 1+maximum]), or -1 on refusal (fractional k keeps the single
    // vanilla slot; never a rounded count). Mirrors
    // civ6x10.transforms.repeat_copies (FLOAT32-aware exactness).
    inline int RepeatCopies(double k, double kErr, int maximum) {
        if (!(k == k)) return -1;
        double r = round(k);
        double tol = kErr + 1e-9 * (1.0 > fabs(k) ? 1.0 : fabs(k));
        if (!(tol < 0.5) || fabs(k - r) > tol) return -1;
        long n = (long)r;
        if (n < 1 || n > 1 + maximum) return -1;
        return (int)(n - 1);
    }
}
