// Wron Motion Path — path editing operations shared by every editor
// (OFX overlay in C++, web path editor in JS; both follow these rules).
//
// Positions are given in N space (normalized frame coords). Angles and
// lengths that the user perceives (smooth-handle collinearity, hit radii) are
// evaluated in D space via the frame aspect, so editing behaves the same at
// any frame shape.
#pragma once

#include <string>

#include "wmp/path.h"

namespace wmp::edit {

enum class HitKind { None, Anchor, InHandle, OutHandle, Segment };

struct Hit {
    HitKind kind = HitKind::None;
    int index = -1;      // point index (anchor/handle) or segment index
    double t = 0.0;      // Bézier parameter on the segment
    double distance = 0; // D units
};

// Anchors win over handles, handles over segments. Handles are only
// hit-tested for `selected` (and only if they are not retracted).
Hit hitTest(const Path& path, double aspect, Vec2 posN, double tolerance, int selected);

// Splits segment `segment` at t with de Casteljau: the curve shape is exactly
// unchanged. Returns the index of the new point.
int insertPoint(Path& path, int segment, double t);

// Removes a point. Open paths keep the remaining segments; closed paths with
// fewer than 2 points become open.
void deletePoint(Path& path, int index);

void moveAnchor(Path& path, int index, Vec2 newPosN);

// Moves one handle to an absolute N position.
//  * Smooth: the opposite handle turns to stay collinear, keeping its length.
//  * Corner: becomes Free (pulling a handle out of a corner).
//  * breakTangent forces Free (independent handles).
void moveHandle(Path& path, int index, bool outHandle, Vec2 handlePosN, double aspect, bool breakTangent);

// Corner: retracts both handles. Smooth: aligns the handles (creating them
// from the neighbours if both were retracted). Free: geometry unchanged.
void setMode(Path& path, int index, HandleMode mode, double aspect);

// Rigid/uniform edits of the whole path around `pivotN` (D-space metric, so a
// rotation does not shear the path on non-square frames).
void transformPath(Path& path, double aspect, Vec2 pivotN, double scale, double rotationDeg, Vec2 translateN);

std::string nextPointId(const Path& path);

}  // namespace wmp::edit
