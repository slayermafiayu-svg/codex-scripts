// End-to-end tests of the real plug-in binary through the mock OFX host.
// Mock-host results are NOT VEGAS results; see docs/03-HOST-VERIFICATION.md.
#include <algorithm>
#include <cmath>
#include <cstring>
#include <set>
#include <thread>

#include "../../core/tests/test.h"
#include "mock_host.h"
#include "ofxKeySyms.h"
#include "wmp/document.h"
#include "wmp/geometry.h"
#include "wmp/presets.h"

#ifndef WMP_PLUGIN_PATH
#error "WMP_PLUGIN_PATH must point at the built .ofx"
#endif

using mock::Host;
using mock::Image;
using mock::ImageSpec;
using mock::Key;
using mock::RenderRequest;

namespace {

constexpr int W = 640, H = 360;

Host& host() {
    static Host* h = [] {
        Host* x = new Host();
        // White 40x40 square centred in the frame (pixel coords, Y up).
        x->sourcePixel = [](double px, double py, double, float c[4]) {
            const bool in = std::fabs(px - W / 2.0) < 20.0 && std::fabs(py - H / 2.0) < 20.0;
            const float v = in ? 1.0f : 0.0f;
            c[0] = c[1] = c[2] = c[3] = v;
        };
        std::string err;
        if (!x->load(WMP_PLUGIN_PATH, err)) {
            std::fprintf(stderr, "plug-in load failed: %s\n", err.c_str());
            std::abort();
        }
        return x;
    }();
    return *h;
}

std::unique_ptr<mock::Effect> newInstance(double fps = 30.0, double par = 1.0) {
    return host().createInstance(W, H, par, fps, 0, 300);
}

struct Centroid {
    double x = 0, y = 0, mass = 0;
};

Centroid centroid(Image& img) {
    Centroid c;
    for (int y = img.y1; y < img.y2; ++y)
        for (int x = img.x1; x < img.x2; ++x) {
            float p[4];
            mock::readPremult(img, x, y, p);
            c.x += (x + 0.5) * p[3];
            c.y += (y + 0.5) * p[3];
            c.mass += p[3];
        }
    if (c.mass > 0) {
        c.x /= c.mass;
        c.y /= c.mass;
    }
    return c;
}

Image renderFrame(mock::Effect* e, double time, double rs = 1.0, ImageSpec spec = {}) {
    if (spec.width == 0) {
        spec.width = W;
        spec.height = H;
    }
    spec.renderScale = rs;
    RenderRequest rq;
    rq.time = time;
    rq.renderScale = rs;
    rq.window[2] = static_cast<int>(std::lround(spec.width * rs));
    rq.window[3] = static_cast<int>(std::lround(spec.height * rs));
    Image out;
    const OfxStatus st = host().render(e, rq, spec, out);
    CHECK(st == kOfxStatOK);
    return out;
}

void linearProgress(mock::Effect* e, double t0, double v0, double t1, double v1) {
    host().setKeys(e, "wmpProgress", {Key{t0, {v0}}, Key{t1, {v1}}});
}

}  // namespace

