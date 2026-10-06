// Wron Motion Path — versioned path document (stored in the OFX string
// parameter "wmpPathData", one per effect instance).
//
// Format v1:
//   {
//     "format": "wron.motionpath",
//     "version": 1,
//     "path": { "closed": false,
//               "points": [ {"id":"p1","p":[x,y],"in":[dx,dy],"out":[dx,dy],"mode":"smooth"}, ... ] },
//     "editor": { ... }            // optional, editor-only state, preserved verbatim
//   }
// Unknown top-level members and unknown point members are preserved when the
// document is rewritten (e.g. by the "Reverse path" button), so newer editors
// can store data that older plugins do not understand.
#pragma once

#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "wmp/json.h"
#include "wmp/path.h"

namespace wmp {

inline constexpr const char* kDocumentFormat = "wron.motionpath";
inline constexpr int kDocumentVersion = 1;
inline constexpr std::size_t kMaxPathPoints = 4096;
// Sanity bound for coordinates (normalized units). 1e4 frames off-screen is
// already meaningless; larger values are treated as corrupt data.
inline constexpr double kMaxCoordinate = 1.0e4;

enum class LoadStatus {
    Ok,                        // current version, loaded
    Migrated,                  // older version, migrated in memory (not written back)
    Empty,                     // empty string: no path stored yet
    Invalid,                   // malformed / corrupt / failed validation
    UnsupportedFutureVersion,  // written by a newer plugin; not interpreted
};

const char* loadStatusName(LoadStatus s);

struct PathDocument {
    Path path;
    // The full JSON object as loaded (after migration). Members other than
    // "format", "version" and "path" are written back unchanged by save().
    json::Value root = json::Value::object();
};

struct LoadResult {
    LoadStatus status = LoadStatus::Invalid;
    int sourceVersion = 0;
    std::string message;   // human-readable reason for Invalid / future / migrated
    std::vector<std::string> warnings;
    PathDocument doc;
    bool usable() const { return status == LoadStatus::Ok || status == LoadStatus::Migrated; }
};

// A migration transforms the JSON tree of version `from` into `from + 1`.
struct Migration {
    int from = 0;
    std::function<bool(json::Value& root, std::string& error)> apply;
};

struct MigrationTable {
    int currentVersion = kDocumentVersion;
    std::vector<Migration> steps;  // must cover every version in [oldest, current)
};

// v1 is the first released format, so the production table has no steps yet.
const MigrationTable& defaultMigrations();

LoadResult loadDocument(std::string_view text, const MigrationTable& table = defaultMigrations());

// Always writes the current version.
std::string saveDocument(const PathDocument& doc);

// Convenience: wrap a bare path in a new document.
PathDocument makeDocument(const Path& path);

}  // namespace wmp
