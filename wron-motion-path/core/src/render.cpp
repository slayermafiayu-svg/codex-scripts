#include "wmp/render.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

#if defined(__SSE2__) || defined(_M_X64) || defined(_M_AMD64)
#include <emmintrin.h>
#define WMP_HAVE_SSE2 1
#endif

namespace wmp {

namespace {

// 4-float RGBA vector: SSE2 on x64 (the only VEGAS target), scalar elsewhere.
#ifdef WMP_HAVE_SSE2
using V4 = __m128;
inline V4 v4zero() { return _mm_setzero_ps(); }
inline V4 v4load(const float* p) { return _mm_loadu_ps(p); }
inline void v4store(float* p, V4 v) { _mm_storeu_ps(p, v); }
inline V4 v4set1(float f) { return _mm_set1_ps(f); }
inline V4 v4add(V4 a, V4 b) { return _mm_add_ps(a, b); }
inline V4 v4sub(V4 a, V4 b) { return _mm_sub_ps(a, b); }
inline V4 v4mul(V4 a, V4 b) { return _mm_mul_ps(a, b); }
#else
struct V4 {
    float v[4];
};
inline V4 v4zero() { return {{0, 0, 0, 0}}; }
inline V4 v4load(const float* p) { return {{p[0], p[1], p[2], p[3]}}; }
inline void v4store(float* p, V4 a) { p[0] = a.v[0]; p[1] = a.v[1]; p[2] = a.v[2]; p[3] = a.v[3]; }
inline V4 v4set1(float f) { return {{f, f, f, f}}; }
inline V4 v4add(V4 a, V4 b) { return {{a.v[0] + b.v[0], a.v[1] + b.v[1], a.v[2] + b.v[2], a.v[3] + b.v[3]}}; }
inline V4 v4sub(V4 a, V4 b) { return {{a.v[0] - b.v[0], a.v[1] - b.v[1], a.v[2] - b.v[2], a.v[3] - b.v[3]}}; }
inline V4 v4mul(V4 a, V4 b) { return {{a.v[0] * b.v[0], a.v[1] * b.v[1], a.v[2] * b.v[2], a.v[3] * b.v[3]}}; }
#endif

inline int fastFloor(double v) {
    const int i = static_cast<int>(v);
    return (v < static_cast<double>(i)) ? i - 1 : i;
}

inline V4 fetchTexel(const SourceTexture::LevelView& L, int li, int lj) {
    if (li < 0 || li >= L.w || lj < 0 || lj >= L.h) return v4zero();
    return v4load(L.px + (static_cast<std::size_t>(lj) * static_cast<std::size_t>(L.w) + static_cast<std::size_t>(li)) * 4u);
}

// Bilinear sample of one level; (x, y) in level-0 pixel coordinates.
// Texels outside the level are transparent black.
inline V4 bilinear(const SourceTexture::LevelView& L, double x, double y) {
    const double fx = x * L.scale - 0.5, fy = y * L.scale - 0.5;
    if (!(fx > -1e9 && fx < 1e9 && fy > -1e9 && fy < 1e9)) return v4zero();
    const int i0 = fastFloor(fx), j0 = fastFloor(fy);
    const int li = i0 - L.x1, lj = j0 - L.y1;
    if (li < -1 || li >= L.w || lj < -1 || lj >= L.h) return v4zero();
    const V4 wx = v4set1(static_cast<float>(fx - i0));
    const V4 wy = v4set1(static_cast<float>(fy - j0));
    V4 a, b, c, d;
    if (li >= 0 && li + 1 < L.w && lj >= 0 && lj + 1 < L.h) {
        const float* p = L.px + (static_cast<std::size_t>(lj) * static_cast<std::size_t>(L.w) + static_cast<std::size_t>(li)) * 4u;
        const std::size_t stride = static_cast<std::size_t>(L.w) * 4u;
        a = v4load(p);
        b = v4load(p + 4);
        c = v4load(p + stride);
        d = v4load(p + stride + 4);
    } else {
        a = fetchTexel(L, li, lj);
        b = fetchTexel(L, li + 1, lj);
        c = fetchTexel(L, li, lj + 1);
        d = fetchTexel(L, li + 1, lj + 1);
    }
    const V4 top = v4add(a, v4mul(v4sub(b, a), wx));
    const V4 bot = v4add(c, v4mul(v4sub(d, c), wx));
    return v4add(top, v4mul(v4sub(bot, top), wy));
}

}  // namespace

RectI RectI::intersect(const RectI& o) const {
    RectI r{std::max(x1, o.x1), std::max(y1, o.y1), std::min(x2, o.x2), std::min(y2, o.y2)};
    if (r.empty()) return {};
    return r;
}

RectI RectI::unite(const RectI& o) const {
    if (empty()) return o;
    if (o.empty()) return *this;
    return {std::min(x1, o.x1), std::min(y1, o.y1), std::max(x2, o.x2), std::max(y2, o.y2)};
}

RectD RectD::unite(const RectD& o) const {
    if (empty()) return o;
    if (o.empty()) return *this;
    return {std::min(x1, o.x1), std::min(y1, o.y1), std::max(x2, o.x2), std::max(y2, o.y2)};
}

RectD transformBounds(const Affine& m, const RectD& r) {
    const Vec2 c[4] = {m.apply({r.x1, r.y1}), m.apply({r.x2, r.y1}), m.apply({r.x1, r.y2}), m.apply({r.x2, r.y2})};
    RectD o{c[0].x, c[0].y, c[0].x, c[0].y};
    for (const Vec2& p : c) {
        o.x1 = std::min(o.x1, p.x);
        o.y1 = std::min(o.y1, p.y);
        o.x2 = std::max(o.x2, p.x);
        o.y2 = std::max(o.y2, p.y);
    }
    return o;
}

RectI enclosingRect(const RectD& r) {
    constexpr double lim = 1073741824.0;  // 2^30
    auto cl = [&](double v) { return std::isfinite(v) ? std::clamp(v, -lim, lim) : 0.0; };
    return {static_cast<int>(std::floor(cl(r.x1))), static_cast<int>(std::floor(cl(r.y1))),
            static_cast<int>(std::ceil(cl(r.x2))), static_cast<int>(std::ceil(cl(r.y2)))};
}

namespace {

inline int floorDiv2(int v) { return v >= 0 ? v / 2 : -((-v + 1) / 2); }
inline int ceilDiv2(int v) { return v >= 0 ? (v + 1) / 2 : -((-v) / 2); }

inline void readPixel(const ImageView& v, const unsigned char* rowPtr, int x, float c[4]) {
    const std::ptrdiff_t idx = x - v.bounds.x1;
    c[0] = c[1] = c[2] = c[3] = 0.0f;
    switch (v.depth) {
        case PixelDepth::U8: {
            const unsigned char* p = rowPtr + idx * 4;
            for (int i = 0; i < 4; ++i) c[i] = static_cast<float>(p[i]) * (1.0f / 255.0f);
            break;
        }
        case PixelDepth::U16: {
            const std::uint16_t* p = reinterpret_cast<const std::uint16_t*>(rowPtr) + idx * 4;
            for (int i = 0; i < 4; ++i) c[i] = static_cast<float>(p[i]) * (1.0f / 65535.0f);
            break;
        }
        case PixelDepth::F32: {
            const float* p = reinterpret_cast<const float*>(rowPtr) + idx * 4;
            for (int i = 0; i < 4; ++i) c[i] = std::isfinite(p[i]) ? p[i] : 0.0f;
            break;
        }
    }
    if (v.order == ChannelOrder::BGRA) std::swap(c[0], c[2]);
    if (v.alpha == AlphaMode::Opaque) {
        c[3] = 1.0f;
    } else if (v.alpha == AlphaMode::Straight) {
        c[0] *= c[3];
        c[1] *= c[3];
        c[2] *= c[3];
    }
}

inline float toUnit(float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }

// c is premultiplied RGBA.
inline void writePixel(const ImageView& v, unsigned char* rowPtr, int x, const float in[4]) {
    float c[4] = {in[0], in[1], in[2], in[3]};
    if (v.alpha == AlphaMode::Straight) {
        if (c[3] > 1e-6f) {
            const float ia = 1.0f / c[3];
            c[0] *= ia;
            c[1] *= ia;
            c[2] *= ia;
        } else {
            c[0] = c[1] = c[2] = 0.0f;
        }
    }
    if (v.order == ChannelOrder::BGRA) std::swap(c[0], c[2]);
    const std::ptrdiff_t idx = x - v.bounds.x1;
    switch (v.depth) {
        case PixelDepth::U8: {
            unsigned char* p = rowPtr + idx * 4;
            for (int i = 0; i < 4; ++i) p[i] = static_cast<unsigned char>(toUnit(c[i]) * 255.0f + 0.5f);
            break;
        }
        case PixelDepth::U16: {
            std::uint16_t* p = reinterpret_cast<std::uint16_t*>(rowPtr) + idx * 4;
            for (int i = 0; i < 4; ++i) p[i] = static_cast<std::uint16_t>(toUnit(c[i]) * 65535.0f + 0.5f);
            break;
        }
        case PixelDepth::F32: {
            float* p = reinterpret_cast<float*>(rowPtr) + idx * 4;
            for (int i = 0; i < 4; ++i) p[i] = c[i];
            break;
        }
    }
}

// Intersects [lo,hi) with {x : lo_v <= k*x + m < hi_v}.
inline void clipLinear(double k, double m, double loV, double hiV, double& lo, double& hi) {
    if (std::fabs(k) < 1e-15) {
        if (!(m >= loV && m < hiV)) hi = lo;  // empty
        return;
    }
    double a = (loV - m) / k, b = (hiV - m) / k;
    if (a > b) std::swap(a, b);
    lo = std::max(lo, a);
    hi = std::min(hi, b);
}

}  // namespace

void SourceTexture::load(const ImageView& src, const RectI& region) {
    levels_.clear();
    Level l0;
    l0.bounds = region.intersect(src.bounds);
    if (!src.data) l0.bounds = {};
    const int w = l0.bounds.width(), h = l0.bounds.height();
    if (!l0.bounds.empty()) {
        l0.px.resize(static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * 4u);
        for (int y = l0.bounds.y1; y < l0.bounds.y2; ++y) {
            const unsigned char* row = src.row(y);
            float* out = &l0.px[static_cast<std::size_t>(y - l0.bounds.y1) * static_cast<std::size_t>(w) * 4u];
            for (int x = l0.bounds.x1; x < l0.bounds.x2; ++x, out += 4) readPixel(src, row, x, out);
        }
    }
    levels_.push_back(std::move(l0));
}

void SourceTexture::ensureLevels(int maxLevel) {
    while (!levels_.empty() && static_cast<int>(levels_.size()) <= maxLevel) {
        const Level& prev = levels_.back();
        if (prev.bounds.empty() || (prev.bounds.width() <= 1 && prev.bounds.height() <= 1)) break;
        Level next;
        next.bounds = {floorDiv2(prev.bounds.x1), floorDiv2(prev.bounds.y1), ceilDiv2(prev.bounds.x2),
                       ceilDiv2(prev.bounds.y2)};
        const int w = next.bounds.width(), h = next.bounds.height();
        const int pw = prev.bounds.width();
        next.px.assign(static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * 4u, 0.0f);
        for (int j = next.bounds.y1; j < next.bounds.y2; ++j) {
            for (int i = next.bounds.x1; i < next.bounds.x2; ++i) {
                float* o = &next.px[(static_cast<std::size_t>(j - next.bounds.y1) * static_cast<std::size_t>(w) +
                                     static_cast<std::size_t>(i - next.bounds.x1)) *
                                    4u];
                for (int dy = 0; dy < 2; ++dy) {
                    const int py = 2 * j + dy;
                    if (py < prev.bounds.y1 || py >= prev.bounds.y2) continue;
                    for (int dx = 0; dx < 2; ++dx) {
                        const int px = 2 * i + dx;
                        if (px < prev.bounds.x1 || px >= prev.bounds.x2) continue;
                        const float* s = &prev.px[(static_cast<std::size_t>(py - prev.bounds.y1) *
                                                       static_cast<std::size_t>(pw) +
                                                   static_cast<std::size_t>(px - prev.bounds.x1)) *
                                                  4u];
                        for (int c = 0; c < 4; ++c) o[c] += 0.25f * s[c];
                    }
                }
            }
        }
        levels_.push_back(std::move(next));
    }
}

SourceTexture::LevelView SourceTexture::level(int index) const {
    LevelView v;
    if (index < 0 || index >= static_cast<int>(levels_.size())) return v;
    const Level& L = levels_[static_cast<std::size_t>(index)];
    v.px = L.px.empty() ? nullptr : L.px.data();
    v.x1 = L.bounds.x1;
    v.y1 = L.bounds.y1;
    v.w = L.bounds.width();
    v.h = L.bounds.height();
    v.scale = std::ldexp(1.0, -index);
    return v;
}

void SourceTexture::sampleBilinear(int lvl, double x, double y, float out[4]) const {
    const LevelView L = level(lvl);
    if (!L.px) {
        out[0] = out[1] = out[2] = out[3] = 0.0f;
        return;
    }
    v4store(out, bilinear(L, x, y));
}

void SourceTexture::sampleTrilinear(double lod, double x, double y, float out[4]) const {
    if (!(lod > 0.0) || levels_.size() < 2) {
        sampleBilinear(0, x, y, out);
        return;
    }
    const int maxLevel = static_cast<int>(levels_.size()) - 1;
    const double fl = std::floor(lod);
    const int l0 = static_cast<int>(fl);
    if (l0 >= maxLevel) {
        sampleBilinear(maxLevel, x, y, out);
        return;
    }
    const float f = static_cast<float>(lod - fl);
    const V4 a = bilinear(level(l0), x, y);
    if (f < 1e-4f) {
        v4store(out, a);
        return;
    }
    const V4 b = bilinear(level(l0 + 1), x, y);
    v4store(out, v4add(a, v4mul(v4sub(b, a), v4set1(f))));
}

double lodFor(const Affine& outToSrc) {
    const double stretch = outToSrc.maxStretch();
    if (!(stretch > 1.0) || !std::isfinite(stretch)) return 0.0;
    const double lod = std::log2(stretch);
    // No filtering for mild minification; ramp continuously so an animated
    // scale never pops between sharp and filtered.
    if (lod < 0.25) return 0.0;
    if (lod < 1.0) return (lod - 0.25) / 0.75;
    return lod;
}

bool prepareSample(RenderSample& s, const SourceTexture& tex, const RectI& window) {
    s.coverage = {};
    if (tex.empty() || !(s.weight > 0.0f) || !s.outToSrc.isFinite()) return false;
    Affine srcToOut;
    if (!s.outToSrc.inverse(srcToOut)) return false;
    s.lod = lodFor(s.outToSrc);
    const RectI b = tex.bounds();
    const double pad = std::ldexp(1.0, static_cast<int>(std::ceil(s.lod))) + 1.0;
    const RectD src{b.x1 - pad, b.y1 - pad, b.x2 + pad, b.y2 + pad};
    s.coverage = enclosingRect(transformBounds(srcToOut, src)).intersect(window);
    return !s.coverage.empty();
}

namespace {

// Accumulates one sample into one output row (premultiplied, weighted).
inline void accumulateRow(const RenderSample& s, const SourceTexture& tex, int levels, const RectI& tb,
                          const RectI& win, int y, float* acc) {
    const Affine& m = s.outToSrc;
    const double cy = y + 0.5;
    // Source position along this row: sx = m.a*(x+0.5) + bx, sy = m.c*(x+0.5) + by.
    const double bx = m.b * cy + m.tx, by = m.d * cy + m.ty;
    const double pad = std::ldexp(1.0, static_cast<int>(std::ceil(s.lod))) + 1.0;
    double Xlo = s.coverage.x1 + 0.5, Xhi = s.coverage.x2 + 0.5;
    clipLinear(m.a, bx, tb.x1 - pad, tb.x2 + pad, Xlo, Xhi);
    clipLinear(m.c, by, tb.y1 - pad, tb.y2 + pad, Xlo, Xhi);
    if (!(Xhi > Xlo)) return;
    const int xs = std::max(s.coverage.x1, static_cast<int>(std::floor(Xlo - 0.5)));
    const int xe = std::min(s.coverage.x2, static_cast<int>(std::ceil(Xhi - 0.5)) + 1);
    // The mip level is constant per sample (affine transform).
    int l0 = 0;
    float frac = 0.0f;
    if (s.lod > 0.0 && levels > 1) {
        const double fl = std::floor(s.lod);
        l0 = static_cast<int>(fl);
        frac = static_cast<float>(s.lod - fl);
        if (l0 >= levels - 1) {
            l0 = levels - 1;
            frac = 0.0f;
        }
    }
    const SourceTexture::LevelView L0 = tex.level(l0);
    const bool tri = frac >= 1e-4f;
    const SourceTexture::LevelView L1 = tri ? tex.level(l0 + 1) : L0;
    const V4 vf = v4set1(frac);
    const V4 vw = v4set1(s.weight);
    double sx = m.a * (xs + 0.5) + bx, sy = m.c * (xs + 0.5) + by;
    float* a = acc + static_cast<std::size_t>(xs - win.x1) * 4u;
    for (int x = xs; x < xe; ++x, a += 4, sx += m.a, sy += m.c) {
        V4 c = bilinear(L0, sx, sy);
        if (tri) c = v4add(c, v4mul(v4sub(bilinear(L1, sx, sy), c), vf));
        v4store(a, v4add(v4load(a), v4mul(c, vw)));
    }
}

// Rows per band: the accumulator for the band and the source lines touched by
// one sample stay in L2 while every sample is applied to the band.
constexpr int kBandRows = 8;

}  // namespace

void renderRows(const RenderJob& job, int y0, int y1, bool (*abort)(void*), void* abortCtx) {
    const RectI& win = job.window;
    y0 = std::max(y0, win.y1);
    y1 = std::min(y1, win.y2);
    if (win.empty() || y0 >= y1 || !job.dst.data) return;
    const std::size_t rowFloats = static_cast<std::size_t>(win.width()) * 4u;
    std::vector<float> acc(rowFloats * kBandRows);
    const SourceTexture* tex = job.texture;
    const bool haveTex = tex && !tex->empty();
    const RectI tb = haveTex ? tex->bounds() : RectI{};
    const int levels = haveTex ? tex->levelCount() : 0;
    for (int band = y0; band < y1; band += kBandRows) {
        if (abort && abort(abortCtx)) return;
        const int bandEnd = std::min(y1, band + kBandRows);
        std::fill(acc.begin(), acc.end(), 0.0f);
        if (haveTex) {
            for (const RenderSample& s : job.samples) {
                const int ys = std::max(band, s.coverage.y1), ye = std::min(bandEnd, s.coverage.y2);
                for (int y = ys; y < ye; ++y)
                    accumulateRow(s, *tex, levels, tb, win, y, &acc[static_cast<std::size_t>(y - band) * rowFloats]);
            }
        }
        for (int y = band; y < bandEnd; ++y) {
            unsigned char* row = job.dst.row(y);
            const float* a = &acc[static_cast<std::size_t>(y - band) * rowFloats];
            for (int x = win.x1; x < win.x2; ++x) writePixel(job.dst, row, x, a + static_cast<std::size_t>(x - win.x1) * 4u);
        }
    }
}

void copyImage(const ImageView& src, const ImageView& dst, const RectI& window) {
    const RectI inside = window.intersect(src.bounds);
    float c[4];
    for (int y = window.y1; y < window.y2; ++y) {
        unsigned char* drow = dst.row(y);
        const bool rowIn = src.data && y >= inside.y1 && y < inside.y2;
        const unsigned char* srow = rowIn ? src.row(y) : nullptr;
        for (int x = window.x1; x < window.x2; ++x) {
            if (rowIn && x >= inside.x1 && x < inside.x2) readPixel(src, srow, x, c);
            else c[0] = c[1] = c[2] = c[3] = 0.0f;
            writePixel(dst, drow, x, c);
        }
    }
}

}  // namespace wmp
