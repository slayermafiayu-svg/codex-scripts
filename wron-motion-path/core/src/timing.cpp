#include "wmp/timing.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace wmp {

double CubicEasing::eval(double x) const {
    if (!std::isfinite(x)) return 0.0;
    x = std::clamp(x, 0.0, 1.0);
    if (isLinear()) return x;
    if (x <= 0.0) return 0.0;
    if (x >= 1.0) return 1.0;
    const double ax1 = std::clamp(x1, 0.0, 1.0), ax2 = std::clamp(x2, 0.0, 1.0);
    // Bernstein form with P0=(0,0), P3=(1,1).
    auto bx = [&](double s) {
        const double u = 1.0 - s;
        return 3.0 * u * u * s * ax1 + 3.0 * u * s * s * ax2 + s * s * s;
    };
    auto dbx = [&](double s) {
        const double u = 1.0 - s;
        return 3.0 * u * u * ax1 + 6.0 * u * s * (ax2 - ax1) + 3.0 * s * s * (1.0 - ax2);
    };
    auto by = [&](double s) {
        const double u = 1.0 - s;
        return 3.0 * u * u * s * y1 + 3.0 * u * s * s * y2 + s * s * s;
    };
    // bx is monotone non-decreasing on [0,1] because ax1, ax2 in [0,1].
    double lo = 0.0, hi = 1.0, s = x;
    for (int i = 0; i < 64; ++i) {
        const double f = bx(s) - x;
        if (std::fabs(f) < 1e-14) break;
        if (f > 0.0) hi = s;
        else lo = s;
        const double d = dbx(s);
        const double next = d > 1e-12 ? s - f / d : -1.0;
        s = (next > lo && next < hi) ? next : 0.5 * (lo + hi);
        if (hi - lo < 1e-15) break;
    }
    return by(s);
}

CubicEasing easingFor(EasingPreset preset, const CubicEasing& custom) {
    switch (preset) {
        case EasingPreset::Linear: return {0.0, 0.0, 1.0, 1.0};
        case EasingPreset::EaseIn: return {0.42, 0.0, 1.0, 1.0};
        case EasingPreset::EaseOut: return {0.0, 0.0, 0.58, 1.0};
        case EasingPreset::EaseInOut: return {0.42, 0.0, 0.58, 1.0};
        case EasingPreset::BackOut: return {0.34, 1.56, 0.64, 1.0};
        case EasingPreset::Custom: return custom;
    }
    return {};
}

double lapEase(double p, const CubicEasing& e) {
    if (!std::isfinite(p)) return 0.0;
    if (e.isLinear()) return p;
    const double base = std::floor(p);
    return base + e.eval(p - base);
}

ProgressState mapProgress(double progress, bool closedPath, const ProgressSettings& s) {
    ProgressState st;
    if (!std::isfinite(progress)) progress = 0.0;
    const double offset = std::isfinite(s.startOffset) ? s.startOffset : 0.0;
    double q = lapEase(progress, s.easing) + offset;
    // Keep floor() exact: beyond 2^52 the fractional part is meaningless anyway.
    q = std::clamp(q, -1.0e12, 1.0e12);
    int sign = 1;
    if (s.reverse) {
        q = 1.0 - q;
        sign = -1;
    }
    if (closedPath) {
        st.u = q - std::floor(q);
        st.direction = sign;
        return st;
    }
    switch (s.endBehavior) {
        case EndBehavior::Clamp:
            st.clamped = q < 0.0 || q > 1.0;
            st.u = std::clamp(q, 0.0, 1.0);
            st.direction = sign;
            break;
        case EndBehavior::Extend:
            st.u = q;
            st.direction = sign;
            break;
        case EndBehavior::Loop: {
            const double lap = (q > 0.0) ? std::ceil(q) - 1.0 : std::floor(q);
            st.u = q - lap;
            st.lap = static_cast<std::int64_t>(lap);
            st.direction = sign;
            break;
        }
        case EndBehavior::PingPong: {
            const double m = q - 2.0 * std::floor(q * 0.5);  // [0,2)
            if (m <= 1.0) {
                st.u = m;
                st.direction = sign;
            } else {
                st.u = 2.0 - m;
                st.direction = -sign;
            }
            break;
        }
    }
    return st;
}

}  // namespace wmp
