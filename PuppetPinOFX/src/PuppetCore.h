/*
  PuppetCore.h - host independent part of the Puppet Pin OFX plug-in.

  Everything in here works on plain pixel buffers and coordinates, so it can be
  unit tested without an OFX host:

    * buildMesh()    detects the surface (alpha > threshold, optionally grown
                     by "expansion") and covers it with a triangle mesh.
    * bindPin()      attaches a pin to the triangle under it (barycentric).
    * ArapSolver     as-rigid-as-possible deformation driven by the pins
                     (local/global iterations, banded Cholesky factorisation).
    * warpBand()     texture maps every deformed triangle from the source.
    * draw*()        burns the mesh and pin markers into an image.

  Coordinates are image pixel coordinates of the current render: x to the
  right, y up (OFX convention), pixel (i, j) covers [i, i+1) x [j, j+1).
*/
#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <vector>

namespace puppet {

struct Vec2 {
  double x = 0.0, y = 0.0;
  Vec2() = default;
  Vec2(double ax, double ay) : x(ax), y(ay) {}
};
inline Vec2 operator+(Vec2 a, Vec2 b) { return Vec2(a.x + b.x, a.y + b.y); }
inline Vec2 operator-(Vec2 a, Vec2 b) { return Vec2(a.x - b.x, a.y - b.y); }
inline Vec2 operator*(Vec2 a, double s) { return Vec2(a.x * s, a.y * s); }
inline double cross(Vec2 a, Vec2 b) { return a.x * b.y - a.y * b.x; }
inline double dot(Vec2 a, Vec2 b) { return a.x * b.x + a.y * b.y; }

inline int floorDiv(double v, double s) { return (int)std::floor(v / s); }
inline int parity(int v) { return v & 1; }  // two's complement: works for negatives

////////////////////////////////////////////////////////////////////////////////
// Images

enum PixelDepth { kDepthU8 = 0, kDepthU16 = 1, kDepthF32 = 2 };

struct ImageRef {
  void* data = nullptr;  // address of pixel (x1, y1)
  int x1 = 0, y1 = 0, x2 = 0, y2 = 0;
  std::ptrdiff_t rowBytes = 0;  // may be negative (bottom-up / top-down buffers)
  int nComp = 4;                // 1 (alpha), 3 (RGB) or 4 (RGBA / BGRA)
  PixelDepth depth = kDepthU8;
  bool straightAlpha = false;   // colours are not premultiplied by alpha
  bool bgra = false;            // channel order, only matters for overlay colours

  int width() const { return x2 - x1; }
  int height() const { return y2 - y1; }
  bool valid() const {
    return data != nullptr && x2 > x1 && y2 > y1 && (nComp == 1 || nComp == 3 || nComp == 4);
  }
  int bytesPerComponent() const { return depth == kDepthU8 ? 1 : (depth == kDepthU16 ? 2 : 4); }
  char* pixelAddress(int x, int y) const {
    return (char*)data + (std::ptrdiff_t)(y - y1) * rowBytes +
           (std::ptrdiff_t)(x - x1) * nComp * bytesPerComponent();
  }
  bool contains(int x, int y) const { return x >= x1 && x < x2 && y >= y1 && y < y2; }
};

struct RectI {
  int x1 = 0, y1 = 0, x2 = 0, y2 = 0;
  RectI() = default;
  RectI(int a, int b, int c, int d) : x1(a), y1(b), x2(c), y2(d) {}
  bool empty() const { return x2 <= x1 || y2 <= y1; }
};

inline RectI intersect(const RectI& a, const RectI& b) {
  return RectI(std::max(a.x1, b.x1), std::max(a.y1, b.y1), std::min(a.x2, b.x2),
               std::min(a.y2, b.y2));
}

template <class T> struct PixelTraits;
template <> struct PixelTraits<uint8_t> {
  static float toFloat(uint8_t v) { return v * (1.0f / 255.0f); }
  static uint8_t fromFloat(float v) {
    v = v * 255.0f + 0.5f;
    return (uint8_t)(v <= 0.0f ? 0.0f : (v >= 255.0f ? 255.0f : v));
  }
};
template <> struct PixelTraits<uint16_t> {
  static float toFloat(uint16_t v) { return v * (1.0f / 65535.0f); }
  static uint16_t fromFloat(float v) {
    v = v * 65535.0f + 0.5f;
    return (uint16_t)(v <= 0.0f ? 0.0f : (v >= 65535.0f ? 65535.0f : v));
  }
};
template <> struct PixelTraits<float> {
  static float toFloat(float v) { return v; }
  static float fromFloat(float v) { return v; }
};

////////////////////////////////////////////////////////////////////////////////
// Squared Euclidean distance transform (Felzenszwalb & Huttenlocher), used to
// grow the detected shape by "expansion" pixels.

// "Far away" marker. Finite on purpose: the lower envelope arithmetic below
// would produce NaN with real infinities.
static const float kEdtFar = 1.0e20f;

inline void edt1d(const float* f, int n, float* d, int* v, float* z) {
  const float INF = std::numeric_limits<float>::infinity();
  int k = 0;
  v[0] = 0;
  z[0] = -INF;
  z[1] = INF;
  for (int q = 1; q < n; ++q) {
    const double fq = (double)f[q] + (double)q * q;
    double s = (fq - ((double)f[v[k]] + (double)v[k] * v[k])) / (2.0 * (q - v[k]));
    while (s <= z[k]) {
      --k;
      s = (fq - ((double)f[v[k]] + (double)v[k] * v[k])) / (2.0 * (q - v[k]));
    }
    ++k;
    v[k] = q;
    z[k] = (float)s;
    z[k + 1] = INF;
  }
  k = 0;
  for (int q = 0; q < n; ++q) {
    while (z[k + 1] < (float)q) ++k;
    const float dq = (float)(q - v[k]);
    d[q] = dq * dq + f[v[k]];
  }
}

// grid: w*h values, 0 for feature pixels and kEdtFar elsewhere. Replaced in
// place by the squared distance to the nearest feature pixel.
inline void edt2d(std::vector<float>& grid, int w, int h) {
  const int n = std::max(w, h);
  std::vector<float> f(n), d(n), z(n + 1);
  std::vector<int> v(n);
  for (int x = 0; x < w; ++x) {
    for (int y = 0; y < h; ++y) f[y] = grid[(size_t)y * w + x];
    edt1d(f.data(), h, d.data(), v.data(), z.data());
    for (int y = 0; y < h; ++y) grid[(size_t)y * w + x] = d[y];
  }
  for (int y = 0; y < h; ++y) {
    float* row = &grid[(size_t)y * w];
    std::copy(row, row + w, f.begin());
    edt1d(f.data(), w, d.data(), v.data(), z.data());
    std::copy(d.begin(), d.begin() + w, row);
  }
}

////////////////////////////////////////////////////////////////////////////////
// Mesh

struct MeshParams {
  bool useAlpha = true;         // detect the surface from alpha, else whole frame
  double alphaThreshold = 0.05; // alpha above this is "surface"
  double cellSize = 32.0;       // grid spacing in pixels (each cell = 2 triangles)
  double expansion = 3.0;       // grow the detected surface by this many pixels
  Vec2 gridOrigin;              // grid is anchored here so it does not swim
  RectI frame;                  // whole-frame mode covers this rectangle
};

struct Mesh {
  double s = 1.0, ox = 0.0, oy = 0.0;  // cell size and grid origin
  int ci0 = 0, cj0 = 0;                // global index of local cell (0, 0)
  int nx = 0, ny = 0;                  // cells
  std::vector<uint8_t> cellOn;         // nx * ny
  std::vector<int> cellTri;            // nx * ny: first of the cell's 2 triangles, or -1
  std::vector<int> gridVertex;         // (nx+1) * (ny+1): vertex index or -1
  std::vector<Vec2> rest;              // vertex rest positions
  std::vector<int> tri;                // 3 vertex indices per triangle, CCW
  std::vector<int> comp;               // connected component of each vertex
  int numComp = 0;

