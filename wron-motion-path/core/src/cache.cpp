#include "wmp/cache.h"

namespace wmp {

std::shared_ptr<const GeometrySnapshot> buildSnapshot(std::string_view pathData, double aspect) {
    auto snap = std::make_shared<GeometrySnapshot>();
    LoadResult r = loadDocument(pathData);
    snap->status = r.status;
    snap->message = r.message;
    if (r.usable()) {
        snap->pointCount = r.doc.path.points.size();
        snap->closed = r.doc.path.closed;
        PathGeometry::Options opts;
        opts.aspect = aspect;
        snap->geometry = std::make_shared<const PathGeometry>(r.doc.path, opts);
    }
    return snap;
}

std::shared_ptr<const GeometrySnapshot> GeometryCache::get(std::string_view pathData, double aspect) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (snapshot_ && aspect == aspect_ && pathData == data_) return snapshot_;
    snapshot_ = buildSnapshot(pathData, aspect);
    data_.assign(pathData.data(), pathData.size());
    aspect_ = aspect;
    ++builds_;
    return snapshot_;
}

void GeometryCache::clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    snapshot_.reset();
    data_.clear();
    aspect_ = -1.0;
}

std::uint64_t GeometryCache::buildCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return builds_;
}

}  // namespace wmp
