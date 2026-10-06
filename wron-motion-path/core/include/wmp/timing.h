// Wron Motion Path — Progress -> position-along-path mapping.
//
// Path (geometry) and Progress (timing) are independent:
//   * The host animates the "Progress" parameter (its keyframes and their
//     interpolation are the time curve).
//   * An optional easing curve reshapes progress per unit lap.
//   * Progress is converted to a fraction u of the path *by arc length*, so
//     linear progress means constant on-screen speed whatever the Bézier
//     parameterization looks like.
// Everything here is a pure function of the parameter values at one time:
// no frame-to-frame state, so scrubbing to any time gives the same result.
#pragma once

#include <cstdint>

namespace wmp {

// What happens when progress leaves [0,1] on an OPEN path. Closed paths
// always loop seamlessly and ignore this setting.
enum class EndBehavior {
    Clamp,     // stop at the end points
    Extend,    // continue in a straight line along the end tangent (overshoot easing)
    Loop,      // jump back to the start after the end (position discontinuity)
    PingPong,  // bounce back and forth, continuous
};

// CSS-style cubic-bezier timing function from (0,0) to (1,1). x1 and x2 are
// clamped to [0,1] so the curve is a function of x; y1/y2 are free, so
// curves may overshoot (back/elastic-like easing).
struct CubicEasing {
    double x1 = 0.0, y1 = 0.0, x2 = 1.0, y2 = 1.0;
    bool isLinear() const { return x1 == y1 && x2 == y2; }
    double eval(double x) const;  // x clamped to [0,1]
};

enum class EasingPreset { Linear, EaseIn, EaseOut, EaseInOut, BackOut, Custom };
CubicEasing easingFor(EasingPreset preset, const CubicEasing& custom);

// Easing applied per unit lap: floor(p) + E(frac(p)). Continuous because
// E(0)=0 and E(1)=1, so multi-lap progress (0..3) eases every lap.
double lapEase(double progress, const CubicEasing& e);

struct ProgressSettings {
    double startOffset = 0.0;  // in progress units, added after easing
    bool reverse = false;      // traverse from the path end towards the start
    EndBehavior endBehavior = EndBehavior::Clamp;
    CubicEasing easing;        // identity by default (constant speed)
};

struct ProgressState {
    double u = 0.0;          // fraction of the path: 0 = start, 1 = end (outside only with Extend)
    int direction = 1;       // +1 if increasing Progress moves toward the path end, -1 toward the start
    std::int64_t lap = 0;    // open-path Loop: lap index (position jumps when it changes); 0 otherwise
    bool clamped = false;    // Clamp mode held the position at an end point
};

// Order: e = lapEase(progress); q = e + startOffset; q = reverse ? 1 - q : q;
// then wrap by end behaviour (closed paths: seamless modulo).
// Open Loop maps whole numbers > 0 to the END of the path (progress 1 ends at
// the end, 1+epsilon restarts at the beginning).
ProgressState mapProgress(double progress, bool closedPath, const ProgressSettings& s);

}  // namespace wmp
