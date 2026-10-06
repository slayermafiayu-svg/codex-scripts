#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

#include "test.h"
#include "wmp/motion.h"
#include "wmp/pipeline.h"

using namespace wmp;

namespace {

struct Image {
    RectI bounds;
    PixelDepth depth = PixelDepth::F32;
    ChannelOrder order = ChannelOrder::RGBA;
    AlphaMode alpha = AlphaMode::Premultiplied;
    std::vector<unsigned char> bytes;

    Image(RectI b, PixelDepth d, AlphaMode a = AlphaMode::Premultiplied, ChannelOrder o = ChannelOrder::RGBA)
        : bounds(b), depth(d), order(o), alpha(a) {
        bytes.assign(static_cast<std::size_t>(b.width()) * b.height() * bpp(), 0);
    }
    int bpp() const { return depth == PixelDepth::U8 ? 4 : depth == PixelDepth::U16 ? 8 : 16; }
    ImageView view() {
        ImageView v;
        v.data = bytes.data();
        v.rowBytes = static_cast<std::ptrdiff_t>(bounds.width()) * bpp();
        v.bounds = bounds;
        v.depth = depth;
        v.order = order;
        v.alpha = alpha;
        return v;
    }
    float* f(int x, int y) {
        return reinterpret_cast<float*>(bytes.data()) +
               (static_cast<std::size_t>(y - bounds.y1) * bounds.width() + (x - bounds.x1)) * 4;
    }
    unsigned char* b(int x, int y) {
        return bytes.data() + (static_cast<std::size_t>(y - bounds.y1) * bounds.width() + (x - bounds.x1)) * 4;
    }
};

// Solid rectangle [rx1,rx2)x[ry1,ry2) of colour c (straight RGBA), transparent elsewhere.
Image solidRect(RectI frame, RectI r, const float c[4]) {
    Image img(frame, PixelDepth::F32);
    for (int y = r.y1; y < r.y2; ++y)
        for (int x = r.x1; x < r.x2; ++x) {
            float* p = img.f(x, y);
            for (int k = 0; k < 3; ++k) p[k] = c[k] * c[3];
            p[3] = c[3];
        }
    return img;
}

RenderContext contextFor(int w, int h) {
    RenderContext ctx;
    ctx.frame = {0, 0, static_cast<double>(w), static_cast<double>(h)};
    ctx.srcRect = {0, 0, static_cast<double>(w), static_cast<double>(h)};
    return ctx;
}

void renderPlan(const FramePlan& plan, Image& src, Image& dst, const RenderContext& ctx, RectI window) {
    SourceTexture tex;
    const RectD roi = planRegionOfInterest(plan, {double(window.x1), double(window.y1), double(window.x2), double(window.y2)}, ctx);
    tex.load(src.view(), enclosingRect(roi));
    const RenderJob job = buildRenderJob(plan, tex, dst.view(), window, ctx);
    renderRows(job, window.y1, window.y2);
}

double alphaSum(Image& img) {
    double s = 0;
    for (int y = img.bounds.y1; y < img.bounds.y2; ++y)
        for (int x = img.bounds.x1; x < img.bounds.x2; ++x) s += img.f(x, y)[3];
    return s;
}

PoseAtTime translation(double pxPerFrameX, double y = 0.0) {
    return [=](double t) {
        SamplePose p;
        p.srcToOut = Affine::translate({pxPerFrameX * t, y});
        return p;
    };
}

}  // namespace