  int numTriangles() const { return (int)tri.size() / 3; }
  bool empty() const { return tri.empty(); }
  void clear() { *this = Mesh(); }
};

namespace detail {
inline int ufFind(std::vector<int>& p, int a) {
  while (p[a] != a) {
    p[a] = p[p[a]];
    a = p[a];
  }
  return a;
}
}  // namespace detail

// Upper bound on the number of cells, to keep memory and solve time sane even
// if a host hands us an absurd image size.
static const int kMaxCells = 200000;

namespace detail {
template <class T>
size_t extractMaskT(const ImageRef& src, float thr, std::vector<uint8_t>& mask, RectI& box) {
  const int w = src.width(), h = src.height();
  const int ac = src.nComp == 4 ? 3 : 0;
  size_t count = 0;
  box = RectI(src.x2, src.y2, src.x1, src.y1);
  for (int y = 0; y < h; ++y) {
    const T* p = (const T*)src.pixelAddress(src.x1, src.y1 + y);
    uint8_t* mrow = &mask[(size_t)y * w];
    int rx1 = w, rx2 = -1;
    for (int x = 0; x < w; ++x, p += src.nComp) {
      const float a = PixelTraits<T>::toFloat(p[ac]);
      if (a > thr) {  // false for NaN
        mrow[x] = 1;
        ++count;
        if (rx1 == w) rx1 = x;
        rx2 = x;
      }
    }
    if (rx2 >= 0) {
      box.x1 = std::min(box.x1, src.x1 + rx1);
      box.x2 = std::max(box.x2, src.x1 + rx2 + 1);
      box.y1 = std::min(box.y1, src.y1 + y);
      box.y2 = std::max(box.y2, src.y1 + y + 1);
    }
  }
  return count;
}
}  // namespace detail

/** mask[w*h] (row y = src.y1 + index) gets 1 where alpha > thr; box is the
    bounding box of those pixels. Returns how many there are. RGB images have
    no alpha and are entirely opaque. */
inline size_t extractMask(const ImageRef& src, float thr, std::vector<uint8_t>& mask, RectI& box) {
  const int w = src.width(), h = src.height();
  if (!src.valid()) return 0;
  if (src.nComp == 3) {
    mask.assign((size_t)w * h, 1);
    box = RectI(src.x1, src.y1, src.x2, src.y2);
    return (size_t)w * h;
  }
  mask.assign((size_t)w * h, 0);
  switch (src.depth) {
    case kDepthU8: return detail::extractMaskT<uint8_t>(src, thr, mask, box);
    case kDepthU16: return detail::extractMaskT<uint16_t>(src, thr, mask, box);
    default: return detail::extractMaskT<float>(src, thr, mask, box);
  }
}

/** Grows a w*h binary mask by e pixels (Euclidean, pixel centre to pixel
    centre). Small radii use exact row sweeps, larger ones a distance
    transform; both give the same answer. */
inline void growMask(const std::vector<uint8_t>& mask, int w, int h, double e,
                     std::vector<uint8_t>& out, bool forceTransform = false) {
  out.assign((size_t)w * h, 0);
  if (e < 1.0) {
    out = mask;
    return;
  }
  const double e2 = e * e + 1e-6;
  if (e <= 8.0 && !forceTransform) {
    const int R = (int)std::floor(e);
    const int far = R + 1;
    std::vector<int> hd((size_t)w * h);  // horizontal distance to the mask in the row, capped
    for (int y = 0; y < h; ++y) {
      const uint8_t* mrow = &mask[(size_t)y * w];
      int* drow = &hd[(size_t)y * w];
      int d = far;
      for (int x = 0; x < w; ++x) {
        d = mrow[x] ? 0 : std::min(d + 1, far);
        drow[x] = d;
      }
      d = far;
      for (int x = w - 1; x >= 0; --x) {
        d = mrow[x] ? 0 : std::min(d + 1, far);
        drow[x] = std::min(drow[x], d);
      }
    }
    for (int y = 0; y < h; ++y)
      for (int x = 0; x < w; ++x) {
        for (int dy = -R; dy <= R; ++dy) {
          const int yy = y + dy;
          if (yy < 0 || yy >= h) continue;
          const int dx = hd[(size_t)yy * w + x];
          if (dx < far && (double)dx * dx + (double)dy * dy <= e2) {
            out[(size_t)y * w + x] = 1;
            break;
          }
        }
      }
    return;
  }
  std::vector<float> dist((size_t)w * h);
  for (size_t i = 0; i < dist.size(); ++i) dist[i] = mask[i] ? 0.0f : kEdtFar;
  edt2d(dist, w, h);
  for (size_t i = 0; i < dist.size(); ++i) out[i] = dist[i] <= e2 ? 1 : 0;
}

/** Builds the triangle mesh covering the surface of src. Returns false (and an
    empty mesh) if there is nothing to cover. */
inline bool buildMesh(const ImageRef& src, const MeshParams& mp, Mesh& m) {
  m.clear();
  if (!(mp.cellSize > 0.0) || !std::isfinite(mp.cellSize)) return false;
  const double s = mp.cellSize;
  m.s = s;
  m.ox = mp.gridOrigin.x;
  m.oy = mp.gridOrigin.y;

  // 1. Domain to cover and, in alpha mode, the grown surface mask.
  RectI dom;
  std::vector<uint8_t> grown;  // alpha mode: 1 where the (expanded) surface is
  bool rectOnly = true;        // the surface is simply the rectangle dom
  if (mp.useAlpha && src.valid()) {
    std::vector<uint8_t> mask;
    RectI box;
    const size_t count = extractMask(src, (float)mp.alphaThreshold, mask, box);
    if (count == 0) return false;  // fully transparent: nothing to deform
    const double e = std::max(0.0, std::min(mp.expansion, 1000.0));
    if (count == (size_t)src.width() * src.height()) {
      // Opaque frame (the usual case for video): the surface is the whole
      // image, grown by the expansion. No need for a distance transform.
      const int g = std::max(1, (int)std::ceil(e));
      dom = RectI(src.x1 - g, src.y1 - g, src.x2 + g, src.y2 + g);
    } else {
      const int pad = (int)std::ceil(e) + 1;
      dom = RectI(box.x1 - pad, box.y1 - pad, box.x2 + pad, box.y2 + pad);
      const int w = dom.x2 - dom.x1, h = dom.y2 - dom.y1;
      if ((double)w * h > 4.0e8) return false;
      std::vector<uint8_t> local((size_t)w * h, 0);
      for (int y = box.y1; y < box.y2; ++y)
        std::memcpy(&local[(size_t)(y - dom.y1) * w + (box.x1 - dom.x1)],
                    &mask[(size_t)(y - src.y1) * src.width() + (box.x1 - src.x1)], box.x2 - box.x1);
      growMask(local, w, h, e, grown);
      rectOnly = false;
    }
  } else {
    if (mp.frame.empty()) return false;
    // one extra pixel so the bilinear fall-off at the frame edge stays inside the mesh
    dom = RectI(mp.frame.x1 - 1, mp.frame.y1 - 1, mp.frame.x2 + 1, mp.frame.y2 + 1);
  }

  // 2. Cell grid over the domain (plus one cell of margin).
  m.ci0 = floorDiv(dom.x1 - m.ox, s) - 1;
  m.cj0 = floorDiv(dom.y1 - m.oy, s) - 1;
  const int ci1 = floorDiv(dom.x2 - m.ox, s) + 1;
  const int cj1 = floorDiv(dom.y2 - m.oy, s) + 1;
  m.nx = ci1 - m.ci0 + 1;
  m.ny = cj1 - m.cj0 + 1;
  if (m.nx <= 0 || m.ny <= 0 || (double)m.nx * m.ny > kMaxCells) {
    m.clear();
    return false;
  }
  m.cellOn.assign((size_t)m.nx * m.ny, 0);

  if (!rectOnly) {
    const int w = dom.x2 - dom.x1;
    for (int y = dom.y1; y < dom.y2; ++y) {
      const uint8_t* grow = &grown[(size_t)(y - dom.y1) * w];
      const int cj = floorDiv(y + 0.5 - m.oy, s) - m.cj0;
      if (cj < 0 || cj >= m.ny) continue;
      uint8_t* cells = &m.cellOn[(size_t)cj * m.nx];
      for (int x = dom.x1; x < dom.x2; ++x) {
        if (!grow[x - dom.x1]) continue;
        const int ci = floorDiv(x + 0.5 - m.ox, s) - m.ci0;
        if (ci >= 0 && ci < m.nx) cells[ci] = 1;
      }
    }
  } else {
    for (int cj = 0; cj < m.ny; ++cj)
      for (int ci = 0; ci < m.nx; ++ci) {
        const double cx1 = m.ox + (ci + m.ci0) * s, cy1 = m.oy + (cj + m.cj0) * s;
        if (cx1 + s > dom.x1 && cx1 < dom.x2 && cy1 + s > dom.y1 && cy1 < dom.y2)
          m.cellOn[(size_t)cj * m.nx + ci] = 1;
      }
  }

  // 3. Vertices, numbered along the shorter grid side first so that the
  //    system matrix stays narrowly banded.
  const int vx = m.nx + 1, vy = m.ny + 1;
  std::vector<uint8_t> used((size_t)vx * vy, 0);
  for (int cj = 0; cj < m.ny; ++cj)
    for (int ci = 0; ci < m.nx; ++ci)
      if (m.cellOn[(size_t)cj * m.nx + ci]) {
        used[(size_t)cj * vx + ci] = used[(size_t)cj * vx + ci + 1] = 1;
        used[(size_t)(cj + 1) * vx + ci] = used[(size_t)(cj + 1) * vx + ci + 1] = 1;
      }
  m.gridVertex.assign((size_t)vx * vy, -1);
  auto addVertex = [&](int i, int j) {
    const size_t g = (size_t)j * vx + i;
    if (!used[g]) return;
    m.gridVertex[g] = (int)m.rest.size();
    m.rest.push_back(Vec2(m.ox + (i + m.ci0) * s, m.oy + (j + m.cj0) * s));
  };
  if (vx >= vy) {
    for (int i = 0; i < vx; ++i)
      for (int j = 0; j < vy; ++j) addVertex(i, j);
  } else {
    for (int j = 0; j < vy; ++j)
      for (int i = 0; i < vx; ++i) addVertex(i, j);
  }
  if (m.rest.empty()) {
    m.clear();
    return false;
  }

  // 4. Two triangles per cell, diagonals alternating in a checkerboard so the
  //    mesh has no preferred direction.
  m.cellTri.assign((size_t)m.nx * m.ny, -1);
  for (int cj = 0; cj < m.ny; ++cj)
    for (int ci = 0; ci < m.nx; ++ci) {
      if (!m.cellOn[(size_t)cj * m.nx + ci]) continue;
      const int v00 = m.gridVertex[(size_t)cj * vx + ci];
      const int v10 = m.gridVertex[(size_t)cj * vx + ci + 1];
      const int v01 = m.gridVertex[(size_t)(cj + 1) * vx + ci];
      const int v11 = m.gridVertex[(size_t)(cj + 1) * vx + ci + 1];
      m.cellTri[(size_t)cj * m.nx + ci] = m.numTriangles();
      if (parity(ci + m.ci0 + cj + m.cj0) == 0) {
        const int t[6] = {v00, v10, v11, v00, v11, v01};
        m.tri.insert(m.tri.end(), t, t + 6);
      } else {
        const int t[6] = {v00, v10, v01, v10, v11, v01};
        m.tri.insert(m.tri.end(), t, t + 6);
      }
    }

  // 5. Connected components (separate pieces deform independently).
  const int nv = (int)m.rest.size();
  std::vector<int> parent(nv);
  for (int i = 0; i < nv; ++i) parent[i] = i;
  for (size_t t = 0; t < m.tri.size(); t += 3) {
    const int a = detail::ufFind(parent, m.tri[t]);
    for (int k = 1; k < 3; ++k) {
      const int b = detail::ufFind(parent, m.tri[t + k]);
      if (a != b) parent[b] = a;
    }
  }
  m.comp.assign(nv, -1);
  std::vector<int> label(nv, -1);
  m.numComp = 0;
  for (int i = 0; i < nv; ++i) {
    const int r = detail::ufFind(parent, i);
    if (label[r] < 0) label[r] = m.numComp++;
    m.comp[i] = label[r];
  }
  return true;
}

////////////////////////////////////////////////////////////////////////////////
// Pins

struct PinBind {
  bool ok = false;
  int tri = -1;
  double w[3] = {0.0, 0.0, 0.0};  // barycentric weights on the triangle's vertices
  int comp = -1;
};

inline void barycentric(Vec2 p, Vec2 a, Vec2 b, Vec2 c, double w[3]) {
  const double area = cross(b - a, c - a);
  if (std::fabs(area) < 1e-300) {
    w[0] = 1.0;
    w[1] = w[2] = 0.0;
    return;
  }
  w[0] = cross(c - b, p - b) / area;
  w[1] = cross(a - c, p - c) / area;
  w[2] = 1.0 - w[0] - w[1];
}

// Closest point to p on triangle (a, b, c), returned as barycentric weights.
inline double closestOnTriangle(Vec2 p, Vec2 a, Vec2 b, Vec2 c, double w[3]) {
  barycentric(p, a, b, c, w);
  if (w[0] >= 0.0 && w[1] >= 0.0 && w[2] >= 0.0) return 0.0;
  double best = std::numeric_limits<double>::infinity();
  const Vec2 v[3] = {a, b, c};
  for (int e = 0; e < 3; ++e) {
    const Vec2 p0 = v[e], p1 = v[(e + 1) % 3];
    const Vec2 d = p1 - p0;
    const double len2 = dot(d, d);
    double t = len2 > 0.0 ? dot(p - p0, d) / len2 : 0.0;
    t = std::max(0.0, std::min(1.0, t));
    const Vec2 q = p0 + d * t;
    const double dist2 = dot(p - q, p - q);
    if (dist2 < best) {
      best = dist2;
      w[0] = w[1] = w[2] = 0.0;
      w[e] = 1.0 - t;
      w[(e + 1) % 3] = t;
    }
  }
  return best;
}

/** Attaches a point to the mesh. Points inside the mesh bind to the triangle
    under them; points just outside (up to searchCells cells away) snap to the
    closest triangle; anything further away stays unbound. */
inline PinBind bindPin(const Mesh& m, Vec2 p, int searchCells = 2) {
  PinBind b;
  if (m.empty() || !std::isfinite(p.x) || !std::isfinite(p.y)) return b;
  const int ci = floorDiv(p.x - m.ox, m.s) - m.ci0;
  const int cj = floorDiv(p.y - m.oy, m.s) - m.cj0;
  double best = std::numeric_limits<double>::infinity();
  for (int r = 0; r <= searchCells && !b.ok; ++r) {
    for (int j = cj - r; j <= cj + r; ++j)
      for (int i = ci - r; i <= ci + r; ++i) {
        if (std::max(std::abs(i - ci), std::abs(j - cj)) != r) continue;  // ring only
        if (i < 0 || j < 0 || i >= m.nx || j >= m.ny) continue;
        const int t0 = m.cellTri[(size_t)j * m.nx + i];
        if (t0 < 0) continue;
        for (int t = t0; t < t0 + 2; ++t) {
          const int* v = &m.tri[(size_t)t * 3];
          double w[3];
          const double d2 = closestOnTriangle(p, m.rest[v[0]], m.rest[v[1]], m.rest[v[2]], w);
          if (d2 < best - 1e-12) {
            best = d2;
            b.ok = true;
            b.tri = t;
            b.w[0] = w[0];
            b.w[1] = w[1];
            b.w[2] = w[2];
            b.comp = m.comp[v[0]];
          }
        }
      }
  }
  return b;
}

////////////////////////////////////////////////////////////////////////////////
// As-rigid-as-possible solver (Sorkine & Alexa local/global iterations with
// per-triangle rotations and cotangent weights). The system matrix only
// depends on the mesh and on where the pins are attached, so it is factored
// once and every iteration is two cheap banded back-substitutions.

class ArapSolver {
 public:
  /** Prepares the solver. Only mesh pieces holding at least one bound pin
      move; the others stay where they are. Returns false if nothing moves. */
  bool setup(const Mesh& m, const std::vector<PinBind>& pins, double pinWeight = 1.0e5) {
    ok_ = false;
    const int nv = (int)m.rest.size();
    active_.assign(nv, -1);
    if (m.empty()) return false;
    std::vector<uint8_t> compActive(m.numComp, 0);
    for (const PinBind& b : pins)
      if (b.ok && b.comp >= 0 && b.comp < m.numComp) compActive[b.comp] = 1;
    n_ = 0;
    for (int v = 0; v < nv; ++v)
      if (compActive[m.comp[v]]) active_[v] = n_++;
    if (n_ == 0) return false;
    pinWeight_ = pinWeight;

    // Cotangent weights per triangle edge: edge k is opposite vertex k.
    const int nt = m.numTriangles();
    weights_.assign((size_t)nt * 3, 0.0);
    for (int t = 0; t < nt; ++t) {
      const int* v = &m.tri[(size_t)t * 3];
      for (int k = 0; k < 3; ++k) {
        const Vec2 a = m.rest[v[k]], b = m.rest[v[(k + 1) % 3]], c = m.rest[v[(k + 2) % 3]];
        const double cr = std::fabs(cross(b - a, c - a));
        const double cot = cr > 1e-12 ? dot(b - a, c - a) / cr : 0.0;
        weights_[(size_t)t * 3 + k] = std::max(0.0, 0.5 * cot);
      }
    }

    // Envelope (skyline) structure: first non-zero column of each row.
    first_.assign(n_, 0);
    for (int i = 0; i < n_; ++i) first_[i] = i;
    for (int t = 0; t < nt; ++t) {
      const int* v = &m.tri[(size_t)t * 3];
      const int a = active_[v[0]], b = active_[v[1]], c = active_[v[2]];
      if (a < 0 || b < 0 || c < 0) continue;
      const int lo = std::min(a, std::min(b, c));
      first_[a] = std::min(first_[a], lo);
      first_[b] = std::min(first_[b], lo);
      first_[c] = std::min(first_[c], lo);
    }
    start_.assign(n_ + 1, 0);
    for (int i = 0; i < n_; ++i) start_[i + 1] = start_[i] + (size_t)(i - first_[i] + 1);
    if (start_[n_] > (size_t)60000000) return false;  // refuse absurd systems
    L_.assign(start_[n_], 0.0);

    // Assemble A = Laplacian + pinWeight * sum(b b^T).
    for (int t = 0; t < nt; ++t) {
      const int* v = &m.tri[(size_t)t * 3];
      if (active_[v[0]] < 0) continue;
      for (int k = 0; k < 3; ++k) {
        const double w = weights_[(size_t)t * 3 + k];
        if (w == 0.0) continue;
        const int i = active_[v[(k + 1) % 3]], j = active_[v[(k + 2) % 3]];
        add(i, i, w);
        add(j, j, w);
        add(i, j, -w);
      }
    }
    for (const PinBind& b : pins) {
      if (!b.ok) continue;
      const int* v = &m.tri[(size_t)b.tri * 3];
      for (int r = 0; r < 3; ++r)
        for (int c = 0; c <= r; ++c) {
          const int i = active_[v[r]], j = active_[v[c]];
          const double val = pinWeight * b.w[r] * b.w[c];
          if (i == j)
            add(i, i, val);
          else
            add(i, j, val);
        }
    }

    // In-place envelope Cholesky: A = L L^T.
    for (int i = 0; i < n_; ++i) {
      double* Li = &L_[start_[i]] - first_[i];  // Li[k] = L(i, k)
      for (int j = first_[i]; j < i; ++j) {
        const double* Lj = &L_[start_[j]] - first_[j];
        const int k0 = std::max(first_[i], first_[j]);
        double sum = Li[j];
        for (int k = k0; k < j; ++k) sum -= Li[k] * Lj[k];
        Li[j] = sum / Lj[j];
      }
      double d = Li[i];
      for (int k = first_[i]; k < i; ++k) d -= Li[k] * Li[k];
      if (!(d > 1e-12)) return false;  // not positive definite (cannot happen with pins)
      Li[i] = std::sqrt(d);
    }
    ok_ = true;
    return true;
  }

