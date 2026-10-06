// Measures CPU render cost per frame for different motion-blur sample counts.
// Usage: wmp_bench [width height] [threads]
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <vector>

#include "wmp/motion.h"
#include "wmp/pipeline.h"
#include "wmp/presets.h"

using namespace wmp;

int main(int argc, char** argv) {
    const int W = argc > 2 ? std::atoi(argv[1]) : 1920;
    const int H = argc > 2 ? std::atoi(argv[2]) : 1080;
    const int threads = argc > 3 ? std::atoi(argv[3]) : static_cast<int>(std::max(1u, std::thread::hardware_concurrency()));

    // 8-bit straight RGBA source with some structure.
    std::vector<unsigned char> src(static_cast<std::size_t>(W) * H * 4);
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            unsigned char* p = &src[(static_cast<std::size_t>(y) * W + x) * 4];
            p[0] = static_cast<unsigned char>(x * 255 / W);
            p[1] = static_cast<unsigned char>(y * 255 / H);
            p[2] = static_cast<unsigned char>(((x / 16 + y / 16) & 1) * 255);
            p[3] = 255;
        }
    std::vector<unsigned char> dst(src.size());
    ImageView sv{src.data(), static_cast<std::ptrdiff_t>(W) * 4, {0, 0, W, H}, PixelDepth::U8, ChannelOrder::RGBA,
                 AlphaMode::Straight};
    ImageView dv = sv;
    dv.data = dst.data();

    RenderContext ctx;
    ctx.frame = {0, 0, double(W), double(H)};
    ctx.srcRect = ctx.frame.w > 0 ? RectD{0, 0, double(W), double(H)} : RectD{};
    const Path path = makePreset(PresetId::SCurve, ctx.frame.aspect());
    PathGeometry geom(path, {ctx.frame.aspect(), 1e-7});
    MotionParams mp;
    mp.scale = {0.5, 0.5};
    mp.orientToPath = true;
    // Progress moves 0.2 per frame -> fast motion, blur clearly active.
    auto poseAt = [&](double t) {
        MotionParams q = mp;
        q.progress = 0.2 * t;
        const Pose pose = evaluatePose(&geom, q);
        SamplePose sp;
        sp.srcToOut = poseToCanonical(pose, ctx.frame);
        sp.opacity = pose.opacity;
        sp.continuityKey = pose.continuityKey;
        return sp;
    };

    std::printf("frame %dx%d, source scaled 50%%, %d thread(s)\n", W, H, threads);
    std::printf("%8s %10s %12s %12s\n", "samples", "travel_px", "plan_ms", "render_ms");
    const int counts[] = {1, 8, 16, 32, 64, 128, 256};
    for (int n : counts) {
        ShutterSettings sh;
        sh.enabled = n > 1;
        sh.adaptive = false;
        sh.samples = n;
        double best = 1e30, planMs = 0;
        FramePlan plan;
        for (int rep = 0; rep < 3; ++rep) {
            const auto t0 = std::chrono::steady_clock::now();
            plan = planFrame(2.5, sh, poseAt, ctx);
            const auto t1 = std::chrono::steady_clock::now();
            SourceTexture tex;
            tex.load(sv, {0, 0, W, H});
            const RenderJob job = buildRenderJob(plan, tex, dv, {0, 0, W, H}, ctx);
            std::vector<std::thread> pool;
            const int band = (H + threads - 1) / threads;
            for (int i = 0; i < threads; ++i)
                pool.emplace_back([&, i] { renderRows(job, i * band, std::min(H, (i + 1) * band)); });
            for (auto& th : pool) th.join();
            const auto t2 = std::chrono::steady_clock::now();
            best = std::min(best, std::chrono::duration<double, std::milli>(t2 - t0).count());
            planMs = std::chrono::duration<double, std::milli>(t1 - t0).count();
        }
        std::printf("%8zu %10.1f %12.3f %12.1f\n", plan.times.size(), plan.travelPx, planMs, best);
    }
    // Adaptive choice for the same motion.
    ShutterSettings sh;
    sh.enabled = true;
    sh.adaptive = true;
    sh.maxSamples = 256;
    for (RenderQuality q : {RenderQuality::Draft, RenderQuality::Preview, RenderQuality::Good, RenderQuality::Best}) {
        ctx.quality = q;
        const FramePlan plan = planFrame(2.5, sh, poseAt, ctx);
        std::printf("adaptive quality=%d -> %zu samples (travel %.1f px)\n", static_cast<int>(q), plan.times.size(),
                    plan.travelPx);
    }
    return 0;
}