TEST(format_conversion_roundtrip) {
    const RectI r{0, 0, 3, 1};
    Image src(r, PixelDepth::U8, AlphaMode::Straight, ChannelOrder::BGRA);
    // pixel0: opaque orange, pixel1: half-transparent blue, pixel2: transparent
    const unsigned char px[12] = {0, 128, 255, 255, 255, 0, 0, 128, 10, 20, 30, 0};
    std::memcpy(src.bytes.data(), px, 12);
    Image f(r, PixelDepth::F32);
    copyImage(src.view(), f.view(), r);
    CHECK_NEAR(f.f(0, 0)[0], 1.0, 1e-6);         // R (was last byte of BGR)
    CHECK_NEAR(f.f(0, 0)[1], 128 / 255.0, 1e-6);
    CHECK_NEAR(f.f(1, 0)[2], 128 / 255.0, 1e-6); // blue premultiplied by alpha 128/255
    CHECK_NEAR(f.f(2, 0)[0], 0.0, 0.0);          // transparent -> premultiplied zero
    Image back(r, PixelDepth::U8, AlphaMode::Straight, ChannelOrder::BGRA);
    copyImage(f.view(), back.view(), r);
    for (int i = 0; i < 8; ++i) CHECK(back.bytes[static_cast<std::size_t>(i)] == px[i]);
    CHECK(back.bytes[11] == 0);
    // 16-bit premultiplied RGBA
    Image s16(r, PixelDepth::U16);
    copyImage(f.view(), s16.view(), r);
    Image f2(r, PixelDepth::F32);
    copyImage(s16.view(), f2.view(), r);
    for (int i = 0; i < 4; ++i) CHECK_NEAR(f2.f(1, 0)[i], f.f(1, 0)[i], 1.0 / 65535.0);
}

TEST(integer_translation_is_exact_and_border_transparent) {
    const RectI frame{0, 0, 32, 16};
    const float red[4] = {1, 0, 0, 1};
    Image src = solidRect(frame, {4, 4, 12, 12}, red);
    RenderContext ctx = contextFor(32, 16);
    FramePlan plan;
    plan.times = {0};
    SamplePose p;
    p.srcToOut = Affine::translate({5, 2});
    plan.poses = {p};
    Image dst(frame, PixelDepth::F32);
    renderPlan(plan, src, dst, ctx, frame);
    for (int y = 0; y < 16; ++y)
        for (int x = 0; x < 32; ++x) {
            const bool in = x >= 9 && x < 17 && y >= 6 && y < 14;
            CHECK_NEAR(dst.f(x, y)[3], in ? 1.0 : 0.0, 1e-6);
            CHECK_NEAR(dst.f(x, y)[0], in ? 1.0 : 0.0, 1e-6);
        }
}

TEST(subpixel_edges_have_no_dark_halo) {
    // Opaque white square on transparent black, moved by half a pixel and
    // written as STRAIGHT alpha: the edge pixels must be white with alpha 0.5,
    // not grey (which is what interpolating straight colour would give).
    const RectI frame{0, 0, 24, 24};
    const float white[4] = {1, 1, 1, 1};
    Image src = solidRect(frame, {8, 8, 16, 16}, white);
    RenderContext ctx = contextFor(24, 24);
    FramePlan plan;
    plan.times = {0};
    SamplePose p;
    p.srcToOut = Affine::translate({0.5, 0.0});
    plan.poses = {p};
    Image dst(frame, PixelDepth::F32, AlphaMode::Straight);
    renderPlan(plan, src, dst, ctx, frame);
    const float* edge = dst.f(8, 12);
    CHECK_NEAR(edge[3], 0.5, 1e-6);
    CHECK_NEAR(edge[0], 1.0, 1e-5);
    CHECK_NEAR(edge[1], 1.0, 1e-5);
    // Rotated + scaled: every pixel with alpha > 0 keeps the pure source colour.
    p.srcToOut = Affine::translate({12, 12}) * Affine::rotate(0.4) * Affine::scale(0.7, 0.9) * Affine::translate({-12, -12});
    plan.poses = {p};
    Image dst2(frame, PixelDepth::F32, AlphaMode::Straight);
    renderPlan(plan, src, dst2, ctx, frame);
    int partial = 0;
    for (int y = 0; y < 24; ++y)
        for (int x = 0; x < 24; ++x) {
            const float* q = dst2.f(x, y);
            if (q[3] > 0.01f) {
                CHECK_NEAR(q[0], 1.0, 1e-4);
                if (q[3] < 0.99f) ++partial;
            }
        }
    CHECK(partial > 10);  // anti-aliased edges exist
}

