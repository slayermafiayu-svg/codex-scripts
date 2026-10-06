#include "wmp/pipeline.h"

#include <algorithm>
#include <cmath>

namespace wmp {

namespace {

constexpr int kProbeIntervals = 16;
constexpr double kStaticTravelPx = 0.05;

double spacingFor(RenderQuality q) {
    switch (q) {
        case RenderQuality::Draft: return 4.0;
        case RenderQuality::Preview: return 2.0;
        case RenderQuality::Good: return 1.0;
        case RenderQuality::Best: return 0.75;
    }
    return 1.0;
}

Affine pixelOf(const RenderContext& ctx, double par) {
    return canonicalToPixel(ctx.renderScaleX, ctx.renderScaleY, par > 0.0 ? par : 1.0);
}

// Source rectangle used to measure on-screen travel: the source RoD, or the
// frame when the RoD is unbounded / empty.
RectD probeRect(const RenderContext& ctx) {
    RectD r = ctx.srcRect;
    const double big = 1e7;
    if (r.empty() || !(r.x2 - r.x1 < big) || !(r.y2 - r.y1 < big))
        r = {ctx.frame.x, ctx.frame.y, ctx.frame.x + ctx.frame.w, ctx.frame.y + ctx.frame.h};
    return r;
}

struct TravelProbe {
    const PoseAtTime& poseAt;
    Vec2 corners[4];
    Affine toPx;
    double travel[4] = {0, 0, 0, 0};

