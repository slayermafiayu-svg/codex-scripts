#include "wmp/edit.h"

#include <algorithm>
#include <cmath>
#include <set>

#include "wmp/bezier.h"

namespace wmp::edit {

namespace {

Vec2 toD(Vec2 n, double aspect) { return {n.x * aspect, n.y}; }
Vec2 toN(Vec2 d, double aspect) { return {d.x / aspect, d.y}; }

Cubic segmentN(const Path& p, int i) {
    const std::size_t n = p.points.size();
    const PathPoint& a = p.points[static_cast<std::size_t>(i)];
    const PathPoint& b = p.points[(static_cast<std::size_t>(i) + 1) % n];
    return {a.p, a.p + a.out, b.p + b.in, b.p};
}

double distToSegmentD(const Cubic& cN, double aspect, Vec2 posD, double& tOut) {
    // Coarse sampling, then golden-section refinement around the best sample.
    constexpr int kSamples = 64;
    auto dist = [&](double t) { return length(toD(cN.eval(t), aspect) - posD); };
    double bestT = 0.0, best = dist(0.0);
    for (int i = 1; i <= kSamples; ++i) {
        const double t = static_cast<double>(i) / kSamples;
        const double d = dist(t);
        if (d < best) {
            best = d;
            bestT = t;
        }
    }
    double lo = std::max(0.0, bestT - 1.0 / kSamples), hi = std::min(1.0, bestT + 1.0 / kSamples);
    const double g = 0.5 * (std::sqrt(5.0) - 1.0);
    double x1 = hi - g * (hi - lo), x2 = lo + g * (hi - lo);
    double f1 = dist(x1), f2 = dist(x2);
    for (int k = 0; k < 40; ++k) {
        if (f1 < f2) {
            hi = x2;
            x2 = x1;
            f2 = f1;
            x1 = hi - g * (hi - lo);
            f1 = dist(x1);
        } else {
            lo = x1;
            x1 = x2;
            f1 = f2;
            x2 = lo + g * (hi - lo);
            f2 = dist(x2);
        }
    }
    const double tm = 0.5 * (lo + hi);
    const double dm = dist(tm);
    if (dm < best) {
        best = dm;
        bestT = tm;
    }
    tOut = bestT;
    return best;
}

}  // namespace

Hit hitTest(const Path& path, double aspect, Vec2 posN, double tol, int selected) {
    Hit hit;
    const Vec2 posD = toD(posN, aspect);
    const int n = static_cast<int>(path.points.size());
    double best = tol;
    for (int i = 0; i < n; ++i) {
        const double d = length(toD(path.points[static_cast<std::size_t>(i)].p, aspect) - posD);
        if (d <= best) {
            best = d;
            hit = {HitKind::Anchor, i, 0.0, d};
        }
    }
    if (hit.kind != HitKind::None) return hit;
    if (selected >= 0 && selected < n) {
        const PathPoint& pt = path.points[static_cast<std::size_t>(selected)];
        const struct {
            HitKind kind;
            Vec2 h;
        } handles[2] = {{HitKind::InHandle, pt.in}, {HitKind::OutHandle, pt.out}};
        for (const auto& h : handles) {
            if (h.h == Vec2{}) continue;
            const double d = length(toD(pt.p + h.h, aspect) - posD);
            if (d <= best) {
                best = d;
                hit = {h.kind, selected, 0.0, d};
            }
        }
        if (hit.kind != HitKind::None) return hit;
    }
    const int segs = static_cast<int>(segmentCount(path));
    for (int s = 0; s < segs; ++s) {
        double t = 0.0;
        const double d = distToSegmentD(segmentN(path, s), aspect, posD, t);
        if (d <= best) {
            best = d;
            hit = {HitKind::Segment, s, t, d};
        }
    }
    return hit;
}

std::string nextPointId(const Path& path) {
    std::set<std::string> used;
    for (const PathPoint& p : path.points) used.insert(p.id);
    for (std::size_t k = path.points.size() + 1;; ++k) {
        std::string id = "p" + std::to_string(k);
        if (!used.count(id)) return id;
    }
}

int insertPoint(Path& path, int segment, double t) {
    const int segs = static_cast<int>(segmentCount(path));
    if (segment < 0 || segment >= segs) return -1;
    t = std::clamp(t, 1e-6, 1.0 - 1e-6);
    Cubic left, right;
    segmentN(path, segment).split(t, left, right);
    const std::size_t n = path.points.size();
    PathPoint& a = path.points[static_cast<std::size_t>(segment)];
    PathPoint& b = path.points[(static_cast<std::size_t>(segment) + 1) % n];
    a.out = left.p1 - left.p0;
    b.in = right.p2 - right.p3;
    PathPoint m;
    m.p = left.p3;
    m.in = left.p2 - left.p3;
    m.out = right.p1 - right.p0;
    m.mode = (m.in == Vec2{} && m.out == Vec2{}) ? HandleMode::Corner : HandleMode::Smooth;
    m.id = nextPointId(path);
    const std::size_t at = static_cast<std::size_t>(segment) + 1;
    path.points.insert(path.points.begin() + static_cast<std::ptrdiff_t>(at), m);
    return static_cast<int>(at);
}

void deletePoint(Path& path, int index) {
    if (index < 0 || index >= static_cast<int>(path.points.size())) return;
    path.points.erase(path.points.begin() + index);
    if (path.points.size() < 2) path.closed = false;
}

void moveAnchor(Path& path, int index, Vec2 newPosN) {
    if (index < 0 || index >= static_cast<int>(path.points.size())) return;
    path.points[static_cast<std::size_t>(index)].p = newPosN;  // handles are relative: they follow
}

void moveHandle(Path& path, int index, bool outHandle, Vec2 handlePosN, double aspect, bool breakTangent) {
    if (index < 0 || index >= static_cast<int>(path.points.size())) return;
    PathPoint& pt = path.points[static_cast<std::size_t>(index)];
    Vec2& h = outHandle ? pt.out : pt.in;
    Vec2& o = outHandle ? pt.in : pt.out;
    h = handlePosN - pt.p;
    if (breakTangent || pt.mode == HandleMode::Corner) {
        pt.mode = HandleMode::Free;
        return;
    }
    if (pt.mode == HandleMode::Smooth) {
        const Vec2 hD = toD(h, aspect);
        const double oLen = length(toD(o, aspect));
        const double hLen = length(hD);
        if (hLen > 0.0 && oLen > 0.0) o = toN(hD * (-oLen / hLen), aspect);
    }
}

void setMode(Path& path, int index, HandleMode mode, double aspect) {
    const int n = static_cast<int>(path.points.size());
    if (index < 0 || index >= n) return;
    PathPoint& pt = path.points[static_cast<std::size_t>(index)];
    pt.mode = mode;
    if (mode == HandleMode::Corner) {
        pt.in = {};
        pt.out = {};
        return;
    }
    if (mode != HandleMode::Smooth) return;
    const Vec2 inD = toD(pt.in, aspect), outD = toD(pt.out, aspect);
    double inLen = length(inD), outLen = length(outD);
    Vec2 dir = normalizeOrZero(outD - inD);
    if (inLen == 0.0 && outLen == 0.0) {
        const bool hasPrev = path.closed || index > 0;
        const bool hasNext = path.closed || index < n - 1;
        const Vec2 p = toD(pt.p, aspect);
        const Vec2 prev = hasPrev ? toD(path.points[static_cast<std::size_t>((index - 1 + n) % n)].p, aspect) : p;
        const Vec2 next = hasNext ? toD(path.points[static_cast<std::size_t>((index + 1) % n)].p, aspect) : p;
        dir = normalizeOrZero(next - prev);
        inLen = length(p - prev) / 3.0;
        outLen = length(next - p) / 3.0;
    }
    if (dir == Vec2{}) return;
    pt.in = toN(dir * -inLen, aspect);
    pt.out = toN(dir * outLen, aspect);
}

void transformPath(Path& path, double aspect, Vec2 pivotN, double scale, double rotationDeg, Vec2 translateN) {
    const double r = degToRad(rotationDeg);
    const double c = std::cos(r) * scale, s = std::sin(r) * scale;
    const Vec2 pv = toD(pivotN, aspect);
    auto lin = [&](Vec2 vD) { return Vec2{c * vD.x - s * vD.y, s * vD.x + c * vD.y}; };
    for (PathPoint& pt : path.points) {
        const Vec2 pD = toD(pt.p, aspect);
        pt.p = toN(pv + lin(pD - pv), aspect) + translateN;
        pt.in = toN(lin(toD(pt.in, aspect)), aspect);
        pt.out = toN(lin(toD(pt.out, aspect)), aspect);
    }
}

}  // namespace wmp::edit
