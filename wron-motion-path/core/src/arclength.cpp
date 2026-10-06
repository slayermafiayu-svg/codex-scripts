#include "wmp/arclength.h"

#include <algorithm>
#include <cmath>

namespace wmp {

namespace {
constexpr double kGx[5] = {-0.90617984593866399280, -0.53846931010568309104, 0.0, 0.53846931010568309104,
                           0.90617984593866399280};
constexpr double kGw[5] = {0.23692688505618908751, 0.47862867049936646804, 0.56888888888888888889,
                           0.47862867049936646804, 0.23692688505618908751};
constexpr int kInitialIntervals = 8;
constexpr int kMaxDepth = 24;
}  // namespace

double ArcLengthTable::gauss(double a, double b) const {
    const double half = 0.5 * (b - a);
    const double mid = 0.5 * (a + b);
    double sum = 0.0;
    for (int i = 0; i < 5; ++i) sum += kGw[i] * speed(mid + half * kGx[i]);
    return sum * half;
}

void ArcLengthTable::refine(double a, double b, double whole, double tol, int depth) {
    const double m = 0.5 * (a + b);
    const double l = gauss(a, m);
    const double r = gauss(m, b);
    if (depth >= kMaxDepth || std::fabs(l + r - whole) <= tol) {
        s_.push_back(s_.back() + l);
        t_.push_back(m);
        s_.push_back(s_.back() + r);
        t_.push_back(b);
        return;
    }
    refine(a, m, l, 0.5 * tol, depth + 1);
    refine(m, b, r, 0.5 * tol, depth + 1);
}

void ArcLengthTable::build(const Cubic& c, double absTolerance) {
    c_ = c;
    tol_ = std::max(absTolerance, 1e-15);
    t_.assign(1, 0.0);
    s_.assign(1, 0.0);
    if (c.isPoint()) {
        t_.push_back(1.0);
        s_.push_back(0.0);
        total_ = 0.0;
        return;
    }
    for (int i = 0; i < kInitialIntervals; ++i) {
        const double a = static_cast<double>(i) / kInitialIntervals;
        const double b = static_cast<double>(i + 1) / kInitialIntervals;
        refine(a, b, gauss(a, b), tol_ * (b - a), 0);
    }
    t_.back() = 1.0;
    total_ = s_.back();
}

double ArcLengthTable::lengthAtT(double t) const {
    if (t_.size() < 2) return 0.0;
    t = std::clamp(t, 0.0, 1.0);
    if (t >= 1.0) return total_;
    auto it = std::upper_bound(t_.begin(), t_.end(), t);
    std::size_t k = static_cast<std::size_t>(std::max<std::ptrdiff_t>(0, (it - t_.begin()) - 1));
    k = std::min(k, t_.size() - 2);
    return s_[k] + gauss(t_[k], t);
}

double ArcLengthTable::tAtLength(double s) const {
    if (t_.size() < 2 || !(total_ > 0.0)) return 0.0;
    if (!(s > 0.0)) return 0.0;
    if (s >= total_) return 1.0;
    auto it = std::upper_bound(s_.begin(), s_.end(), s);
    std::size_t k = static_cast<std::size_t>(std::max<std::ptrdiff_t>(0, (it - s_.begin()) - 1));
    k = std::min(k, s_.size() - 2);
    const double a = t_[k], b = t_[k + 1];
    const double span = s_[k + 1] - s_[k];
    const double target = s - s_[k];
    if (!(span > 0.0)) return a;
    double lo = a, hi = b;
    double t = a + (b - a) * (target / span);
    const double ftol = tol_ * 1e-3;
    for (int iter = 0; iter < 40; ++iter) {
        const double f = gauss(a, t) - target;
        if (std::fabs(f) <= ftol) break;
        if (f > 0.0) hi = t;
        else lo = t;
        if (hi - lo <= 1e-15) break;
        const double d = speed(t);
        const double next = (d > 0.0) ? t - f / d : lo - 1.0;
        t = (next > lo && next < hi) ? next : 0.5 * (lo + hi);
    }
    return t;
}

}  // namespace wmp