TEST(tiles_match_full_render) {
    const RectI frame{0, 0, 40, 30};
    const float c[4] = {0.2f, 0.6f, 0.9f, 0.8f};
    Image src = solidRect(frame, {5, 3, 30, 20}, c);
    RenderContext ctx = contextFor(40, 30);
    FramePlan plan;
    plan.times = {0};
    SamplePose p;
    p.srcToOut = Affine::translate({20, 15}) * Affine::rotate(0.7) * Affine::scale(0.8, 1.1) * Affine::translate({-17, -11});
    plan.poses = {p};
    Image full(frame, PixelDepth::F32);
    renderPlan(plan, src, full, ctx, frame);
    Image tiled(frame, PixelDepth::F32);
    for (int ty = 0; ty < 30; ty += 7)
        for (int tx = 0; tx < 40; tx += 9) {
            const RectI win{tx, ty, std::min(40, tx + 9), std::min(30, ty + 7)};
            renderPlan(plan, src, tiled, ctx, win);  // each tile fetches only its RoI
        }
    CHECK(full.bytes == tiled.bytes);
}

TEST(motion_blur_conserves_coverage_and_spreads) {
    const RectI frame{0, 0, 64, 8};
    const float c[4] = {1, 1, 1, 1};
    Image src = solidRect(frame, {10, 2, 14, 6}, c);
    RenderContext ctx = contextFor(64, 8);
    ShutterSettings sh;
    sh.enabled = true;
    sh.adaptive = true;
    sh.maxSamples = 256;
    const FramePlan plan = planFrame(1.0, sh, translation(20.0), ctx);  // 20 px/frame
    CHECK(plan.blurActive);
    CHECK_NEAR(plan.travelPx, 10.0, 1e-9);  // 180° shutter -> half a frame of motion
    CHECK(plan.times.size() >= 10);
    CHECK_NEAR(plan.times.front() + plan.times.back(), 2.0, 1e-12);  // centred on t
    Image dst(frame, PixelDepth::F32);
    renderPlan(plan, src, dst, ctx, frame);
    CHECK_NEAR(alphaSum(dst), 16.0, 1e-3);  // 4x4 opaque pixels worth of coverage
    // Streak spans from x = 10+15 to 14+25 (centre at +20).
    CHECK(dst.f(26, 4)[3] > 0.05f);
    CHECK(dst.f(37, 4)[3] > 0.05f);
    CHECK(dst.f(31, 4)[3] > 0.3f);
    CHECK_NEAR(dst.f(20, 4)[3], 0.0, 1e-6);
}

TEST(static_pose_renders_once_even_with_blur) {
    RenderContext ctx = contextFor(64, 64);
    ShutterSettings sh;
    sh.enabled = true;
    sh.adaptive = false;
    sh.samples = 64;
    const FramePlan plan = planFrame(3.0, sh, translation(0.0, 4.0), ctx);
    CHECK(plan.times.size() == 1);
    CHECK(!plan.blurActive);
    CHECK_NEAR(plan.times[0], 3.0, 0.0);
    // Fast motion: adaptive count grows and is capped.
    sh.adaptive = true;
    sh.maxSamples = 32;
    CHECK(planFrame(3.0, sh, translation(1000.0), ctx).times.size() == 32);
    sh.maxSamples = 500;  // hard cap
    CHECK(planFrame(3.0, sh, translation(100000.0), ctx).times.size() == static_cast<std::size_t>(kMaxShutterSamples));
    // Preview: reduced and off.
    ctx.previewRender = true;
    sh.preview = PreviewBlur::Off;
    CHECK(planFrame(3.0, sh, translation(1000.0), ctx).times.size() == 1);
    sh.preview = PreviewBlur::Reduced;
    sh.adaptive = false;
    sh.samples = 64;
    CHECK(planFrame(3.0, sh, translation(1000.0), ctx).times.size() == 16);
}

