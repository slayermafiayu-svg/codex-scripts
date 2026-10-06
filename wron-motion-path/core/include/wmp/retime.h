// Wron Motion Path — keyframe timing adaptation for timing presets.
//
// A timing preset stores the *source* timing: the source event duration and
// the keyframes relative to the source event start (seconds). Applying it to
// a target event is always computed from that source, never from previously
// adapted keys, so applying it again and again cannot drift.
//   FitToEvent     t' = t * (targetDuration / sourceDuration)
//                  (keys before 0 or after the source end keep their relative
//                  position: they are scaled, never clamped into the event;
//                  host slopes in value/second are divided by the same factor)
//   PreserveTiming t' = t   (original duration and speed)
// Frame quantization is deliberately not applied here.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace wmp {

enum class TimingFit { FitToEvent, PreserveTiming };

struct Keyframe {
    double time = 0.0;           // seconds from the event start
    double value = 0.0;
    std::string interpolation;   // host curve type, e.g. "linear", "smooth", "hold", "manual", "split"
    bool hasSlopes = false;      // host "manual"/"split" slopes, value per second
    double slopeIn = 0.0, slopeOut = 0.0;
};

struct TimingSource {
    double duration = 0.0;       // seconds, > 0
    std::vector<Keyframe> keys;
};

// Returns an empty vector if the source is unusable (duration <= 0 or a
// non-finite value) and the fit needs the duration.
std::vector<Keyframe> retimeKeys(const TimingSource& src, double targetDuration, TimingFit mode);

std::uint64_t hashKeys(const std::vector<Keyframe>& keys);

// Re-apply logic for a document that remembers the source timing and the
// hash of what was last written. If the current keys still match that hash
// the source is reused; if the user edited the keys since, the current keys
// become the new source (re-based at the current duration).
struct TimingMemory {
    TimingSource source;
    std::uint64_t appliedHash = 0;
};

std::vector<Keyframe> reapplyTiming(TimingMemory& memory, const std::vector<Keyframe>& currentKeys,
                                    double currentDuration, double targetDuration, TimingFit mode);

}  // namespace wmp