TEST(ofx_describe_and_param_freeze) {
    Host& h = host();
    CHECK(std::string(h.plugin()->pluginIdentifier) == "com.wronsvp.ofx.MotionPath");
    CHECK(h.plugin()->pluginVersionMajor == 0);
    mock::PropertySet& d = h.descriptor()->props;
    CHECK(d.getString(kOfxImageEffectPropSupportedContexts) == kOfxImageEffectContextFilter);
    CHECK(d.dimension(kOfxImageEffectPropSupportedContexts) == 1);
    CHECK(d.dimension(kOfxImageEffectPropSupportedPixelDepths) == 3);
    CHECK(d.getString(kOfxImageEffectPluginRenderThreadSafety) == kOfxImageEffectRenderFullySafe);
    CHECK(d.getInt(kOfxImageEffectPropSupportsTiles) == 1);
    CHECK(d.getInt(kOfxImageEffectPropSupportsMultiResolution) == 1);
    CHECK(d.getInt(kOfxImageEffectPropTemporalClipAccess) == 0);
    CHECK(d.getPointer(kOfxImageEffectPluginPropOverlayInteractV2) != nullptr);
    CHECK(d.getString(kOfxPropLabel).find("DEV") != std::string::npos);  // dev build is labelled

    // FROZEN parameter identifiers and types: renaming breaks saved projects.
    const std::vector<std::pair<std::string, std::string>> expected = {
        {"wmpGrpPath", kOfxParamTypeGroup},        {"wmpPathData", kOfxParamTypeString},
        {"wmpPathInfo", kOfxParamTypeString},      {"wmpReversePath", kOfxParamTypePushButton},
        {"wmpShowOverlay", kOfxParamTypeBoolean},  {"wmpOverlayTool", kOfxParamTypeChoice},
        {"wmpGrpTiming", kOfxParamTypeGroup},      {"wmpProgress", kOfxParamTypeDouble},
        {"wmpStartOffset", kOfxParamTypeDouble},   {"wmpReverse", kOfxParamTypeBoolean},
        {"wmpEndBehavior", kOfxParamTypeChoice},   {"wmpSpeedMode", kOfxParamTypeChoice},
        {"wmpEasing", kOfxParamTypeChoice},        {"wmpEaseP1", kOfxParamTypeDouble2D},
        {"wmpEaseP2", kOfxParamTypeDouble2D},      {"wmpGrpTransform", kOfxParamTypeGroup},
        {"wmpPivot", kOfxParamTypeDouble2D},       {"wmpOffset", kOfxParamTypeDouble2D},
        {"wmpUniformScale", kOfxParamTypeBoolean}, {"wmpScale", kOfxParamTypeDouble2D},
        {"wmpRotation", kOfxParamTypeDouble},      {"wmpOpacity", kOfxParamTypeDouble},
        {"wmpGrpOrientation", kOfxParamTypeGroup}, {"wmpOrient", kOfxParamTypeBoolean},
        {"wmpOrientOffset", kOfxParamTypeDouble},  {"wmpOrientMode", kOfxParamTypeChoice},
        {"wmpOrientSmooth", kOfxParamTypeDouble},  {"wmpGrpBlur", kOfxParamTypeGroup},
        {"wmpBlur", kOfxParamTypeBoolean},         {"wmpShutterAngle", kOfxParamTypeDouble},
        {"wmpShutterPhase", kOfxParamTypeDouble},  {"wmpBlurAdaptive", kOfxParamTypeBoolean},
        {"wmpBlurSamples", kOfxParamTypeInteger},  {"wmpBlurMaxSamples", kOfxParamTypeInteger},
        {"wmpBlurPreview", kOfxParamTypeChoice},   {"wmpGrpPresets", kOfxParamTypeGroup},
        {"wmpPreset", kOfxParamTypeChoice},        {"wmpApplyPreset", kOfxParamTypePushButton},
        {"wmpGrpDiagnostics", kOfxParamTypeGroup}, {"wmpHostInfo", kOfxParamTypeString},
    };
    const auto& params = h.contextDescriptor()->params.params;
    CHECK(params.size() == expected.size());
    for (std::size_t i = 0; i < std::min(params.size(), expected.size()); ++i) {
        CHECK(params[i]->name == expected[i].first);
        CHECK(params[i]->type == expected[i].second);
    }
    // Choice option counts (values are stored by index: append-only).
    const std::vector<std::pair<std::string, int>> choices = {{"wmpOverlayTool", 3}, {"wmpEndBehavior", 4},
                                                              {"wmpSpeedMode", 2},   {"wmpEasing", 6},
                                                              {"wmpOrientMode", 2},  {"wmpBlurPreview", 3},
                                                              {"wmpPreset", 7}};
    for (const auto& c : choices)
        CHECK(h.contextDescriptor()->params.find(c.first)->props.dimension(kOfxParamPropChoiceOption) == c.second);
    const mock::Param* data = h.contextDescriptor()->params.find("wmpPathData");
    CHECK(data->props.getInt(kOfxParamPropSecret) == 1);
    CHECK(data->props.getInt(kOfxParamPropAnimates) == 0);
    CHECK(data->props.getInt(kOfxParamPropPersistant) == 1);
    CHECK(wmp::loadDocument(data->props.getString(kOfxParamPropDefault)).status == wmp::LoadStatus::Ok);
    CHECK(h.contextDescriptor()->params.find("wmpPathInfo")->props.getInt(kOfxParamPropPersistant) == 0);
    // Normalized 2D params must be Plain so hosts never convert or flip them.
    CHECK(h.contextDescriptor()->params.find("wmpPivot")->props.getString(kOfxParamPropDoubleType) ==
          kOfxParamDoubleTypePlain);
    CHECK(h.contextDescriptor()->clips.count(kOfxImageEffectSimpleSourceClipName) == 1);
}

