#include "wmp/retime.h"

#include <cmath>
#include <cstring>

namespace wmp {

std::vector<Keyframe> retimeKeys(const TimingSource& src, double targetDuration, TimingFit mode) {
    if (mode == TimingFit::PreserveTiming) return src.keys;
    if (!(src.duration > 0.0) || !std::isfinite(src.duration) || !(targetDuration > 0.0) ||
        !std::isfinite(targetDuration))
        return {};
    const double k = targetDuration / src.duration;
    std::vector<Keyframe> out = src.keys;
    for (Keyframe& key : out) {
        key.time = key.time * k;
        if (key.hasSlopes) {
            key.slopeIn /= k;
            key.slopeOut /= k;
        }
    }
    return out;
}

namespace {
void mix(std::uint64_t& h, const void* data, std::size_t n) {
    const unsigned char* p = static_cast<const unsigned char*>(data);
    for (std::size_t i = 0; i < n; ++i) {
        h ^= p[i];
        h *= 1099511628211ull;
    }
}
void mixDouble(std::uint64_t& h, double v) {
    if (v == 0.0) v = 0.0;  // fold -0
    std::uint64_t bits;
    std::memcpy(&bits, &v, sizeof bits);
    mix(h, &bits, sizeof bits);
}
}  // namespace

std::uint64_t hashKeys(const std::vector<Keyframe>& keys) {
    std::uint64_t h = 1469598103934665603ull;
    for (const Keyframe& k : keys) {
        mixDouble(h, k.time);
        mixDouble(h, k.value);
        mix(h, k.interpolation.data(), k.interpolation.size());
        const unsigned char flag = k.hasSlopes ? 1 : 0;
        mix(h, &flag, 1);
        if (k.hasSlopes) {
            mixDouble(h, k.slopeIn);
            mixDouble(h, k.slopeOut);
        }
    }
    return h;
}

std::vector<Keyframe> reapplyTiming(TimingMemory& memory, const std::vector<Keyframe>& currentKeys,
                                    double currentDuration, double targetDuration, TimingFit mode) {
    if (memory.appliedHash == 0 || hashKeys(currentKeys) != memory.appliedHash) {
        memory.source.keys = currentKeys;
        memory.source.duration = currentDuration;
    }
    std::vector<Keyframe> out = retimeKeys(memory.source, targetDuration, mode);
    memory.appliedHash = hashKeys(out);
    return out;
}

}  // namespace wmp
