// Wron Motion Path — per-instance geometry snapshot cache.
//
// One cache lives in each effect instance (never global). Render threads call
// get() with the current path-data string and frame aspect; the string is
// parsed and the arc-length tables are built only when either changes. The
// result is an immutable shared snapshot, so a render thread keeps a
// consistent geometry even if the UI thread edits the path meanwhile.
#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>

#include "wmp/document.h"
#include "wmp/geometry.h"

namespace wmp {

struct GeometrySnapshot {
    std::shared_ptr<const PathGeometry> geometry;  // null unless status is usable
    LoadStatus status = LoadStatus::Empty;
    std::string message;
    std::size_t pointCount = 0;
    bool closed = false;
};

class GeometryCache {
public:
    std::shared_ptr<const GeometrySnapshot> get(std::string_view pathData, double aspect);
    void clear();
    std::uint64_t buildCount() const;

private:
    mutable std::mutex mutex_;
    std::string data_;
    double aspect_ = -1.0;
    std::shared_ptr<const GeometrySnapshot> snapshot_;
    std::uint64_t builds_ = 0;
};

std::shared_ptr<const GeometrySnapshot> buildSnapshot(std::string_view pathData, double aspect);

}  // namespace wmp