TEST(ofx_straight_path_start_mid_end) {
    auto e = newInstance();
    linearProgress(e.get(), 0, 0.0, 100, 1.0);
    const double xs[3] = {0.2 * W, 0.5 * W, 0.8 * W};
    const double times[3] = {0, 50, 100};
    for (int i = 0; i < 3; ++i) {
        Image out = renderFrame(e.get(), times[i]);
        const Centroid c = centroid(out);
        CHECK_NEAR(c.x, xs[i], 0.05);
        CHECK_NEAR(c.y, H / 2.0, 0.05);
        CHECK_NEAR(c.mass, 1600.0, 1.0);  // nothing lost or doubled
    }
    host().destroyInstance(e);
}

TEST(ofx_constant_speed_on_bezier_preset) {
    auto e = newInstance();
    host().setInt(e.get(), "wmpPreset", static_cast<int>(wmp::PresetId::SCurve));
    host().pressButton(e.get(), "wmpApplyPreset");
    linearProgress(e.get(), 0, 0.0, 100, 1.0);
    std::vector<Centroid> pts;
    for (int f = 0; f <= 100; f += 5) {
        Image img = renderFrame(e.get(), f);
        pts.push_back(centroid(img));
    }
    // Equal time steps travel (almost) equal on-screen chord lengths along a
    // gently curving path; Bézier-t stepping would vary several-fold.
    double mn = 1e9, mx = 0;
    for (std::size_t i = 1; i < pts.size(); ++i) {
        const double d = std::hypot(pts[i].x - pts[i - 1].x, pts[i].y - pts[i - 1].y);
        mn = std::min(mn, d);
        mx = std::max(mx, d);
    }
    CHECK(mx / mn < 1.03);
    host().destroyInstance(e);
}

TEST(ofx_fractional_frame_rates) {
    for (double fps : {24000.0 / 1001.0, 30000.0 / 1001.0, 60000.0 / 1001.0}) {
        auto e = newInstance(fps);
        // OFX time is in frames: a 10-second move ends at a fractional frame.
        const double endFrame = 10.0 * fps;
        linearProgress(e.get(), 0.0, 0.0, endFrame, 1.0);
        const double k = std::round(5.0 * fps);
        Image out = renderFrame(e.get(), k);
        CHECK_NEAR(centroid(out).x, 0.2 * W + 0.6 * W * (k / endFrame), 0.05);
        // Sub-frame times (field renders / motion-blur samples) work as well.
        Image half = renderFrame(e.get(), k + 0.5);
        CHECK_NEAR(centroid(half).x, 0.2 * W + 0.6 * W * ((k + 0.5) / endFrame), 0.05);
        host().destroyInstance(e);
    }
}

TEST(ofx_save_reopen_is_identical) {
    auto e = newInstance();
    host().setInt(e.get(), "wmpPreset", static_cast<int>(wmp::PresetId::Circle));
    host().pressButton(e.get(), "wmpApplyPreset");
    linearProgress(e.get(), 0, 0.0, 120, 1.0);
    host().setInt(e.get(), "wmpOrient", 1);
    host().setDouble(e.get(), "wmpRotation", 15.0);
    host().setDouble2(e.get(), "wmpScale", 80.0, 80.0);
    host().setInt(e.get(), "wmpBlur", 1);
    Image before = renderFrame(e.get(), 37);
    const std::string saved = host().saveParams(e.get());
    host().destroyInstance(e);
    auto reopened = newInstance();
    host().loadParams(reopened.get(), saved);
    Image after = renderFrame(reopened.get(), 37);
    CHECK(before.data == after.data);
    host().destroyInstance(reopened);
}