TEST(loop_wrap_inside_shutter_draws_no_streak) {
    // Open straight path from x=0.1 to x=0.9 of a 200 px frame, Loop, Progress
    // = time. At t = 1.0 the shutter straddles the wrap: half the samples are
    // near the end, half near the start, nothing in between.
    const int W = 200, H = 20;
    RenderContext ctx = contextFor(W, H);
    Path path;
    PathPoint a, b;
    a.p = {0.1, 0.5};
    b.p = {0.9, 0.5};
    path.points = {a, b};
    PathGeometry g(path, {ctx.frame.aspect(), 1e-9});
    MotionParams mp;
    mp.progressSettings.endBehavior = EndBehavior::Loop;
    mp.pivot = {0.5, 0.5};
    auto poseAt = [&](double t) {
        MotionParams q = mp;
        q.progress = t;
        const Pose pose = evaluatePose(&g, q);
        SamplePose sp;
        sp.srcToOut = poseToCanonical(pose, ctx.frame);
        sp.continuityKey = pose.continuityKey;
        return sp;
    };
    ShutterSettings sh;
    sh.enabled = true;
    sh.adaptive = true;
    sh.maxSamples = 64;
    const FramePlan plan = planFrame(1.0, sh, poseAt, ctx);
    // Travel = real motion only: 0.5 frame * 160 px/frame, NOT the 160 px jump
    // (the interval holding the wrap is bisected down to 1/16/1024 of the shutter).
    CHECK_NEAR(plan.travelPx, 80.0, 0.01);
    const float c[4] = {1, 1, 1, 1};
    Image src = solidRect({0, 0, W, H}, {98, 8, 102, 12}, c);  // small square at the centre
    Image dst({0, 0, W, H}, PixelDepth::F32);
    renderPlan(plan, src, dst, ctx, {0, 0, W, H});
    double mid = 0, ends = 0;
    for (int x = 0; x < W; ++x) {
        const double al = dst.f(x, 10)[3];
        if (x > 70 && x < 130) mid += al;
        else ends += al;
    }
    CHECK_NEAR(mid, 0.0, 1e-9);
    CHECK(ends > 1.0);
}

TEST(render_scale_and_par_keep_placement) {
    // A path point at normalized (0.75, 0.25) must land at the same relative
    // place whatever the render scale or pixel aspect ratio.
    Path path;
    PathPoint a;
    a.p = {0.75, 0.25};
    path.points = {a};
    for (double par : {1.0, 2.0, 0.9}) {
        for (double rs : {1.0, 0.5, 0.25}) {
            const int pxW = 200, pxH = 100;
            RenderContext ctx;
            ctx.frame = {0, 0, pxW * par, double(pxH)};  // canonical width includes PAR
            ctx.srcRect = {0, 0, pxW * par, double(pxH)};
            ctx.setPixelMapping(rs, rs, par, par);
            PathGeometry g(path, {ctx.frame.aspect(), 1e-9});
            MotionParams mp;
            const Pose pose = evaluatePose(&g, mp);
            const Affine m = poseToCanonical(pose, ctx.frame);
            // Source centre pixel -> output pixel.
            const Affine toPx = canonicalToPixel(rs, rs, par);
            Affine fromPx;
            toPx.inverse(fromPx);
            const Vec2 outPx = toPx.apply(m.apply(fromPx.apply({pxW * rs * 0.5, pxH * rs * 0.5})));
            CHECK_NEAR(outPx.x, pxW * rs * 0.75, 1e-9);
            CHECK_NEAR(outPx.y, pxH * rs * 0.75, 1e-9);  // Y up in pixel space: 1 - 0.25
        }
    }
}

TEST(rotation_is_isotropic_with_non_square_pixels) {
    // 90° rotation with PAR 2: a source vector of 10 display units along x
    // must become 10 display units along y (not 5 or 20).
    const double par = 2.0;
    RenderContext ctx;
    ctx.frame = {0, 0, 100 * par, 100};
    MotionParams mp;
    mp.rotationDeg = 90;
    const Affine m = poseToCanonical(evaluatePose(nullptr, mp), ctx.frame);
    const Vec2 d = m.applyLinear({10.0, 0.0});  // canonical units are display units
    CHECK_NEAR(std::fabs(d.y), 10.0, 1e-9);
    CHECK_NEAR(d.x, 0.0, 1e-9);
}

TEST(minification_uses_mips_without_aliasing) {
    // 1-px checkerboard scaled to 1/8: without filtering the result would be
    // a random mix of black/white pixels; with mips it is ~uniform grey.
    const RectI frame{0, 0, 256, 256};
    Image src(frame, PixelDepth::F32);
    for (int y = 0; y < 256; ++y)
        for (int x = 0; x < 256; ++x) {
            float* p = src.f(x, y);
            const float v = ((x + y) & 1) ? 1.0f : 0.0f;
            p[0] = p[1] = p[2] = v;
            p[3] = 1.0f;
        }
    RenderContext ctx = contextFor(256, 256);
    FramePlan plan;
    plan.times = {0};
    SamplePose p;
    p.srcToOut = Affine::translate({128, 128}) * Affine::scale(0.125, 0.125) * Affine::translate({-128.0 + 0.37, -128.0 + 0.21});
    plan.poses = {p};
    Image dst(frame, PixelDepth::F32);
    renderPlan(plan, src, dst, ctx, frame);
    double minV = 1, maxV = 0;
    for (int y = 116; y < 140; ++y)
        for (int x = 116; x < 140; ++x) {
            minV = std::min<double>(minV, dst.f(x, y)[0]);
            maxV = std::max<double>(maxV, dst.f(x, y)[0]);
        }
    CHECK(maxV - minV < 0.05);
    CHECK_NEAR(0.5 * (maxV + minV), 0.5, 0.05);
}

