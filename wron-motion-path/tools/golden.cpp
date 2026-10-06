// Writes golden vectors from the C++ core so the JavaScript editor core can be
// checked against the renderer's math (same document, same numbers).
// Usage: wmp_golden > testdata/golden.json
#include <cstdio>
#include <string>
#include <vector>

#include "wmp/document.h"
#include "wmp/edit.h"
#include "wmp/geometry.h"
#include "wmp/json.h"
#include "wmp/motion.h"
#include "wmp/presets.h"
#include "wmp/timing.h"

using namespace wmp;
using json::Value;

namespace {

Value vec(Vec2 v) {
    Value a = Value::array();
    a.push(Value::number(v.x));
    a.push(Value::number(v.y));
    return a;
}

Value num(double d) { return Value::number(d); }

Path customPath() {
    // Uneven handles, a zero-length segment, a retracted corner and a cusp-ish segment.
    Path p;
    auto add = [&](Vec2 a, Vec2 in, Vec2 out, HandleMode m) {
        PathPoint q;
        q.p = a;
        q.in = in;
        q.out = out;
        q.mode = m;
        q.id = "p" + std::to_string(p.points.size() + 1);
        p.points.push_back(q);
    };
    add({0.1, 0.8}, {}, {0.02, -0.02}, HandleMode::Free);
    add({0.45, 0.3}, {-0.3, 0.0}, {0.05, 0.3}, HandleMode::Free);
    add({0.45, 0.3}, {}, {}, HandleMode::Corner);
    add({0.7, 0.6}, {0.2, -0.4}, {-0.2, 0.4}, HandleMode::Smooth);
    add({0.95, 0.2}, {}, {}, HandleMode::Corner);
    return p;
}

Value geometryCase(const std::string& name, const Path& path, double aspect) {
    Value c = Value::object();
    c.set("name", Value::string(name));
    c.set("aspect", num(aspect));
    c.set("doc", Value::string(saveDocument(makeDocument(path))));
    PathGeometry g(path, {aspect, 1e-9});
    c.set("length", num(g.length()));
    Value segs = Value::array();
    for (int i = 0; i < g.segmentCount(); ++i) segs.push(num(g.segmentLength(i)));
    c.set("segmentLengths", segs);
    Value samples = Value::array();
    for (int mode = 0; mode < 2; ++mode) {
        for (int i = -2; i <= 22; ++i) {
            const double u = i / 20.0;
            for (int bias : {1, -1}) {
                const bool extend = (i < 0 || i > 20);
                const double d = g.distanceForU(u, static_cast<SpeedMode>(mode));
                const PathSample s = g.sampleAtDistance(d, bias, extend);
                Value o = Value::object();
                o.set("u", num(u));
                o.set("mode", num(mode));
                o.set("bias", num(bias));
                o.set("extend", Value::boolean(extend));
                o.set("d", num(d));
                o.set("pos", vec(s.pos));
                o.set("tan", vec(s.tangent));
                o.set("tanValid", Value::boolean(s.tangentValid));
                o.set("seg", num(s.segment));
                o.set("t", num(s.t));
                samples.push(o);
            }
        }
    }
    c.set("samples", samples);
    Value smooth = Value::array();
    for (int i = 0; i <= 10; ++i) {
        const double d = g.length() * i / 10.0;
        for (double sigma : {0.02, 0.1}) {
            Vec2 t;
            const bool ok = g.smoothedTangent(d, sigma * g.length(), 1, t);
            Value o = Value::object();
            o.set("d", num(d));
            o.set("sigma", num(sigma * g.length()));
            o.set("ok", Value::boolean(ok));
            o.set("tan", vec(t));
            smooth.push(o);
        }
    }
    c.set("smoothed", smooth);
    return c;
}

}  // namespace

