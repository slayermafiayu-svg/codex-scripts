#include <cmath>
#include <random>
#include <vector>

#include "test.h"
#include "wmp/motion.h"

using namespace wmp;

namespace {
PathPoint pt(double x, double y, Vec2 in = {}, Vec2 out = {}) {
    PathPoint p;
    p.p = {x, y};
    p.in = in;
    p.out = out;
    p.mode = (in == Vec2{} && out == Vec2{}) ? HandleMode::Corner : HandleMode::Smooth;
    return p;
}

Path circlePath(double cx, double cy, double r, double aspect) {
    // 4-point circle in display space, converted to normalized x.
    const double k = 0.5522847498307936 * r;
    Path p;
    p.closed = true;
    auto add = [&](double x, double y, Vec2 in, Vec2 out) {
        p.points.push_back(pt((cx + x) / aspect, cy + y, {in.x / aspect, in.y}, {out.x / aspect, out.y}));
    };
    add(r, 0, {0, -k}, {0, k});
    add(0, r, {k, 0}, {-k, 0});
    add(-r, 0, {0, k}, {0, -k});
    add(0, -r, {-k, 0}, {k, 0});
    return p;
}
}  // namespace

TEST(easing_curves) {
    const CubicEasing lin{};
    CHECK(lin.isLinear());
    CHECK_NEAR(lin.eval(0.3), 0.3, 0.0);
    const CubicEasing io = easingFor(EasingPreset::EaseInOut, {});
    CHECK_NEAR(io.eval(0.5), 0.5, 1e-9);
    CHECK_NEAR(io.eval(0.0), 0.0, 0.0);
    CHECK_NEAR(io.eval(1.0), 1.0, 0.0);
    double prev = -1.0;
    for (int i = 0; i <= 100; ++i) {
        const double y = io.eval(i / 100.0);
        CHECK(y >= prev);
        prev = y;
    }
    CHECK(io.eval(0.1) < 0.1);  // slow start
    const CubicEasing back = easingFor(EasingPreset::BackOut, {});
    double maxY = 0.0;
    for (int i = 0; i <= 100; ++i) maxY = std::max(maxY, back.eval(i / 100.0));
    CHECK(maxY > 1.05);  // overshoots
    // Out-of-range x1/x2 are clamped so the timing stays a function.
    const CubicEasing wild{-2.0, 0.0, 3.0, 1.0};
    CHECK(std::isfinite(wild.eval(0.5)));
}

TEST(lap_ease_is_continuous) {
    const CubicEasing io = easingFor(EasingPreset::EaseInOut, {});
    for (double k : {-2.0, -1.0, 0.0, 1.0, 2.0, 3.0}) {
        CHECK_NEAR(lapEase(k - 1e-9, io), lapEase(k + 1e-9, io), 1e-6);
        CHECK_NEAR(lapEase(k, io), k, 1e-12);
    }
}

TEST(progress_end_behaviours_open_path) {
    ProgressSettings s;
    s.endBehavior = EndBehavior::Clamp;
    CHECK_NEAR(mapProgress(-0.5, false, s).u, 0.0, 0.0);
    CHECK_NEAR(mapProgress(0.25, false, s).u, 0.25, 0.0);
    CHECK_NEAR(mapProgress(1.7, false, s).u, 1.0, 0.0);
    CHECK(mapProgress(1.7, false, s).clamped);

    s.endBehavior = EndBehavior::Extend;
    CHECK_NEAR(mapProgress(1.25, false, s).u, 1.25, 1e-15);
    CHECK_NEAR(mapProgress(-0.25, false, s).u, -0.25, 1e-15);

    s.endBehavior = EndBehavior::Loop;
    CHECK_NEAR(mapProgress(0.0, false, s).u, 0.0, 0.0);
    CHECK_NEAR(mapProgress(1.0, false, s).u, 1.0, 0.0);   // whole numbers > 0 end at the END
    CHECK_NEAR(mapProgress(1.25, false, s).u, 0.25, 1e-15);
    CHECK_NEAR(mapProgress(2.0, false, s).u, 1.0, 0.0);
    CHECK_NEAR(mapProgress(-0.25, false, s).u, 0.75, 1e-15);
    CHECK(mapProgress(0.9, false, s).lap == 0);
    CHECK(mapProgress(1.1, false, s).lap == 1);

    s.endBehavior = EndBehavior::PingPong;
    CHECK_NEAR(mapProgress(0.5, false, s).u, 0.5, 0.0);
    CHECK(mapProgress(0.5, false, s).direction == 1);
    CHECK_NEAR(mapProgress(1.0, false, s).u, 1.0, 0.0);
    CHECK_NEAR(mapProgress(1.5, false, s).u, 0.5, 1e-15);
    CHECK(mapProgress(1.5, false, s).direction == -1);
    CHECK_NEAR(mapProgress(2.25, false, s).u, 0.25, 1e-15);
    CHECK(mapProgress(2.25, false, s).direction == 1);
    // Continuous everywhere.
    for (int i = 0; i < 400; ++i) {
        const double p = -2.0 + i * 0.01;
        CHECK_NEAR(mapProgress(p, false, s).u, mapProgress(p + 1e-9, false, s).u, 1e-8);
    }
}