  bool ready() const { return ok_; }

  /** Deforms the mesh so that each bound pin lands on its target. out gets a
      position for every mesh vertex (pieces without pins keep their rest
      position). */
  void solve(const Mesh& m, const std::vector<PinBind>& pins, const std::vector<Vec2>& targets,
             int iterations, std::vector<Vec2>& out) const {
    out = m.rest;
    if (!ok_) return;
    const int nt = m.numTriangles();
    std::vector<double> rx(n_), ry(n_), x(n_), y(n_);
    std::vector<double> rc((size_t)nt, 1.0), rs((size_t)nt, 0.0);  // rotation cos/sin
    iterations = std::max(0, std::min(iterations, 1000));
    for (int it = 0; it <= iterations; ++it) {
      if (it > 0) {
        // local step: best rotation per triangle
        for (int t = 0; t < nt; ++t) {
          const int* v = &m.tri[(size_t)t * 3];
          if (active_[v[0]] < 0) continue;
          double s00 = 0, s01 = 0, s10 = 0, s11 = 0;
          for (int k = 0; k < 3; ++k) {
            const double w = weights_[(size_t)t * 3 + k];
            const int a = v[(k + 1) % 3], b = v[(k + 2) % 3];
            const Vec2 e = m.rest[a] - m.rest[b];
            const double ex = out[a].x - out[b].x, ey = out[a].y - out[b].y;
            s00 += w * e.x * ex;
            s01 += w * e.x * ey;
            s10 += w * e.y * ex;
            s11 += w * e.y * ey;
          }
          const double th = std::atan2(s01 - s10, s00 + s11);
          rc[t] = std::cos(th);
          rs[t] = std::sin(th);
        }
      }
      // global step: right hand side
      std::fill(rx.begin(), rx.end(), 0.0);
      std::fill(ry.begin(), ry.end(), 0.0);
      for (int t = 0; t < nt; ++t) {
        const int* v = &m.tri[(size_t)t * 3];
        if (active_[v[0]] < 0) continue;
        for (int k = 0; k < 3; ++k) {
          const double w = weights_[(size_t)t * 3 + k];
          if (w == 0.0) continue;
          const int a = v[(k + 1) % 3], b = v[(k + 2) % 3];
          const Vec2 e = m.rest[a] - m.rest[b];
          const double dx = w * (rc[t] * e.x - rs[t] * e.y);
          const double dy = w * (rs[t] * e.x + rc[t] * e.y);
          rx[active_[a]] += dx;
          ry[active_[a]] += dy;
          rx[active_[b]] -= dx;
          ry[active_[b]] -= dy;
        }
      }
      for (size_t p = 0; p < pins.size() && p < targets.size(); ++p) {
        const PinBind& b = pins[p];
        if (!b.ok) continue;
        const int* v = &m.tri[(size_t)b.tri * 3];
        for (int r = 0; r < 3; ++r) {
          rx[active_[v[r]]] += pinWeight_ * b.w[r] * targets[p].x;
          ry[active_[v[r]]] += pinWeight_ * b.w[r] * targets[p].y;
        }
      }
      backSolve(rx, x);
      backSolve(ry, y);
      for (int v = 0; v < (int)m.rest.size(); ++v) {
        const int i = active_[v];
        if (i >= 0) out[v] = Vec2(x[i], y[i]);
      }
    }
    for (const Vec2& p : out)
      if (!std::isfinite(p.x) || !std::isfinite(p.y)) {  // never hand garbage to the rasteriser
        out = m.rest;
        return;
      }
  }

