// Wron Motion Path — pose evaluation (parameters at one time -> transform).
//
// Transform order (D space, Y down; positive rotation = clockwise on screen):
//
//     out = P + R(rotation) * S(scaleX, scaleY) * (src - pivot)
//     P   = pathPoint(progress) + positionOffset
//     rotation = Rotation + (OrientToPath ? pathAngle + OrientOffset : 0)
//
// i.e. move the source pivot to the origin, scale in the source's own axes,
// rotate, then place the pivot on the path point plus the extra offset. The
// path position, the source pivot and the position offset are three separate
// inputs. Without a usable path the pivot stays where it is (P = pivot), so
// a neutral transform leaves the image untouched.
#pragma once

#include <cstdint>

#include "wmp/geometry.h"
#include "wmp/timing.h"
#include "wmp/transform.h"

namespace wmp {

enum class OrientMode {
    TravelDirection,  // face the direction of motion: flips 180° when travel reverses (Reverse, ping-pong return leg)
    PathDirection,    // always face the path's own forward direction, whatever the travel direction
};

struct MotionParams {
    // Timing
    double progress = 0.0;
    ProgressSettings progressSettings;
    SpeedMode speedMode = SpeedMode::ConstantSpeed;
    // Transform. Points in N space, scale as a fraction (1 = 100 %), degrees.
    Vec2 pivot{0.5, 0.5};
    Vec2 positionOffset{0.0, 0.0};
    Vec2 scale{1.0, 1.0};
    double rotationDeg = 0.0;
    double opacity = 1.0;  // 0..1
    // Orientation
    bool orientToPath = false;
    double orientOffsetDeg = 0.0;
    OrientMode orientMode = OrientMode::TravelDirection;
    double orientSmoothing = 0.0;  // half window as a fraction of the path length; 0 = off
};

struct Pose {
    Vec2 position;            // N space: where the source pivot lands
    Vec2 pivot{0.5, 0.5};     // N space
    Vec2 scale{1.0, 1.0};
    double rotationDeg = 0.0; // total rotation
    double opacity = 1.0;
    bool onPath = false;      // a path with at least one point was used
    double pathAngleDeg = 0.0;
    bool pathAngleValid = false;
    ProgressState progress;
    double distance = 0.0;    // arc length from the path start, frame heights
    // Changes whenever the pose jumps discontinuously in time (open-path Loop
    // wrap, travel-direction flip). Motion blur never measures or bridges
    // motion across a key change.
    std::int64_t continuityKey = 0;
};

Pose evaluatePose(const PathGeometry* geometry, const MotionParams& p);

// Source canonical -> output canonical for this pose.
Affine poseToCanonical(const Pose& pose, const FrameRect& frame);

}  // namespace wmp