TEST(progress_reverse_offset_closed) {
    ProgressSettings s;
    s.reverse = true;
    CHECK_NEAR(mapProgress(0.0, false, s).u, 1.0, 0.0);
    CHECK_NEAR(mapProgress(0.25, false, s).u, 0.75, 0.0);
    CHECK(mapProgress(0.25, false, s).direction == -1);
    s.reverse = false;
    s.startOffset = 0.25;
    CHECK_NEAR(mapProgress(0.5, false, s).u, 0.75, 0.0);
    // Closed path: always seamless modulo, end behaviour ignored.
    s.endBehavior = EndBehavior::Clamp;
    CHECK_NEAR(mapProgress(0.9, true, s).u, 0.15, 1e-12);
    s.startOffset = 0.0;
    s.reverse = true;
    CHECK_NEAR(mapProgress(0.0, true, s).u, 0.0, 0.0);  // starts at the same place, moving backwards
    CHECK_NEAR(mapProgress(0.25, true, s).u, 0.75, 1e-12);
}

TEST(progress_is_deterministic_for_random_access) {
    // Evaluating times in any order (scrubbing, render farm) gives identical results.
    ProgressSettings s;
    s.endBehavior = EndBehavior::PingPong;
    s.easing = easingFor(EasingPreset::EaseInOut, {});
    s.startOffset = 0.1;
    std::vector<double> ps;
    for (int i = 0; i < 300; ++i) ps.push_back(i * 0.0137 - 1.0);
    std::vector<double> forward;
    for (double p : ps) forward.push_back(mapProgress(p, false, s).u);
    std::mt19937 rng(7);
    for (int k = 0; k < 2000; ++k) {
        const std::size_t i = rng() % ps.size();
        CHECK(mapProgress(ps[i], false, s).u == forward[i]);
    }
}

TEST(pose_identity_without_path) {
    MotionParams mp;
    const Pose pose = evaluatePose(nullptr, mp);
    const FrameRect f{0, 0, 1920, 1080};
    const Affine a = poseToCanonical(pose, f);
    CHECK_NEAR(a.a, 1.0, 1e-12);
    CHECK_NEAR(a.d, 1.0, 1e-12);
    CHECK_NEAR(a.b, 0.0, 1e-12);
    CHECK_NEAR(a.tx, 0.0, 1e-9);
    CHECK_NEAR(a.ty, 0.0, 1e-9);
}

TEST(pose_places_pivot_on_path_point) {
    Path p;
    p.points = {pt(0.2, 0.3), pt(0.8, 0.3)};
    const FrameRect f{0, 0, 1920, 1080};
    PathGeometry g(p, {f.aspect(), 1e-10});
    MotionParams mp;
    mp.progress = 0.5;
    mp.pivot = {0.5, 0.5};
    const Pose pose = evaluatePose(&g, mp);
    CHECK_NEAR(pose.position.x, 0.5, 1e-9);
    CHECK_NEAR(pose.position.y, 0.3, 1e-12);
    // Source pivot (frame centre) lands on the path point. Canonical Y is up:
    // normalized y 0.3 -> canonical y = 1080 * 0.7.
    const Affine a = poseToCanonical(pose, f);
    const Vec2 c = a.apply({960, 540});
    CHECK_NEAR(c.x, 960, 1e-6);
    CHECK_NEAR(c.y, 1080 * 0.7, 1e-6);
    // Offset is separate from the path and the pivot.
    mp.positionOffset = {0.1, 0.0};
    CHECK_NEAR(evaluatePose(&g, mp).position.x, 0.6, 1e-9);
}

TEST(rotation_is_clockwise_on_screen) {
    MotionParams mp;
    mp.rotationDeg = 90.0;
    const FrameRect f{0, 0, 1000, 1000};
    const Affine a = poseToCanonical(evaluatePose(nullptr, mp), f);
    // A point to the right of the centre moves BELOW it on screen = lower canonical y.
    const Vec2 c = a.apply({600, 500});
    CHECK_NEAR(c.x, 500, 1e-9);
    CHECK_NEAR(c.y, 400, 1e-9);
}