  int numUnknowns() const { return n_; }

 private:
  void add(int i, int j, double v) {  // requires first_[max] <= min
    if (j > i) std::swap(i, j);
    L_[start_[i] + (size_t)(j - first_[i])] += v;
  }
  void backSolve(const std::vector<double>& b, std::vector<double>& x) const {
    for (int i = 0; i < n_; ++i) {
      const double* Li = &L_[start_[i]] - first_[i];
      double sum = b[i];
      for (int k = first_[i]; k < i; ++k) sum -= Li[k] * x[k];
      x[i] = sum / Li[i];
    }
    for (int i = n_ - 1; i >= 0; --i) {
      const double* Li = &L_[start_[i]] - first_[i];
      x[i] /= Li[i];
      const double xi = x[i];
      for (int k = first_[i]; k < i; ++k) x[k] -= Li[k] * xi;
    }
  }

  bool ok_ = false;
  int n_ = 0;
  double pinWeight_ = 1.0e5;
  std::vector<int> active_;
  std::vector<double> weights_;
  std::vector<int> first_;
  std::vector<size_t> start_;
  std::vector<double> L_;
};

////////////////////////////////////////////////////////////////////////////////
// Rasterisation: every deformed triangle is filled by sampling the source at
// the matching point of the rest triangle (bilinear).

struct WarpTriangle {
  Vec2 d[3];        // destination (deformed) corners
  Vec2 s[3];        // source (rest) corners
  double invArea;   // 1 / cross(d1 - d0, d2 - d0)
  int bx1, by1, bx2, by2;  // pixel bounding box (inclusive-exclusive)
};

inline void prepareTriangles(const Mesh& m, const std::vector<Vec2>& deformed,
                             std::vector<WarpTriangle>& out) {
  out.clear();
  const int nt = m.numTriangles();
  out.reserve(nt);
  for (int t = 0; t < nt; ++t) {
    const int* v = &m.tri[(size_t)t * 3];
    WarpTriangle w;
    for (int k = 0; k < 3; ++k) {
      w.d[k] = deformed[v[k]];
      w.s[k] = m.rest[v[k]];
    }
    const double area = cross(w.d[1] - w.d[0], w.d[2] - w.d[0]);
    if (!(std::fabs(area) > 1e-9)) continue;  // collapsed triangle covers nothing
    w.invArea = 1.0 / area;
    const double minx = std::min(w.d[0].x, std::min(w.d[1].x, w.d[2].x));
    const double maxx = std::max(w.d[0].x, std::max(w.d[1].x, w.d[2].x));
    const double miny = std::min(w.d[0].y, std::min(w.d[1].y, w.d[2].y));
    const double maxy = std::max(w.d[0].y, std::max(w.d[1].y, w.d[2].y));
    if (!(maxx - minx < 1e7 && maxy - miny < 1e7)) continue;
    w.bx1 = (int)std::ceil(minx - 0.5 - 1e-6);
    w.bx2 = (int)std::floor(maxx - 0.5 + 1e-6) + 1;
    w.by1 = (int)std::ceil(miny - 0.5 - 1e-6);
    w.by2 = (int)std::floor(maxy - 0.5 + 1e-6) + 1;
    out.push_back(w);
  }
}

#if defined(_MSC_VER)
#define PUPPET_FORCE_INLINE __forceinline
#else
#define PUPPET_FORCE_INLINE inline __attribute__((always_inline))
#endif

/** Bilinear sample at (sx, sy) in pixel coordinates, normalised to 0..1 (float
    images keep their values). Taps outside the image count as transparent.
    Straight (un-premultiplied) RGBA is interpolated premultiplied so edges
    do not pick up dark fringes. */
template <class T, int N>
PUPPET_FORCE_INLINE void sampleBilinear(const ImageRef& src, double sx, double sy, float out[4]) {
  const double fx = sx - 0.5, fy = sy - 0.5;
  if (!(fx > -1e9 && fx < 1e9 && fy > -1e9 && fy < 1e9)) {  // NaN / absurd
    for (int c = 0; c < N; ++c) out[c] = 0.0f;
    return;
  }
  // floor without a libm call (this is the hottest loop of the plug-in)
  int x0 = (int)fx, y0 = (int)fy;
  x0 -= fx < x0;
  y0 -= fy < y0;
  const float tx = (float)(fx - x0), ty = (float)(fy - y0);
  float w0 = (1 - tx) * (1 - ty), w1 = tx * (1 - ty), w2 = (1 - tx) * ty, w3 = tx * ty;
  const T *p0, *p1, *p2, *p3;
  static const T kZero[4] = {T(0), T(0), T(0), T(0)};
  if (x0 >= src.x1 && x0 + 1 < src.x2 && y0 >= src.y1 && y0 + 1 < src.y2) {
    p0 = (const T*)((const char*)src.data + (std::ptrdiff_t)(y0 - src.y1) * src.rowBytes) +
         (std::ptrdiff_t)(x0 - src.x1) * N;
    p1 = p0 + N;
    p2 = (const T*)((const char*)p0 + src.rowBytes);
    p3 = p2 + N;
  } else {  // at the border
    p0 = src.contains(x0, y0) ? (const T*)src.pixelAddress(x0, y0) : kZero;
    p1 = src.contains(x0 + 1, y0) ? (const T*)src.pixelAddress(x0 + 1, y0) : kZero;
    p2 = src.contains(x0, y0 + 1) ? (const T*)src.pixelAddress(x0, y0 + 1) : kZero;
    p3 = src.contains(x0 + 1, y0 + 1) ? (const T*)src.pixelAddress(x0 + 1, y0 + 1) : kZero;
  }
  if (N == 4 && src.straightAlpha) {
    const float a = w0 * PixelTraits<T>::toFloat(p0[3]) + w1 * PixelTraits<T>::toFloat(p1[3]) +
                    w2 * PixelTraits<T>::toFloat(p2[3]) + w3 * PixelTraits<T>::toFloat(p3[3]);
    out[3] = a;
    if (!(a > 1e-8f)) {
      out[0] = out[1] = out[2] = 0.0f;
      return;
    }
    w0 *= PixelTraits<T>::toFloat(p0[3]) / a;
    w1 *= PixelTraits<T>::toFloat(p1[3]) / a;
    w2 *= PixelTraits<T>::toFloat(p2[3]) / a;
    w3 *= PixelTraits<T>::toFloat(p3[3]) / a;
    for (int c = 0; c < 3; ++c)
      out[c] = w0 * PixelTraits<T>::toFloat(p0[c]) + w1 * PixelTraits<T>::toFloat(p1[c]) +
               w2 * PixelTraits<T>::toFloat(p2[c]) + w3 * PixelTraits<T>::toFloat(p3[c]);
    return;
  }
  for (int c = 0; c < N; ++c)
    out[c] = w0 * PixelTraits<T>::toFloat(p0[c]) + w1 * PixelTraits<T>::toFloat(p1[c]) +
             w2 * PixelTraits<T>::toFloat(p2[c]) + w3 * PixelTraits<T>::toFloat(p3[c]);
}

/** 8-bit fast path (premultiplied or opaque images, the VEGAS default):
    bilinear in 8.8 fixed point, written straight to the destination. */
template <int N>
PUPPET_FORCE_INLINE void sampleWriteU8(const ImageRef& src, double sx, double sy, uint8_t* out) {
  const double fx = sx - 0.5, fy = sy - 0.5;
  if (!(fx > -1e9 && fx < 1e9 && fy > -1e9 && fy < 1e9)) {
    for (int c = 0; c < N; ++c) out[c] = 0;
    return;
  }
  int x0 = (int)fx, y0 = (int)fy;
  x0 -= fx < x0;
  y0 -= fy < y0;
  const int tx = (int)((fx - x0) * 256.0 + 0.5), ty = (int)((fy - y0) * 256.0 + 0.5);
  const int w0 = (256 - tx) * (256 - ty), w1 = tx * (256 - ty), w2 = (256 - tx) * ty, w3 = tx * ty;
  static const uint8_t kZero[4] = {0, 0, 0, 0};
  const uint8_t *p0, *p1, *p2, *p3;
  if (x0 >= src.x1 && x0 + 1 < src.x2 && y0 >= src.y1 && y0 + 1 < src.y2) {
    p0 = (const uint8_t*)src.data + (std::ptrdiff_t)(y0 - src.y1) * src.rowBytes +
         (std::ptrdiff_t)(x0 - src.x1) * N;
    p1 = p0 + N;
    p2 = p0 + src.rowBytes;
    p3 = p2 + N;
  } else {
    p0 = src.contains(x0, y0) ? (const uint8_t*)src.pixelAddress(x0, y0) : kZero;
    p1 = src.contains(x0 + 1, y0) ? (const uint8_t*)src.pixelAddress(x0 + 1, y0) : kZero;
    p2 = src.contains(x0, y0 + 1) ? (const uint8_t*)src.pixelAddress(x0, y0 + 1) : kZero;
    p3 = src.contains(x0 + 1, y0 + 1) ? (const uint8_t*)src.pixelAddress(x0 + 1, y0 + 1) : kZero;
  }
  for (int c = 0; c < N; ++c)
    out[c] = (uint8_t)((w0 * p0[c] + w1 * p1[c] + w2 * p2[c] + w3 * p3[c] + 32768) >> 16);
}

/** Fills rows [yBegin, yEnd) of dst (clipped to window) with the warped
    source. The rows must already be cleared to transparent. */
template <class T, int N>
inline void warpBandT(const ImageRef& src, const ImageRef& dst, const RectI& window,
                      const std::vector<WarpTriangle>& tris, int yBegin, int yEnd) {
  const RectI clip = intersect(intersect(window, RectI(dst.x1, dst.y1, dst.x2, dst.y2)),
                               RectI(window.x1, yBegin, window.x2, yEnd));
  if (clip.empty()) return;
  const double eps = 1e-7;
  float px[4];
  const bool fastU8 = sizeof(T) == 1 && !(N == 4 && src.straightAlpha);
  for (const WarpTriangle& w : tris) {
    const int x1 = std::max(w.bx1, clip.x1), x2 = std::min(w.bx2, clip.x2);
    const int y1 = std::max(w.by1, clip.y1), y2 = std::min(w.by2, clip.y2);
    if (x1 >= x2 || y1 >= y2) continue;
    // Barycentric weights are linear in x along a row: l = a + b * (x + 0.5).
    const Vec2 e0 = w.d[2] - w.d[1], e1 = w.d[0] - w.d[2];
    const double b0 = -e0.y * w.invArea, b1 = -e1.y * w.invArea, b2 = -b0 - b1;
    for (int y = y1; y < y2; ++y) {
      const double py = y + 0.5;
      const double a0 = (e0.x * (py - w.d[1].y) + e0.y * w.d[1].x) * w.invArea;
      const double a1 = (e1.x * (py - w.d[2].y) + e1.y * w.d[2].x) * w.invArea;
      const double a2 = 1.0 - a0 - a1;
      // Conservative span of pixel centres inside the triangle; the exact
      // per-pixel test below decides, so shared edges never leave gaps.
      double lo = x1 + 0.5, hi = x2 - 0.5;
      const double as[3] = {a0, a1, a2}, bs[3] = {b0, b1, b2};
      bool empty = false;
      for (int k = 0; k < 3; ++k) {
        if (bs[k] > 1e-14) lo = std::max(lo, (-eps - as[k]) / bs[k]);
        else if (bs[k] < -1e-14) hi = std::min(hi, (-eps - as[k]) / bs[k]);
        else if (as[k] < -eps) empty = true;
      }
      if (empty || lo > hi + 1.0) continue;
      const int sx1 = std::max(x1, (int)std::floor(lo - 0.5) - 1);
      const int sx2 = std::min(x2, (int)std::ceil(hi - 0.5) + 2);
      // the source position is linear in x as well
      const double sxa = a0 * w.s[0].x + a1 * w.s[1].x + a2 * w.s[2].x;
      const double sxb = b0 * w.s[0].x + b1 * w.s[1].x + b2 * w.s[2].x;
      const double sya = a0 * w.s[0].y + a1 * w.s[1].y + a2 * w.s[2].y;
      const double syb = b0 * w.s[0].y + b1 * w.s[1].y + b2 * w.s[2].y;
      T* row = (T*)dst.pixelAddress(sx1, y);
      for (int x = sx1; x < sx2; ++x, row += N) {
        const double pxc = x + 0.5;
        const double l0 = a0 + b0 * pxc, l1 = a1 + b1 * pxc;
        if (l0 < -eps || l1 < -eps || 1.0 - l0 - l1 < -eps) continue;
        if (fastU8) {
          sampleWriteU8<N>(src, sxa + sxb * pxc, sya + syb * pxc, (uint8_t*)row);
        } else {
          sampleBilinear<T, N>(src, sxa + sxb * pxc, sya + syb * pxc, px);
          for (int c = 0; c < N; ++c) row[c] = PixelTraits<T>::fromFloat(px[c]);
        }
      }
    }
  }
}

inline void warpBand(const ImageRef& src, const ImageRef& dst, const RectI& window,
                     const std::vector<WarpTriangle>& tris, int yBegin, int yEnd) {
  switch (dst.depth) {
    case kDepthU8:
      if (dst.nComp == 4) warpBandT<uint8_t, 4>(src, dst, window, tris, yBegin, yEnd);
      else if (dst.nComp == 3) warpBandT<uint8_t, 3>(src, dst, window, tris, yBegin, yEnd);
      else warpBandT<uint8_t, 1>(src, dst, window, tris, yBegin, yEnd);
      break;
    case kDepthU16:
      if (dst.nComp == 4) warpBandT<uint16_t, 4>(src, dst, window, tris, yBegin, yEnd);
      else if (dst.nComp == 3) warpBandT<uint16_t, 3>(src, dst, window, tris, yBegin, yEnd);
      else warpBandT<uint16_t, 1>(src, dst, window, tris, yBegin, yEnd);
      break;
    default:
      if (dst.nComp == 4) warpBandT<float, 4>(src, dst, window, tris, yBegin, yEnd);
      else if (dst.nComp == 3) warpBandT<float, 3>(src, dst, window, tris, yBegin, yEnd);
      else warpBandT<float, 1>(src, dst, window, tris, yBegin, yEnd);
      break;
  }
}

/** Sets rows [yBegin, yEnd) of the window to transparent black. */
inline void clearBand(const ImageRef& dst, const RectI& window, int yBegin, int yEnd) {
  const RectI r = intersect(intersect(window, RectI(dst.x1, dst.y1, dst.x2, dst.y2)),
                            RectI(window.x1, yBegin, window.x2, yEnd));
  if (r.empty()) return;
  const size_t bytes = (size_t)(r.x2 - r.x1) * dst.nComp * dst.bytesPerComponent();
  for (int y = r.y1; y < r.y2; ++y) std::memset(dst.pixelAddress(r.x1, y), 0, bytes);
}

/** Copies src into rows [yBegin, yEnd) of the window (transparent outside src).
    Both images must share depth and component count. */
inline void copyBand(const ImageRef& src, const ImageRef& dst, const RectI& window, int yBegin,
                     int yEnd) {
  clearBand(dst, window, yBegin, yEnd);
  const RectI r = intersect(intersect(intersect(window, RectI(dst.x1, dst.y1, dst.x2, dst.y2)),
                                      RectI(src.x1, src.y1, src.x2, src.y2)),
                            RectI(window.x1, yBegin, window.x2, yEnd));
  if (r.empty()) return;
  const size_t bytes = (size_t)(r.x2 - r.x1) * dst.nComp * dst.bytesPerComponent();
  for (int y = r.y1; y < r.y2; ++y)
    std::memcpy(dst.pixelAddress(r.x1, y), src.pixelAddress(r.x1, y), bytes);
}

////////////////////////////////////////////////////////////////////////////////
// Burned-in overlay (mesh wireframe and pin markers). VEGAS cannot always
// draw plug-in overlays, so the plug-in can draw them into the picture.

struct Rgb {
  float r, g, b;
};

class Painter {
 public:
  Painter(const ImageRef& img, const RectI& window)
      : img_(img), clip_(intersect(window, RectI(img.x1, img.y1, img.x2, img.y2))) {}

