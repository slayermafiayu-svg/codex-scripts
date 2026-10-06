#include <clocale>
#include <string>

#include "test.h"
#include "wmp/document.h"
#include "wmp/json.h"

using namespace wmp;

// Kept outside the CHECK macro: MSVC mis-parses raw string literals inside macro arguments.
static const char* kEscapes = R"({"a":[1,2.5,-3e-2,true,false,null],"s":"x\"y\\z\n\u00e7\ud83d\ude00"})";

TEST(json_roundtrip_and_escapes) {
    json::Value v;
    CHECK(json::parse(kEscapes, v));
    CHECK(v.isObject());
    const json::Value* s = v.find("s");
    CHECK(s && s->isString());
    if (s) CHECK(s->asString() == std::string("x\"y\\z\n\xC3\xA7\xF0\x9F\x98\x80"));
    const std::string out = json::serialize(v);
    json::Value w;
    CHECK(json::parse(out, w));
    CHECK(json::serialize(w) == out);
    CHECK_NEAR(v.find("a")->items()[2].asNumber(), -0.03, 0.0);
}

TEST(json_rejects_bad_input) {
    json::Value v;
    CHECK(!json::parse("", v));
    CHECK(!json::parse("{", v));
    CHECK(!json::parse("[1,]", v));
    CHECK(!json::parse("{\"a\":1} x", v));
    CHECK(!json::parse("01", v));
    CHECK(!json::parse("1e999", v));       // overflow -> not finite
    CHECK(!json::parse("NaN", v));
    CHECK(!json::parse("\"\\ud800\"", v)); // unpaired surrogate
    CHECK(!json::parse("\"a\tb\"", v));    // raw control character
    std::string deep(200, '[');
    deep += std::string(200, ']');
    CHECK(!json::parse(deep, v));          // depth limit
    json::ParseLimits small;
    small.maxBytes = 4;
    CHECK(!json::parse("[1,2,3]", v, nullptr, small));
}

TEST(json_numbers_ignore_process_locale) {
    // VEGAS may run with a locale whose decimal separator is ','. Parsing and
    // printing must not change. (If no such locale is installed the check
    // still runs under "C".)
    const char* prev = std::setlocale(LC_ALL, nullptr);
    std::string saved = prev ? prev : "C";
    if (!std::setlocale(LC_ALL, "tr_TR.UTF-8")) std::setlocale(LC_ALL, "de_DE.UTF-8");
    json::Value v;
    CHECK(json::parse("[0.125,1.5e3]", v));
    CHECK_NEAR(v.items()[0].asNumber(), 0.125, 0.0);
    CHECK(json::serialize(v) == "[0.125,1500]");
    std::setlocale(LC_ALL, saved.c_str());
}

static const char* kValidDoc = R"({
  "format": "wron.motionpath", "version": 1,
  "path": { "closed": false, "points": [
      {"id":"p1","p":[0.2,0.5],"in":[0,0],"out":[0.1,-0.2],"mode":"smooth","futureFlag":42},
      {"id":"p2","p":[0.8,0.5],"in":[-0.1,0.2],"out":[0,0],"mode":"free"} ] },
  "editor": {"grid": true, "zoom": 1.5},
  "newerSection": {"x": [1,2,3]}
})";

TEST(document_loads_v1) {
    const LoadResult r = loadDocument(kValidDoc);
    CHECK(r.status == LoadStatus::Ok);
    CHECK(r.sourceVersion == 1);
    CHECK(r.doc.path.points.size() == 2);
    CHECK(!r.doc.path.closed);
    CHECK(r.doc.path.points[0].mode == HandleMode::Smooth);
    CHECK_NEAR(r.doc.path.points[0].out.y, -0.2, 0.0);
    CHECK(r.doc.path.points[1].id == "p2");
}

TEST(document_preserves_unknown_members) {
    const LoadResult r = loadDocument(kValidDoc);
    const std::string saved = saveDocument(r.doc);
    const LoadResult again = loadDocument(saved);
    CHECK(again.status == LoadStatus::Ok);
    const json::Value* editor = again.doc.root.find("editor");
    CHECK(editor && editor->find("zoom") && editor->find("zoom")->asNumber() == 1.5);
    CHECK(again.doc.root.find("newerSection") != nullptr);
    const json::Value* ff = again.doc.path.points[0].extra.find("futureFlag");
    CHECK(ff && ff->asNumber() == 42);
    // Saving twice is stable (no drift).
    CHECK(saveDocument(again.doc) == saved);
}

