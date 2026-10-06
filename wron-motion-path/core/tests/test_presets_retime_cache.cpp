#include <atomic>
#include <cmath>
#include <set>
#include <thread>
#include <vector>

#include "test.h"
#include "wmp/cache.h"
#include "wmp/document.h"
#include "wmp/presets.h"
#include "wmp/retime.h"

using namespace wmp;

TEST(presets_are_editable_paths) {
    for (int i = 0; i < kPresetCount; ++i) {
        const PresetId id = static_cast<PresetId>(i);
        const Path p = makePreset(id, 16.0 / 9.0);
        CHECK(p.points.size() >= 2);
        std::set<std::string> ids;
        for (const PathPoint& q : p.points) {
            ids.insert(q.id);
            CHECK(isFinite(q.p) && isFinite(q.in) && isFinite(q.out));
        }
        CHECK(ids.size() == p.points.size());
        PresetId back;
        CHECK(presetFromKey(presetKey(id), back) && back == id);
        // Round trip through the stored document format.
        const LoadResult r = loadDocument(saveDocument(makeDocument(p)));
        CHECK(r.status == LoadStatus::Ok);
        CHECK(r.doc.path.points.size() == p.points.size());
        PathGeometry g(r.doc.path, {16.0 / 9.0, 1e-9});
        CHECK(g.length() > 0.1);
    }
}

TEST(circle_preset_is_round_on_screen) {
    for (double aspect : {16.0 / 9.0, 1.0, 9.0 / 16.0}) {
        const Path p = makePreset(PresetId::Circle, aspect);
        CHECK(p.closed);
        PathGeometry g(p, {aspect, 1e-10});
        CHECK_NEAR(g.length(), 2.0 * kPi * 0.3, 2.0 * kPi * 0.3 * 1e-3);
        const Vec2 c{0.5 * aspect, 0.5};
        double minR = 1e9, maxR = 0;
        for (int i = 0; i < 200; ++i) {
            const double r = length(g.sampleAtDistance(g.length() * i / 200.0, 1, false).pos - c);
            minR = std::min(minR, r);
            maxR = std::max(maxR, r);
        }
        CHECK(maxR - minR < 0.3 * 0.001);  // cubic circle approximation error ~0.03 %
    }
}

TEST(retime_fit_and_preserve) {
    TimingSource src;
    src.duration = 4.0;
    src.keys = {{-1.0, 0.0, "linear"}, {0.0, 0.0, "smooth"}, {2.0, 0.5, "manual", true, 0.2, 0.3},
                {4.0, 1.0, "linear"}, {6.0, 1.5, "hold"}};
    const auto fit = retimeKeys(src, 8.0, TimingFit::FitToEvent);
    CHECK(fit.size() == 5);
    CHECK_NEAR(fit[0].time, -2.0, 0.0);  // outside keys keep relative position (not clamped)
    CHECK_NEAR(fit[2].time, 4.0, 0.0);
    CHECK_NEAR(fit[2].slopeIn, 0.1, 1e-15);
    CHECK_NEAR(fit[4].time, 12.0, 0.0);
    CHECK(fit[4].interpolation == "hold");
    CHECK_NEAR(fit[2].value, 0.5, 0.0);  // values untouched
    const auto keep = retimeKeys(src, 8.0, TimingFit::PreserveTiming);
    CHECK_NEAR(keep[3].time, 4.0, 0.0);
    TimingSource bad;
    bad.duration = 0.0;
    bad.keys = src.keys;
    CHECK(retimeKeys(bad, 5.0, TimingFit::FitToEvent).empty());
}

TEST(retime_repeated_application_does_not_drift) {
    TimingMemory mem;
    std::vector<Keyframe> keys = {{0.0, 0.0, "linear"}, {1.0 / 3.0, 0.3, "smooth"}, {2.9, 1.0, "linear"}};
    double duration = 2.9;
    const double durations[] = {7.1, 1.3, 23.976 / 7.0, 100.0, 0.01, 2.9};
    for (int round = 0; round < 498; ++round) {  // ends on durations[5] == 2.9
        const double target = durations[round % 6];
        keys = reapplyTiming(mem, keys, duration, target, TimingFit::FitToEvent);
        duration = target;
    }
    // Last target equals the original duration: keys are bit-identical to the source.
    CHECK(keys[1].time == 1.0 / 3.0);
    CHECK(keys[2].time == 2.9);
    // A user edit between applications re-bases the source.
    keys[1].time = 1.0;
    keys = reapplyTiming(mem, keys, duration, 5.8, TimingFit::FitToEvent);
    CHECK_NEAR(keys[1].time, 2.0, 1e-12);
}

TEST(cache_builds_once_and_isolates_instances) {
    const std::string a = saveDocument(makeDocument(makePreset(PresetId::Arc, 16.0 / 9.0)));
    const std::string b = saveDocument(makeDocument(makePreset(PresetId::Spiral, 16.0 / 9.0)));
    GeometryCache c1, c2;
    const auto s1 = c1.get(a, 16.0 / 9.0);
    const auto s1again = c1.get(a, 16.0 / 9.0);
    CHECK(s1 == s1again);
    CHECK(c1.buildCount() == 1);
    const auto s2 = c2.get(b, 16.0 / 9.0);
    CHECK(s2->pointCount != s1->pointCount);  // instances do not share paths
    CHECK(c1.get(a, 4.0 / 3.0) != s1);        // aspect change rebuilds
    CHECK(c1.buildCount() == 2);
    // Snapshots stay valid after the cache moves on.
    c1.get(b, 16.0 / 9.0);
    CHECK(s1->geometry && s1->geometry->segmentCount() == 2);
    // Corrupt data: usable == false, no geometry, no throw.
    const auto bad = c2.get("{garbage", 16.0 / 9.0);
    CHECK(bad->status == LoadStatus::Invalid && !bad->geometry);
}

TEST(cache_is_thread_safe) {
    const std::string a = saveDocument(makeDocument(makePreset(PresetId::Circle, 16.0 / 9.0)));
    const std::string b = saveDocument(makeDocument(makePreset(PresetId::Zigzag, 16.0 / 9.0)));
    GeometryCache cache;
    std::atomic<int> errors{0};
    std::vector<std::thread> threads;
    for (int t = 0; t < 8; ++t) {
        threads.emplace_back([&, t] {
            for (int i = 0; i < 300; ++i) {
                const bool useA = ((i + t) % 3) != 0;
                const auto s = cache.get(useA ? a : b, 16.0 / 9.0);
                if (!s->geometry) ++errors;
                else if (s->closed != useA) ++errors;  // circle is closed, zigzag is open
                else {
                    // Evaluate while other threads replace the snapshot.
                    const PathSample ps = s->geometry->sampleAtDistance(0.3, 1, false);
                    if (!isFinite(ps.pos)) ++errors;
                }
            }
        });
    }
    for (auto& th : threads) th.join();
    CHECK(errors.load() == 0);
}
