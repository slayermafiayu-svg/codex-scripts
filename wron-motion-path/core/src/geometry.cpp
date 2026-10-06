#include "wmp/geometry.h"

#include <algorithm>
#include <cmath>

namespace wmp {

PathGeometry::PathGeometry(const Path& path, const Options& opts) {
    aspect_ = (std::isfinite(opts.aspect) && opts.aspect > 0.0) ? opts.aspect : 1.0;
    const double tol = (std::isfinite(opts.tolerance) && opts.tolerance > 0.0) ? opts.tolerance : 1e-7;
    points_ = path.points.size();
    closed_ = path.closed;
    cum_.assign(1, 0.0);
    if (points_ == 0) return;
    firstPoint_ = toDisplay(path.points[0].p);
    const std::size_t nseg = wmp::segmentCount(path);
    segs_.reserve(nseg);
    tables_.resize(nseg);
    for (std::size_t i = 0; i < nseg; ++i) {
        const PathPoint& a = path.points[i];
        const PathPoint& b = path.points[(i + 1) % points_];
        segs_.push_back({toDisplay(a.p), toDisplay(a.p + a.out), toDisplay(b.p + b.in), toDisplay(b.p)});
        tables_[i].build(segs_.back(), tol);
        cum_.push_back(cum_.back() + tables_[i].length());
    }
}

double PathGeometry::wrap(double d) const {
    const double L = length();
    double w = d - L * std::floor(d / L);
    if (w < 0.0 || w > L) w = 0.0;  // guard against rounding at huge |d|
    return w;
}

bool PathGeometry::neighbourTangent(int seg, int bias, Vec2& out) const {
    const int n = segmentCount();
    for (int pass = 0; pass < 2; ++pass) {
        const int dir = (pass == 0) ? (bias >= 0 ? 1 : -1) : (bias >= 0 ? -1 : 1);
        int k = seg;
        for (int step = 0; step < n; ++step) {
            k += dir;
            if (closed_) k = (k % n + n) % n;
            else if (k < 0 || k >= n) break;
            if (tables_[static_cast<std::size_t>(k)].length() > 0.0) {
                // Entering forward: start tangent of the next segment; going back: end tangent.
                return robustTangent(segs_[static_cast<std::size_t>(k)], dir > 0 ? 0.0 : 1.0, dir, out);
            }
        }
    }
    out = {0.0, 0.0};
    return false;
}

bool PathGeometry::tangentAt(int seg, double t, int bias, Vec2& out) const {
    if (tables_[static_cast<std::size_t>(seg)].length() > 0.0 &&
        robustTangent(segs_[static_cast<std::size_t>(seg)], t, bias, out))
        return true;
    return neighbourTangent(seg, bias, out);
}

PathSample PathGeometry::sampleAtDistance(double d, int bias, bool extend) const {
    PathSample s;
    const double L = length();
    if (segs_.empty() || !(L > 0.0)) {
        s.pos = segs_.empty() ? firstPoint_ : segs_[0].p0;
        s.segment = segs_.empty() ? -1 : 0;
        s.tangentValid = false;
        return s;
    }
    if (!std::isfinite(d)) d = 0.0;
    if (closed_) {
        d = wrap(d);
        if (bias < 0 && d == 0.0) d = L;  // arriving at the loop start from its last segment
        if (bias >= 0 && d == L) d = 0.0;
    } else if (d < 0.0 || d > L) {
        if (extend) {
            const bool before = d < 0.0;
            PathSample e = sampleAtDistance(before ? 0.0 : L, before ? 1 : -1, false);
            if (e.tangentValid) e.pos += e.tangent * (before ? d : d - L);
            return e;
        }
        d = std::clamp(d, 0.0, L);
    }

    const int n = segmentCount();
    int k = 0;
    double local = 0.0;
    // Distances within jointEps of a joint count as "at the joint", so the
    // bias (travel direction) decides the segment despite rounding in cum_.
    const double jointEps = 1e-12 * std::max(1.0, L);
    if (bias >= 0) {
        auto it = std::upper_bound(cum_.begin() + 1, cum_.end(), d + jointEps);
        if (it == cum_.end()) {
            k = n - 1;
            while (k > 0 && !(tables_[static_cast<std::size_t>(k)].length() > 0.0)) --k;
            local = tables_[static_cast<std::size_t>(k)].length();
        } else {
            k = static_cast<int>(it - cum_.begin()) - 1;
            local = d - cum_[static_cast<std::size_t>(k)];
        }
    } else {
        auto it = std::lower_bound(cum_.begin(), cum_.end() - 1, d - jointEps);
        const int j = static_cast<int>(it - cum_.begin());
        if (j == 0) {
            k = 0;
            while (k < n - 1 && !(tables_[static_cast<std::size_t>(k)].length() > 0.0)) ++k;
            local = 0.0;
        } else {
            k = j - 1;
            local = d - cum_[static_cast<std::size_t>(k)];
        }
    }
    const ArcLengthTable& tab = tables_[static_cast<std::size_t>(k)];
    local = std::clamp(local, 0.0, tab.length());
    const double t = tab.tAtLength(local);
    s.pos = segs_[static_cast<std::size_t>(k)].eval(t);
    s.segment = k;
    s.t = t;
    s.tangentValid = tangentAt(k, t, bias, s.tangent);
    return s;
}

double PathGeometry::distanceForU(double u, SpeedMode mode) const {
    if (!std::isfinite(u)) u = 0.0;
    const double L = length();
    if (mode == SpeedMode::ConstantSpeed || segs_.empty()) return u * L;
    const int n = segmentCount();
    const double x = u * n;
    if (x <= 0.0) return x * tables_.front().length();
    if (x >= n) return L + (x - n) * tables_.back().length();
    const int k = std::min(n - 1, static_cast<int>(std::floor(x)));
    return cum_[static_cast<std::size_t>(k)] + (x - k) * tables_[static_cast<std::size_t>(k)].length();
}

bool PathGeometry::smoothedTangent(double d, double sigma, int bias, Vec2& out) const {
    const PathSample raw = sampleAtDistance(d, bias, !closed_);
    const double L = length();
    if (!(sigma > 0.0) || !(L > 0.0)) {
        out = raw.tangent;
        return raw.tangentValid;
    }
    sigma = std::min(sigma, closed_ ? 0.5 * L : L);
    // Composite 4-point Gauss–Legendre on each half window.
    static constexpr double gx[4] = {-0.86113631159405257522, -0.33998104358485626480, 0.33998104358485626480,
                                     0.86113631159405257522};
    static constexpr double gw[4] = {0.34785484513745385737, 0.65214515486254614263, 0.65214515486254614263,
                                     0.34785484513745385737};
    constexpr int kSub = 4;
    auto meanPos = [&](double from, double to) {
        Vec2 acc;
        const double h = (to - from) / kSub;
        for (int i = 0; i < kSub; ++i) {
            const double mid = from + (i + 0.5) * h;
            for (int j = 0; j < 4; ++j) {
                const Vec2 p = sampleAtDistance(mid + 0.5 * h * gx[j], bias, !closed_).pos;
                acc += p * (gw[j] * 0.5);
            }
        }
        return acc / static_cast<double>(kSub);
    };
    const Vec2 dir = meanPos(d, d + sigma) - meanPos(d - sigma, d);
    if (wmp::length(dir) > 1e-9 * sigma) {
        out = normalizeOrZero(dir);
        return true;
    }
    out = raw.tangent;
    return raw.tangentValid;
}

}  // namespace wmp