TEST(ofx_copied_event_is_independent) {
    auto a = newInstance();
    linearProgress(a.get(), 0, 0.0, 100, 1.0);
    const std::string state = host().saveParams(a.get());
    auto b = newInstance();
    host().loadParams(b.get(), state);  // "copy event"
    const Image a0 = renderFrame(a.get(), 30);
    host().setInt(b.get(), "wmpPreset", static_cast<int>(wmp::PresetId::Zigzag));
    host().pressButton(b.get(), "wmpApplyPreset");
    const Image a1 = renderFrame(a.get(), 30);
    const Image b1 = renderFrame(b.get(), 30);
    CHECK(a0.data == a1.data);  // editing the copy never touches the original
    CHECK(a1.data != b1.data);
    CHECK(host().getString(a.get(), "wmpPathData") != host().getString(b.get(), "wmpPathData"));
    host().destroyInstance(a);
    host().destroyInstance(b);
}

TEST(ofx_concurrent_instances_are_deterministic) {
    std::vector<std::unique_ptr<mock::Effect>> inst;
    const wmp::PresetId ids[3] = {wmp::PresetId::Arc, wmp::PresetId::Spiral, wmp::PresetId::Ellipse};
    for (wmp::PresetId id : ids) {
        inst.push_back(newInstance());
        host().setInt(inst.back().get(), "wmpPreset", static_cast<int>(id));
        host().pressButton(inst.back().get(), "wmpApplyPreset");
        linearProgress(inst.back().get(), 0, 0.0, 60, 1.0);
        host().setInt(inst.back().get(), "wmpOrient", 1);
    }
    // Baseline, sequential.
    std::vector<std::vector<unsigned char>> base;
    for (auto& e : inst)
        for (int f = 0; f < 6; ++f) base.push_back(renderFrame(e.get(), f * 10).data);
    // Same frames from 6 host threads at once (plug-in declares FullySafe).
    std::vector<std::vector<unsigned char>> got(base.size());
    std::vector<std::thread> threads;
    for (std::size_t job = 0; job < base.size(); ++job) {
        threads.emplace_back([&, job] {
            mock::RenderRequest rq;
            rq.time = static_cast<double>(job % 6) * 10;
            rq.window[2] = W;
            rq.window[3] = H;
            ImageSpec spec;
            spec.width = W;
            spec.height = H;
            Image out;
            host().render(inst[job / 6].get(), rq, spec, out);
            got[job] = out.data;
        });
    }
    for (auto& t : threads) t.join();
    for (std::size_t i = 0; i < base.size(); ++i) CHECK(base[i] == got[i]);
    for (auto& e : inst) {
        CHECK(e->params.find("wmpPathData")->concurrentStringGets.load() == 0);
        host().destroyInstance(e);
    }
}

TEST(ofx_tiles_and_region_of_interest) {
    auto e = newInstance();
    host().setInt(e.get(), "wmpPreset", static_cast<int>(wmp::PresetId::Arc));
    host().pressButton(e.get(), "wmpApplyPreset");
    linearProgress(e.get(), 0, 0.0, 100, 1.0);
    host().setDouble(e.get(), "wmpRotation", 33.0);
    host().setDouble2(e.get(), "wmpScale", 70.0, 70.0);
    const Image full = renderFrame(e.get(), 40);
    // Tiles, each fetching ONLY the source region the plug-in requested.
    host().cropSourceToRequest = true;
    Image tiled;
    tiled.data.assign(full.data.size(), 0);
    for (int ty = 0; ty < H; ty += 97)
        for (int tx = 0; tx < W; tx += 131) {
            RenderRequest rq;
            rq.time = 40;
            rq.window[0] = tx;
            rq.window[1] = ty;
            rq.window[2] = std::min(W, tx + 131);
            rq.window[3] = std::min(H, ty + 97);
            ImageSpec spec;
            spec.width = W;
            spec.height = H;
            Image out;
            CHECK(host().render(e.get(), rq, spec, out) == kOfxStatOK);
            for (int y = rq.window[1]; y < rq.window[3]; ++y)
                std::memcpy(&tiled.data[(static_cast<std::size_t>(y) * W + tx) * 16],
                            &out.data[(static_cast<std::size_t>(y) * W + tx) * 16],
                            static_cast<std::size_t>(rq.window[2] - tx) * 16);
            // Nothing written outside the window (sentinel bytes intact).
            if (tx > 0) CHECK(out.data[(static_cast<std::size_t>(rq.window[1]) * W + tx - 1) * 16] == 0x5A);
        }
    host().cropSourceToRequest = false;
    CHECK(tiled.data == full.data);
    // RoD covers the moved image.
    mock::PropertySet in, out;
    in.setDouble(kOfxPropTime, 40);
    in.setDouble(kOfxImageEffectPropRenderScale, 1.0, 0);
    in.setDouble(kOfxImageEffectPropRenderScale, 1.0, 1);
    CHECK(host().action(kOfxImageEffectActionGetRegionOfDefinition, e.get(), &in, &out) == kOfxStatOK);
    Image f2 = renderFrame(e.get(), 40);
    const Centroid c = centroid(f2);
    CHECK(out.getDouble(kOfxImageEffectPropRegionOfDefinition, 0) <= c.x);
    CHECK(out.getDouble(kOfxImageEffectPropRegionOfDefinition, 2) >= c.x);
    host().destroyInstance(e);
}