int main() {
    Value root = Value::object();
    root.set("generator", Value::string("wmp_golden (C++ core)"));
    Value cases = Value::array();
    const double aspects[] = {16.0 / 9.0, 9.0 / 16.0};
    for (int i = 0; i < kPresetCount; ++i)
        for (double a : aspects)
            cases.push(geometryCase(std::string(presetKey(static_cast<PresetId>(i))), makePreset(static_cast<PresetId>(i), a), a));
    cases.push(geometryCase("custom", customPath(), 16.0 / 9.0));
    Path closedCustom = customPath();
    closedCustom.closed = true;
    cases.push(geometryCase("custom-closed", closedCustom, 4.0 / 3.0));
    root.set("geometry", cases);

    Value presets = Value::array();
    for (int i = 0; i < kPresetCount; ++i)
        for (double a : aspects) {
            Value o = Value::object();
            o.set("key", Value::string(presetKey(static_cast<PresetId>(i))));
            o.set("aspect", num(a));
            o.set("doc", Value::string(saveDocument(makeDocument(makePreset(static_cast<PresetId>(i), a)))));
            presets.push(o);
        }
    root.set("presets", presets);

    Value easing = Value::array();
    const CubicEasing curves[] = {easingFor(EasingPreset::EaseIn, {}), easingFor(EasingPreset::EaseOut, {}),
                                  easingFor(EasingPreset::EaseInOut, {}), easingFor(EasingPreset::BackOut, {}),
                                  {0.1, -0.6, 0.2, 1.7}};
    for (const CubicEasing& e : curves)
        for (int i = 0; i <= 20; ++i) {
            Value o = Value::object();
            Value cv = Value::array();
            for (double v : {e.x1, e.y1, e.x2, e.y2}) cv.push(num(v));
            o.set("curve", cv);
            o.set("x", num(i / 20.0));
            o.set("y", num(e.eval(i / 20.0)));
            easing.push(o);
        }
    root.set("easing", easing);

    Value progress = Value::array();
    const char* ends[] = {"clamp", "extend", "loop", "pingpong"};
    for (int end = 0; end < 4; ++end)
        for (int closed = 0; closed < 2; ++closed)
            for (int rev = 0; rev < 2; ++rev)
                for (int ease = 0; ease < 2; ++ease)
                    for (int i = -6; i <= 30; ++i) {
                        ProgressSettings s;
                        s.endBehavior = static_cast<EndBehavior>(end);
                        s.reverse = rev != 0;
                        s.startOffset = 0.15;
                        s.easing = ease ? easingFor(EasingPreset::EaseInOut, {}) : CubicEasing{};
                        const double p = i * 0.1;
                        const ProgressState st = mapProgress(p, closed != 0, s);
                        Value o = Value::object();
                        o.set("p", num(p));
                        o.set("end", Value::string(ends[end]));
                        o.set("closed", Value::boolean(closed != 0));
                        o.set("reverse", Value::boolean(rev != 0));
                        o.set("offset", num(0.15));
                        o.set("ease", Value::boolean(ease != 0));
                        o.set("u", num(st.u));
                        o.set("direction", num(st.direction));
                        o.set("lap", num(static_cast<double>(st.lap)));
                        progress.push(o);
                    }
    root.set("progress", progress);

    // Editing operations.
    Value edits = Value::array();
    {
        Path p = makePreset(PresetId::SCurve, 16.0 / 9.0);
        Value o = Value::object();
        o.set("op", Value::string("insert"));
        o.set("before", Value::string(saveDocument(makeDocument(p))));
        const int idx = edit::insertPoint(p, 1, 0.3);
        o.set("segment", num(1));
        o.set("t", num(0.3));
        o.set("index", num(idx));
        o.set("after", Value::string(saveDocument(makeDocument(p))));
        edits.push(o);
    }
    {
        Path p = makePreset(PresetId::SCurve, 16.0 / 9.0);
        Value o = Value::object();
        o.set("op", Value::string("moveHandle"));
        o.set("before", Value::string(saveDocument(makeDocument(p))));
        edit::moveHandle(p, 1, true, p.points[1].p + Vec2{0.05, 0.1}, 16.0 / 9.0, false);
        o.set("index", num(1));
        o.set("out", Value::boolean(true));
        o.set("to", vec(makePreset(PresetId::SCurve, 16.0 / 9.0).points[1].p + Vec2{0.05, 0.1}));
        o.set("aspect", num(16.0 / 9.0));
        o.set("after", Value::string(saveDocument(makeDocument(p))));
        edits.push(o);
    }
    {
        Path p = makePreset(PresetId::Zigzag, 16.0 / 9.0);
        Value o = Value::object();
        o.set("op", Value::string("setModeSmooth"));
        o.set("before", Value::string(saveDocument(makeDocument(p))));
        edit::setMode(p, 2, HandleMode::Smooth, 16.0 / 9.0);
        o.set("index", num(2));
        o.set("aspect", num(16.0 / 9.0));
        o.set("after", Value::string(saveDocument(makeDocument(p))));
        edits.push(o);
    }
    {
        Path p = makePreset(PresetId::Ellipse, 16.0 / 9.0);
        Value o = Value::object();
        o.set("op", Value::string("transform"));
        o.set("before", Value::string(saveDocument(makeDocument(p))));
        edit::transformPath(p, 16.0 / 9.0, {0.4, 0.6}, 0.7, 33.0, {0.05, -0.02});
        o.set("aspect", num(16.0 / 9.0));
        o.set("after", Value::string(saveDocument(makeDocument(p))));
        edits.push(o);
    }
    {
        Path p = makePreset(PresetId::Circle, 16.0 / 9.0);
        Value o = Value::object();
        o.set("op", Value::string("reverse"));
        o.set("before", Value::string(saveDocument(makeDocument(p))));
        o.set("after", Value::string(saveDocument(makeDocument(reversed(p)))));
        edits.push(o);
    }
    root.set("edits", edits);

    const std::string out = json::serialize(root);
    std::fwrite(out.data(), 1, out.size(), stdout);
    std::fputc('\n', stdout);
    return 0;
}
