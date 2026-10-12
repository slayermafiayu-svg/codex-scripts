// Unit tests for PuppetCore.h (no OFX host needed).
#include "PuppetCore.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <vector>

using namespace puppet;

static int gFailures = 0;
#define CHECK(cond)                                                          \
  do {                                                                       \
    if (!(cond)) {                                                           \
      std::printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);          \
      ++gFailures;                                                           \
    }                                                                        \
  } while (0)
#define CHECK_NEAR(a, b, tol)                                                          \
  do {                                                                                 \
    const double va_ = (a), vb_ = (b);                                                 \
    if (!(std::fabs(va_ - vb_) <= (tol))) {                                            \
      std::printf("  FAIL %s:%d: %s = %g, expected %g (tol %g)\n", __FILE__, __LINE__, \
                  #a, va_, vb_, (double)(tol));                                        \
      ++gFailures;                                                                     \
    }                                                                                  \
  } while (0)

template <class T>
struct TestImage {
  std::vector<T> buf;
  ImageRef ref;
  TestImage(int x1, int y1, int w, int h, int nComp, bool flipRows = false) {
    buf.assign((size_t)w * h * nComp, T(0));
    ref.x1 = x1;
    ref.y1 = y1;
    ref.x2 = x1 + w;
    ref.y2 = y1 + h;
    ref.nComp = nComp;
    ref.depth = sizeof(T) == 1 ? kDepthU8 : (sizeof(T) == 2 ? kDepthU16 : kDepthF32);
    const std::ptrdiff_t rb = (std::ptrdiff_t)w * nComp * sizeof(T);
    if (flipRows) {  // top-down memory: first row in memory is the top row
      ref.data = (char*)buf.data() + (h - 1) * rb;
      ref.rowBytes = -rb;
    } else {
      ref.data = buf.data();
      ref.rowBytes = rb;
    }
  }
  T* at(int x, int y) { return (T*)ref.pixelAddress(x, y); }
};

static void testEdt() {
  std::printf("edt\n");
  const int w = 9, h = 7;
  std::vector<float> g((size_t)w * h, kEdtFar);
  g[3 * w + 4] = 0.0f;
  edt2d(g, w, h);
  for (int y = 0; y < h; ++y)
    for (int x = 0; x < w; ++x) CHECK_NEAR(g[(size_t)y * w + x], (x - 4) * (x - 4) + (y - 3) * (y - 3), 1e-3);
  std::vector<float> empty((size_t)w * h, kEdtFar);
  edt2d(empty, w, h);
  for (float v : empty) CHECK(v > 1e19f);
}

static void testGrowMask() {
  std::printf("grow mask (sweep == distance transform)\n");
  std::mt19937 rng(3);
  const int w = 61, h = 47;
  std::vector<uint8_t> mask((size_t)w * h, 0);
  for (auto& v : mask) v = (rng() % 37) == 0;
  for (double e : {1.0, 1.5, 2.9, 3.0, 5.7, 8.0}) {
    std::vector<uint8_t> a, b;
    growMask(mask, w, h, e, a, false);
    growMask(mask, w, h, e, b, true);
    int diff = 0;
    for (size_t i = 0; i < a.size(); ++i) diff += a[i] != b[i];
    CHECK(diff == 0);
    // brute force reference
    int bad = 0;
    for (int y = 0; y < h; ++y)
      for (int x = 0; x < w; ++x) {
        bool in = false;
        for (int yy = 0; yy < h && !in; ++yy)
          for (int xx = 0; xx < w && !in; ++xx)
            if (mask[(size_t)yy * w + xx] && (xx - x) * (xx - x) + (yy - y) * (yy - y) <= e * e + 1e-6) in = true;
        bad += in != (a[(size_t)y * w + x] != 0);
      }
    CHECK(bad == 0);
  }
}

static void testWholeFrameMesh() {
  std::printf("whole frame mesh\n");
  MeshParams mp;
  mp.useAlpha = false;
  mp.cellSize = 10;
  mp.frame = RectI(0, 0, 100, 60);
  Mesh m;
  CHECK(buildMesh(ImageRef(), mp, m));
  CHECK(m.numComp == 1);
  // frame + 1px margin spans cells -1..10 in x and -1..6 in y
  CHECK(m.numTriangles() == 12 * 8 * 2);
  // every triangle is counter-clockwise and non degenerate
  for (int t = 0; t < m.numTriangles(); ++t) {
    const int* v = &m.tri[(size_t)t * 3];
    CHECK(cross(m.rest[v[1]] - m.rest[v[0]], m.rest[v[2]] - m.rest[v[0]]) > 0);
  }
}

static void testAlphaMeshComponents() {
  std::printf("alpha mesh components\n");
  TestImage<uint8_t> img(0, 0, 200, 100, 4);
  for (int y = 0; y < 100; ++y)
    for (int x = 0; x < 200; ++x) {
      const bool a = std::hypot(x - 40.0, y - 50.0) < 25.0;
      const bool b = std::hypot(x - 160.0, y - 50.0) < 25.0;
      if (a || b) img.at(x, y)[3] = 255;
    }
  MeshParams mp;
  mp.useAlpha = true;
  mp.cellSize = 8;
  mp.expansion = 2;
  Mesh m;
  CHECK(buildMesh(img.ref, mp, m));
  CHECK(m.numComp == 2);
  // every opaque pixel centre must be inside the mesh
  int missing = 0;
  for (int y = 0; y < 100; ++y)
    for (int x = 0; x < 200; ++x) {
      if (!img.at(x, y)[3]) continue;
      const int ci = floorDiv(x + 0.5 - m.ox, m.s) - m.ci0, cj = floorDiv(y + 0.5 - m.oy, m.s) - m.cj0;
      if (ci < 0 || cj < 0 || ci >= m.nx || cj >= m.ny || !m.cellOn[(size_t)cj * m.nx + ci]) ++missing;
    }
  CHECK(missing == 0);
  // the gap between the discs is not meshed
  const int ci = floorDiv(100 - m.ox, m.s) - m.ci0, cj = floorDiv(50 - m.oy, m.s) - m.cj0;
  CHECK(!m.cellOn[(size_t)cj * m.nx + ci]);

  // fully transparent image -> no mesh
  TestImage<uint8_t> clear(0, 0, 50, 50, 4);
  Mesh m2;
  CHECK(!buildMesh(clear.ref, mp, m2));
  CHECK(m2.empty());
}

static Mesh frameMesh(int w, int h, double cell) {
  MeshParams mp;
  mp.useAlpha = false;
  mp.cellSize = cell;
  mp.frame = RectI(0, 0, w, h);
  Mesh m;
  buildMesh(ImageRef(), mp, m);
  return m;
}

static Vec2 evalPin(const Mesh& m, const PinBind& b, const std::vector<Vec2>& v) {
  const int* t = &m.tri[(size_t)b.tri * 3];
  return v[t[0]] * b.w[0] + v[t[1]] * b.w[1] + v[t[2]] * b.w[2];
}

static void testBinding() {
  std::printf("pin binding\n");
  Mesh m = frameMesh(100, 100, 10);
  const Vec2 p(33.3, 71.7);
  PinBind b = bindPin(m, p);
  CHECK(b.ok);
  CHECK_NEAR(b.w[0] + b.w[1] + b.w[2], 1.0, 1e-12);
  const Vec2 q = evalPin(m, b, m.rest);
  CHECK_NEAR(q.x, p.x, 1e-9);
  CHECK_NEAR(q.y, p.y, 1e-9);
  // slightly outside the mesh snaps, far outside does not bind
  CHECK(bindPin(m, Vec2(-15, 50)).ok);
  CHECK(!bindPin(m, Vec2(-500, 50)).ok);
}

static void testArapIdentityAndTranslation() {
  std::printf("arap identity / translation\n");
  Mesh m = frameMesh(160, 90, 10);
  std::vector<Vec2> anchors = {Vec2(20, 20), Vec2(140, 25), Vec2(80, 70)};
  std::vector<PinBind> pins;
  for (const Vec2& a : anchors) pins.push_back(bindPin(m, a));
  ArapSolver solver;
  CHECK(solver.setup(m, pins));
  std::vector<Vec2> out;
  solver.solve(m, pins, anchors, 10, out);
  double maxErr = 0;
  for (size_t i = 0; i < out.size(); ++i)
    maxErr = std::max(maxErr, std::hypot(out[i].x - m.rest[i].x, out[i].y - m.rest[i].y));
  CHECK(maxErr < 1e-6);

  std::vector<Vec2> moved;
  for (const Vec2& a : anchors) moved.push_back(a + Vec2(12.5, -7.25));
  solver.solve(m, pins, moved, 10, out);
  maxErr = 0;
  for (size_t i = 0; i < out.size(); ++i)
    maxErr = std::max(maxErr, std::hypot(out[i].x - m.rest[i].x - 12.5, out[i].y - m.rest[i].y + 7.25));
  CHECK(maxErr < 1e-3);

  // a single pin drags its whole piece along rigidly
  std::vector<PinBind> one = {bindPin(m, Vec2(50, 50))};
  ArapSolver s1;
  CHECK(s1.setup(m, one));
  s1.solve(m, one, {Vec2(70, 40)}, 5, out);
  maxErr = 0;
  for (size_t i = 0; i < out.size(); ++i)
    maxErr = std::max(maxErr, std::hypot(out[i].x - m.rest[i].x - 20, out[i].y - m.rest[i].y + 10));
  CHECK(maxErr < 1e-3);
}

static void testArapRotation() {
  std::printf("arap rigid rotation\n");
  Mesh m = frameMesh(120, 120, 8);
  const Vec2 c(60, 60);
  const double th = 40.0 * 3.14159265358979323846 / 180.0;
  auto rot = [&](Vec2 p) {
    const Vec2 d = p - c;
    return c + Vec2(std::cos(th) * d.x - std::sin(th) * d.y, std::sin(th) * d.x + std::cos(th) * d.y);
  };
  std::vector<Vec2> anchors = {Vec2(20, 20), Vec2(100, 25), Vec2(60, 100), Vec2(30, 80)};
  std::vector<PinBind> pins;
  std::vector<Vec2> targets;
  for (const Vec2& a : anchors) {
    pins.push_back(bindPin(m, a));
    targets.push_back(rot(a));
  }
  ArapSolver solver;
  CHECK(solver.setup(m, pins));
  std::vector<Vec2> out;
  solver.solve(m, pins, targets, 60, out);
  double maxErr = 0;
  for (size_t i = 0; i < out.size(); ++i) {
    const Vec2 e = rot(m.rest[i]);
    maxErr = std::max(maxErr, std::hypot(out[i].x - e.x, out[i].y - e.y));
  }
  std::printf("  max deviation from rigid rotation: %.4f px\n", maxErr);
  CHECK(maxErr < 0.5);
  for (size_t p = 0; p < pins.size(); ++p) {
    const Vec2 q = evalPin(m, pins[p], out);
    CHECK_NEAR(q.x, targets[p].x, 0.05);
    CHECK_NEAR(q.y, targets[p].y, 0.05);
  }
}

static void testArapBendIsSane() {
  std::printf("arap bend keeps triangles\n");
  // a long horizontal bar, left end held, right end swung up 90 degrees
  TestImage<uint8_t> img(0, 0, 300, 100, 4);
  for (int y = 35; y < 65; ++y)
    for (int x = 20; x < 280; ++x) img.at(x, y)[3] = 255;
  MeshParams mp;
  mp.cellSize = 10;
  mp.expansion = 1;
  Mesh m;
  CHECK(buildMesh(img.ref, mp, m));
  std::vector<Vec2> anchors = {Vec2(30, 50), Vec2(60, 50), Vec2(270, 50)};
  std::vector<Vec2> targets = {Vec2(30, 50), Vec2(60, 50), Vec2(150, 200)};
  std::vector<PinBind> pins;
  for (const Vec2& a : anchors) pins.push_back(bindPin(m, a));
  ArapSolver solver;
  CHECK(solver.setup(m, pins));
  std::vector<Vec2> out;
  solver.solve(m, pins, targets, 30, out);
  int flipped = 0;
  double minRatio = 1e9, maxRatio = 0;
  for (int t = 0; t < m.numTriangles(); ++t) {
    const int* v = &m.tri[(size_t)t * 3];
    const double a0 = cross(m.rest[v[1]] - m.rest[v[0]], m.rest[v[2]] - m.rest[v[0]]);
    const double a1 = cross(out[v[1]] - out[v[0]], out[v[2]] - out[v[0]]);
    if (a1 <= 0) ++flipped;
    minRatio = std::min(minRatio, a1 / a0);
    maxRatio = std::max(maxRatio, a1 / a0);
  }
  std::printf("  flipped=%d area ratio [%.3f, %.3f]\n", flipped, minRatio, maxRatio);
  CHECK(flipped == 0);
  CHECK(minRatio > 0.3 && maxRatio < 3.0);
}

static void testWarpIdentityAndShift() {
  std::printf("warp identity / integer shift (u8, negative rowBytes)\n");
  std::mt19937 rng(7);
  const int W = 64, H = 48;
  TestImage<uint8_t> src(0, 0, W, H, 4, true);
  for (int y = 0; y < H; ++y)
    for (int x = 0; x < W; ++x)
      for (int c = 0; c < 4; ++c) src.at(x, y)[c] = (uint8_t)(rng() & 255);
  for (int y = 0; y < H; ++y)
    for (int x = 0; x < W; ++x) src.at(x, y)[3] = 255;
  Mesh m = frameMesh(W, H, 7);
  std::vector<WarpTriangle> tris;
  prepareTriangles(m, m.rest, tris);
  TestImage<uint8_t> dst(0, 0, W, H, 4);
  const RectI win(0, 0, W, H);
  clearBand(dst.ref, win, 0, H);
  warpBand(src.ref, dst.ref, win, tris, 0, H);
  int diff = 0;
  for (int y = 0; y < H; ++y)
    for (int x = 0; x < W; ++x)
      for (int c = 0; c < 4; ++c) diff += src.at(x, y)[c] != dst.at(x, y)[c];
  CHECK(diff == 0);

  std::vector<Vec2> shifted = m.rest;
  for (Vec2& p : shifted) p = p + Vec2(3, -2);
  prepareTriangles(m, shifted, tris);
  clearBand(dst.ref, win, 0, H);
  warpBand(src.ref, dst.ref, win, tris, 0, H);
  diff = 0;
  for (int y = 0; y < H; ++y)
    for (int x = 0; x < W; ++x) {
      const int sx = x - 3, sy = y + 2;
      for (int c = 0; c < 4; ++c) {
        const int expect = src.ref.contains(sx, sy) ? src.at(sx, sy)[c] : 0;
        diff += expect != dst.at(x, y)[c];
      }
    }
  CHECK(diff == 0);
}

static void testWarpNoHoles() {
  std::printf("warp has no pinholes (float)\n");
  const int W = 200, H = 150;
  TestImage<float> src(0, 0, W, H, 4);
  for (float& v : src.buf) v = 1.0f;
  Mesh m = frameMesh(W, H, 13);
  // smooth swirl deformation
  std::vector<Vec2> def = m.rest;
  for (Vec2& p : def) {
    const double dx = p.x - 100, dy = p.y - 75, r = std::hypot(dx, dy);
    const double a = 0.6 * std::exp(-r * r / (2 * 50.0 * 50.0));
    p = Vec2(100 + std::cos(a) * dx - std::sin(a) * dy, 75 + std::sin(a) * dx + std::cos(a) * dy);
  }
  std::vector<WarpTriangle> tris;
  prepareTriangles(m, def, tris);
  TestImage<float> dst(0, 0, W, H, 4);
  const RectI win(0, 0, W, H);
  clearBand(dst.ref, win, 0, H);
  // render in uneven bands like the host threads would
  warpBand(src.ref, dst.ref, win, tris, 0, 37);
  warpBand(src.ref, dst.ref, win, tris, 37, 101);
  warpBand(src.ref, dst.ref, win, tris, 101, H);
  int holes = 0;
  for (int y = 1; y < H - 1; ++y)
    for (int x = 1; x < W - 1; ++x) {
      if (dst.at(x, y)[3] > 0.5f) continue;
      int n = 0;
      for (int dy = -1; dy <= 1; ++dy)
        for (int dx = -1; dx <= 1; ++dx) n += dst.at(x + dx, y + dy)[3] > 0.5f;
      holes += n >= 7;
    }
  CHECK(holes == 0);
  // centre is covered
  CHECK(dst.at(100, 75)[3] > 0.99f);
}

static void testStraightAlphaSampling() {
  std::printf("straight alpha sampling\n");
  TestImage<uint8_t> img(0, 0, 2, 1, 4);
  img.ref.straightAlpha = true;
  uint8_t* a = img.at(0, 0);
  a[0] = 255; a[1] = 0; a[2] = 0; a[3] = 255;  // opaque red
  // pixel 1 stays transparent black
  float out[4];
  sampleBilinear<uint8_t, 4>(img.ref, 1.0, 0.5, out);  // halfway between the two
  CHECK_NEAR(out[0], 1.0, 1e-6);  // still pure red, no dark fringe
  CHECK_NEAR(out[3], 0.5, 1e-6);
  img.ref.straightAlpha = false;
  sampleBilinear<uint8_t, 4>(img.ref, 1.0, 0.5, out);
  CHECK_NEAR(out[0], 0.5, 1e-6);  // premultiplied: colour scales with alpha
}

static void testPainterBgra() {
  std::printf("overlay colours honour BGRA\n");
  TestImage<uint8_t> img(0, 0, 4, 4, 4);
  img.ref.bgra = true;
  Painter p(img.ref, RectI(0, 0, 4, 4));
  p.blend(1, 1, Rgb{1.0f, 0.5f, 0.0f}, 1.0f);
  CHECK(img.at(1, 1)[0] == 0);    // B
  CHECK(img.at(1, 1)[1] == 128);  // G
  CHECK(img.at(1, 1)[2] == 255);  // R
  CHECK(img.at(1, 1)[3] == 255);
  p.blend(10, 10, Rgb{1, 1, 1}, 1.0f);  // outside: ignored, no crash
  std::vector<OverlayPin> pins(1);
  pins[0].p = Vec2(2, 2);
  pins[0].number = 12;
  pins[0].state = kPinActive;
  drawPins(p, pins, 1.0);  // must stay inside the clip
}

static void testPerformance() {
  std::printf("performance (1920x1080, 60 cells across)\n");
  const int W = 1920, H = 1080;
  TestImage<uint8_t> src(0, 0, W, H, 4);
  for (int y = 0; y < H; ++y)
    for (int x = 0; x < W; ++x) {
      uint8_t* p = src.at(x, y);
      p[0] = (uint8_t)x; p[1] = (uint8_t)y; p[2] = 128; p[3] = 255;
    }
  auto t0 = std::chrono::steady_clock::now();
  MeshParams mp;
  mp.cellSize = W / 60.0;
  mp.expansion = 3;
  Mesh m;
  CHECK(buildMesh(src.ref, mp, m));
  auto t1 = std::chrono::steady_clock::now();
  std::vector<Vec2> anchors = {Vec2(300, 300), Vec2(1600, 300), Vec2(960, 800), Vec2(960, 200)};
  std::vector<Vec2> targets = {Vec2(300, 300), Vec2(1600, 300), Vec2(1200, 900), Vec2(960, 200)};
  std::vector<PinBind> pins;
  for (const Vec2& a : anchors) pins.push_back(bindPin(m, a));
  ArapSolver solver;
  CHECK(solver.setup(m, pins));
  std::vector<Vec2> out;
  solver.solve(m, pins, targets, 20, out);
  auto t2 = std::chrono::steady_clock::now();
  std::vector<WarpTriangle> tris;
  prepareTriangles(m, out, tris);
  TestImage<uint8_t> dst(0, 0, W, H, 4);
  const RectI win(0, 0, W, H);
  clearBand(dst.ref, win, 0, H);
  warpBand(src.ref, dst.ref, win, tris, 0, H);
  auto t3 = std::chrono::steady_clock::now();
  auto ms = [](auto a, auto b) { return std::chrono::duration<double, std::milli>(b - a).count(); };
  std::printf("  vertices=%zu triangles=%d unknowns=%d\n", m.rest.size(), m.numTriangles(), solver.numUnknowns());
  std::printf("  mesh %.1f ms, solve %.1f ms, warp %.1f ms (single thread)\n", ms(t0, t1), ms(t1, t2), ms(t2, t3));
}

int main() {
  testEdt();
  testGrowMask();
  testWholeFrameMesh();
  testAlphaMeshComponents();
  testBinding();
  testArapIdentityAndTranslation();
  testArapRotation();
  testArapBendIsSane();
  testWarpIdentityAndShift();
  testWarpNoHoles();
  testStraightAlphaSampling();
  testPainterBgra();
  testPerformance();
  if (gFailures) {
    std::printf("%d check(s) FAILED\n", gFailures);
    return 1;
  }
  std::printf("all core tests passed\n");
  return 0;
}
