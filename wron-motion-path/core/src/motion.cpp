#include "wmp/motion.h"

#include <algorithm>
#include <cmath>

namespace wmp {

Pose evaluatePose(const PathGeometry* g, const MotionParams& p) {
    Pose pose;
    pose.pivot = p.pivot;
    pose.scale = p.scale;
    pose.opacity = std::isfinite(p.opacity) ? std::clamp(p.opacity, 0.0, 1.0) : 1.0;
    Vec2 pathPoint = p.pivot;
    double pathAngle = 0.0;
    bool angleValid = false;
    if (g && !g->empty()) {
        pose.onPath = true;
        const ProgressState st = mapProgress(p.progress, g->closed(), p.progressSettings);
        pose.progress = st;
        const bool extend = !g->closed() && p.progressSettings.endBehavior == EndBehavior::Extend;
        const double d = g->distanceForU(st.u, p.speedMode);
        pose.distance = d;
        const int bias = st.direction;
        const PathSample s = g->sampleAtDistance(d, bias, extend);
        pathPoint = g->toNormalized(s.pos);
        if (p.orientToPath) {
            Vec2 dir = s.tangent;
            bool ok = s.tangentValid;
            if (p.orientSmoothing > 0.0 && std::isfinite(p.orientSmoothing))
                ok = g->smoothedTangent(d, p.orientSmoothing * g->length(), bias, dir);
            if (ok) {
                if (p.orientMode == OrientMode::TravelDirection && st.direction < 0) dir = -dir;
                pathAngle = radToDeg(std::atan2(dir.y, dir.x));
                angleValid = true;
            }
        }
        const bool openLoop = !g->closed() && p.progressSettings.endBehavior == EndBehavior::Loop;
        const bool flipMatters = p.orientToPath && p.orientMode == OrientMode::TravelDirection;
        pose.continuityKey = (openLoop ? st.lap : 0) * 2 + ((flipMatters && st.direction < 0) ? 1 : 0);
    }
    pose.position = pathPoint + p.positionOffset;
    pose.pathAngleDeg = pathAngle;
    pose.pathAngleValid = angleValid;
    pose.rotationDeg = p.rotationDeg + (p.orientToPath ? pathAngle + p.orientOffsetDeg : 0.0);
    return pose;
}

Affine poseToCanonical(const Pose& pose, const FrameRect& frame) {
    const double A = frame.aspect();
    const Vec2 P{pose.position.x * A, pose.position.y};
    const Vec2 Q{pose.pivot.x * A, pose.pivot.y};
    const Affine td = Affine::translate(P) * Affine::rotate(degToRad(pose.rotationDeg)) *
                      Affine::scale(pose.scale.x, pose.scale.y) * Affine::translate(-Q);
    return displayToCanonical(frame) * td * canonicalToDisplay(frame);
}

}  // namespace wmp