  void blend(int x, int y, Rgb c, float opacity) {
    if (x < clip_.x1 || x >= clip_.x2 || y < clip_.y1 || y >= clip_.y2 || opacity <= 0.0f) return;
    opacity = std::min(opacity, 1.0f);
    switch (img_.depth) {
      case kDepthU8: blendT<uint8_t>(x, y, c, opacity); break;
      case kDepthU16: blendT<uint16_t>(x, y, c, opacity); break;
      default: blendT<float>(x, y, c, opacity); break;
    }
  }

  void line(Vec2 a, Vec2 b, Rgb c, float opacity) {
    // clip roughly against the window so huge coordinates cannot stall us
    const double minx = clip_.x1 - 2, maxx = clip_.x2 + 2, miny = clip_.y1 - 2, maxy = clip_.y2 + 2;
    if ((a.x < minx && b.x < minx) || (a.x > maxx && b.x > maxx) || (a.y < miny && b.y < miny) ||
        (a.y > maxy && b.y > maxy))
      return;
    const double dx = b.x - a.x, dy = b.y - a.y;
    const double len = std::max(std::fabs(dx), std::fabs(dy));
    if (!(len < 1e6)) return;
    const int steps = std::max(1, (int)std::ceil(len));
    int lastX = INT32_MIN, lastY = INT32_MIN;
    for (int i = 0; i <= steps; ++i) {
      const double t = (double)i / steps;
      const int x = (int)std::floor(a.x + dx * t), y = (int)std::floor(a.y + dy * t);
      if (x == lastX && y == lastY) continue;
      lastX = x;
      lastY = y;
      blend(x, y, c, opacity);
    }
  }