TEST(document_handles_empty_and_corrupt) {
    CHECK(loadDocument("").status == LoadStatus::Empty);
    CHECK(loadDocument("  \n").status == LoadStatus::Empty);
    CHECK(loadDocument("not json").status == LoadStatus::Invalid);
    CHECK(loadDocument("[]").status == LoadStatus::Invalid);
    CHECK(loadDocument(R"({"format":"other","version":1,"path":{}})").status == LoadStatus::Invalid);
    CHECK(loadDocument(R"({"format":"wron.motionpath","path":{}})").status == LoadStatus::Invalid);
    CHECK(loadDocument(R"({"format":"wron.motionpath","version":1.5,"path":{}})").status == LoadStatus::Invalid);
    CHECK(loadDocument(R"({"format":"wron.motionpath","version":1})").status == LoadStatus::Invalid);
    // Truncated (e.g. damaged project file).
    const std::string full = kValidDoc;
    CHECK(loadDocument(full.substr(0, full.size() / 2)).status == LoadStatus::Invalid);
    // Point problems.
    CHECK(loadDocument(R"({"format":"wron.motionpath","version":1,"path":{"points":[{"in":[0,0]}]}})").status ==
          LoadStatus::Invalid);
    CHECK(loadDocument(R"({"format":"wron.motionpath","version":1,"path":{"points":[{"p":[0.5]}]}})").status ==
          LoadStatus::Invalid);
    CHECK(loadDocument(R"({"format":"wron.motionpath","version":1,"path":{"points":[{"p":[1e9,0]}]}})").status ==
          LoadStatus::Invalid);
    CHECK(loadDocument(R"({"format":"wron.motionpath","version":1,"path":{"closed":"yes"}})").status ==
          LoadStatus::Invalid);
    // Empty path is valid (renders unchanged).
    const LoadResult e = loadDocument(R"({"format":"wron.motionpath","version":1,"path":{"points":[]}})");
    CHECK(e.status == LoadStatus::Ok && e.doc.path.points.empty());
}

TEST(document_future_version_is_not_interpreted) {
    const LoadResult r = loadDocument(R"({"format":"wron.motionpath","version":7,"path":{"points":[]}})");
    CHECK(r.status == LoadStatus::UnsupportedFutureVersion);
    CHECK(!r.usable());
    CHECK(r.sourceVersion == 7);
}

TEST(document_unknown_mode_and_defaults) {
    const LoadResult r = loadDocument(
        R"({"format":"wron.motionpath","version":1,"path":{"points":[{"p":[0,0],"mode":"bogus"},{"p":[1,1]},{"p":[2,2],"out":[1,0]}]}})");
    CHECK(r.status == LoadStatus::Ok);
    CHECK(r.warnings.size() == 1);
    CHECK(r.doc.path.points[0].mode == HandleMode::Free);
    CHECK(r.doc.path.points[1].mode == HandleMode::Corner);  // no handles -> corner
    CHECK(r.doc.path.points[2].mode == HandleMode::Free);    // handles but no mode -> free (shape kept)
}

TEST(document_migration_chain) {
    // Test-only table: pretend a "version 0" stored bare points under "pts"
    // as {"x":..,"y":..}. Proves the chain mechanism; v1 is the first real format.
    MigrationTable table;
    table.currentVersion = 1;
    table.steps.push_back({0, [](json::Value& root, std::string& err) {
                               const json::Value* pts = root.find("pts");
                               if (!pts || !pts->isArray()) {
                                   err = "missing pts";
                                   return false;
                               }
                               json::Value path = json::Value::object();
                               json::Value arr = json::Value::array();
                               for (const auto& q : pts->items()) {
                                   json::Value o = json::Value::object();
                                   json::Value p = json::Value::array();
                                   p.push(*q.find("x"));
                                   p.push(*q.find("y"));
                                   o.set("p", p);
                                   arr.push(o);
                               }
                               path.set("points", arr);
                               root.set("path", path);
                               root.erase("pts");
                               return true;
                           }});
    const LoadResult r =
        loadDocument(R"({"format":"wron.motionpath","version":0,"pts":[{"x":0.1,"y":0.2},{"x":0.9,"y":0.8}]})", table);
    CHECK(r.status == LoadStatus::Migrated);
    CHECK(r.sourceVersion == 0);
    CHECK(r.doc.path.points.size() == 2);
    CHECK_NEAR(r.doc.path.points[1].p.x, 0.9, 0.0);
    // Missing step -> invalid, never a half-migrated document.
    MigrationTable noSteps;
    noSteps.currentVersion = 1;
    CHECK(loadDocument(R"({"format":"wron.motionpath","version":0,"pts":[]})", noSteps).status ==
          LoadStatus::Invalid);
    // Failing step -> invalid.
    CHECK(loadDocument(R"({"format":"wron.motionpath","version":0})", table).status == LoadStatus::Invalid);
}

TEST(path_reverse_keeps_shape_and_closed_start) {
    Path p;
    p.points = {{{0, 0}, {0, 0}, {1, 0}, HandleMode::Free, "a", json::Value::object()},
                {{2, 0}, {-1, 1}, {1, -1}, HandleMode::Smooth, "b", json::Value::object()},
                {{4, 0}, {-1, 0}, {0, 0}, HandleMode::Free, "c", json::Value::object()}};
    const Path r = reversed(p);
    CHECK(r.points.size() == 3);
    CHECK(r.points[0].id == "c" && r.points[2].id == "a");
    CHECK(r.points[1].in == Vec2(1, -1) && r.points[1].out == Vec2(-1, 1));
    p.closed = true;
    const Path rc = reversed(p);
    CHECK(rc.points[0].id == "a" && rc.points[1].id == "c" && rc.points[2].id == "b");
    const Path back = reversed(reversed(p));
    for (std::size_t i = 0; i < 3; ++i) {
        CHECK(back.points[i].id == p.points[i].id);
        CHECK(back.points[i].in == p.points[i].in && back.points[i].out == p.points[i].out);
    }
}
