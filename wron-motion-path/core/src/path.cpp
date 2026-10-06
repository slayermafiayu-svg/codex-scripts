#include "wmp/path.h"

#include <algorithm>
#include <utility>

namespace wmp {

const char* handleModeName(HandleMode m) {
    switch (m) {
        case HandleMode::Corner: return "corner";
        case HandleMode::Smooth: return "smooth";
        case HandleMode::Free: return "free";
    }
    return "free";
}

HandleMode handleModeFromName(const std::string& name, bool* known) {
    if (known) *known = true;
    if (name == "corner") return HandleMode::Corner;
    if (name == "smooth") return HandleMode::Smooth;
    if (name == "free") return HandleMode::Free;
    if (known) *known = false;
    return HandleMode::Free;
}

Path reversed(const Path& p) {
    Path r;
    r.closed = p.closed;
    const std::size_t n = p.points.size();
    r.points.reserve(n);
    auto flip = [](PathPoint q) {
        std::swap(q.in, q.out);
        return q;
    };
    if (n == 0) return r;
    if (p.closed) {
        // Keep the start point so a closed loop still begins where it did:
        // P0, Pn-1, ..., P1.
        r.points.push_back(flip(p.points[0]));
        for (std::size_t i = n - 1; i >= 1; --i) r.points.push_back(flip(p.points[i]));
    } else {
        for (std::size_t i = n; i-- > 0;) r.points.push_back(flip(p.points[i]));
    }
    return r;
}

}  // namespace wmp