  void disc(Vec2 p, double radius, Rgb c, float opacity) {
    const int x1 = (int)std::floor(p.x - radius - 1), x2 = (int)std::ceil(p.x + radius + 1);
    const int y1 = (int)std::floor(p.y - radius - 1), y2 = (int)std::ceil(p.y + radius + 1);
    for (int y = std::max(y1, clip_.y1); y < std::min(y2, clip_.y2); ++y)
      for (int x = std::max(x1, clip_.x1); x < std::min(x2, clip_.x2); ++x) {
        const double d = std::hypot(x + 0.5 - p.x, y + 0.5 - p.y);
        const double cov = std::max(0.0, std::min(1.0, radius + 0.5 - d));
        if (cov > 0.0) blend(x, y, c, opacity * (float)cov);
      }
  }

  // 3x5 pixel digits, scaled; (x, y) is the bottom-left corner.
  void number(int value, double x, double y, int scale, Rgb c) {
    static const char* kDigits[10] = {"111101101101111", "010110010010111", "111001111100111",
                                      "111001111001111", "101101111001001", "111100111001111",
                                      "111100111101111", "111001010010010", "111101111101111",
                                      "111101111001111"};
    char buf[16];
    int n = 0;
    if (value < 0) value = 0;
    do {
      buf[n++] = (char)('0' + value % 10);
      value /= 10;
    } while (value > 0 && n < 15);
    for (int i = 0; i < n; ++i) {
      const char* g = kDigits[buf[n - 1 - i] - '0'];
      const double gx = x + i * 4 * scale;
      for (int row = 0; row < 5; ++row)
        for (int col = 0; col < 3; ++col) {
          if (g[row * 3 + col] != '1') continue;
          const int px = (int)std::floor(gx + col * scale), py = (int)std::floor(y + (4 - row) * scale);
          for (int yy = 0; yy < scale; ++yy)
            for (int xx = 0; xx < scale; ++xx) blend(px + xx, py + yy, c, 1.0f);
        }
    }
  }