TEST(ofx_render_scale_and_pixel_aspect) {
    auto e = newInstance();
    linearProgress(e.get(), 0, 0.0, 100, 1.0);
    for (double rs : {0.5, 0.25}) {
        Image out = renderFrame(e.get(), 50, rs);
        const Centroid c = centroid(out);
        CHECK_NEAR(c.x, 0.5 * W * rs, 0.05);
        CHECK_NEAR(c.y, 0.5 * H * rs, 0.05);
        CHECK_NEAR(c.mass, 1600.0 * rs * rs, 1.0);
    }
    host().destroyInstance(e);
    // Anamorphic project (PAR 2): normalized positions keep their place in pixels.
    auto a = newInstance(30.0, 2.0);
    linearProgress(a.get(), 0, 0.0, 100, 1.0);
    Image out = renderFrame(a.get(), 25);
    CHECK_NEAR(centroid(out).x, (0.2 + 0.6 * 0.25) * W, 0.05);
    host().destroyInstance(a);
}

TEST(ofx_bgra_byte_straight_alpha) {
    Host& h = host();
    auto keep = h.sourcePixel;
    h.sourcePixel = [](double px, double py, double, float c[4]) {
        const bool in = std::fabs(px - W / 2.0) < 20.0 && std::fabs(py - H / 2.0) < 20.0;
        const float a = in ? 1.0f : 0.0f;
        c[0] = 1.0f * a;
        c[1] = 0.5f * a;
        c[2] = 0.0f;
        c[3] = a;
    };
    auto e = newInstance();
    h.setDouble2(e.get(), "wmpOffset", 0.3 / W, 0.0);  // sub-pixel shift -> soft edges
    h.setDouble(e.get(), "wmpProgress", 0.5);
    ImageSpec spec;
    spec.width = W;
    spec.height = H;
    spec.depth = kOfxBitDepthByte;
    spec.premult = kOfxImageUnPreMultiplied;
    spec.bgra = true;
    Image out = renderFrame(e.get(), 0, 1.0, spec);
    const unsigned char* centre = out.b(W / 2, H / 2);
    CHECK(centre[0] == 0 && centre[1] == 128 && centre[2] == 255 && centre[3] == 255);  // B G R A
    // Left edge column is partially covered: straight colour stays orange (no halo).
    int partial = 0;
    for (int x = W / 2 - 22; x < W / 2 - 17; ++x) {
        const unsigned char* p = out.b(x, H / 2);
        if (p[3] > 10 && p[3] < 245) {
            ++partial;
            CHECK(std::abs(p[2] - 255) <= 1 && std::abs(p[1] - 128) <= 2 && p[0] <= 1);
        }
    }
    CHECK(partial >= 1);
    h.sourcePixel = keep;
    h.destroyInstance(e);
}

