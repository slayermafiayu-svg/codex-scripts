// Wron Motion Path — cubic Bézier segment.
#pragma once

#include <algorithm>
#include <cmath>

#include "wmp/vec2.h"

namespace wmp {

struct Cubic {
    Vec2 p0, p1, p2, p3;

    Vec2 eval(double t) const {
        const double u = 1.0 - t;
        const double b0 = u * u * u, b1 = 3.0 * u * u * t, b2 = 3.0 * u * t * t, b3 = t * t * t;
        return {b0 * p0.x + b1 * p1.x + b2 * p2.x + b3 * p3.x, b0 * p0.y + b1 * p1.y + b2 * p2.y + b3 * p3.y};
    }
    // First derivative dB/dt.
    Vec2 d1(double t) const {
        const double u = 1.0 - t;
        const Vec2 a = p1 - p0, b = p2 - p1, c = p3 - p2;
        return (a * (u * u) + b * (2.0 * u * t) + c * (t * t)) * 3.0;
    }
    Vec2 d2(double t) const {
        const Vec2 a = p2 - p1 * 2.0 + p0, b = p3 - p2 * 2.0 + p1;
        return (a * (1.0 - t) + b * t) * 6.0;
    }
    Vec2 d3() const { return (p3 - p2 * 3.0 + p1 * 3.0 - p0) * 6.0; }

    // de Casteljau split at t: [0,t] -> a, [t,1] -> b. Shape is unchanged.
    void split(double t, Cubic& a, Cubic& b) const {
        const Vec2 p01 = lerp(p0, p1, t), p12 = lerp(p1, p2, t), p23 = lerp(p2, p3, t);
        const Vec2 p012 = lerp(p01, p12, t), p123 = lerp(p12, p23, t);
        const Vec2 m = lerp(p012, p123, t);
        a = {p0, p01, p012, m};
        b = {m, p123, p23, p3};
    }

    // Size of the control polygon; used for relative epsilons.
    double extent() const {
        const double minx = std::min({p0.x, p1.x, p2.x, p3.x}), maxx = std::max({p0.x, p1.x, p2.x, p3.x});
        const double miny = std::min({p0.y, p1.y, p2.y, p3.y}), maxy = std::max({p0.y, p1.y, p2.y, p3.y});
        return std::max(maxx - minx, maxy - miny);
    }

    bool isPoint() const { return p0 == p1 && p1 == p2 && p2 == p3; }
};

// Unit forward tangent (direction of increasing t) at t, robust to vanishing
// derivatives (retracted handles, cusps). `side` selects the one-sided limit
// at a point where B'(t) == 0: +1 = just after t, -1 = just before t. At t==0
// only "after" exists, at t==1 only "before". Returns false when the segment
// has no direction at all (all control points coincide).
inline bool robustTangent(const Cubic& c, double t, int side, Vec2& out) {
    const double scale = std::max(c.extent(), 1e-300);
    const double eps = scale * 1e-9;
    const Vec2 v1 = c.d1(t);
    if (length(v1) > eps) {
        out = normalizeOrZero(v1);
        return true;
    }
    // B'(t+h) ~= h * B''(t): after the point the direction is +B'', before it is -B''.
    // At (or numerically at) a segment end only the limit inside the segment exists.
    int s = side >= 0 ? 1 : -1;
    if (t <= 1e-9) s = 1;
    if (t >= 1.0 - 1e-9) s = -1;
    const Vec2 v2 = c.d2(t);
    if (length(v2) > eps) {
        out = normalizeOrZero(v2 * static_cast<double>(s));
        return true;
    }
    // B'(t+h) ~= h^2/2 * B''' on both sides.
    const Vec2 v3 = c.d3();
    if (length(v3) > eps) {
        out = normalizeOrZero(v3);
        return true;
    }
    const Vec2 chord = c.p3 - c.p0;
    if (length(chord) > eps) {
        out = normalizeOrZero(chord);
        return true;
    }
    out = {0.0, 0.0};
    return false;
}

}  // namespace wmp