 private:
  template <class T>
  void blendT(int x, int y, Rgb c, float a) {
    T* p = (T*)img_.pixelAddress(x, y);
    if (img_.nComp == 1) {
      p[0] = PixelTraits<T>::fromFloat(PixelTraits<T>::toFloat(p[0]) * (1 - a) + a);
      return;
    }
    const float col[3] = {img_.bgra ? c.b : c.r, c.g, img_.bgra ? c.r : c.b};
    if (img_.nComp == 4 && img_.straightAlpha) {
      const float da = PixelTraits<T>::toFloat(p[3]);
      const float oa = a + da * (1 - a);
      for (int k = 0; k < 3; ++k) {
        const float dc = PixelTraits<T>::toFloat(p[k]);
        p[k] = PixelTraits<T>::fromFloat(oa > 1e-6f ? (col[k] * a + dc * da * (1 - a)) / oa : 0.0f);
      }
      p[3] = PixelTraits<T>::fromFloat(oa);
      return;
    }
    for (int k = 0; k < 3; ++k)
      p[k] = PixelTraits<T>::fromFloat(col[k] * a + PixelTraits<T>::toFloat(p[k]) * (1 - a));
    if (img_.nComp == 4) p[3] = PixelTraits<T>::fromFloat(a + PixelTraits<T>::toFloat(p[3]) * (1 - a));
  }

