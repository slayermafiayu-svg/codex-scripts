// Wron Motion Path — per-frame render planning (motion blur, RoD, RoI).
//
// Motion blur samples the *transform* over the shutter interval. Every sample
// re-evaluates all animated parameters (Progress, easing, rotation, scale,
// opacity, ...) at its own sub-frame time, so easing and loops are evaluated
// at the right time; samples are averaged in premultiplied space.
//   * Shutter interval (frames): [t + phase/360, t + (phase + angle)/360].
//     angle 180 / phase -90 is a centred half-frame shutter.
//   * Sample i of N sits at the centre of the i-th of N equal sub-intervals.
//   * Adaptive mode measures how far the source corners travel on screen
//     (output pixels) across the shutter, using 16 probe intervals, and picks
//     N = ceil(travel / spacing), clamped to [2, maxSamples]; a pose that moves
//     less than 0.05 px renders once (no blur cost when static).
//   * A discontinuity (open-path Loop wrap, travel-direction flip) is never
//     measured as motion: probe pairs whose continuity keys differ are
//     skipped, and samples on each side render at their true positions, so
//     no streak is drawn between the two path ends.
#pragma once

#include <cstdint>
#include <functional>
#include <vector>

#include "wmp/render.h"
#include "wmp/transform.h"

namespace wmp {

inline constexpr int kMaxShutterSamples = 256;

enum class RenderQuality { Draft, Preview, Good, Best };
enum class PreviewBlur { Full, Reduced, Off };

struct ShutterSettings {
    bool enabled = false;
    double angleDeg = 180.0;   // 0..720
    double phaseDeg = -90.0;   // -360..360
    int samples = 16;          // fixed count (adaptive off)
    bool adaptive = true;
    int maxSamples = 64;       // adaptive upper bound
    PreviewBlur preview = PreviewBlur::Reduced;
};

struct RenderContext {
    FrameRect frame;            // project frame, canonical
    double renderScaleX = 1.0, renderScaleY = 1.0;
    double srcPar = 1.0, dstPar = 1.0;
    RectD srcRect;              // source region of definition, canonical
    RenderQuality quality = RenderQuality::Good;
    bool previewRender = false; // host signalled a preview (draft quality or render scale < 1)
};

struct SamplePose {
    Affine srcToOut;            // source canonical -> output canonical
    double opacity = 1.0;
    std::int64_t continuityKey = 0;
};

using PoseAtTime = std::function<SamplePose(double time)>;

struct FramePlan {
    std::vector<double> times;
    std::vector<SamplePose> poses;
    double travelPx = 0.0;      // measured corner travel across the shutter (0 when blur off)
    bool blurActive = false;
};

FramePlan planFrame(double time, const ShutterSettings& shutter, const PoseAtTime& poseAt, const RenderContext& ctx);

// True when the frame is a pure pass-through (one sample, identity, opaque 100 %).
bool isIdentityPlan(const FramePlan& plan, const RenderContext& ctx);

// Canonical bounds of everything the plan can draw.
RectD planRegionOfDefinition(const FramePlan& plan, const RenderContext& ctx);

// Canonical source region needed to render `outWindow` (canonical).
RectD planRegionOfInterest(const FramePlan& plan, const RectD& outWindow, const RenderContext& ctx);

// Builds the render job for an output window in pixel coordinates.
RenderJob buildRenderJob(const FramePlan& plan, SourceTexture& texture, const ImageView& dst, const RectI& window,
                         const RenderContext& ctx);

}  // namespace wmp
