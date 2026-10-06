// Wron Motion Path — OFX image effect (filter context).
//
// Target host: VEGAS Pro 2026 (Windows 11 x64). Written against the public
// OpenFX 1.5.1 C API, using only properties that are either standard OFX 1.x
// or read with a fallback, so behaviour does not depend on the host name.
//
// Threading contract:
//  * All per-instance state lives in `Instance` (OFX instance data); there is
//    no global path state.
//  * Render threads read parameters through paramGetValueAtTime only and
//    share immutable geometry snapshots from the instance's GeometryCache.
//  * The path string is copied out of the host buffer immediately, under a
//    per-instance mutex (VEGAS 2026 Build 143 fixed a use-after-free around
//    string parameter buffers; we never hold the host pointer).
//  * No UI calls during render. Push buttons are handled in instanceChanged
//    (UI thread) and wrapped in paramEditBegin/End so the host records one
//    undo step.
//  * No persistent threads: render fans out with the host MultiThread suite
//    (or std::thread, joined before returning), so closing a project or
//    unloading the plug-in leaves nothing running.
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "ofx_util.h"
#include "overlay.h"
#include "params.h"
#include "vegas_ext.h"
#include "wmp/cache.h"
#include "wmp/document.h"
#include "wmp/motion.h"
#include "wmp/pipeline.h"
#include "wmp/presets.h"

#if defined(_WIN32)
#define WMP_OFX_EXPORT extern "C" __declspec(dllexport)
#else
#define WMP_OFX_EXPORT extern "C" __attribute__((visibility("default")))
#endif

#ifndef WMP_DEV_BUILD
#error "Only developer builds exist: the Wron licence module is not integrated (see CMakeLists.txt)."
#endif