TEST(ofx_corrupt_and_future_data_are_safe) {
    Host& h = host();
    auto e = newInstance();
    h.setString(e.get(), "wmpPathData", "{\"format\":\"wron.motionpath\",\"version\":1,\"path\":{\"points\":[{\"p\":[");
    CHECK(h.getString(e.get(), "wmpPathInfo").find("Invalid") != std::string::npos);
    Image out = renderFrame(e.get(), 0);
    CHECK(h.lastWasIdentity());  // no usable path + neutral transform = untouched source
    CHECK_NEAR(centroid(out).x, W / 2.0, 0.01);
    const std::string corrupt = h.getString(e.get(), "wmpPathData");
    h.pressButton(e.get(), "wmpReversePath");
    CHECK(h.getString(e.get(), "wmpPathData") == corrupt);  // never overwritten
    const std::string future = "{\"format\":\"wron.motionpath\",\"version\":99,\"path\":{},\"x\":1}";
    h.setString(e.get(), "wmpPathData", future);
    CHECK(h.getString(e.get(), "wmpPathInfo").find("newer") != std::string::npos);
    h.pressButton(e.get(), "wmpApplyPreset");
    CHECK(h.getString(e.get(), "wmpPathData") == future);
    h.destroyInstance(e);
}

TEST(ofx_buttons_write_once_inside_undo_block) {
    Host& h = host();
    auto e = newInstance();
    const std::size_t editsBefore = e->params.editLabels.size();
    h.setInt(e.get(), "wmpPreset", static_cast<int>(wmp::PresetId::Circle));
    h.pressButton(e.get(), "wmpApplyPreset");
    CHECK(e->params.editLabels.size() == editsBefore + 1);
    CHECK(e->params.editLabels.back() == "Load Preset Path");
    CHECK(e->params.setsOutsideEdit == 0);
    const wmp::LoadResult r = wmp::loadDocument(h.getString(e.get(), "wmpPathData"));
    CHECK(r.status == wmp::LoadStatus::Ok && r.doc.path.closed && r.doc.path.points.size() == 4);
    CHECK(h.getString(e.get(), "wmpPathInfo").find("4 points, closed") != std::string::npos);
    h.pressButton(e.get(), "wmpReversePath");
    CHECK(e->params.editLabels.back() == "Reverse Path Direction");
    const wmp::LoadResult rr = wmp::loadDocument(h.getString(e.get(), "wmpPathData"));
    CHECK(rr.doc.path.points[0].id == r.doc.path.points[0].id);  // closed loop keeps its start
    CHECK(rr.doc.path.points[1].id == r.doc.path.points[3].id);
    h.destroyInstance(e);
}

TEST(ofx_identity_and_abort) {
    Host& h = host();
    auto e = newInstance();
    h.setString(e.get(), "wmpPathData", "{\"format\":\"wron.motionpath\",\"version\":1,\"path\":{\"points\":[]}}");
    renderFrame(e.get(), 3);
    CHECK(h.lastWasIdentity());
    h.setDouble(e.get(), "wmpRotation", 10.0);
    renderFrame(e.get(), 3);
    CHECK(!h.lastWasIdentity());
    e->abortFlag = true;
    renderFrame(e.get(), 4);  // returns promptly, releases everything
    e->abortFlag = false;
    CHECK(h.liveImages.load() == 0);
    h.destroyInstance(e);
}

TEST(ofx_motion_blur_follows_shutter) {
    Host& h = host();
    auto e = newInstance();
    linearProgress(e.get(), 0, 0.0, 10, 1.0);  // fast: 38.4 px per frame
    auto softPixels = [&](double time) {
        Image out = renderFrame(e.get(), time);
        int soft = 0;
        for (int x = 0; x < W; ++x) {
            float p[4];
            mock::readPremult(out, x, H / 2, p);
            if (p[3] > 0.02f && p[3] < 0.98f) ++soft;
        }
        return soft;
    };
    CHECK(softPixels(5) <= 2);
    h.setInt(e.get(), "wmpBlur", 1);
    const int s180 = softPixels(5);
    h.setDouble(e.get(), "wmpShutterAngle", 360.0);
    h.setDouble(e.get(), "wmpShutterPhase", -180.0);
    const int s360 = softPixels(5);
    CHECK(s180 > 30);
    CHECK(s360 > s180 + 25);
    // Coverage is conserved by blurring.
    Image out = renderFrame(e.get(), 5);
    CHECK_NEAR(centroid(out).mass, 1600.0, 2.0);
    h.destroyInstance(e);
}

