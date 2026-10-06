#include <algorithm>
#include <cmath>
#include <vector>

#include "test.h"
#include "wmp/geometry.h"

using namespace wmp;

namespace {

PathPoint pt(double x, double y, Vec2 in = {}, Vec2 out = {}, HandleMode m = HandleMode::Free) {
    PathPoint p;
    p.p = {x, y};
    p.in = in;
    p.out = out;
    p.mode = m;
    return p;
}

// Independent reference: dense polyline length of a cubic from 0 to t.
double polylineLength(const Cubic& c, double t1, int steps = 200000) {
    double len = 0.0;
    Vec2 prev = c.eval(0.0);
    for (int i = 1; i <= steps; ++i) {
        const Vec2 q = c.eval(t1 * i / steps);
        len += length(q - prev);
        prev = q;
    }
    return len;
}

}  // namespace

TEST(straight_path_start_mid_end) {
    Path p;
    p.points = {pt(0.2, 0.5), pt(0.8, 0.5)};
    PathGeometry g(p, {16.0 / 9.0, 1e-9});
    CHECK_NEAR(g.length(), 0.6 * 16.0 / 9.0, 1e-12);
    for (double u : {0.0, 0.25, 0.5, 0.75, 1.0}) {
        const PathSample s = g.sampleAtDistance(g.distanceForU(u, SpeedMode::ConstantSpeed), 1, false);
        const Vec2 n = g.toNormalized(s.pos);
        CHECK_NEAR(n.x, 0.2 + 0.6 * u, 1e-9);
        CHECK_NEAR(n.y, 0.5, 1e-12);
        CHECK(s.tangentValid);
        CHECK_NEAR(s.tangent.x, 1.0, 1e-12);
    }
}

TEST(arc_length_matches_dense_polyline) {
    const Cubic curves[] = {
        {{0, 0}, {1, 2}, {3, -2}, {4, 0}},
        {{0, 0}, {0, 0}, {1, 0}, {1, 1}},          // retracted start handle
        {{0, 0}, {2, 1}, {-1, 1}, {1, 0}},         // self-crossing handles (cusp-like)
        {{0, 0}, {1e-3, 0}, {1 - 1e-3, 0}, {1, 0}} // nearly degenerate speed at ends
    };
    for (const Cubic& c : curves) {
        ArcLengthTable t;
        t.build(c, 1e-10);
        const double ref = polylineLength(c, 1.0, 400000);
        CHECK_NEAR(t.length(), ref, 2e-8 * std::max(1.0, ref));
        for (double tt : {0.1, 0.37, 0.5, 0.81, 0.999}) {
            const double s = t.lengthAtT(tt);
            CHECK_NEAR(t.tAtLength(s), tt, 1e-8);
        }
    }
}

TEST(constant_speed_on_bezier) {
    // Strongly uneven handles: equal Bézier-t steps are very unequal in distance.
    Path p;
    p.points = {pt(0.1, 0.8, {}, {0.02, -0.02}), pt(0.9, 0.2, {-0.6, 0.0}, {})};
    PathGeometry g(p, {16.0 / 9.0, 1e-10});
    const Cubic& c = g.segment(0);
    const double L = g.length();
    CHECK_NEAR(L, polylineLength(c, 1.0), 1e-7);
    // Sensitivity check: naive t spacing varies a lot on this curve.
    double minStep = 1e9, maxStep = 0.0;
    for (int i = 0; i < 20; ++i) {
        const double d = length(c.eval((i + 1) / 20.0) - c.eval(i / 20.0));
        minStep = std::min(minStep, d);
        maxStep = std::max(maxStep, d);
    }
    CHECK(maxStep / minStep > 3.0);
    // Arc-length mapping: reference distance at the returned t equals u * L.
    for (int i = 0; i <= 20; ++i) {
        const double u = i / 20.0;
        const PathSample s = g.sampleAtDistance(g.distanceForU(u, SpeedMode::ConstantSpeed), 1, false);
        CHECK_NEAR(polylineLength(c, s.t, 100000), u * L, 2e-6);
    }
}

