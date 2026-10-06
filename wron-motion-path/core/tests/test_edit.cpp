#include <cmath>

#include "test.h"
#include "wmp/edit.h"
#include "wmp/geometry.h"
#include "wmp/presets.h"

using namespace wmp;

TEST(insert_point_preserves_shape) {
    const double aspect = 16.0 / 9.0;
    for (PresetId id : {PresetId::SCurve, PresetId::Circle, PresetId::Straight, PresetId::Spiral}) {
        const Path before = makePreset(id, aspect);
        Path after = before;
        const int idx = edit::insertPoint(after, 0, 0.37);
        CHECK(idx == 1);
        CHECK(after.points.size() == before.points.size() + 1);
        PathGeometry g0(before, {aspect, 1e-10}), g1(after, {aspect, 1e-10});
        CHECK_NEAR(g0.length(), g1.length(), 1e-9);
        for (int i = 0; i <= 50; ++i) {
            const double d = g0.length() * i / 50.0;
            const Vec2 a = g0.sampleAtDistance(d, 1, false).pos, b = g1.sampleAtDistance(d, 1, false).pos;
            CHECK_NEAR(a.x, b.x, 1e-8);
            CHECK_NEAR(a.y, b.y, 1e-8);
        }
        // Unique id assigned.
        for (std::size_t k = 0; k < after.points.size(); ++k)
            if (static_cast<int>(k) != idx) CHECK(after.points[k].id != after.points[static_cast<std::size_t>(idx)].id);
    }
    // Closing segment of a closed path inserts at the end.
    Path c = makePreset(PresetId::Circle, aspect);
    CHECK(edit::insertPoint(c, 3, 0.5) == 4);
    CHECK(c.points.size() == 5);
}

TEST(smooth_handles_stay_collinear_in_display_space) {
    const double aspect = 16.0 / 9.0;
    Path p = makePreset(PresetId::SCurve, aspect);
    const Vec2 oldIn = p.points[1].in;
    const double oldInLenD = std::hypot(oldIn.x * aspect, oldIn.y);
    edit::moveHandle(p, 1, true, p.points[1].p + Vec2{0.05, 0.1}, aspect, false);
    const Vec2 inD{p.points[1].in.x * aspect, p.points[1].in.y}, outD{p.points[1].out.x * aspect, p.points[1].out.y};
    CHECK_NEAR(cross(inD, outD), 0.0, 1e-12);
    CHECK(dot(inD, outD) < 0.0);
    CHECK_NEAR(std::hypot(inD.x, inD.y), oldInLenD, 1e-12);  // opposite length kept
    CHECK(p.points[1].mode == HandleMode::Smooth);
    // Alt/break -> free, opposite untouched.
    const Vec2 keep = p.points[1].in;
    edit::moveHandle(p, 1, true, p.points[1].p + Vec2{0.0, -0.1}, aspect, true);
    CHECK(p.points[1].mode == HandleMode::Free);
    CHECK(p.points[1].in == keep);
    // Pulling a handle out of a corner point makes it free.
    Path z = makePreset(PresetId::Zigzag, aspect);
    edit::moveHandle(z, 2, true, z.points[2].p + Vec2{0.05, 0.0}, aspect, false);
    CHECK(z.points[2].mode == HandleMode::Free);
}

TEST(set_mode_rules) {
    const double aspect = 1.0;
    Path z = makePreset(PresetId::Zigzag, aspect);
    edit::setMode(z, 2, HandleMode::Smooth, aspect);
    CHECK(z.points[2].mode == HandleMode::Smooth);
    CHECK(!(z.points[2].in == Vec2{}));
    CHECK_NEAR(cross(z.points[2].in, z.points[2].out), 0.0, 1e-12);
    edit::setMode(z, 2, HandleMode::Corner, aspect);
    CHECK(z.points[2].in == Vec2{} && z.points[2].out == Vec2{});
}

TEST(hit_test_priorities) {
    const double aspect = 16.0 / 9.0;
    Path p = makePreset(PresetId::SCurve, aspect);
    const double tol = 0.02;
    edit::Hit h = edit::hitTest(p, aspect, p.points[1].p + Vec2{0.001, 0.0}, tol, -1);
    CHECK(h.kind == edit::HitKind::Anchor && h.index == 1);
    h = edit::hitTest(p, aspect, p.points[1].p + p.points[1].out, tol, 1);
    CHECK(h.kind == edit::HitKind::OutHandle && h.index == 1);
    h = edit::hitTest(p, aspect, p.points[1].p + p.points[1].out, tol, -1);  // handles only for the selection
    CHECK(h.kind != edit::HitKind::OutHandle);
    PathGeometry g(p, {aspect, 1e-10});
    const Vec2 onCurve = g.toNormalized(g.sampleAtDistance(g.length() * 0.2, 1, false).pos);
    h = edit::hitTest(p, aspect, onCurve, tol, -1);
    CHECK(h.kind == edit::HitKind::Segment && h.index == 0);
    CHECK(edit::hitTest(p, aspect, {0.5, 0.05}, tol, -1).kind == edit::HitKind::None);
}

TEST(delete_and_transform) {
    const double aspect = 16.0 / 9.0;
    Path c = makePreset(PresetId::Circle, aspect);
    edit::deletePoint(c, 0);
    CHECK(c.points.size() == 3 && c.closed);
    edit::deletePoint(c, 0);
    edit::deletePoint(c, 0);
    CHECK(c.points.size() == 1 && !c.closed);
    // 90° rotation around the frame centre keeps a circle round (D-space metric).
    Path circle = makePreset(PresetId::Circle, aspect);
    const double L0 = PathGeometry(circle, {aspect, 1e-10}).length();
    edit::transformPath(circle, aspect, {0.5, 0.5}, 1.5, 90.0, {0.0, 0.0});
    CHECK_NEAR(PathGeometry(circle, {aspect, 1e-10}).length(), 1.5 * L0, 1e-9);
    // Top point rotated clockwise lands on the right.
    CHECK_NEAR(circle.points[0].p.x * aspect, 0.5 * aspect + 1.5 * 0.3, 1e-12);
    CHECK_NEAR(circle.points[0].p.y, 0.5, 1e-12);
}