TEST(scale_in_source_axes_then_rotate) {
    MotionParams mp;
    mp.scale = {2.0, 1.0};
    mp.rotationDeg = 90.0;
    const FrameRect f{0, 0, 1000, 1000};
    const Affine a = poseToCanonical(evaluatePose(nullptr, mp), f);
    // Source x-axis is stretched first, then rotated to point down the screen.
    const Vec2 c = a.apply({600, 500});
    CHECK_NEAR(c.x, 500, 1e-9);
    CHECK_NEAR(c.y, 300, 1e-9);
}

TEST(orient_to_path_and_reverse_modes) {
    Path p;
    p.points = {pt(0.2, 0.5), pt(0.8, 0.5)};
    PathGeometry g(p, {1.0, 1e-10});
    MotionParams mp;
    mp.progress = 0.5;
    mp.orientToPath = true;
    CHECK_NEAR(evaluatePose(&g, mp).rotationDeg, 0.0, 1e-9);
    mp.orientOffsetDeg = 90.0;
    mp.rotationDeg = 10.0;
    CHECK_NEAR(evaluatePose(&g, mp).rotationDeg, 100.0, 1e-9);
    mp.orientOffsetDeg = 0.0;
    mp.rotationDeg = 0.0;
    mp.progressSettings.reverse = true;
    mp.orientMode = OrientMode::TravelDirection;
    CHECK_NEAR(std::fabs(evaluatePose(&g, mp).rotationDeg), 180.0, 1e-9);
    mp.orientMode = OrientMode::PathDirection;
    CHECK_NEAR(evaluatePose(&g, mp).rotationDeg, 0.0, 1e-9);
    // Ping-pong return leg flips only in travel mode, and changes the continuity key.
    mp.progressSettings.reverse = false;
    mp.progressSettings.endBehavior = EndBehavior::PingPong;
    mp.orientMode = OrientMode::TravelDirection;
    mp.progress = 1.5;
    const Pose back = evaluatePose(&g, mp);
    CHECK_NEAR(std::fabs(back.rotationDeg), 180.0, 1e-9);
    mp.progress = 0.5;
    CHECK(evaluatePose(&g, mp).continuityKey != back.continuityKey);
}

TEST(orientation_continuous_around_circle) {
    // Going round a circle the angle crosses ±180°. The transform (what is
    // actually rendered) must change smoothly: compare consecutive matrices.
    const FrameRect f{0, 0, 1920, 1080};
    PathGeometry g(circlePath(f.aspect() * 0.5, 0.5, 0.3, f.aspect()), {f.aspect(), 1e-10});
    MotionParams mp;
    mp.orientToPath = true;
    Affine prev{};
    double maxDelta = 0.0;
    for (int i = 0; i <= 2000; ++i) {
        mp.progress = i / 1000.0;  // two laps
        const Affine a = poseToCanonical(evaluatePose(&g, mp), f);
        if (i > 0) {
            maxDelta = std::max({maxDelta, std::fabs(a.a - prev.a), std::fabs(a.b - prev.b), std::fabs(a.c - prev.c),
                                 std::fabs(a.d - prev.d)});
        }
        prev = a;
    }
    // 360° per 1000 steps = 0.36° per step -> linear-part change ~0.0063.
    CHECK(maxDelta < 0.01);
    // The tangent of a 4-arc approximation is continuous (smooth points).
    mp.progress = 0.0;
    const double a0 = evaluatePose(&g, mp).rotationDeg;
    mp.progress = 1.0;
    const double a1 = evaluatePose(&g, mp).rotationDeg;
    CHECK_NEAR(std::remainder(a1 - a0, 360.0), 0.0, 1e-6);
}

TEST(open_loop_wrap_changes_continuity_key) {
    Path p;
    p.points = {pt(0.2, 0.5), pt(0.8, 0.5)};
    PathGeometry g(p, {1.0, 1e-10});
    MotionParams mp;
    mp.progressSettings.endBehavior = EndBehavior::Loop;
    mp.progress = 0.99;
    const Pose a = evaluatePose(&g, mp);
    mp.progress = 1.01;
    const Pose b = evaluatePose(&g, mp);
    CHECK(a.continuityKey != b.continuityKey);
    CHECK(a.position.x > 0.79 && b.position.x < 0.21);
    // Closed loops never change the key.
    Path c = p;
    c.closed = true;
    PathGeometry gc(c, {1.0, 1e-10});
    mp.progress = 0.99;
    const Pose ca = evaluatePose(&gc, mp);
    mp.progress = 1.01;
    CHECK(evaluatePose(&gc, mp).continuityKey == ca.continuityKey);
}