  ImageRef img_;
  RectI clip_;
};

enum PinState { kPinDisabled = 0, kPinActive = 1, kPinUnbound = 2 };

struct OverlayPin {
  Vec2 p;
  int number = 0;
  int state = kPinDisabled;
};

inline const Rgb& meshColour() {
  static const Rgb c = {0.35f, 0.85f, 1.0f};
  return c;
}

/** Draws every unique mesh edge at the given vertex positions. */
inline void drawMesh(Painter& paint, const Mesh& m, const std::vector<Vec2>& verts, float opacity) {
  std::vector<uint64_t> edges;
  edges.reserve(m.tri.size());
  for (size_t t = 0; t < m.tri.size(); t += 3)
    for (int k = 0; k < 3; ++k) {
      uint32_t a = (uint32_t)m.tri[t + k], b = (uint32_t)m.tri[t + (k + 1) % 3];
      if (a > b) std::swap(a, b);
      edges.push_back(((uint64_t)a << 32) | b);
    }
  std::sort(edges.begin(), edges.end());
  edges.erase(std::unique(edges.begin(), edges.end()), edges.end());
  for (uint64_t e : edges) {
    const int a = (int)(e >> 32), b = (int)(e & 0xffffffffu);
    if (a < (int)verts.size() && b < (int)verts.size()) paint.line(verts[a], verts[b], meshColour(), opacity);
  }
}

/** Pin markers: yellow = active, red = could not attach to the mesh,
    grey = disabled (only shown while placing pins). */
inline void drawPins(Painter& paint, const std::vector<OverlayPin>& pins, double scale) {
  const double r = std::max(2.5, 6.0 * scale);
  const int digit = std::max(1, (int)std::lround(2.0 * scale));
  const Rgb black = {0, 0, 0}, white = {1, 1, 1};
  const Rgb yellow = {1.0f, 0.85f, 0.1f}, red = {1.0f, 0.2f, 0.15f}, grey = {0.6f, 0.6f, 0.6f};
  for (const OverlayPin& p : pins) {
    if (!std::isfinite(p.p.x) || !std::isfinite(p.p.y)) continue;
    const Rgb& fill = p.state == kPinActive ? yellow : (p.state == kPinUnbound ? red : grey);
    const float op = p.state == kPinDisabled ? 0.6f : 1.0f;
    const double rr = p.state == kPinDisabled ? r * 0.6 : r;
    paint.disc(p.p, rr + std::max(1.0, scale), black, op);
    paint.disc(p.p, rr, fill, op);
    const double tx = p.p.x + rr + 2.0 * digit, ty = p.p.y + rr;
    paint.number(p.number, tx + digit, ty - digit, digit, black);  // shadow
    paint.number(p.number, tx, ty, digit, p.state == kPinDisabled ? grey : white);
  }
}

}  // namespace puppet