    void add(const SamplePose& a, const SamplePose& b) {
        for (int c = 0; c < 4; ++c) {
            const double d = length(toPx.apply(b.srcToOut.apply(corners[c])) - toPx.apply(a.srcToOut.apply(corners[c])));
            if (std::isfinite(d)) travel[c] += d;
        }
    }
    // Interval containing a discontinuity: bisect so the continuous motion on
    // both sides is still measured; the jump itself never is.
    void measure(double t0, const SamplePose& p0, double t1, const SamplePose& p1, int depth) {
        if (p0.continuityKey == p1.continuityKey) {
            add(p0, p1);
            return;
        }
        if (depth >= 10) return;
        const double tm = 0.5 * (t0 + t1);
        const SamplePose pm = poseAt(tm);
        measure(t0, p0, tm, pm, depth + 1);
        measure(tm, pm, t1, p1, depth + 1);
    }
};

double measureTravel(double open, double close, const PoseAtTime& poseAt, const RenderContext& ctx) {
    const RectD r = probeRect(ctx);
    TravelProbe probe{poseAt, {{r.x1, r.y1}, {r.x2, r.y1}, {r.x1, r.y2}, {r.x2, r.y2}}, pixelOf(ctx, ctx.dstPar)};
    double tPrev = open;
    SamplePose prev = poseAt(open);
    for (int i = 1; i <= kProbeIntervals; ++i) {
        const double t = open + (close - open) * i / kProbeIntervals;
        const SamplePose cur = poseAt(t);
        probe.measure(tPrev, prev, t, cur, 0);
        prev = cur;
        tPrev = t;
    }
    return std::max({probe.travel[0], probe.travel[1], probe.travel[2], probe.travel[3]});
}

}  // namespace

FramePlan planFrame(double time, const ShutterSettings& sh, const PoseAtTime& poseAt, const RenderContext& ctx) {
    FramePlan plan;
    const bool previewOff = ctx.previewRender && sh.preview == PreviewBlur::Off;
    const double angle = std::isfinite(sh.angleDeg) ? std::clamp(sh.angleDeg, 0.0, 720.0) : 0.0;
    if (!sh.enabled || previewOff || angle <= 0.0) {
        plan.times = {time};
        plan.poses = {poseAt(time)};
        return plan;
    }
    const double phase = std::isfinite(sh.phaseDeg) ? std::clamp(sh.phaseDeg, -360.0, 360.0) : 0.0;
    const double open = time + phase / 360.0;
    const double close = open + angle / 360.0;
    plan.travelPx = measureTravel(open, close, poseAt, ctx);

    RenderQuality q = ctx.quality;
    if (ctx.previewRender && sh.preview == PreviewBlur::Full) q = RenderQuality::Good;
    const bool reduced = ctx.previewRender && sh.preview == PreviewBlur::Reduced;

    int n = 1;
    if (plan.travelPx >= kStaticTravelPx) {
        if (sh.adaptive) {
            const double spacing = spacingFor(q);
            const int maxN = std::clamp(sh.maxSamples, 2, kMaxShutterSamples);
            const double want = std::ceil(plan.travelPx / spacing);
            n = static_cast<int>(std::clamp(want, 2.0, static_cast<double>(maxN)));
        } else {
            n = std::clamp(sh.samples, 1, kMaxShutterSamples);
            if (reduced) n = std::max(1, (n + 3) / 4);
        }
    }
    plan.blurActive = n > 1;
    plan.times.reserve(static_cast<std::size_t>(n));
    plan.poses.reserve(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i) {
        const double t = (n == 1) ? time : open + (close - open) * (i + 0.5) / n;
        plan.times.push_back(t);
        plan.poses.push_back(poseAt(t));
    }
    return plan;
}

bool isIdentityPlan(const FramePlan& plan, const RenderContext& ctx) {
    if (plan.poses.size() != 1) return false;
    const SamplePose& p = plan.poses[0];
    if (!(p.opacity >= 1.0)) return false;
    const Affine& m = p.srcToOut;
    const double pxPerCanon = std::max(ctx.renderScaleX, ctx.renderScaleY) * 2.0;
    return std::fabs(m.a - 1.0) < 1e-9 && std::fabs(m.b) < 1e-9 && std::fabs(m.c) < 1e-9 &&
           std::fabs(m.d - 1.0) < 1e-9 && std::fabs(m.tx) * pxPerCanon < 1e-4 && std::fabs(m.ty) * pxPerCanon < 1e-4;
}

RectD planRegionOfDefinition(const FramePlan& plan, const RenderContext& ctx) {
    RectD rod;
    const RectD src = probeRect(ctx);
    for (const SamplePose& p : plan.poses) rod = rod.unite(transformBounds(p.srcToOut, src));
    return rod;
}

RectD planRegionOfInterest(const FramePlan& plan, const RectD& outWindow, const RenderContext& ctx) {
    RectD roi;
    const Affine srcPx = pixelOf(ctx, ctx.srcPar);
    const Affine dstPx = pixelOf(ctx, ctx.dstPar);
    Affine dstPxInv;
    dstPx.inverse(dstPxInv);
    for (const SamplePose& p : plan.poses) {
        Affine inv;
        if (!p.srcToOut.inverse(inv)) continue;
        RectD r = transformBounds(inv, outWindow);
        // Bilinear/mip footprint padding, expressed in canonical units.
        const double lod = lodFor(srcPx * inv * dstPxInv);
        const double padPx = std::ldexp(1.0, static_cast<int>(std::ceil(lod))) + 2.0;
        const double padX = padPx * ctx.srcPar / std::max(ctx.renderScaleX, 1e-9);
        const double padY = padPx / std::max(ctx.renderScaleY, 1e-9);
        r.x1 -= padX;
        r.x2 += padX;
        r.y1 -= padY;
        r.y2 += padY;
        roi = roi.unite(r);
    }
    return roi;
}

RenderJob buildRenderJob(const FramePlan& plan, SourceTexture& texture, const ImageView& dst, const RectI& window,
                         const RenderContext& ctx) {
    RenderJob job;
    job.texture = &texture;
    job.window = window;
    job.dst = dst;
    const Affine srcPx = pixelOf(ctx, ctx.srcPar);
    const Affine dstPx = pixelOf(ctx, ctx.dstPar);
    Affine dstPxInv;
    dstPx.inverse(dstPxInv);
    const double n = static_cast<double>(std::max<std::size_t>(1, plan.poses.size()));
    int maxLevel = 0;
    for (const SamplePose& p : plan.poses) {
        Affine inv;
        if (!p.srcToOut.inverse(inv)) continue;
        RenderSample s;
        s.outToSrc = srcPx * inv * dstPxInv;
        s.weight = static_cast<float>(std::clamp(p.opacity, 0.0, 1.0) / n);
        if (!prepareSample(s, texture, window)) continue;
        maxLevel = std::max(maxLevel, static_cast<int>(std::ceil(s.lod)));
        job.samples.push_back(s);
    }
    if (maxLevel > 0) texture.ensureLevels(maxLevel);
    return job;
}

}  // namespace wmp