TEST(different_segment_lengths_and_speed_modes) {
    Path p;
    p.points = {pt(0.0, 0.5), pt(0.1, 0.5), pt(1.0, 0.5)};  // 0.1 then 0.9 (normalized x)
    PathGeometry g(p, {1.0, 1e-10});
    CHECK_NEAR(g.length(), 1.0, 1e-12);
    // Constant speed: half progress is halfway along the distance, inside segment 2.
    PathSample s = g.sampleAtDistance(g.distanceForU(0.5, SpeedMode::ConstantSpeed), 1, false);
    CHECK_NEAR(s.pos.x, 0.5, 1e-9);
    CHECK(s.segment == 1);
    // Equal time per segment: half progress sits exactly on the middle point.
    s = g.sampleAtDistance(g.distanceForU(0.5, SpeedMode::EqualTimePerSegment), 1, false);
    CHECK_NEAR(s.pos.x, 0.1, 1e-9);
    s = g.sampleAtDistance(g.distanceForU(0.25, SpeedMode::EqualTimePerSegment), 1, false);
    CHECK_NEAR(s.pos.x, 0.05, 1e-9);
}

TEST(zero_length_segments_and_coincident_points) {
    Path p;
    p.points = {pt(0.2, 0.5), pt(0.2, 0.5), pt(0.5, 0.5), pt(0.5, 0.5), pt(0.5, 0.5), pt(0.8, 0.2)};
    PathGeometry g(p, {1.0, 1e-10});
    CHECK(g.segmentCount() == 5);
    CHECK_NEAR(g.segmentLength(0), 0.0, 0.0);
    CHECK_NEAR(g.length(), 0.3 + std::hypot(0.3, 0.3), 1e-12);
    for (int i = 0; i <= 100; ++i) {
        for (SpeedMode m : {SpeedMode::ConstantSpeed, SpeedMode::EqualTimePerSegment}) {
            for (int bias : {1, -1}) {
                const PathSample s = g.sampleAtDistance(g.distanceForU(i / 100.0, m), bias, false);
                CHECK(isFinite(s.pos));
                CHECK(s.tangentValid);
                CHECK(isFinite(s.tangent));
            }
        }
    }
    // At the start (inside the zero-length segment) the tangent comes from the next real segment.
    const PathSample s0 = g.sampleAtDistance(0.0, 1, false);
    CHECK_NEAR(s0.tangent.x, 1.0, 1e-12);
    // Arriving at the shared point (0.5,0.5) going forward: incoming direction (+x);
    // leaving it: outgoing direction (diagonal up-right).
    const PathSample arrive = g.sampleAtDistance(0.3, -1, false);
    const PathSample leave = g.sampleAtDistance(0.3, 1, false);
    CHECK_NEAR(arrive.tangent.x, 1.0, 1e-9);
    CHECK_NEAR(leave.tangent.x, std::sqrt(0.5), 1e-9);
    CHECK_NEAR(leave.tangent.y, -std::sqrt(0.5), 1e-9);
}

TEST(degenerate_paths) {
    Path empty;
    PathGeometry ge(empty, {});
    CHECK(ge.empty());
    CHECK_NEAR(ge.length(), 0.0, 0.0);
    Path single;
    single.points = {pt(0.3, 0.4)};
    PathGeometry gs(single, {2.0, 1e-9});
    const PathSample s = gs.sampleAtDistance(5.0, 1, true);
    CHECK_NEAR(gs.toNormalized(s.pos).x, 0.3, 1e-12);
    CHECK(!s.tangentValid);
    Path dup;
    dup.points = {pt(0.5, 0.5), pt(0.5, 0.5)};
    dup.closed = true;
    PathGeometry gd(dup, {1.0, 1e-9});
    CHECK_NEAR(gd.length(), 0.0, 0.0);
    CHECK(!gd.sampleAtDistance(0.0, 1, false).tangentValid);
    // Retracted handles at both ends: still a straight, well-defined segment.
    Path corners;
    corners.points = {pt(0, 0, {}, {}, HandleMode::Corner), pt(1, 0, {}, {}, HandleMode::Corner)};
    PathGeometry gc(corners, {1.0, 1e-9});
    CHECK(gc.sampleAtDistance(0.0, 1, false).tangentValid);
    CHECK(gc.sampleAtDistance(gc.length(), -1, false).tangentValid);
    CHECK_NEAR(gc.sampleAtDistance(gc.length(), -1, false).tangent.x, 1.0, 1e-12);
}