namespace wmpofx {

struct ParamHandles {
    OfxParamHandle pathData = nullptr, pathInfo = nullptr, hostInfo = nullptr;
    OfxParamHandle progress = nullptr, startOffset = nullptr, reverse = nullptr, endBehavior = nullptr,
                   speedMode = nullptr, easing = nullptr, easeP1 = nullptr, easeP2 = nullptr;
    OfxParamHandle pivot = nullptr, offset = nullptr, uniformScale = nullptr, scale = nullptr, rotation = nullptr,
                   opacity = nullptr;
    OfxParamHandle orient = nullptr, orientOffset = nullptr, orientMode = nullptr, orientSmooth = nullptr;
    OfxParamHandle blur = nullptr, shutterAngle = nullptr, shutterPhase = nullptr, blurAdaptive = nullptr,
                   blurSamples = nullptr, blurMaxSamples = nullptr, blurPreview = nullptr;
    OfxParamHandle preset = nullptr, showOverlay = nullptr, overlayTool = nullptr;
};

struct Instance {
    OfxImageEffectHandle effect = nullptr;
    OfxParamSetHandle paramSet = nullptr;
    OfxImageClipHandle src = nullptr, dst = nullptr;
    ParamHandles p;
    wmp::GeometryCache cache;
    std::mutex pathStringMutex;
    std::string vegasContext;
    OverlayState overlay;  // UI-thread only (interact actions)
};

namespace {

constexpr const char* kLabel = "Wron Motion Path (DEV build - not for distribution)";
constexpr const char* kShortLabel = "Wron Motion Path";
constexpr const char* kDescription =
    "Moves the image along an editable Bezier motion path. Progress (0 = path start, 1 = path end) is "
    "animated independently of the path geometry and mapped by arc length for constant speed. "
    "Coordinates are normalized to the project frame: (0,0) top-left, (0.5,0.5) centre, +Y down. "
    "Positive rotation is clockwise on screen.";

// ----------------------------------------------------------------------------
// Parameter definition helpers (describe in context)
// ----------------------------------------------------------------------------

OfxPropertySetHandle define(OfxParamSetHandle ps, const char* type, const char* name, const char* label,
                            const char* hint, const char* parent) {
    OfxPropertySetHandle props = nullptr;
    suites().param->paramDefine(ps, type, name, &props);
    setString(props, kOfxPropLabel, label);
    if (hint) setString(props, kOfxParamPropHint, hint);
    if (parent) setString(props, kOfxParamPropParent, parent);
    return props;
}

void defineGroup(OfxParamSetHandle ps, const char* name, const char* label, bool open) {
    OfxPropertySetHandle props = define(ps, kOfxParamTypeGroup, name, label, nullptr, nullptr);
    setInt(props, kOfxParamPropGroupOpen, open ? 1 : 0);
}

struct Range {
    double min, max, dmin, dmax;
};

OfxPropertySetHandle defineDouble(OfxParamSetHandle ps, const char* name, const char* label, const char* hint,
                                  const char* parent, double def, Range r, const char* doubleType, bool animates,
                                  double increment, int digits) {
    OfxPropertySetHandle props = define(ps, kOfxParamTypeDouble, name, label, hint, parent);
    setDouble(props, kOfxParamPropDefault, def);
    setDouble(props, kOfxParamPropMin, r.min);
    setDouble(props, kOfxParamPropMax, r.max);
    setDouble(props, kOfxParamPropDisplayMin, r.dmin);
    setDouble(props, kOfxParamPropDisplayMax, r.dmax);
    setString(props, kOfxParamPropDoubleType, doubleType);
    setInt(props, kOfxParamPropAnimates, animates ? 1 : 0);
    setDouble(props, kOfxParamPropIncrement, increment);
    setInt(props, kOfxParamPropDigits, digits);
    return props;
}

OfxPropertySetHandle defineDouble2D(OfxParamSetHandle ps, const char* name, const char* label, const char* hint,
                                    const char* parent, double dx, double dy, Range rx, Range ry, bool animates,
                                    double increment, int digits) {
    OfxPropertySetHandle props = define(ps, kOfxParamTypeDouble2D, name, label, hint, parent);
    const double defs[2] = {dx, dy}, mins[2] = {rx.min, ry.min}, maxs[2] = {rx.max, ry.max},
                 dmins[2] = {rx.dmin, ry.dmin}, dmaxs[2] = {rx.dmax, ry.dmax};
    setDoubleN(props, kOfxParamPropDefault, 2, defs);
    setDoubleN(props, kOfxParamPropMin, 2, mins);
    setDoubleN(props, kOfxParamPropMax, 2, maxs);
    setDoubleN(props, kOfxParamPropDisplayMin, 2, dmins);
    setDoubleN(props, kOfxParamPropDisplayMax, 2, dmaxs);
    // Plain: the host must not convert these normalized values to canonical
    // coordinates or flip their Y axis.
    setString(props, kOfxParamPropDoubleType, kOfxParamDoubleTypePlain);
    setString(props, kOfxParamPropDimensionLabel, "x", 0);
    setString(props, kOfxParamPropDimensionLabel, "y", 1);
    setInt(props, kOfxParamPropAnimates, animates ? 1 : 0);
    setDouble(props, kOfxParamPropIncrement, increment);
    setInt(props, kOfxParamPropDigits, digits);
    return props;
}

void defineBool(OfxParamSetHandle ps, const char* name, const char* label, const char* hint, const char* parent,
                bool def) {
    OfxPropertySetHandle props = define(ps, kOfxParamTypeBoolean, name, label, hint, parent);
    setInt(props, kOfxParamPropDefault, def ? 1 : 0);
    setInt(props, kOfxParamPropAnimates, 0);
}

void defineInt(OfxParamSetHandle ps, const char* name, const char* label, const char* hint, const char* parent,
               int def, int mn, int mx) {
    OfxPropertySetHandle props = define(ps, kOfxParamTypeInteger, name, label, hint, parent);
    setInt(props, kOfxParamPropDefault, def);
    setInt(props, kOfxParamPropMin, mn);
    setInt(props, kOfxParamPropMax, mx);
    setInt(props, kOfxParamPropDisplayMin, mn);
    setInt(props, kOfxParamPropDisplayMax, mx);
    setInt(props, kOfxParamPropAnimates, 0);
}

void defineChoice(OfxParamSetHandle ps, const char* name, const char* label, const char* hint, const char* parent,
                  std::initializer_list<const char*> options, int def) {
    OfxPropertySetHandle props = define(ps, kOfxParamTypeChoice, name, label, hint, parent);
    int i = 0;
    for (const char* o : options) setString(props, kOfxParamPropChoiceOption, o, i++);
    setInt(props, kOfxParamPropDefault, def);
    setInt(props, kOfxParamPropAnimates, 0);
}

void defineButton(OfxParamSetHandle ps, const char* name, const char* label, const char* hint, const char* parent) {
    define(ps, kOfxParamTypePushButton, name, label, hint, parent);
}

void defineLabel(OfxParamSetHandle ps, const char* name, const char* label, const char* parent) {
    OfxPropertySetHandle props = define(ps, kOfxParamTypeString, name, label, nullptr, parent);
    setString(props, kOfxParamPropStringMode, kOfxParamStringIsLabel);
    setString(props, kOfxParamPropDefault, "");
    setInt(props, kOfxParamPropAnimates, 0);
    setInt(props, kOfxParamPropPersistant, 0);
    setInt(props, kOfxParamPropEvaluateOnChange, 0);
}

std::string defaultPathDocument() {
    // Straight path through the frame centre; at Progress 0 the pivot sits on
    // the left end. Built for 16:9; the path is in normalized coordinates, so
    // it keeps its meaning for any frame size.
    return wmp::saveDocument(wmp::makeDocument(wmp::makePreset(wmp::PresetId::Straight, 16.0 / 9.0)));
}

// ----------------------------------------------------------------------------
// Instance helpers
// ----------------------------------------------------------------------------

Instance* instanceOf(OfxImageEffectHandle effect) {
    OfxPropertySetHandle props = nullptr;
    if (!effect || suites().effect->getPropertySet(effect, &props) != kOfxStatOK) return nullptr;
    return static_cast<Instance*>(getPointer(props, kOfxPropInstanceData));
}

double paramDouble(OfxParamHandle h, double t, double def = 0.0) {
    double v = def;
    if (!h || suites().param->paramGetValueAtTime(h, t, &v) != kOfxStatOK || !std::isfinite(v)) return def;
    return v;
}

wmp::Vec2 paramDouble2D(OfxParamHandle h, double t, wmp::Vec2 def) {
    double x = def.x, y = def.y;
    if (!h || suites().param->paramGetValueAtTime(h, t, &x, &y) != kOfxStatOK || !std::isfinite(x) || !std::isfinite(y))
        return def;
    return {x, y};
}

int paramInt(OfxParamHandle h, double t, int def = 0) {
    int v = def;
    if (!h || suites().param->paramGetValueAtTime(h, t, &v) != kOfxStatOK) return def;
    return v;
}

bool paramBool(OfxParamHandle h, double t, bool def = false) { return paramInt(h, t, def ? 1 : 0) != 0; }

int paramChoice(OfxParamHandle h, double t, int count, int def) {
    const int v = paramInt(h, t, def);
    return (v >= 0 && v < count) ? v : def;
}

std::string readPathData(Instance& in) {
    std::lock_guard<std::mutex> lock(in.pathStringMutex);
    char* v = nullptr;
    if (!in.p.pathData || suites().param->paramGetValue(in.p.pathData, &v) != kOfxStatOK || !v) return {};
    return std::string(v);  // copy now: the host buffer is temporary
}

void writePathData(Instance& in, const std::string& json, const char* undoLabel) {
    // No lock held while calling the host: hosts may call instanceChanged
    // synchronously from inside paramSetValue, which reads the path again.
    suites().param->paramEditBegin(in.paramSet, undoLabel);
    suites().param->paramSetValue(in.p.pathData, json.c_str());
    suites().param->paramEditEnd(in.paramSet);
}

void setLabel(OfxParamHandle h, const std::string& text) {
    if (!h) return;
    char* cur = nullptr;
    if (suites().param->paramGetValue(h, &cur) == kOfxStatOK && cur && text == cur) return;
    suites().param->paramSetValue(h, text.c_str());
}

void setEnabled(OfxParamHandle h, bool enabled) {
    OfxPropertySetHandle props = nullptr;
    if (h && suites().param->paramGetPropertySet(h, &props) == kOfxStatOK)
        setInt(props, kOfxParamPropEnabled, enabled ? 1 : 0);
}

wmp::MotionParams readMotion(const Instance& in, double t) {
    const ParamHandles& p = in.p;
    wmp::MotionParams m;
    m.progress = paramDouble(p.progress, t);
    m.progressSettings.startOffset = paramDouble(p.startOffset, t);
    m.progressSettings.reverse = paramBool(p.reverse, t);
    m.progressSettings.endBehavior = static_cast<wmp::EndBehavior>(paramChoice(p.endBehavior, t, 4, 0));
    m.speedMode = static_cast<wmp::SpeedMode>(paramChoice(p.speedMode, t, 2, 0));
    const int easing = paramChoice(p.easing, t, 6, 0);
    const wmp::Vec2 e1 = paramDouble2D(p.easeP1, t, {0.42, 0.0});
    const wmp::Vec2 e2 = paramDouble2D(p.easeP2, t, {0.58, 1.0});
    m.progressSettings.easing = wmp::easingFor(static_cast<wmp::EasingPreset>(easing), {e1.x, e1.y, e2.x, e2.y});
    m.pivot = paramDouble2D(p.pivot, t, {0.5, 0.5});
    m.positionOffset = paramDouble2D(p.offset, t, {0.0, 0.0});
    const wmp::Vec2 sc = paramDouble2D(p.scale, t, {100.0, 100.0});
    const bool uniform = paramBool(p.uniformScale, t, true);
    m.scale = {sc.x / 100.0, (uniform ? sc.x : sc.y) / 100.0};
    m.rotationDeg = paramDouble(p.rotation, t);
    m.opacity = paramDouble(p.opacity, t, 100.0) / 100.0;
    m.orientToPath = paramBool(p.orient, t);
    m.orientOffsetDeg = paramDouble(p.orientOffset, t);
    m.orientMode = static_cast<wmp::OrientMode>(paramChoice(p.orientMode, t, 2, 0));
    m.orientSmoothing = std::clamp(paramDouble(p.orientSmooth, t), 0.0, 50.0) / 100.0;
    return m;
}

wmp::ShutterSettings readShutter(const Instance& in, double t) {
    const ParamHandles& p = in.p;
    wmp::ShutterSettings s;
    s.enabled = paramBool(p.blur, t);
    s.angleDeg = paramDouble(p.shutterAngle, t, 180.0);
    s.phaseDeg = paramDouble(p.shutterPhase, t, -90.0);
    s.adaptive = paramBool(p.blurAdaptive, t, true);
    s.samples = paramInt(p.blurSamples, t, 16);
    s.maxSamples = paramInt(p.blurMaxSamples, t, 64);
    s.preview = static_cast<wmp::PreviewBlur>(paramChoice(p.blurPreview, t, 3, 1));
    return s;
}

struct FrameSetup {
    wmp::FrameRect frame;
    wmp::RectD srcRod;
    double projectPar = 1.0;
};

FrameSetup frameSetup(const Instance& in, double time) {
    FrameSetup fs;
    OfxPropertySetHandle props = nullptr;
    suites().effect->getPropertySet(in.effect, &props);
    double size[2] = {0, 0}, off[2] = {0, 0};
    getDoubleN(props, kOfxImageEffectPropProjectSize, 2, size);
    getDoubleN(props, kOfxImageEffectPropProjectOffset, 2, off);
    fs.projectPar = getDouble(props, kOfxImageEffectPropProjectPixelAspectRatio, 0, 1.0);
    if (!(fs.projectPar > 0.0)) fs.projectPar = 1.0;
    OfxRectD rod{0, 0, 0, 0};
    if (in.src && suites().effect->clipGetRegionOfDefinition(in.src, time, &rod) == kOfxStatOK)
        fs.srcRod = {rod.x1, rod.y1, rod.x2, rod.y2};
    if (size[0] > 0.0 && size[1] > 0.0 && std::isfinite(size[0]) && std::isfinite(size[1])) {
        fs.frame = {off[0], off[1], size[0], size[1]};
    } else if (!fs.srcRod.empty() && fs.srcRod.x2 - fs.srcRod.x1 < 1e7) {
        fs.frame = {fs.srcRod.x1, fs.srcRod.y1, fs.srcRod.x2 - fs.srcRod.x1, fs.srcRod.y2 - fs.srcRod.y1};
    } else {
        fs.frame = {0, 0, 1920.0 * fs.projectPar, 1080.0};
    }
    return fs;
}

struct Evaluator {
    const Instance& in;
    const wmp::PathGeometry* geometry;
    wmp::FrameRect frame;