TEST(ofx_overlay_draws_and_edits_with_one_undo_step) {
    Host& h = host();
    auto e = newInstance();
    auto it = h.createInteract(e.get());
    mock::PropertySet in;
    in.setDouble(kOfxPropTime, 0);
    in.setDouble(kOfxInteractPropPixelScale, 1.0, 0);
    in.setDouble(kOfxInteractPropPixelScale, 1.0, 1);
    in.setPointer(kOfxInteractPropDrawContext, reinterpret_cast<void*>(0x1));
    h.drawCalls.clear();
    CHECK(h.interactAction(it.get(), kOfxInteractActionDraw, in) == kOfxStatOK);
    bool strip = false;
    for (const auto& c : h.drawCalls)
        if (c.primitive == kOfxDrawPrimitiveLineStrip && c.count >= 49) strip = true;
    CHECK(strip);
    // Drag the first anchor (normalized (0.2,0.5) -> canonical (128,180)) to (100,150).
    const std::size_t edits = e->params.editLabels.size();
    in.setDouble(kOfxInteractPropPenPosition, 128.0, 0);
    in.setDouble(kOfxInteractPropPenPosition, 180.0, 1);
    CHECK(h.interactAction(it.get(), kOfxInteractActionPenDown, in) == kOfxStatOK);
    for (int i = 1; i <= 10; ++i) {
        in.setDouble(kOfxInteractPropPenPosition, 128.0 - 2.8 * i, 0);
        in.setDouble(kOfxInteractPropPenPosition, 180.0 - 3.0 * i, 1);
        CHECK(h.interactAction(it.get(), kOfxInteractActionPenMotion, in) == kOfxStatOK);
    }
    CHECK(e->params.editLabels.size() == edits);  // nothing written during the drag
    CHECK(h.interactAction(it.get(), kOfxInteractActionPenUp, in) == kOfxStatOK);
    CHECK(e->params.editLabels.size() == edits + 1);
    CHECK(e->params.editLabels.back() == "Move Path Point");
    wmp::LoadResult r = wmp::loadDocument(h.getString(e.get(), "wmpPathData"));
    CHECK_NEAR(r.doc.path.points[0].p.x, 100.0 / W, 1e-9);
    CHECK_NEAR(r.doc.path.points[0].p.y, 1.0 - 150.0 / H, 1e-9);  // canonical Y up -> normalized Y down
    // Add Point tool on the curve: one more point, shape unchanged.
    const wmp::Path before = r.doc.path;
    h.setInt(e.get(), "wmpOverlayTool", 1);
    wmp::PathGeometry g(before, {double(W) / H, 1e-9});
    const wmp::Vec2 mid = g.toNormalized(g.sampleAtDistance(g.length() * 0.5, 1, false).pos);
    in.setDouble(kOfxInteractPropPenPosition, mid.x * W, 0);
    in.setDouble(kOfxInteractPropPenPosition, (1.0 - mid.y) * H, 1);
    CHECK(h.interactAction(it.get(), kOfxInteractActionPenDown, in) == kOfxStatOK);
    r = wmp::loadDocument(h.getString(e.get(), "wmpPathData"));
    CHECK(r.doc.path.points.size() == before.points.size() + 1);
    wmp::PathGeometry g2(r.doc.path, {double(W) / H, 1e-9});
    CHECK_NEAR(g2.length(), g.length(), 1e-9);
    // Delete key removes the selected (new) point; unrelated keys pass through to the host.
    h.setInt(e.get(), "wmpOverlayTool", 0);
    mock::PropertySet key;
    key.setDouble(kOfxPropTime, 0);
    key.setInt(kOfxPropKeySym, kOfxKey_Delete);
    CHECK(h.interactAction(it.get(), kOfxInteractActionKeyDown, key) == kOfxStatOK);
    CHECK(wmp::loadDocument(h.getString(e.get(), "wmpPathData")).doc.path.points.size() == before.points.size());
    key.setInt(kOfxPropKeySym, kOfxKey_space);
    CHECK(h.interactAction(it.get(), kOfxInteractActionKeyDown, key) == kOfxStatReplyDefault);
    CHECK(it->redraws.load() > 0);
    h.destroyInstance(e);
}

TEST(ofx_resources_released_and_unload) {
    Host& h = host();
    CHECK(h.liveImages.load() == 0);
    CHECK(h.liveThreads.load() == 0);
    CHECK(h.maxThreads.load() > 1);  // rendering was actually parallel
    h.unload();                       // Unload action + dlclose
    CHECK(h.plugin() == nullptr);
}
