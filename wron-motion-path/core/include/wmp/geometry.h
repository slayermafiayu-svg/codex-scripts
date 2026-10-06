// Wron Motion Path — evaluated path geometry (immutable snapshot).
//
// The path is evaluated in *display space* ("D space"): x_D = u * aspect,
// y_D = v, where aspect = frame display width / height (pixel aspect ratio
// already applied). D space is isotropic and measured in frame heights, so
// arc length, constant speed and tangent angles match what the viewer sees
// at any resolution, render scale or PAR. A PathGeometry is built once per
// (path data, aspect) pair and shared read-only between render threads.
#pragma once

#include <memory>
#include <vector>

#include "wmp/arclength.h"
#include "wmp/path.h"

namespace wmp {

enum class SpeedMode {
    ConstantSpeed,        // progress is proportional to arc length (default)
    EqualTimePerSegment,  // every segment gets the same progress span; constant speed within a segment
};

struct PathSample {
    Vec2 pos;              // D space
    Vec2 tangent;          // unit forward tangent (direction of the path, not of travel), D space
    bool tangentValid = false;
    int segment = -1;
    double t = 0.0;        // Bézier parameter inside the segment
};

class PathGeometry {
public:
    struct Options {
        double aspect = 16.0 / 9.0;
        // Arc-length error bound in frame heights. 1e-7 is ~0.0002 px at 2160p.
        double tolerance = 1e-7;
    };

    PathGeometry(const Path& path, const Options& opts);

    bool empty() const { return points_ == 0; }
    std::size_t pointCount() const { return points_; }
    bool closed() const { return closed_; }
    double aspect() const { return aspect_; }
    double length() const { return cum_.empty() ? 0.0 : cum_.back(); }
    int segmentCount() const { return static_cast<int>(segs_.size()); }
    double segmentStart(int i) const { return cum_[static_cast<std::size_t>(i)]; }
    double segmentLength(int i) const { return tables_[static_cast<std::size_t>(i)].length(); }
    const Cubic& segment(int i) const { return segs_[static_cast<std::size_t>(i)]; }
    const ArcLengthTable& table(int i) const { return tables_[static_cast<std::size_t>(i)]; }

    Vec2 toDisplay(const Vec2& n) const { return {n.x * aspect_, n.y}; }
    Vec2 toNormalized(const Vec2& d) const { return {d.x / aspect_, d.y}; }

    // Position and forward tangent at arc length d.
    //  * bias +1 selects the segment that starts at a joint, -1 the one that
    //    ends there (corners switch direction exactly at the joint, on the
    //    side the motion is heading to).
    //  * Open paths: d outside [0, L] is clamped, or continued in a straight
    //    line along the end tangent when `extend` is true.
    //  * Closed paths: d is wrapped modulo L.
    PathSample sampleAtDistance(double d, int bias, bool extend) const;

    // Unit progress u (0 = start, 1 = end) -> arc length. Values outside
    // [0,1] extrapolate (used by the Extend end behaviour).
    double distanceForU(double u, SpeedMode mode) const;

    // Tangent averaged over [d - sigma, d + sigma] with a triangular kernel.
    // Because the tangent is the derivative of position, the weighted average
    // equals (mean position of the right half) - (mean position of the left
    // half): evaluated from positions only, continuous in d, independent of
    // time or previously rendered frames. Returns false if undefined.
    bool smoothedTangent(double d, double sigma, int bias, Vec2& out) const;

private:
    bool tangentAt(int seg, double t, int bias, Vec2& out) const;
    bool neighbourTangent(int seg, int bias, Vec2& out) const;
    double wrap(double d) const;

    std::size_t points_ = 0;
    bool closed_ = false;
    double aspect_ = 1.0;
    Vec2 firstPoint_;
    std::vector<Cubic> segs_;
    std::vector<ArcLengthTable> tables_;
    std::vector<double> cum_;  // cum_[i] = arc length at the start of segment i, back() = total
};

using GeometryPtr = std::shared_ptr<const PathGeometry>;

}  // namespace wmp
