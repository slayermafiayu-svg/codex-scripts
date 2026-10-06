#include "wmp/document.h"

#include <cmath>
#include <string>

namespace wmp {

const char* loadStatusName(LoadStatus s) {
    switch (s) {
        case LoadStatus::Ok: return "ok";
        case LoadStatus::Migrated: return "migrated";
        case LoadStatus::Empty: return "empty";
        case LoadStatus::Invalid: return "invalid";
        case LoadStatus::UnsupportedFutureVersion: return "unsupported-future-version";
    }
    return "invalid";
}

const MigrationTable& defaultMigrations() {
    static const MigrationTable table{kDocumentVersion, {}};
    return table;
}

namespace {

bool readVec2(const json::Value& v, Vec2& out) {
    if (!v.isArray() || v.items().size() != 2) return false;
    const auto& a = v.items()[0];
    const auto& b = v.items()[1];
    if (!a.isNumber() || !b.isNumber()) return false;
    out = {a.asNumber(), b.asNumber()};
    return isFinite(out) && std::fabs(out.x) <= kMaxCoordinate && std::fabs(out.y) <= kMaxCoordinate;
}

json::Value writeVec2(const Vec2& v) {
    json::Value a = json::Value::array();
    a.push(json::Value::number(v.x));
    a.push(json::Value::number(v.y));
    return a;
}

bool isBlank(std::string_view s) {
    for (char c : s)
        if (c != ' ' && c != '\t' && c != '\r' && c != '\n') return false;
    return true;
}

bool parsePath(const json::Value& pv, Path& path, std::vector<std::string>& warnings, std::string& error) {
    if (!pv.isObject()) {
        error = "\"path\" must be an object";
        return false;
    }
    path = Path();
    if (const json::Value* c = pv.find("closed")) {
        if (!c->isBool()) {
            error = "\"path.closed\" must be a boolean";
            return false;
        }
        path.closed = c->asBool();
    }
    const json::Value* pts = pv.find("points");
    if (!pts) return true;  // no points: empty path
    if (!pts->isArray()) {
        error = "\"path.points\" must be an array";
        return false;
    }
    if (pts->items().size() > kMaxPathPoints) {
        error = "too many path points";
        return false;
    }
    std::size_t index = 0;
    for (const json::Value& item : pts->items()) {
        const std::string where = "point " + std::to_string(index);
        if (!item.isObject()) {
            error = where + " is not an object";
            return false;
        }
        PathPoint pt;
        bool hasMode = false;
        for (const auto& m : item.members()) {
            const std::string& key = m.first;
            if (key == "p") {
                if (!readVec2(m.second, pt.p)) {
                    error = where + ": invalid \"p\"";
                    return false;
                }
            } else if (key == "in") {
                if (!readVec2(m.second, pt.in)) {
                    error = where + ": invalid \"in\"";
                    return false;
                }
            } else if (key == "out") {
                if (!readVec2(m.second, pt.out)) {
                    error = where + ": invalid \"out\"";
                    return false;
                }
            } else if (key == "mode") {
                if (!m.second.isString()) {
                    error = where + ": \"mode\" must be a string";
                    return false;
                }
                bool known = true;
                pt.mode = handleModeFromName(m.second.asString(), &known);
                if (!known) warnings.push_back(where + ": unknown mode \"" + m.second.asString() + "\", using free");
                hasMode = true;
            } else if (key == "id") {
                if (m.second.isString()) pt.id = m.second.asString();
            } else {
                pt.extra.set(key, m.second);
            }
        }
        if (!item.find("p")) {
            error = where + ": missing \"p\"";
            return false;
        }
        if (!hasMode) {
            const bool zero = pt.in == Vec2{} && pt.out == Vec2{};
            pt.mode = zero ? HandleMode::Corner : HandleMode::Free;
        }
        path.points.push_back(std::move(pt));
        ++index;
    }
    return true;
}

json::Value writePath(const Path& path) {
    json::Value pv = json::Value::object();
    pv.set("closed", json::Value::boolean(path.closed));
    json::Value pts = json::Value::array();
    for (const PathPoint& pt : path.points) {
        json::Value o = json::Value::object();
        if (!pt.id.empty()) o.set("id", json::Value::string(pt.id));
        o.set("p", writeVec2(pt.p));
        o.set("in", writeVec2(pt.in));
        o.set("out", writeVec2(pt.out));
        o.set("mode", json::Value::string(handleModeName(pt.mode)));
        for (const auto& m : pt.extra.members())
            if (!o.find(m.first)) o.set(m.first, m.second);
        pts.push(std::move(o));
    }
    pv.set("points", std::move(pts));
    return pv;
}

}  // namespace

LoadResult loadDocument(std::string_view text, const MigrationTable& table) {
    LoadResult r;
    if (isBlank(text)) {
        r.status = LoadStatus::Empty;
        return r;
    }
    json::Value root;
    json::ParseError perr;
    if (!json::parse(text, root, &perr)) {
        r.status = LoadStatus::Invalid;
        r.message = "JSON error at byte " + std::to_string(perr.offset) + ": " + perr.message;
        return r;
    }
    if (!root.isObject()) {
        r.status = LoadStatus::Invalid;
        r.message = "document is not a JSON object";
        return r;
    }
    const json::Value* fmt = root.find("format");
    if (!fmt || !fmt->isString() || fmt->asString() != kDocumentFormat) {
        r.status = LoadStatus::Invalid;
        r.message = "not a Wron motion path document";
        return r;
    }
    const json::Value* ver = root.find("version");
    if (!ver || !ver->isNumber() || ver->asNumber() != std::floor(ver->asNumber()) || ver->asNumber() < 0 ||
        ver->asNumber() > 1e6) {
        r.status = LoadStatus::Invalid;
        r.message = "missing or invalid \"version\"";
        return r;
    }
    int version = static_cast<int>(ver->asNumber());
    r.sourceVersion = version;
    if (version > table.currentVersion) {
        r.status = LoadStatus::UnsupportedFutureVersion;
        r.message = "document version " + std::to_string(version) + " is newer than supported version " +
                    std::to_string(table.currentVersion);
        return r;
    }
    while (version < table.currentVersion) {
        const Migration* step = nullptr;
        for (const Migration& m : table.steps)
            if (m.from == version) step = &m;
        if (!step || !step->apply) {
            r.status = LoadStatus::Invalid;
            r.message = "no migration from version " + std::to_string(version);
            return r;
        }
        std::string err;
        if (!step->apply(root, err)) {
            r.status = LoadStatus::Invalid;
            r.message = "migration from version " + std::to_string(version) + " failed: " + err;
            return r;
        }
        ++version;
        root.set("version", json::Value::number(version));
    }
    const json::Value* pv = root.find("path");
    if (!pv) {
        r.status = LoadStatus::Invalid;
        r.message = "missing \"path\"";
        return r;
    }
    std::string err;
    if (!parsePath(*pv, r.doc.path, r.warnings, err)) {
        r.status = LoadStatus::Invalid;
        r.message = err;
        r.doc = PathDocument();
        return r;
    }
    r.doc.root = std::move(root);
    r.status = (r.sourceVersion == table.currentVersion) ? LoadStatus::Ok : LoadStatus::Migrated;
    if (r.status == LoadStatus::Migrated)
        r.message = "migrated from version " + std::to_string(r.sourceVersion);
    return r;
}

std::string saveDocument(const PathDocument& doc) {
    json::Value root = doc.root.isObject() ? doc.root : json::Value::object();
    root.set("format", json::Value::string(kDocumentFormat));
    root.set("version", json::Value::number(kDocumentVersion));
    root.set("path", writePath(doc.path));
    return json::serialize(root);
}

PathDocument makeDocument(const Path& path) {
    PathDocument d;
    d.root = json::Value::object();
    d.root.set("format", json::Value::string(kDocumentFormat));
    d.root.set("version", json::Value::number(kDocumentVersion));
    d.path = path;
    return d;
}

}  // namespace wmp
