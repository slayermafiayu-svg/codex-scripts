// Wron Motion Path — CPU renderer (reference implementation).
//
// Alpha contract:
//   * The source is converted once per render into a premultiplied float
//     RGBA texture (straight inputs are premultiplied, opaque inputs get
//     alpha 1). All filtering and motion-blur accumulation happen on
//     premultiplied values, so transparent texels contribute no colour and
//     edges get no dark halo.
//   * Everything outside the source region is transparent black (0,0,0,0):
//     a moved/rotated image never smears its border pixels.
//   * The result is converted to the destination's declared alpha mode,
//     depth and channel order only when written.
// Minification uses a mip pyramid (2x2 box, origin aligned) with trilinear
// filtering, so a scaled-down moving image does not shimmer.
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "wmp/transform.h"

namespace wmp {

enum class PixelDepth { U8, U16, F32 };
enum class ChannelOrder { RGBA, BGRA };
enum class AlphaMode { Premultiplied, Straight, Opaque };

struct RectI {
    int x1 = 0, y1 = 0, x2 = 0, y2 = 0;  // [x1,x2) x [y1,y2)
    bool empty() const { return x2 <= x1 || y2 <= y1; }
    int width() const { return x2 - x1; }
    int height() const { return y2 - y1; }
    RectI intersect(const RectI& o) const;
    RectI unite(const RectI& o) const;
};

struct RectD {
    double x1 = 0, y1 = 0, x2 = 0, y2 = 0;
    bool empty() const { return !(x2 > x1) || !(y2 > y1); }
    RectD unite(const RectD& o) const;
};

// Axis-aligned bounds of rect r transformed by m.
RectD transformBounds(const Affine& m, const RectD& r);
// Smallest integer rect containing r (clamped to +-2^30).
RectI enclosingRect(const RectD& r);

struct ImageView {
    void* data = nullptr;         // address of pixel (bounds.x1, bounds.y1)
    std::ptrdiff_t rowBytes = 0;  // may be negative
    RectI bounds;
    PixelDepth depth = PixelDepth::F32;
    ChannelOrder order = ChannelOrder::RGBA;
    AlphaMode alpha = AlphaMode::Premultiplied;

    int bytesPerPixel() const { return depth == PixelDepth::U8 ? 4 : depth == PixelDepth::U16 ? 8 : 16; }
    unsigned char* row(int y) const {
        return static_cast<unsigned char*>(data) + static_cast<std::ptrdiff_t>(y - bounds.y1) * rowBytes;
    }
};

class SourceTexture {
public:
    // Converts `region` ∩ src.bounds into premultiplied float RGBA (level 0).
    void load(const ImageView& src, const RectI& region);
    // Builds levels up to `maxLevel` (no-op for levels already built).
    void ensureLevels(int maxLevel);
    int levelCount() const { return static_cast<int>(levels_.size()); }
    RectI bounds() const { return levels_.empty() ? RectI{} : levels_[0].bounds; }
    bool empty() const { return levels_.empty() || levels_[0].bounds.empty(); }

    // Coordinates are level-0 pixel coordinates (pixel (i,j) centre at i+0.5).
    void sampleBilinear(int level, double x, double y, float out[4]) const;
    void sampleTrilinear(double lod, double x, double y, float out[4]) const;

    // Raw access for the render kernel.
    struct LevelView {
        const float* px = nullptr;
        int x1 = 0, y1 = 0, w = 0, h = 0;
        double scale = 1.0;  // level-0 pixel coords -> this level's pixel coords
    };
    LevelView level(int index) const;

private:
    struct Level {
        RectI bounds;
        std::vector<float> px;  // RGBA premultiplied, row-major
    };
    std::vector<Level> levels_;
};

struct RenderSample {
    Affine outToSrc;   // output pixel coords -> source level-0 pixel coords
    float weight = 1;  // opacity / sample count
    double lod = 0.0;  // mip level of detail (0 = full resolution)
    RectI coverage;    // output pixels this sample may touch (inside the window)
};

struct RenderJob {
    const SourceTexture* texture = nullptr;
    std::vector<RenderSample> samples;
    RectI window;      // output pixels to produce
    ImageView dst;     // must contain the window
};

// Prepares coverage and LOD for a sample whose transform maps output pixels
// to source pixels. Returns false if the sample cannot contribute.
bool prepareSample(RenderSample& s, const SourceTexture& tex, const RectI& window);

// Mip level needed by a sample (for region-of-interest padding).
double lodFor(const Affine& outToSrc);

// Renders rows [y0, y1) of job.window. Bands are independent, so callers can
// split the window across threads. `abort` is polled once per row.
void renderRows(const RenderJob& job, int y0, int y1, bool (*abort)(void*) = nullptr, void* abortCtx = nullptr);

// Copies src to dst over `window` with format conversion (identity render).
void copyImage(const ImageView& src, const ImageView& dst, const RectI& window);

}  // namespace wmp