TEST(identity_plan_detection) {
    RenderContext ctx = contextFor(100, 100);
    FramePlan plan;
    plan.times = {0};
    SamplePose p;
    plan.poses = {p};
    CHECK(isIdentityPlan(plan, ctx));
    plan.poses[0].opacity = 0.5;
    CHECK(!isIdentityPlan(plan, ctx));
    plan.poses[0].opacity = 1.0;
    plan.poses[0].srcToOut = Affine::translate({0.01, 0});
    CHECK(!isIdentityPlan(plan, ctx));
}

TEST(field_output_maps_rows_of_one_field) {
    // A horizontal 1-px line at full-frame pixel row 6 (OFX rows, Y up) belongs
    // to the lower field (even rows) -> field row 3, and is absent from the
    // upper field render except for interpolation.
    const RectI full{0, 0, 16, 16};
    const float c[4] = {1, 1, 1, 1};
    Image src = solidRect(full, {0, 6, 16, 7}, c);
    RenderContext ctx = contextFor(16, 16);
    FramePlan plan;
    plan.times = {0};
    plan.poses = {SamplePose{}};
    ctx.setFieldOutput(false);
    Image lower({0, 0, 16, 8}, PixelDepth::F32);
    renderPlan(plan, src, lower, ctx, {0, 0, 16, 8});
    CHECK_NEAR(lower.f(5, 3)[3], 1.0, 1e-6);
    CHECK_NEAR(lower.f(5, 2)[3], 0.0, 1e-6);
    RenderContext ctxU = contextFor(16, 16);
    ctxU.setFieldOutput(true);
    Image upper({0, 0, 16, 8}, PixelDepth::F32);
    renderPlan(plan, src, upper, ctxU, {0, 0, 16, 8});
    // Upper field rows are full rows 1,3,5,7: the even-row line is not in it,
    // and the fields are never blended together.
    for (int y = 0; y < 8; ++y) CHECK_NEAR(upper.f(5, y)[3], 0.0, 1e-6);
}

TEST(one_axis_squash_keeps_other_axis_sharp) {
    // Vertical 1-px stripes, squashed to 25 % in Y only: stripes must stay
    // crisp horizontally (isotropic mip selection would blur them to grey).
    const RectI frame{0, 0, 64, 64};
    Image src(frame, PixelDepth::F32);
    for (int y = 0; y < 64; ++y)
        for (int x = 0; x < 64; ++x) {
            float* p = src.f(x, y);
            const float v = (x & 1) ? 1.0f : 0.0f;
            p[0] = p[1] = p[2] = v;
            p[3] = 1.0f;
        }
    RenderContext ctx = contextFor(64, 64);
    FramePlan plan;
    plan.times = {0};
    SamplePose p;
    p.srcToOut = Affine::translate({0, 32}) * Affine::scale(1.0, 0.25) * Affine::translate({0, -32});
    plan.poses = {p};
    Image dst(frame, PixelDepth::F32);
    renderPlan(plan, src, dst, ctx, frame);
    CHECK_NEAR(dst.f(10, 32)[0], 0.0, 1e-5);
    CHECK_NEAR(dst.f(11, 32)[0], 1.0, 1e-5);
    const FilterFootprint f = filterFor(Affine::scale(1.0, 4.0));
    CHECK(f.taps == 4);
    CHECK_NEAR(f.lod, 0.0, 0.0);
    CHECK_NEAR(std::fabs(f.tapStep.y), 1.0, 1e-12);
    // Uniform minification: no extra taps.
    CHECK(filterFor(Affine::scale(4.0, 4.0)).taps == 1);
    CHECK_NEAR(filterFor(Affine::scale(4.0, 4.0)).lod, 2.0, 1e-12);
}