TEST(closed_path_connection) {
    Path sq;
    sq.points = {pt(0, 0), pt(1, 0), pt(1, 1), pt(0, 1)};
    sq.closed = true;
    PathGeometry g(sq, {1.0, 1e-10});
    CHECK(g.segmentCount() == 4);
    CHECK_NEAR(g.length(), 4.0, 1e-12);
    const PathSample a = g.sampleAtDistance(0.0, 1, false);
    const PathSample b = g.sampleAtDistance(4.0, 1, false);
    CHECK_NEAR(a.pos.x, b.pos.x, 1e-12);
    CHECK_NEAR(a.pos.y, b.pos.y, 1e-12);
    // Closing segment exists: just before the end we are on (0, small).
    const PathSample c = g.sampleAtDistance(3.9, 1, false);
    CHECK_NEAR(c.pos.x, 0.0, 1e-12);
    CHECK_NEAR(c.pos.y, 0.1, 1e-9);
    // Wraps for any distance.
    const PathSample w = g.sampleAtDistance(4.5 + 8.0, 1, false);
    CHECK_NEAR(w.pos.x, 0.5, 1e-9);
    // Arriving at the seam from the last segment keeps its direction (-y).
    CHECK_NEAR(g.sampleAtDistance(4.0, -1, false).tangent.y, -1.0, 1e-12);
}

TEST(extend_continues_along_end_tangents) {
    Path p;
    p.points = {pt(0.2, 0.5), pt(0.8, 0.5)};
    PathGeometry g(p, {1.0, 1e-10});
    const PathSample after = g.sampleAtDistance(g.length() + 0.1, 1, true);
    CHECK_NEAR(after.pos.x, 0.9, 1e-12);
    const PathSample before = g.sampleAtDistance(-0.1, 1, true);
    CHECK_NEAR(before.pos.x, 0.1, 1e-12);
    const PathSample clamped = g.sampleAtDistance(g.length() + 0.1, 1, false);
    CHECK_NEAR(clamped.pos.x, 0.8, 1e-12);
}

TEST(aspect_keeps_normalized_meaning) {
    // Same normalized path at 16:9 and 9:16: start/end/centre stay at the same
    // normalized positions; arc length is measured in the isotropic display space.
    Path p;
    p.points = {pt(0.1, 0.5), pt(0.9, 0.5)};
    for (double aspect : {16.0 / 9.0, 9.0 / 16.0, 4.0 / 3.0}) {
        PathGeometry g(p, {aspect, 1e-10});
        CHECK_NEAR(g.length(), 0.8 * aspect, 1e-12);
        const Vec2 mid = g.toNormalized(g.sampleAtDistance(g.distanceForU(0.5, SpeedMode::ConstantSpeed), 1, false).pos);
        CHECK_NEAR(mid.x, 0.5, 1e-9);
    }
}

TEST(smoothed_tangent_is_continuous_through_corner) {
    Path p;
    p.points = {pt(0.0, 0.5), pt(0.5, 0.5), pt(0.5, 0.0)};  // 90° left turn (y down: up the screen)
    PathGeometry g(p, {1.0, 1e-10});
    double prev = 0.0;
    bool first = true;
    double maxStep = 0.0;
    for (int i = 0; i <= 400; ++i) {
        const double d = 0.3 + 0.4 * i / 400.0;  // passes the corner at 0.5
        Vec2 t;
        CHECK(g.smoothedTangent(d, 0.05, 1, t));
        const double ang = std::atan2(t.y, t.x);
        if (!first) maxStep = std::max(maxStep, std::fabs(ang - prev));
        prev = ang;
        first = false;
    }
    // The 90° turn is spread over 2*sigma = 0.1 of distance: per 0.001 step
    // the angle changes by at most a few degrees, never a jump.
    CHECK(maxStep < degToRad(3.5));
    Vec2 before, after;
    g.smoothedTangent(0.3, 0.05, 1, before);
    g.smoothedTangent(0.7, 0.05, 1, after);
    CHECK_NEAR(before.x, 1.0, 1e-9);
    CHECK_NEAR(after.y, -1.0, 1e-9);
    // Without smoothing the corner switches exactly at the corner point, in the travel direction.
    CHECK_NEAR(g.sampleAtDistance(0.5, 1, false).tangent.y, -1.0, 1e-12);
    CHECK_NEAR(g.sampleAtDistance(0.5, -1, false).tangent.x, 1.0, 1e-12);
}