    wmp::SamplePose operator()(double t) const {
        const wmp::Pose pose = wmp::evaluatePose(geometry, readMotion(in, t));
        wmp::SamplePose sp;
        sp.srcToOut = wmp::poseToCanonical(pose, frame);
        sp.opacity = pose.opacity;
        sp.continuityKey = pose.continuityKey;
        return sp;
    }
};

void readQuality(OfxPropertySetHandle inArgs, const double rs[2], wmp::RenderQuality& q, bool& preview) {
    q = wmp::RenderQuality::Good;
    preview = false;
    const std::string vq = getString(inArgs, kWmpVegasPropRenderQuality);
    if (vq == kWmpVegasRenderQualityDraft) q = wmp::RenderQuality::Draft;
    else if (vq == kWmpVegasRenderQualityPreview) q = wmp::RenderQuality::Preview;
    else if (vq == kWmpVegasRenderQualityBest) q = wmp::RenderQuality::Best;
    else if (vq.empty() && getInt(inArgs, kOfxImageEffectPropRenderQualityDraft, 0, 0) != 0)
        q = wmp::RenderQuality::Draft;  // standard OFX 1.4 integer flag
    preview = q == wmp::RenderQuality::Draft || q == wmp::RenderQuality::Preview || rs[0] < 0.999 || rs[1] < 0.999;
    if (preview && q == wmp::RenderQuality::Good) q = wmp::RenderQuality::Preview;
}

struct PreparedFrame {
    std::shared_ptr<const wmp::GeometrySnapshot> snapshot;
    FrameSetup fs;
    wmp::RenderContext ctx;
    wmp::FramePlan plan;
};

PreparedFrame prepare(Instance& in, double time, const double rs[2], OfxPropertySetHandle inArgs,
                      double srcPar, double dstPar) {
    PreparedFrame pf;
    pf.fs = frameSetup(in, time);
    pf.ctx.frame = pf.fs.frame;
    pf.ctx.srcRect = pf.fs.srcRod;
    pf.ctx.setPixelMapping(rs[0], rs[1], srcPar, dstPar);
    const std::string field = getString(inArgs, kOfxImageEffectPropFieldToRender, 0, kOfxImageFieldNone);
    if (field == kOfxImageFieldLower) pf.ctx.setFieldOutput(false);
    else if (field == kOfxImageFieldUpper) pf.ctx.setFieldOutput(true);
    readQuality(inArgs, rs, pf.ctx.quality, pf.ctx.previewRender);
    pf.snapshot = in.cache.get(readPathData(in), pf.fs.frame.aspect());
    const wmp::PathGeometry* geom = pf.snapshot->geometry.get();
    const Evaluator eval{in, geom, pf.fs.frame};
    pf.plan = wmp::planFrame(time, readShutter(in, time), eval, pf.ctx);
    return pf;
}

void renderScaleOf(OfxPropertySetHandle inArgs, double rs[2]) {
    rs[0] = rs[1] = 1.0;
    getDoubleN(inArgs, kOfxImageEffectPropRenderScale, 2, rs);
    if (!(rs[0] > 0.0)) rs[0] = 1.0;
    if (!(rs[1] > 0.0)) rs[1] = 1.0;
}

// ----------------------------------------------------------------------------
// UI state (UI thread only)
// ----------------------------------------------------------------------------

std::string describePath(const wmp::GeometrySnapshot& s) {
    char buf[256];
    switch (s.status) {
        case wmp::LoadStatus::Empty: return "No path stored: the image is not moved by a path.";
        case wmp::LoadStatus::Invalid:
            return "Invalid path data (" + s.message + "). Rendering without a path; the stored data is kept unchanged.";
        case wmp::LoadStatus::UnsupportedFutureVersion:
            return "Path data was saved by a newer Wron Motion Path (" + s.message +
                   "). Rendering without a path; the data is kept unchanged.";
        case wmp::LoadStatus::Ok:
        case wmp::LoadStatus::Migrated: break;
    }
    if (s.pointCount == 0) return "Empty path: the image is not moved by a path.";
    std::snprintf(buf, sizeof buf, "%zu point%s, %s, length %.3f frame heights%s", s.pointCount,
                  s.pointCount == 1 ? "" : "s", s.closed ? "closed" : "open",
                  s.geometry ? s.geometry->length() : 0.0,
                  s.status == wmp::LoadStatus::Migrated ? " (migrated from an older format)" : "");
    return buf;
}

void updateUi(Instance& in, double time) {
    const FrameSetup fs = frameSetup(in, time);
    const auto snap = in.cache.get(readPathData(in), fs.frame.aspect());
    setLabel(in.p.pathInfo, describePath(*snap));
    const bool closed = snap->geometry && snap->closed;
    setEnabled(in.p.endBehavior, !closed);
    setEnabled(in.p.easeP1, paramChoice(in.p.easing, time, 6, 0) == 5);
    setEnabled(in.p.easeP2, paramChoice(in.p.easing, time, 6, 0) == 5);
    setEnabled(in.p.scale, true);
    const bool orient = paramBool(in.p.orient, time);
    setEnabled(in.p.orientOffset, orient);
    setEnabled(in.p.orientMode, orient);
    setEnabled(in.p.orientSmooth, orient);
    const bool blur = paramBool(in.p.blur, time);
    const bool adaptive = paramBool(in.p.blurAdaptive, time, true);
    setEnabled(in.p.shutterAngle, blur);
    setEnabled(in.p.shutterPhase, blur);
    setEnabled(in.p.blurAdaptive, blur);
    setEnabled(in.p.blurSamples, blur && !adaptive);
    setEnabled(in.p.blurMaxSamples, blur && adaptive);
    setEnabled(in.p.blurPreview, blur);

    // Diagnostics for real-host verification (what the host actually reports).
    OfxPropertySetHandle props = nullptr;
    suites().effect->getPropertySet(in.effect, &props);
    double range[2] = {0, 0};
    const bool hasRange = getDoubleN(props, kOfxImageEffectPropFrameRange, 2, range);
    const double fps = getDouble(props, kOfxImageEffectPropFrameRate, 0, 0.0);
    char buf[512];
    std::snprintf(buf, sizeof buf, "Host: %s %s | context: %s | frame range: %s%.3f..%.3f | fps: %.5f | project %.1fx%.1f PAR %.4f | overlays: %s",
                  suites().hostName.c_str(), suites().hostVersion.c_str(),
                  in.vegasContext.empty() ? "n/a" : in.vegasContext.c_str(), hasRange ? "" : "(not reported) ", range[0],
                  range[1], fps, fs.frame.w, fs.frame.h, fs.projectPar, suites().supportsOverlays ? "yes" : "no");
    setLabel(in.p.hostInfo, buf);
}

void applyPresetPath(Instance& in, double time) {
    const int idx = paramChoice(in.p.preset, time, wmp::kPresetCount, 0);
    const FrameSetup fs = frameSetup(in, time);
    const wmp::LoadResult cur = wmp::loadDocument(readPathData(in));
    if (cur.status == wmp::LoadStatus::UnsupportedFutureVersion) {
        setLabel(in.p.pathInfo, "Preset not loaded: the stored path is from a newer version and would be lost.");
        return;
    }
    wmp::PathDocument doc = cur.usable() ? cur.doc : wmp::makeDocument({});
    doc.path = wmp::makePreset(static_cast<wmp::PresetId>(idx), fs.frame.aspect());
    writePathData(in, wmp::saveDocument(doc), "Load Preset Path");
}

void reversePathGeometry(Instance& in) {
    const wmp::LoadResult cur = wmp::loadDocument(readPathData(in));
    if (!cur.usable()) {
        setLabel(in.p.pathInfo, "Reverse not applied: the stored path data cannot be read.");
        return;
    }
    wmp::PathDocument doc = cur.doc;
    doc.path = wmp::reversed(doc.path);
    writePathData(in, wmp::saveDocument(doc), "Reverse Path Direction");
}

// ----------------------------------------------------------------------------
// Actions
// ----------------------------------------------------------------------------

OfxStatus onLoad() {
    HostSuites& s = suites();
    if (!s.host) return kOfxStatErrMissingHostFeature;
    auto fetch = [&](const char* name, int version) { return s.host->fetchSuite(s.host->host, name, version); };
    s.prop = static_cast<const OfxPropertySuiteV1*>(fetch(kOfxPropertySuite, 1));
    s.effect = static_cast<const OfxImageEffectSuiteV1*>(fetch(kOfxImageEffectSuite, 1));
    s.param = static_cast<const OfxParameterSuiteV1*>(fetch(kOfxParameterSuite, 1));
    s.memory = static_cast<const OfxMemorySuiteV1*>(fetch(kOfxMemorySuite, 1));
    s.thread = static_cast<const OfxMultiThreadSuiteV1*>(fetch(kOfxMultiThreadSuite, 1));
    s.message = static_cast<const OfxMessageSuiteV1*>(fetch(kOfxMessageSuite, 1));
    s.interact = static_cast<const OfxInteractSuiteV1*>(fetch(kOfxInteractSuite, 1));
    s.draw = static_cast<const OfxDrawSuiteV1*>(fetch(kOfxDrawSuite, 1));
    if (!s.prop || !s.effect || !s.param) return kOfxStatErrMissingHostFeature;
    s.hostName = getString(s.host->host, kOfxPropName);
    s.hostLabel = getString(s.host->host, kOfxPropLabel);
    s.hostVersion = getString(s.host->host, kOfxPropVersionLabel);
    // Overlay editing needs the interact suite and a way to draw.
    s.supportsOverlays = getInt(s.host->host, kOfxImageEffectPropSupportsOverlays, 0, 0) != 0 && s.interact &&
                         overlayCanDraw();
    return kOfxStatOK;
}

OfxStatus onDescribe(OfxImageEffectHandle effect) {
    OfxPropertySetHandle props = nullptr;
    suites().effect->getPropertySet(effect, &props);
    setString(props, kOfxPropLabel, kLabel);
    setString(props, kOfxPropShortLabel, kShortLabel);
    setString(props, kOfxPropLongLabel, kLabel);
    setString(props, kOfxPropPluginDescription, kDescription);
    setString(props, kOfxImageEffectPluginPropGrouping, kPluginGrouping);
    setString(props, kOfxImageEffectPropSupportedContexts, kOfxImageEffectContextFilter, 0);
    setString(props, kOfxImageEffectPropSupportedPixelDepths, kOfxBitDepthByte, 0);
    setString(props, kOfxImageEffectPropSupportedPixelDepths, kOfxBitDepthShort, 1);
    setString(props, kOfxImageEffectPropSupportedPixelDepths, kOfxBitDepthFloat, 2);
    setInt(props, kOfxImageEffectPluginPropSingleInstance, 0);
    setString(props, kOfxImageEffectPluginRenderThreadSafety, kOfxImageEffectRenderFullySafe);
    setInt(props, kOfxImageEffectPluginPropHostFrameThreading, 0);
    setInt(props, kOfxImageEffectPropSupportsMultiResolution, 1);
    setInt(props, kOfxImageEffectPropSupportsTiles, 1);
    setInt(props, kOfxImageEffectPropTemporalClipAccess, 0);
    setInt(props, kOfxImageEffectPropSupportsMultipleClipDepths, 0);
    setInt(props, kOfxImageEffectPropSupportsMultipleClipPARs, 0);
    setInt(props, kOfxImageEffectPluginPropFieldRenderTwiceAlways, 0);
    if (suites().supportsOverlays) {
#if defined(WMP_OVERLAY_OPENGL)
        setPointer(props, kOfxImageEffectPluginPropOverlayInteractV1, reinterpret_cast<void*>(&overlayMain));
#endif
        // OFX 1.5: hosts that provide the Draw Suite use the V2 entry point.
        if (suites().draw)
            setPointer(props, kOfxImageEffectPluginPropOverlayInteractV2, reinterpret_cast<void*>(&overlayMain));
    }
    return kOfxStatOK;
}

OfxStatus onDescribeInContext(OfxImageEffectHandle effect, OfxPropertySetHandle inArgs) {
    if (getString(inArgs, kOfxImageEffectPropContext) != kOfxImageEffectContextFilter) return kOfxStatErrUnsupported;
    OfxPropertySetHandle clip = nullptr;
    suites().effect->clipDefine(effect, kClipSource, &clip);
    setString(clip, kOfxImageEffectPropSupportedComponents, kOfxImageComponentRGBA, 0);
    setInt(clip, kOfxImageEffectPropSupportsTiles, 1);
    setInt(clip, kOfxImageClipPropOptional, 0);
    setString(clip, kOfxImageClipPropFieldExtraction, kOfxImageFieldDoubled);
    suites().effect->clipDefine(effect, kClipOutput, &clip);
    setString(clip, kOfxImageEffectPropSupportedComponents, kOfxImageComponentRGBA, 0);
    setInt(clip, kOfxImageEffectPropSupportsTiles, 1);

    OfxParamSetHandle ps = nullptr;
    suites().effect->getParamSet(effect, &ps);
    const Range unbounded{-1.0e6, 1.0e6, 0.0, 1.0};

    defineGroup(ps, kGrpPath, "Path", true);
    {
        OfxPropertySetHandle d = define(ps, kOfxParamTypeString, kPathData, "Path Data",
                                        "Versioned path document (edited by the Wron path editor).", kGrpPath);
        setString(d, kOfxParamPropDefault, defaultPathDocument().c_str());
        setString(d, kOfxParamPropStringMode, kOfxParamStringIsMultiLine);
        setInt(d, kOfxParamPropSecret, 1);
        setInt(d, kOfxParamPropAnimates, 0);
        setInt(d, kOfxParamPropPersistant, 1);
        setInt(d, kOfxParamPropEvaluateOnChange, 1);
        setString(d, kOfxParamPropCacheInvalidation, kOfxParamInvalidateValueChange);
    }
    defineLabel(ps, kPathInfo, "Path", kGrpPath);
    defineButton(ps, kReversePath, "Reverse Path Direction",
                 "Reverses the path geometry (start becomes end). Timing parameters are not changed.", kGrpPath);
    defineBool(ps, kShowOverlay, "Show Path in Preview",
               "Draws the editable path over the video preview (only on hosts with OFX overlay support).", kGrpPath,
               true);
    defineChoice(ps, kOverlayTool, "Preview Tool",
                 "What a click in the video preview does: move points/handles, add a point (on the curve: split "
                 "without changing its shape; elsewhere: append), or delete a point.",
                 kGrpPath, {"Edit Points", "Add Point", "Delete Point"}, 0);

    defineGroup(ps, kGrpTiming, "Timing", true);
    defineDouble(ps, kProgress, "Progress", "0 = path start, 1 = path end. Animate this to move along the path.",
                 kGrpTiming, 0.0, unbounded, kOfxParamDoubleTypePlain, true, 0.01, 4);
    defineDouble(ps, kStartOffset, "Start Offset", "Shifts where Progress 0 sits on the path (in progress units).",
                 kGrpTiming, 0.0, {-1.0e6, 1.0e6, -1.0, 1.0}, kOfxParamDoubleTypePlain, true, 0.01, 4);
    defineBool(ps, kReverse, "Reverse Travel", "Travel from the path end to its start (geometry unchanged).",
               kGrpTiming, false);
    defineChoice(ps, kEndBehavior, "Open Path Ends",
                 "Behaviour when Progress (after easing and offset) leaves 0..1 on an open path. Closed paths always loop.",
                 kGrpTiming, {"Clamp", "Extend", "Loop", "Ping-Pong"}, 0);
    defineChoice(ps, kSpeedMode, "Speed", "Constant speed by arc length, or the same Progress span for every segment.",
                 kGrpTiming, {"Constant Speed", "Equal Time per Segment"}, 0);
    defineChoice(ps, kEasing, "Easing", "Timing curve applied to Progress per unit lap.", kGrpTiming,
                 {"Linear", "Ease In", "Ease Out", "Ease In-Out", "Back Out (overshoot)", "Custom"}, 0);
    defineDouble2D(ps, kEaseP1, "Custom Ease P1", "First cubic-bezier control point (x 0..1, y free).", kGrpTiming,
                   0.42, 0.0, {0, 1, 0, 1}, {-5, 5, -1, 2}, false, 0.01, 3);
    defineDouble2D(ps, kEaseP2, "Custom Ease P2", "Second cubic-bezier control point (x 0..1, y free).", kGrpTiming,
                   0.58, 1.0, {0, 1, 0, 1}, {-5, 5, -1, 2}, false, 0.01, 3);

    defineGroup(ps, kGrpTransform, "Transform", true);
    defineDouble2D(ps, kPivot, "Source Pivot",
                   "Point of the source image placed on the path (normalized, (0.5,0.5) = centre, +Y down).",
                   kGrpTransform, 0.5, 0.5, {-1e4, 1e4, 0, 1}, {-1e4, 1e4, 0, 1}, true, 0.01, 4);
    defineDouble2D(ps, kOffset, "Position Offset", "Extra offset added to the path position (normalized).",
                   kGrpTransform, 0.0, 0.0, {-1e4, 1e4, -1, 1}, {-1e4, 1e4, -1, 1}, true, 0.01, 4);
    defineBool(ps, kUniformScale, "Uniform Scale", "Use the X scale for both axes.", kGrpTransform, true);
    defineDouble2D(ps, kScale, "Scale (%)", "Scale in the source image's own axes.", kGrpTransform, 100.0, 100.0,
                   {-1e5, 1e5, 0, 400}, {-1e5, 1e5, 0, 400}, true, 1.0, 2);
    defineDouble(ps, kRotation, "Rotation", "Degrees, positive = clockwise on screen.", kGrpTransform, 0.0,
                 {-1e6, 1e6, -360, 360}, kOfxParamDoubleTypeAngle, true, 1.0, 2);
    defineDouble(ps, kOpacity, "Opacity (%)", nullptr, kGrpTransform, 100.0, {0, 100, 0, 100},
                 kOfxParamDoubleTypePlain, true, 1.0, 1);

    defineGroup(ps, kGrpOrientation, "Orientation", false);
    defineBool(ps, kOrient, "Orient to Path", "Rotate the image with the path tangent.", kGrpOrientation, false);
    defineDouble(ps, kOrientOffset, "Rotation Offset", "Degrees added to the path angle (e.g. 90 for an image that faces up).",
                 kGrpOrientation, 0.0, {-1e6, 1e6, -180, 180}, kOfxParamDoubleTypeAngle, true, 1.0, 2);
    defineChoice(ps, kOrientMode, "Face", "Travel direction flips when the motion reverses (Reverse Travel, ping-pong return).",
                 kGrpOrientation, {"Travel Direction", "Path Direction"}, 0);
    defineDouble(ps, kOrientSmooth, "Direction Smoothing (%)",
                 "Averages the direction over this share of the path length on each side; rounds sharp corners. Spatial, "
                 "never depends on other frames.",
                 kGrpOrientation, 0.0, {0, 50, 0, 20}, kOfxParamDoubleTypePlain, true, 0.5, 2);

    defineGroup(ps, kGrpBlur, "Motion Blur", false);
    defineBool(ps, kBlur, "Motion Blur", "Blurs the transform motion (position, rotation, scale) across the shutter.",
               kGrpBlur, false);
    defineDouble(ps, kShutterAngle, "Shutter Angle", "Degrees of a frame the shutter is open (180 = half a frame).",
                 kGrpBlur, 180.0, {0, 720, 0, 360}, kOfxParamDoubleTypePlain, true, 1.0, 1);
    defineDouble(ps, kShutterPhase, "Shutter Phase", "Shutter opening relative to the frame time (-90 with 180 = centred).",
                 kGrpBlur, -90.0, {-360, 360, -180, 180}, kOfxParamDoubleTypePlain, true, 1.0, 1);
    defineBool(ps, kBlurAdaptive, "Adaptive Samples",
               "Choose the sample count from the on-screen travel (about 1 px between samples).", kGrpBlur, true);
    defineInt(ps, kBlurSamples, "Samples", "Fixed sample count when Adaptive Samples is off.", kGrpBlur, 16, 1,
              wmp::kMaxShutterSamples);
    defineInt(ps, kBlurMaxSamples, "Max Samples", "Upper limit for adaptive sampling.", kGrpBlur, 64, 2,
              wmp::kMaxShutterSamples);
    defineChoice(ps, kBlurPreview, "Preview Blur",
                 "Blur quality for draft/preview renders (host preview quality or reduced render scale).", kGrpBlur,
                 {"Full", "Reduced", "Off"}, 1);

    defineGroup(ps, kGrpPresets, "Presets", false);
    defineChoice(ps, kPreset, "Preset Path", nullptr, kGrpPresets,
                 {"Straight", "Arc", "Circle", "Ellipse", "S-Curve", "Zigzag", "Spiral"}, 0);
    defineButton(ps, kApplyPreset, "Load Preset Path",
                 "Replaces the path geometry with the selected preset (editable points). Timing is not changed.",
                 kGrpPresets);

    defineGroup(ps, kGrpDiagnostics, "Diagnostics", false);
    defineLabel(ps, kHostInfo, "Host", kGrpDiagnostics);
    return kOfxStatOK;
}

OfxStatus onCreateInstance(OfxImageEffectHandle effect) {
    auto in = std::make_unique<Instance>();
    in->effect = effect;
    suites().effect->getParamSet(effect, &in->paramSet);
    suites().effect->clipGetHandle(effect, kClipSource, &in->src, nullptr);
    suites().effect->clipGetHandle(effect, kClipOutput, &in->dst, nullptr);
    auto h = [&](const char* name) {
        OfxParamHandle ph = nullptr;
        suites().param->paramGetHandle(in->paramSet, name, &ph, nullptr);
        return ph;
    };
    ParamHandles& p = in->p;
    p.pathData = h(kPathData);
    p.pathInfo = h(kPathInfo);
    p.hostInfo = h(kHostInfo);
    p.progress = h(kProgress);
    p.startOffset = h(kStartOffset);
    p.reverse = h(kReverse);
    p.endBehavior = h(kEndBehavior);
    p.speedMode = h(kSpeedMode);
    p.easing = h(kEasing);
    p.easeP1 = h(kEaseP1);
    p.easeP2 = h(kEaseP2);
    p.pivot = h(kPivot);
    p.offset = h(kOffset);
    p.uniformScale = h(kUniformScale);
    p.scale = h(kScale);
    p.rotation = h(kRotation);
    p.opacity = h(kOpacity);
    p.orient = h(kOrient);
    p.orientOffset = h(kOrientOffset);
    p.orientMode = h(kOrientMode);
    p.orientSmooth = h(kOrientSmooth);
    p.blur = h(kBlur);
    p.shutterAngle = h(kShutterAngle);
    p.shutterPhase = h(kShutterPhase);
    p.blurAdaptive = h(kBlurAdaptive);
    p.blurSamples = h(kBlurSamples);
    p.blurMaxSamples = h(kBlurMaxSamples);
    p.blurPreview = h(kBlurPreview);
    p.preset = h(kPreset);
    p.showOverlay = h(kShowOverlay);
    p.overlayTool = h(kOverlayTool);
    if (!p.pathData || !p.progress || !in->src || !in->dst) return kOfxStatFailed;

    OfxPropertySetHandle props = nullptr;
    suites().effect->getPropertySet(effect, &props);
    in->vegasContext = getString(props, kWmpVegasPropContext);
    Instance* raw = in.release();
    setPointer(props, kOfxPropInstanceData, raw);
    updateUi(*raw, 0.0);
    return kOfxStatOK;
}

OfxStatus onDestroyInstance(OfxImageEffectHandle effect) {
    OfxPropertySetHandle props = nullptr;
    suites().effect->getPropertySet(effect, &props);
    delete static_cast<Instance*>(getPointer(props, kOfxPropInstanceData));
    setPointer(props, kOfxPropInstanceData, nullptr);
    return kOfxStatOK;
}

struct ThreadJob {
    const wmp::RenderJob* job = nullptr;
    OfxImageEffectHandle effect = nullptr;
};

bool abortRequested(void* ctx) {
    const ThreadJob* tj = static_cast<const ThreadJob*>(ctx);
    return suites().effect->abort(tj->effect) != 0;
}

void renderThread(unsigned int index, unsigned int count, void* arg) {
    ThreadJob* tj = static_cast<ThreadJob*>(arg);
    const wmp::RectI& w = tj->job->window;
    const int rows = w.height();
    const int n = static_cast<int>(std::max(1u, count));
    const int i = static_cast<int>(index);
    const int y0 = w.y1 + rows * i / n, y1 = w.y1 + rows * (i + 1) / n;
    wmp::renderRows(*tj->job, y0, y1, &abortRequested, tj);
}

void runThreaded(ThreadJob& tj) {
    const HostSuites& s = suites();
    unsigned int cpus = 1;
    if (s.thread && s.thread->multiThreadNumCPUs(&cpus) == kOfxStatOK && cpus >= 1 &&
        s.thread->multiThread(&renderThread, cpus, &tj) == kOfxStatOK)
        return;
    // Fallback: host suite missing or refused (e.g. already on a host worker thread).
    if (s.thread && s.thread->multiThreadIsSpawnedThread && s.thread->multiThreadIsSpawnedThread()) {
        renderThread(0, 1, &tj);
        return;
    }
    const unsigned int n = std::max(1u, std::min(16u, std::thread::hardware_concurrency()));
    std::vector<std::thread> pool;
    pool.reserve(n);
    for (unsigned int i = 0; i < n; ++i) pool.emplace_back(&renderThread, i, n, &tj);
    for (auto& t : pool) t.join();
}

OfxStatus onRender(Instance& in, OfxPropertySetHandle inArgs) {
    const double time = getDouble(inArgs, kOfxPropTime);
    int win[4] = {0, 0, 0, 0};
    if (!getIntN(inArgs, kOfxImageEffectPropRenderWindow, 4, win)) return kOfxStatFailed;
    double rs[2];
    renderScaleOf(inArgs, rs);

    ImageHandle dstImg(in.dst, time, nullptr);
    if (!dstImg) return kOfxStatFailed;
    wmp::ImageView dst;
    double dstPar = 1.0;
    std::string err;
    if (!imageView(dstImg.props(), dst, dstPar, err)) return kOfxStatErrImageFormat;
    const wmp::RectI window = wmp::RectI{win[0], win[1], win[2], win[3]}.intersect(dst.bounds);
    if (window.empty()) return kOfxStatOK;

    OfxPropertySetHandle srcClipProps = nullptr;
    suites().effect->clipGetPropertySet(in.src, &srcClipProps);
    double srcPar = getDouble(srcClipProps, kOfxImagePropPixelAspectRatio, 0, dstPar);
    if (!(srcPar > 0.0)) srcPar = dstPar;

    PreparedFrame pf = prepare(in, time, rs, inArgs, srcPar, dstPar);

    // Source region actually needed for this window.
    wmp::Affine dstPxInv;
    pf.ctx.dstToPixel.inverse(dstPxInv);
    const wmp::RectD winCanon =
        wmp::transformBounds(dstPxInv, {double(window.x1), double(window.y1), double(window.x2), double(window.y2)});
    const wmp::RectD roi = wmp::planRegionOfInterest(pf.plan, winCanon, pf.ctx);
    OfxRectD region{roi.x1, roi.y1, roi.x2, roi.y2};
    ImageHandle srcImg(in.src, time, roi.empty() ? nullptr : &region);
    wmp::ImageView src;
    double par2 = srcPar;
    const bool haveSrc = srcImg && imageView(srcImg.props(), src, par2, err);
    if (srcImg && !haveSrc) return kOfxStatErrImageFormat;

    if (wmp::isIdentityPlan(pf.plan, pf.ctx)) {
        wmp::copyImage(haveSrc ? src : wmp::ImageView{}, dst, window);
        return kOfxStatOK;
    }
    wmp::SourceTexture tex;
    if (haveSrc) tex.load(src, wmp::enclosingRect(wmp::transformBounds(pf.ctx.srcToPixel, roi)));
    const wmp::RenderJob job = wmp::buildRenderJob(pf.plan, tex, dst, window, pf.ctx);
    ThreadJob tj{&job, in.effect};
    runThreaded(tj);
    return kOfxStatOK;
}

OfxStatus onIsIdentity(Instance& in, OfxPropertySetHandle inArgs, OfxPropertySetHandle outArgs) {
    const double time = getDouble(inArgs, kOfxPropTime);
    double rs[2];
    renderScaleOf(inArgs, rs);
    OfxPropertySetHandle srcClipProps = nullptr;
    suites().effect->clipGetPropertySet(in.src, &srcClipProps);
    const double par = getDouble(srcClipProps, kOfxImagePropPixelAspectRatio, 0, 1.0);
    const PreparedFrame pf = prepare(in, time, rs, inArgs, par, par);
    if (!wmp::isIdentityPlan(pf.plan, pf.ctx)) return kOfxStatReplyDefault;
    setString(outArgs, kOfxPropName, kClipSource);
    setDouble(outArgs, kOfxPropTime, time);
    return kOfxStatOK;
}

OfxStatus onRegionOfDefinition(Instance& in, OfxPropertySetHandle inArgs, OfxPropertySetHandle outArgs) {
    const double time = getDouble(inArgs, kOfxPropTime);
    double rs[2];
    renderScaleOf(inArgs, rs);
    OfxPropertySetHandle srcClipProps = nullptr;
    suites().effect->clipGetPropertySet(in.src, &srcClipProps);
    const double par = getDouble(srcClipProps, kOfxImagePropPixelAspectRatio, 0, 1.0);
    const PreparedFrame pf = prepare(in, time, rs, inArgs, par, par);
    const wmp::RectD rod = wmp::planRegionOfDefinition(pf.plan, pf.ctx);
    const double v[4] = {rod.x1, rod.y1, rod.empty() ? rod.x1 : rod.x2, rod.empty() ? rod.y1 : rod.y2};
    setDoubleN(outArgs, kOfxImageEffectPropRegionOfDefinition, 4, v);
    return kOfxStatOK;
}

OfxStatus onRegionsOfInterest(Instance& in, OfxPropertySetHandle inArgs, OfxPropertySetHandle outArgs) {
    const double time = getDouble(inArgs, kOfxPropTime);
    double rs[2];
    renderScaleOf(inArgs, rs);
    double r[4] = {0, 0, 0, 0};
    if (!getDoubleN(inArgs, kOfxImageEffectPropRegionOfInterest, 4, r)) return kOfxStatReplyDefault;
    OfxPropertySetHandle srcClipProps = nullptr;
    suites().effect->clipGetPropertySet(in.src, &srcClipProps);
    const double par = getDouble(srcClipProps, kOfxImagePropPixelAspectRatio, 0, 1.0);
    const PreparedFrame pf = prepare(in, time, rs, inArgs, par, par);
    wmp::RectD roi = wmp::planRegionOfInterest(pf.plan, {r[0], r[1], r[2], r[3]}, pf.ctx);
    if (roi.empty()) roi = {r[0], r[1], r[0], r[1]};
    const double v[4] = {roi.x1, roi.y1, roi.x2, roi.y2};
    const std::string name = std::string("OfxImageClipPropRoI_") + kClipSource;
    setDoubleN(outArgs, name.c_str(), 4, v);
    return kOfxStatOK;
}

OfxStatus onClipPreferences(Instance& in, OfxPropertySetHandle outArgs) {
    OfxPropertySetHandle srcClipProps = nullptr;
    suites().effect->clipGetPropertySet(in.src, &srcClipProps);
    const std::string pm = getString(srcClipProps, kOfxImageEffectPropPreMultiplication, 0, kOfxImagePreMultiplied);
    // Moving the image uncovers transparent pixels: an opaque input becomes premultiplied output.
    setString(outArgs, kOfxImageEffectPropPreMultiplication,
              pm == kOfxImageUnPreMultiplied ? kOfxImageUnPreMultiplied : kOfxImagePreMultiplied);
    return kOfxStatOK;
}

OfxStatus onInstanceChanged(Instance& in, OfxPropertySetHandle inArgs) {
    if (getString(inArgs, kOfxPropType) != kOfxTypeParameter) return kOfxStatReplyDefault;
    const std::string name = getString(inArgs, kOfxPropName);
    const double time = getDouble(inArgs, kOfxPropTime);
    if (name == kApplyPreset) applyPresetPath(in, time);
    else if (name == kReversePath) reversePathGeometry(in);
    if (name == kPathInfo || name == kHostInfo) return kOfxStatOK;  // our own label writes
    updateUi(in, time);
    return kOfxStatOK;
}

OfxStatus mainEntry(const char* action, const void* handle, OfxPropertySetHandle inArgs,
                    OfxPropertySetHandle outArgs) {
    try {
        OfxImageEffectHandle effect = static_cast<OfxImageEffectHandle>(const_cast<void*>(handle));
        const std::string a = action ? action : "";
        if (a == kOfxActionLoad) return onLoad();
        if (a == kOfxActionUnload) return kOfxStatOK;
        if (a == kOfxActionDescribe) return onDescribe(effect);
        if (a == kOfxImageEffectActionDescribeInContext) return onDescribeInContext(effect, inArgs);
        if (a == kOfxActionCreateInstance) return onCreateInstance(effect);
        if (a == kOfxActionDestroyInstance) return onDestroyInstance(effect);
        Instance* in = instanceOf(effect);
        if (!in) return kOfxStatReplyDefault;
        if (a == kOfxImageEffectActionRender) return onRender(*in, inArgs);
        if (a == kOfxImageEffectActionIsIdentity) return onIsIdentity(*in, inArgs, outArgs);
        if (a == kOfxImageEffectActionGetRegionOfDefinition) return onRegionOfDefinition(*in, inArgs, outArgs);
        if (a == kOfxImageEffectActionGetRegionsOfInterest) return onRegionsOfInterest(*in, inArgs, outArgs);
        if (a == kOfxImageEffectActionGetClipPreferences) return onClipPreferences(*in, outArgs);
        if (a == kOfxActionInstanceChanged) return onInstanceChanged(*in, inArgs);
        if (a == kOfxActionPurgeCaches) {
            in->cache.clear();
            return kOfxStatOK;
        }
        return kOfxStatReplyDefault;
    } catch (const std::bad_alloc&) {
        return kOfxStatErrMemory;
    } catch (...) {
        return kOfxStatFailed;
    }
}

void setHost(OfxHost* host) { suites().host = host; }

OfxPlugin gPlugin = {kOfxImageEffectPluginApi, 1, kPluginIdentifier, kPluginVersionMajor, kPluginVersionMinor,
                     &setHost, &mainEntry};

}  // namespace

// Accessors used by the overlay (same translation unit family).
Instance* overlayInstance(OfxImageEffectHandle effect) { return instanceOf(effect); }
OverlayState* overlayStateOf(Instance* in) { return in ? &in->overlay : nullptr; }
std::string overlayReadPath(Instance* in) { return in ? readPathData(*in) : std::string(); }
void overlayWritePath(Instance* in, const std::string& json, const char* undo) {
    if (in) writePathData(*in, json, undo);
}
wmp::FrameRect overlayFrame(Instance* in, double time) { return in ? frameSetup(*in, time).frame : wmp::FrameRect{}; }

bool overlayCurrentPoint(Instance* in, double time, wmp::Vec2& pathPointN) {
    if (!in) return false;
    const FrameSetup fs = frameSetup(*in, time);
    const auto snap = in->cache.get(readPathData(*in), fs.frame.aspect());
    if (!snap->geometry || snap->geometry->empty()) return false;
    const wmp::MotionParams mp = readMotion(*in, time);
    const wmp::Pose pose = wmp::evaluatePose(snap->geometry.get(), mp);
    pathPointN = pose.position - mp.positionOffset;
    return true;
}

int overlayTool(Instance* in, double time) { return in ? paramChoice(in->p.overlayTool, time, 3, 0) : 0; }
bool overlayVisible(Instance* in, double time) { return in && paramBool(in->p.showOverlay, time, true); }

}  // namespace wmpofx

WMP_OFX_EXPORT int OfxGetNumberOfPlugins(void) { return 1; }

WMP_OFX_EXPORT OfxPlugin* OfxGetPlugin(int nth) { return nth == 0 ? &wmpofx::gPlugin : nullptr; }
