// Wron Motion Path — path data model.
//
// Coordinates are *normalized frame coordinates* ("N space"):
//   (0,0) = top-left corner of the project frame, (1,1) = bottom-right,
//   (0.5,0.5) = frame centre, +X to the right, +Y DOWN.
// Values outside [0,1] are allowed (off-screen path points).
// Handles are stored relative to their anchor point, also in N space.
#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "wmp/json.h"
#include "wmp/vec2.h"

namespace wmp {

// Editing constraint of a point's handles. Geometry always uses the stored
// handle vectors; the mode only tells the editors how to move them.
//   Corner: handles retracted (zero) -> straight segment ends, sharp corner.
//           Pulling a handle out of a corner point turns it into Free.
//   Smooth: in/out handles stay collinear and opposite; lengths independent.
//   Free  : in/out handles are fully independent (broken tangent).
enum class HandleMode { Corner, Smooth, Free };

const char* handleModeName(HandleMode m);
// Unknown names map to Free (keeps geometry, never silently straightens).
HandleMode handleModeFromName(const std::string& name, bool* known = nullptr);

struct PathPoint {
    Vec2 p;         // anchor
    Vec2 in;        // incoming handle, relative to p
    Vec2 out;       // outgoing handle, relative to p
    HandleMode mode = HandleMode::Corner;
    std::string id; // stable editor id ("p1", ...); not used by the renderer
    // Unknown members of the point's JSON object, preserved on rewrite.
    json::Value extra = json::Value::object();
};

struct Path {
    std::vector<PathPoint> points;
    bool closed = false;
};

// Open path with n points has n-1 segments, closed path has n (n >= 1).
inline std::size_t segmentCount(const Path& p) {
    const std::size_t n = p.points.size();
    if (n == 0) return 0;
    return p.closed ? n : n - 1;
}

// Geometry edits used by the OFX "Reverse path" button, presets and tests.
// Reversing swaps in/out handles so the curve shape is unchanged.
Path reversed(const Path& p);

}  // namespace wmp
