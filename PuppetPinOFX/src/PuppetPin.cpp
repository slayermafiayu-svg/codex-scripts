/*
  Puppet Pin - an OpenFX image effect for VEGAS Pro (2026 / 26 and earlier
  OFX-capable versions), written directly against the OpenFX C API.

  The effect covers the picture (or the opaque part of it) with a triangle
  mesh and bends that mesh as-rigidly-as-possible so that the pins you place
  follow their keyframed positions - the same idea as the puppet tools in
  other applications.

  How the pins work
  -----------------
  Every pin has a visible, keyframable "Position" (a 2D point parameter, so
  VEGAS shows its cross-hair in the video preview) and a hidden "Anchor" that
  records where the pin is stuck to the picture.

    * While "Setup Mode" is on, the picture is not deformed and moving a pin
      moves its anchor with it: you are sticking pins onto the image.
    * With "Setup Mode" off, moving a pin drags the image.
    * Enabling a pin anchors it where it currently is, so adding a pin never
      makes the image jump.

  On hosts that support OFX overlays (VEGAS does on video events) the plug-in
  also draws the pins and the mesh over the preview: click on the picture to
  add a pin, drag a pin to move it, Ctrl+click (or Delete) to remove it.

  VEGAS specifics handled here (see the Sony/MAGIX OFX extension header and
  the notes in README.md):
    * 8-bit images may come in BGRA order (OfxImageEffectPropPixelOrder).
    * The render scale is given in the render arguments only, not on images.
    * Nothing but property/param handle fetching happens in createInstance
      (VEGAS crashes on edit blocks there).
    * Every property read tolerates a missing property.
*/

#include "ofxCore.h"
#include "ofxImageEffect.h"
#include "ofxInteract.h"
#include "ofxKeySyms.h"
#include "ofxMemory.h"
#include "ofxMessage.h"
#include "ofxMultiThread.h"
#include "ofxOld.h"
#include "ofxParam.h"
#include "ofxProperty.h"
#include "ofxDrawSuite.h"

#include "PuppetCore.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <new>
#include <string>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#endif

using puppet::Vec2;

////////////////////////////////////////////////////////////////////////////////
// Identity

#define kPluginIdentifier "io.github.slayermafiayu.PuppetPin"
#define kPluginVersionMajor 1
#define kPluginVersionMinor 0
#define kPluginLabel "Puppet Pin"
#define kPluginGrouping "Distort"
#define kPluginDescription                                                                   \
  "Puppet-style mesh warp. The picture (or the opaque part of it) is covered with a "      \
  "triangle mesh that bends as rigidly as possible so that every pin follows its "         \
  "Position. Turn on Setup Mode, enable pins and place them on the picture; then turn "    \
  "Setup Mode off and move or keyframe the pins to animate."

// VEGAS extensions (values from the Sony/MAGIX OFX extension header).
#define kVegasPropPixelOrder "OfxImageEffectPropPixelOrder"
#define kVegasPixelOrderBGRA "OfxImagePixelOrderBGRA"
#define kVegasBitDepthByteBGR "OfxBitDepthByteBGR"
#define kVegasBitDepthShortBGR "OfxBitDepthShortBGR"
#define kVegasBitDepthFloatBGR "OfxBitDepthFloatBGR"

static const int kNumPins = 20;

// Parameter names (also what VEGAS scripts see through OFXEffect.FindParameterByName).
#define kParamSetup "setupMode"
#define kParamResetAll "resetAllPins"
#define kParamGroupMesh "meshGroup"
#define kParamMeshArea "meshArea"
#define kParamAlphaThreshold "alphaThreshold"
#define kParamDensity "meshDensity"
#define kParamExpansion "expansion"
#define kParamIterations "rigidity"
#define kParamGroupDisplay "displayGroup"
#define kParamOverlayMesh "overlayMesh"
#define kParamBurnOverlay "burnOverlay"
#define kParamFlipY "invertPinY"

enum { kMeshAreaAlpha = 0, kMeshAreaWholeFrame = 1 };

static std::string pinParamName(int k, const char* suffix) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "pin%02d%s", k + 1, suffix);
  return buf;
}

// Parses "pinNN_xxx" -> (NN-1, "_xxx"). Returns -1 for anything else.
static int parsePinParam(const char* name, std::string& suffix) {
  if (!name || std::strncmp(name, "pin", 3) != 0) return -1;
  if (name[3] < '0' || name[3] > '9' || name[4] < '0' || name[4] > '9') return -1;
  const int k = (name[3] - '0') * 10 + (name[4] - '0') - 1;
  if (k < 0 || k >= kNumPins) return -1;
  suffix = name + 5;
  return k;
}

////////////////////////////////////////////////////////////////////////////////
// Host suites

static OfxHost* gHost = nullptr;
static const OfxPropertySuiteV1* gProp = nullptr;
static const OfxImageEffectSuiteV1* gEffect = nullptr;
static const OfxParameterSuiteV1* gParam = nullptr;
static const OfxMultiThreadSuiteV1* gThread = nullptr;
static const OfxInteractSuiteV1* gInteract = nullptr;
static const OfxDrawSuiteV1* gDraw = nullptr;
static bool gHostSupportsOverlays = false;

////////////////////////////////////////////////////////////////////////////////
// Property helpers: a missing property is never an error, it yields the default.

static int propInt(OfxPropertySetHandle h, const char* name, int idx, int def) {
  int v = def;
  if (h && gProp && gProp->propGetInt(h, name, idx, &v) == kOfxStatOK) return v;
  return def;
}
static double propDouble(OfxPropertySetHandle h, const char* name, int idx, double def) {
  double v = def;
  if (h && gProp && gProp->propGetDouble(h, name, idx, &v) == kOfxStatOK && std::isfinite(v)) return v;
  return def;
}
static std::string propString(OfxPropertySetHandle h, const char* name, int idx, const char* def = "") {
  char* v = nullptr;
  if (h && gProp && gProp->propGetString(h, name, idx, &v) == kOfxStatOK && v) return v;
  return def;
}
static void* propPointer(OfxPropertySetHandle h, const char* name, int idx) {
  void* v = nullptr;
  if (h && gProp && gProp->propGetPointer(h, name, idx, &v) == kOfxStatOK) return v;
  return nullptr;
}
static void setStr(OfxPropertySetHandle h, const char* name, int idx, const char* v) {
  if (h) gProp->propSetString(h, name, idx, v);
}
static void setInt(OfxPropertySetHandle h, const char* name, int idx, int v) {
  if (h) gProp->propSetInt(h, name, idx, v);
}
static void setDbl(OfxPropertySetHandle h, const char* name, int idx, double v) {
  if (h) gProp->propSetDouble(h, name, idx, v);
}

////////////////////////////////////////////////////////////////////////////////
// Instance data

struct OverlaySnapshot {
  bool valid = false;
  double time = 0.0;
  std::vector<Vec2> edges;   // pairs of canonical coordinates
  bool pinBound[kNumPins] = {};
};

struct Instance {
  OfxImageEffectHandle effect = nullptr;
  OfxParamSetHandle paramSet = nullptr;
  OfxImageClipHandle srcClip = nullptr, dstClip = nullptr;
  OfxParamHandle setup = nullptr, resetAll = nullptr, meshArea = nullptr, alphaThreshold = nullptr,
                 density = nullptr, expansion = nullptr, iterations = nullptr, overlayMesh = nullptr,
                 burnOverlay = nullptr, flipY = nullptr;
  OfxParamHandle pinOn[kNumPins] = {}, pinPos[kNumPins] = {}, pinAnchor[kNumPins] = {},
                 pinReset[kNumPins] = {};

  std::mutex snapshotMutex;
  OverlaySnapshot snapshot;
};

static OfxParamHandle fetchParam(OfxParamSetHandle ps, const char* name) {
  OfxParamHandle h = nullptr;
  if (ps && gParam->paramGetHandle(ps, name, &h, nullptr) != kOfxStatOK) h = nullptr;
  return h;
}

static Instance* getInstance(OfxImageEffectHandle effect) {
  OfxPropertySetHandle props = nullptr;
  if (!effect || gEffect->getPropertySet(effect, &props) != kOfxStatOK) return nullptr;
  return (Instance*)propPointer(props, kOfxPropInstanceData, 0);
}

////////////////////////////////////////////////////////////////////////////////
// Parameter value helpers (null handles give the default)

static bool getBool(OfxParamHandle h, double t, bool def) {
  int v = def ? 1 : 0;
  if (h && gParam->paramGetValueAtTime(h, t, &v) == kOfxStatOK) return v != 0;
  return def;
}
static int getInt(OfxParamHandle h, double t, int def) {
  int v = def;
  if (h && gParam->paramGetValueAtTime(h, t, &v) == kOfxStatOK) return v;
  return def;
}
static double getDouble(OfxParamHandle h, double t, double def) {
  double v = def;
  if (h && gParam->paramGetValueAtTime(h, t, &v) == kOfxStatOK && std::isfinite(v)) return v;
  return def;
}
static bool get2D(OfxParamHandle h, double t, double& x, double& y) {
  double a = 0, b = 0;
  if (h && gParam->paramGetValueAtTime(h, t, &a, &b) == kOfxStatOK && std::isfinite(a) && std::isfinite(b)) {
    x = a;
    y = b;
    return true;
  }
  return false;
}
// Sets a 2D value: as a keyframe at time t when the parameter is animated,
// otherwise as its (static) value.
static void set2D(OfxParamHandle h, double t, double x, double y) {
  if (!h) return;
  unsigned int keys = 0;
  if (gParam->paramGetNumKeys(h, &keys) == kOfxStatOK && keys > 0)
    gParam->paramSetValueAtTime(h, t, x, y);
  else
    gParam->paramSetValue(h, x, y);
}
static void setBool(OfxParamHandle h, bool v) {
  if (h) gParam->paramSetValue(h, v ? 1 : 0);
}

////////////////////////////////////////////////////////////////////////////////
// Geometry: maps normalised pin values to image pixels and canonical coordinates.

struct Geometry {
  double projX = 0, projY = 0, projW = 1, projH = 1;  // canonical
  double par = 1.0;
  double rsx = 1.0, rsy = 1.0;
  bool flipY = false;

  Vec2 normToCanonical(double u, double v) const {
    if (flipY) v = 1.0 - v;
    return Vec2(u * projW + projX, v * projH + projY);
  }
  Vec2 canonicalToNorm(Vec2 c) const {
    double u = (c.x - projX) / projW, v = (c.y - projY) / projH;
    if (flipY) v = 1.0 - v;
    return Vec2(u, v);
  }
  Vec2 canonicalToPixel(Vec2 c) const { return Vec2(c.x * rsx / par, c.y * rsy); }
  Vec2 pixelToCanonical(Vec2 p) const { return Vec2(p.x * par / rsx, p.y / rsy); }
  Vec2 normToPixel(double u, double v) const { return canonicalToPixel(normToCanonical(u, v)); }
};

static Geometry makeGeometry(Instance* inst, double time, double rsx, double rsy, bool flipY) {
  Geometry g;
  g.rsx = rsx > 0 ? rsx : 1.0;
  g.rsy = rsy > 0 ? rsy : 1.0;
  g.flipY = flipY;
  OfxPropertySetHandle fx = nullptr, clipProps = nullptr;
  gEffect->getPropertySet(inst->effect, &fx);
  const OfxImageClipHandle clip = inst->srcClip ? inst->srcClip : inst->dstClip;
  if (clip) gEffect->clipGetPropertySet(clip, &clipProps);
  g.par = propDouble(clipProps, kOfxImagePropPixelAspectRatio, 0, 1.0);
  if (!(g.par > 0.01 && g.par < 100.0)) g.par = 1.0;
  g.projW = propDouble(fx, kOfxImageEffectPropProjectSize, 0, 0.0);
  g.projH = propDouble(fx, kOfxImageEffectPropProjectSize, 1, 0.0);
  g.projX = propDouble(fx, kOfxImageEffectPropProjectOffset, 0, 0.0);
  g.projY = propDouble(fx, kOfxImageEffectPropProjectOffset, 1, 0.0);
  if (!(g.projW > 0.5 && g.projH > 0.5)) {
    // fall back to the source region of definition
    OfxRectD rod = {0, 0, 0, 0};
    if (clip && gEffect->clipGetRegionOfDefinition(clip, time, &rod) == kOfxStatOK &&
        rod.x2 - rod.x1 > 0.5 && rod.y2 - rod.y1 > 0.5) {
      g.projX = rod.x1;
      g.projY = rod.y1;
      g.projW = rod.x2 - rod.x1;
      g.projH = rod.y2 - rod.y1;
    } else {
      g.projX = g.projY = 0;
      g.projW = 1920;
      g.projH = 1080;
    }
  }
  return g;
}

////////////////////////////////////////////////////////////////////////////////
// Settings read at a given time

struct PinValues {
  bool on = false;
  double pu = 0, pv = 0;  // position (normalised)
  double au = 0, av = 0;  // anchor (normalised)
};

struct Settings {
  bool setup = true;
  int meshArea = kMeshAreaAlpha;
  double alphaThreshold = 0.05;
  int density = 48;
  double expansion = 3.0;
  int iterations = 20;
  bool overlayMesh = true;
  bool burnOverlay = false;
  bool flipY = false;
  PinValues pins[kNumPins];
};

static Settings readSettings(Instance* inst, double t) {
  Settings s;
  s.setup = getBool(inst->setup, t, true);
  s.meshArea = getInt(inst->meshArea, t, kMeshAreaAlpha);
  s.alphaThreshold = std::max(0.0, std::min(1.0, getDouble(inst->alphaThreshold, t, 0.05)));
  s.density = std::max(4, std::min(400, getInt(inst->density, t, 48)));
  s.expansion = std::max(0.0, std::min(1000.0, getDouble(inst->expansion, t, 3.0)));
  s.iterations = std::max(0, std::min(200, getInt(inst->iterations, t, 20)));
  s.overlayMesh = getBool(inst->overlayMesh, t, true);
  s.burnOverlay = getBool(inst->burnOverlay, t, false);
  s.flipY = getBool(inst->flipY, t, false);
  for (int k = 0; k < kNumPins; ++k) {
    PinValues& p = s.pins[k];
    p.on = getBool(inst->pinOn[k], t, false);
    if (!get2D(inst->pinPos[k], t, p.pu, p.pv)) p.on = false;
    if (!get2D(inst->pinAnchor[k], t, p.au, p.av)) {
      p.au = p.pu;
      p.av = p.pv;
    }
  }
  return s;
}

static bool anyPinDisplaced(const Settings& s) {
  for (int k = 0; k < kNumPins; ++k) {
    const PinValues& p = s.pins[k];
    if (p.on && (std::fabs(p.pu - p.au) > 1e-9 || std::fabs(p.pv - p.av) > 1e-9)) return true;
  }
  return false;
}

// Burned-in overlay: while setting up, the mesh (the detected surface) and
// the pins are drawn into the picture so they are visible whatever the host
// can draw; afterwards only if the user asks for it.
static bool burnsMesh(const Settings& s) { return s.setup || s.burnOverlay; }
static bool burnsPins(const Settings& s) { return s.setup || s.burnOverlay; }

////////////////////////////////////////////////////////////////////////////////
// Images

struct ImageHolder {
  OfxPropertySetHandle h = nullptr;
  ImageHolder() = default;
  ImageHolder(const ImageHolder&) = delete;
  ImageHolder& operator=(const ImageHolder&) = delete;
  ~ImageHolder() {
    if (h) gEffect->clipReleaseImage(h);
  }
};

// Fills ref from an OFX image. Returns kOfxStatOK or an error code.
static OfxStatus describeImage(OfxPropertySetHandle img, OfxImageClipHandle clip, puppet::ImageRef& ref) {
  ref = puppet::ImageRef();
  ref.data = propPointer(img, kOfxImagePropData, 0);
  ref.x1 = propInt(img, kOfxImagePropBounds, 0, 0);
  ref.y1 = propInt(img, kOfxImagePropBounds, 1, 0);
  ref.x2 = propInt(img, kOfxImagePropBounds, 2, 0);
  ref.y2 = propInt(img, kOfxImagePropBounds, 3, 0);
  ref.rowBytes = propInt(img, kOfxImagePropRowBytes, 0, 0);
  const std::string depth = propString(img, kOfxImageEffectPropPixelDepth, 0);
  bool bgrDepth = false;
  if (depth == kOfxBitDepthByte) ref.depth = puppet::kDepthU8;
  else if (depth == kOfxBitDepthShort) ref.depth = puppet::kDepthU16;
  else if (depth == kOfxBitDepthFloat) ref.depth = puppet::kDepthF32;
  else if (depth == kVegasBitDepthByteBGR) ref.depth = puppet::kDepthU8, bgrDepth = true;
  else if (depth == kVegasBitDepthShortBGR) ref.depth = puppet::kDepthU16, bgrDepth = true;
  else if (depth == kVegasBitDepthFloatBGR) ref.depth = puppet::kDepthF32, bgrDepth = true;
  else return kOfxStatErrUnsupported;
  const std::string comps = propString(img, kOfxImageEffectPropComponents, 0);
  if (comps == kOfxImageComponentRGBA) ref.nComp = 4;
  else if (comps == kOfxImageComponentRGB) ref.nComp = 3;
  else if (comps == kOfxImageComponentAlpha) ref.nComp = 1;
  else return kOfxStatErrUnsupported;
  ref.straightAlpha = propString(img, kOfxImageEffectPropPreMultiplication, 0) == kOfxImageUnPreMultiplied;
  std::string order = propString(img, kVegasPropPixelOrder, 0);
  if (order.empty() && clip) {
    OfxPropertySetHandle cp = nullptr;
    gEffect->clipGetPropertySet(clip, &cp);
    order = propString(cp, kVegasPropPixelOrder, 0);
  }
  ref.bgra = bgrDepth || order == kVegasPixelOrderBGRA;
  if (!ref.valid()) return kOfxStatFailed;
  const long long minRow = (long long)(ref.x2 - ref.x1) * ref.nComp * ref.bytesPerComponent();
  if (ref.rowBytes == 0 || std::llabs((long long)ref.rowBytes) < minRow) return kOfxStatFailed;
  return kOfxStatOK;
}

////////////////////////////////////////////////////////////////////////////////
// Multi-threaded band processing

struct BandJob {
  enum Kind { kCopy, kWarp } kind = kCopy;
  const puppet::ImageRef* src = nullptr;
  const puppet::ImageRef* dst = nullptr;
  puppet::RectI window;
  const std::vector<puppet::WarpTriangle>* tris = nullptr;
};

static void runBand(const BandJob& job, int y1, int y2) {
  if (job.kind == BandJob::kCopy) {
    puppet::copyBand(*job.src, *job.dst, job.window, y1, y2);
  } else {
    puppet::clearBand(*job.dst, job.window, y1, y2);
    puppet::warpBand(*job.src, *job.dst, job.window, *job.tris, y1, y2);
  }
}

static void bandThread(unsigned int index, unsigned int count, void* arg) {
  const BandJob& job = *(const BandJob*)arg;
  if (count == 0) return;
  const int h = job.window.y2 - job.window.y1;
  const int y1 = job.window.y1 + (int)((long long)h * index / count);
  const int y2 = job.window.y1 + (int)((long long)h * (index + 1) / count);
  if (y2 > y1) runBand(job, y1, y2);
}

static void runBands(const BandJob& job) {
  const int h = job.window.y2 - job.window.y1;
  if (h <= 0) return;
  unsigned int cpus = 1;
  if (gThread && gThread->multiThreadNumCPUs && gThread->multiThreadNumCPUs(&cpus) != kOfxStatOK) cpus = 1;
  cpus = std::max(1u, std::min(cpus, 64u));
  const unsigned int n = std::min<unsigned int>(cpus, (unsigned int)std::max(1, h / 16));
  if (n > 1 && gThread && gThread->multiThread &&
      gThread->multiThread(bandThread, n, (void*)&job) == kOfxStatOK)
    return;
  runBand(job, job.window.y1, job.window.y2);
}

////////////////////////////////////////////////////////////////////////////////
// The puppet computation shared by render and the overlay snapshot

struct PuppetResult {
  puppet::Mesh mesh;
  std::vector<Vec2> deformed;  // per vertex, pixels
  bool meshOk = false;
  bool pinBound[kNumPins] = {};
  Vec2 pinAnchorPx[kNumPins], pinPosPx[kNumPins];
};

static void computePuppet(const Settings& s, const Geometry& g, const puppet::ImageRef& src,
                          bool deform, PuppetResult& r) {
  puppet::MeshParams mp;
  mp.useAlpha = s.meshArea == kMeshAreaAlpha;
  mp.alphaThreshold = s.alphaThreshold;
  const double frameWpx = g.projW * g.rsx / g.par, frameHpx = g.projH * g.rsy;
  double cell = frameWpx / std::max(1, s.density);
  // keep the number of cells reasonable whatever the host hands us
  const double maxCells = 60000.0;
  const double area = std::max(1.0, (double)src.width() * src.height());
  cell = std::max(cell, std::sqrt(area / maxCells));
  cell = std::max(cell, 1.0);
  mp.cellSize = cell;
  mp.expansion = s.expansion * g.rsx;
  const Vec2 origin = g.canonicalToPixel(Vec2(g.projX, g.projY));
  mp.gridOrigin = origin;
  mp.frame = puppet::RectI(src.x1, src.y1, src.x2, src.y2);
  (void)frameHpx;
  r.meshOk = puppet::buildMesh(src, mp, r.mesh);

  std::vector<puppet::PinBind> binds;
  std::vector<Vec2> targets;
  for (int k = 0; k < kNumPins; ++k) {
    const PinValues& p = s.pins[k];
    r.pinAnchorPx[k] = g.normToPixel(p.au, p.av);
    r.pinPosPx[k] = g.normToPixel(p.pu, p.pv);
    r.pinBound[k] = false;
    if (!p.on || !r.meshOk) continue;
    puppet::PinBind b = puppet::bindPin(r.mesh, r.pinAnchorPx[k]);
    r.pinBound[k] = b.ok;
    if (b.ok) {
      binds.push_back(b);
      targets.push_back(r.pinPosPx[k]);
    }
  }
  r.deformed = r.mesh.rest;
  if (deform && r.meshOk && !binds.empty()) {
    puppet::ArapSolver solver;
    if (solver.setup(r.mesh, binds)) solver.solve(r.mesh, binds, targets, s.iterations, r.deformed);
  }
}

static void storeSnapshot(Instance* inst, double time, const Geometry& g, const PuppetResult& r) {
  OverlaySnapshot snap;
  snap.valid = r.meshOk;
  snap.time = time;
  for (int k = 0; k < kNumPins; ++k) snap.pinBound[k] = r.pinBound[k];
  if (r.meshOk) {
    std::vector<uint64_t> edges;
    edges.reserve(r.mesh.tri.size());
    for (size_t t = 0; t < r.mesh.tri.size(); t += 3)
      for (int e = 0; e < 3; ++e) {
        uint32_t a = (uint32_t)r.mesh.tri[t + e], b = (uint32_t)r.mesh.tri[t + (e + 1) % 3];
        if (a > b) std::swap(a, b);
        edges.push_back(((uint64_t)a << 32) | b);
      }
    std::sort(edges.begin(), edges.end());
    edges.erase(std::unique(edges.begin(), edges.end()), edges.end());
    snap.edges.reserve(edges.size() * 2);
    for (uint64_t e : edges) {
      snap.edges.push_back(g.pixelToCanonical(r.deformed[(size_t)(e >> 32)]));
      snap.edges.push_back(g.pixelToCanonical(r.deformed[(size_t)(e & 0xffffffffu)]));
    }
  }
  std::lock_guard<std::mutex> lock(inst->snapshotMutex);
  inst->snapshot.valid = snap.valid;
  inst->snapshot.time = snap.time;
  inst->snapshot.edges.swap(snap.edges);
  for (int k = 0; k < kNumPins; ++k) inst->snapshot.pinBound[k] = snap.pinBound[k];
}

////////////////////////////////////////////////////////////////////////////////
// Actions

static OfxStatus actionLoad() {
  if (!gHost || !gHost->fetchSuite) return kOfxStatErrMissingHostFeature;
  gProp = (const OfxPropertySuiteV1*)gHost->fetchSuite(gHost->host, kOfxPropertySuite, 1);
  gEffect = (const OfxImageEffectSuiteV1*)gHost->fetchSuite(gHost->host, kOfxImageEffectSuite, 1);
  gParam = (const OfxParameterSuiteV1*)gHost->fetchSuite(gHost->host, kOfxParameterSuite, 1);
  gThread = (const OfxMultiThreadSuiteV1*)gHost->fetchSuite(gHost->host, kOfxMultiThreadSuite, 1);
  gInteract = (const OfxInteractSuiteV1*)gHost->fetchSuite(gHost->host, kOfxInteractSuite, 1);
  gDraw = (const OfxDrawSuiteV1*)gHost->fetchSuite(gHost->host, kOfxDrawSuite, 1);
  if (!gProp || !gEffect || !gParam) return kOfxStatErrMissingHostFeature;
  gHostSupportsOverlays = propInt(gHost->host, kOfxImageEffectPropSupportsOverlays, 0, 0) != 0 && gInteract;
  return kOfxStatOK;
}

static OfxStatus overlayMainV1(const char* action, const void* handle, OfxPropertySetHandle inArgs,
                               OfxPropertySetHandle outArgs);
static OfxStatus overlayMainV2(const char* action, const void* handle, OfxPropertySetHandle inArgs,
                               OfxPropertySetHandle outArgs);

static OfxStatus actionDescribe(OfxImageEffectHandle effect) {
  OfxPropertySetHandle p = nullptr;
  if (gEffect->getPropertySet(effect, &p) != kOfxStatOK) return kOfxStatFailed;
  setStr(p, kOfxPropLabel, 0, kPluginLabel);
  setStr(p, kOfxPropShortLabel, 0, kPluginLabel);
  setStr(p, kOfxPropLongLabel, 0, "Puppet Pin (mesh warp)");
  setStr(p, kOfxPropPluginDescription, 0, kPluginDescription);
  setStr(p, kOfxImageEffectPluginPropGrouping, 0, kPluginGrouping);
  setStr(p, kOfxPropVersionLabel, 0, "1.0");
  setInt(p, kOfxPropVersion, 0, kPluginVersionMajor);
  setInt(p, kOfxPropVersion, 1, kPluginVersionMinor);
  setStr(p, kOfxImageEffectPropSupportedContexts, 0, kOfxImageEffectContextFilter);
  setStr(p, kOfxImageEffectPropSupportedContexts, 1, kOfxImageEffectContextGeneral);
  setStr(p, kOfxImageEffectPropSupportedPixelDepths, 0, kOfxBitDepthByte);
  setStr(p, kOfxImageEffectPropSupportedPixelDepths, 1, kOfxBitDepthShort);
  setStr(p, kOfxImageEffectPropSupportedPixelDepths, 2, kOfxBitDepthFloat);
  setInt(p, kOfxImageEffectPluginPropSingleInstance, 0, 0);
  setInt(p, kOfxImageEffectPluginPropHostFrameThreading, 0, 0);
  setInt(p, kOfxImageEffectPropSupportsMultiResolution, 0, 1);
  setInt(p, kOfxImageEffectPropSupportsTiles, 0, 0);
  setInt(p, kOfxImageEffectPropTemporalClipAccess, 0, 0);
  setInt(p, kOfxImageEffectPluginPropFieldRenderTwiceAlways, 0, 0);
  setInt(p, kOfxImageEffectPropSupportsMultipleClipDepths, 0, 0);
  setInt(p, kOfxImageEffectPropSupportsMultipleClipPARs, 0, 0);
  setStr(p, kOfxImageEffectPluginRenderThreadSafety, 0, kOfxImageEffectRenderFullySafe);
  gProp->propSetPointer(p, kOfxImageEffectPluginPropOverlayInteractV1, 0, (void*)&overlayMainV1);
  if (gDraw) gProp->propSetPointer(p, kOfxImageEffectPluginPropOverlayInteractV2, 0, (void*)&overlayMainV2);
  return kOfxStatOK;
}

// --- parameter definition helpers -------------------------------------------

struct ParamDefiner {
  OfxParamSetHandle ps = nullptr;
  std::vector<std::string> topLevel;

  OfxPropertySetHandle define(const char* type, const std::string& name, const char* label,
                              const char* hint, const char* parent) {
    OfxPropertySetHandle props = nullptr;
    if (gParam->paramDefine(ps, type, name.c_str(), &props) != kOfxStatOK) return nullptr;
    setStr(props, kOfxPropLabel, 0, label);
    setStr(props, kOfxPropShortLabel, 0, label);
    setStr(props, kOfxPropLongLabel, 0, label);
    setStr(props, kOfxParamPropScriptName, 0, name.c_str());
    if (hint) setStr(props, kOfxParamPropHint, 0, hint);
    if (parent) setStr(props, kOfxParamPropParent, 0, parent);
    else if (std::strcmp(type, kOfxParamTypePage) != 0) topLevel.push_back(name);
    return props;
  }
  void group(const std::string& name, const char* label, bool open, const char* hint = nullptr) {
    OfxPropertySetHandle p = define(kOfxParamTypeGroup, name, label, hint, nullptr);
    setInt(p, kOfxParamPropGroupOpen, 0, open ? 1 : 0);
  }
  void boolean(const std::string& name, const char* label, const char* hint, bool def,
               const char* parent, bool animates = false) {
    OfxPropertySetHandle p = define(kOfxParamTypeBoolean, name, label, hint, parent);
    setInt(p, kOfxParamPropDefault, 0, def ? 1 : 0);
    setInt(p, kOfxParamPropAnimates, 0, animates ? 1 : 0);
  }
  void button(const std::string& name, const char* label, const char* hint, const char* parent) {
    define(kOfxParamTypePushButton, name, label, hint, parent);
  }
  void integer(const std::string& name, const char* label, const char* hint, int def, int mn, int mx,
               int dmn, int dmx, const char* parent) {
    OfxPropertySetHandle p = define(kOfxParamTypeInteger, name, label, hint, parent);
    setInt(p, kOfxParamPropDefault, 0, def);
    setInt(p, kOfxParamPropMin, 0, mn);
    setInt(p, kOfxParamPropMax, 0, mx);
    setInt(p, kOfxParamPropDisplayMin, 0, dmn);
    setInt(p, kOfxParamPropDisplayMax, 0, dmx);
    setInt(p, kOfxParamPropAnimates, 0, 0);
  }
  void real(const std::string& name, const char* label, const char* hint, double def, double mn,
            double mx, double dmn, double dmx, double inc, int digits, const char* parent) {
    OfxPropertySetHandle p = define(kOfxParamTypeDouble, name, label, hint, parent);
    setStr(p, kOfxParamPropDoubleType, 0, kOfxParamDoubleTypePlain);
    setDbl(p, kOfxParamPropDefault, 0, def);
    setDbl(p, kOfxParamPropMin, 0, mn);
    setDbl(p, kOfxParamPropMax, 0, mx);
    setDbl(p, kOfxParamPropDisplayMin, 0, dmn);
    setDbl(p, kOfxParamPropDisplayMax, 0, dmx);
    setDbl(p, kOfxParamPropIncrement, 0, inc);
    setInt(p, kOfxParamPropDigits, 0, digits);
    setInt(p, kOfxParamPropAnimates, 0, 0);
  }
  void choice(const std::string& name, const char* label, const char* hint,
              const std::vector<const char*>& options, int def, const char* parent) {
    OfxPropertySetHandle p = define(kOfxParamTypeChoice, name, label, hint, parent);
    for (size_t i = 0; i < options.size(); ++i) setStr(p, kOfxParamPropChoiceOption, (int)i, options[i]);
    setInt(p, kOfxParamPropDefault, 0, def);
    setInt(p, kOfxParamPropAnimates, 0, 0);
  }
  void point(const std::string& name, const char* label, const char* hint, double u, double v,
             const char* parent, bool animates, bool secret) {
    OfxPropertySetHandle p = define(kOfxParamTypeDouble2D, name, label, hint, parent);
    // Normalised absolute position (OFX 1.1, as VEGAS's own effects use): the
    // host draws a cross-hair for it and the values stay 0..1 inside the frame.
    setStr(p, kOfxParamPropDoubleType, 0, kOfxParamDoubleTypeNormalisedXYAbsolute);
    setDbl(p, kOfxParamPropDefault, 0, u);
    setDbl(p, kOfxParamPropDefault, 1, v);
    setDbl(p, kOfxParamPropMin, 0, -10.0);
    setDbl(p, kOfxParamPropMin, 1, -10.0);
    setDbl(p, kOfxParamPropMax, 0, 11.0);
    setDbl(p, kOfxParamPropMax, 1, 11.0);
    setDbl(p, kOfxParamPropDisplayMin, 0, 0.0);
    setDbl(p, kOfxParamPropDisplayMin, 1, 0.0);
    setDbl(p, kOfxParamPropDisplayMax, 0, 1.0);
    setDbl(p, kOfxParamPropDisplayMax, 1, 1.0);
    setDbl(p, kOfxParamPropIncrement, 0, 0.001);
    setInt(p, kOfxParamPropDigits, 0, 4);
    setInt(p, kOfxParamPropAnimates, 0, animates ? 1 : 0);
    if (secret) setInt(p, kOfxParamPropSecret, 0, 1);
  }
};

static void defaultPinLocation(int k, double& u, double& v) {
  // spread the (initially disabled) pins over the picture in a 5 x 4 grid
  const int cols = 5;
  u = 0.2 + 0.15 * (k % cols);
  v = 0.8 - 0.2 * (k / cols);
}

static OfxStatus actionDescribeInContext(OfxImageEffectHandle effect, OfxPropertySetHandle inArgs) {
  const std::string context = propString(inArgs, kOfxImageEffectPropContext, 0, kOfxImageEffectContextFilter);
  OfxPropertySetHandle clip = nullptr;
  if (gEffect->clipDefine(effect, kOfxImageEffectSimpleSourceClipName, &clip) != kOfxStatOK) return kOfxStatFailed;
  setStr(clip, kOfxImageEffectPropSupportedComponents, 0, kOfxImageComponentRGBA);
  setStr(clip, kOfxImageEffectPropSupportedComponents, 1, kOfxImageComponentRGB);
  setStr(clip, kOfxImageEffectPropSupportedComponents, 2, kOfxImageComponentAlpha);
  setInt(clip, kOfxImageEffectPropTemporalClipAccess, 0, 0);
  setInt(clip, kOfxImageEffectPropSupportsTiles, 0, 0);
  // interlaced material: full height, line doubled fields keep pin coordinates valid
  setStr(clip, kOfxImageClipPropFieldExtraction, 0, kOfxImageFieldDoubled);
  if (context == kOfxImageEffectContextGeneral) {
    setInt(clip, kOfxImageClipPropOptional, 0, 0);
    setInt(clip, kOfxImageClipPropIsMask, 0, 0);
  }
  if (gEffect->clipDefine(effect, kOfxImageEffectOutputClipName, &clip) != kOfxStatOK) return kOfxStatFailed;
  setStr(clip, kOfxImageEffectPropSupportedComponents, 0, kOfxImageComponentRGBA);
  setStr(clip, kOfxImageEffectPropSupportedComponents, 1, kOfxImageComponentRGB);
  setStr(clip, kOfxImageEffectPropSupportedComponents, 2, kOfxImageComponentAlpha);
  setInt(clip, kOfxImageEffectPropSupportsTiles, 0, 0);

  ParamDefiner d;
  if (gEffect->getParamSet(effect, &d.ps) != kOfxStatOK) return kOfxStatFailed;

  d.boolean(kParamSetup, "Setup Mode (place pins)",
            "On: the picture is shown undeformed with its mesh, and moving a pin sticks it to a new "
            "spot on the picture. Off: moving a pin bends the picture.",
            true, nullptr);
  d.button(kParamResetAll, "Reset All Pins",
           "Moves every enabled pin back onto its anchor (undoes the deformation at this frame).", nullptr);

  d.group(kParamGroupMesh, "Mesh", true, "How the surface is detected and triangulated.");
  d.choice(kParamMeshArea, "Mesh Area",
           "Alpha: only the opaque part of the picture is meshed (a cut-out character, a logo...). "
           "Whole frame: the entire frame is meshed.",
           {"Alpha (detect shape)", "Whole frame"}, kMeshAreaAlpha, kParamGroupMesh);
  d.real(kParamAlphaThreshold, "Alpha Threshold",
         "Pixels more opaque than this belong to the surface (Alpha mesh area).", 0.05, 0.0, 1.0, 0.0,
         1.0, 0.01, 3, kParamGroupMesh);
  d.integer(kParamDensity, "Mesh Density",
            "Number of mesh cells across the frame width (each cell is two triangles). Higher = "
            "smoother bends, slower.",
            48, 4, 400, 8, 160, kParamGroupMesh);
  d.real(kParamExpansion, "Expansion",
         "Grows the detected shape by this many pixels so soft edges stay inside the mesh.", 3.0, 0.0,
         200.0, 0.0, 30.0, 1.0, 1, kParamGroupMesh);
  d.integer(kParamIterations, "Rigidity",
            "Solver iterations. More keeps the picture stiffer and bends more naturally; fewer is "
            "faster and softer.",
            20, 0, 200, 1, 60, kParamGroupMesh);

  d.group(kParamGroupDisplay, "Display", false, "Overlay options.");
  d.boolean(kParamOverlayMesh, "Show Mesh On Preview",
            "Draw the deformed mesh in the preview overlay (never rendered).", true, kParamGroupDisplay);
  d.boolean(kParamBurnOverlay, "Draw Mesh & Pins Into Video",
            "Draws the mesh and numbered pins into the picture itself, in every mode. Use it when the "
            "preview overlay is not available. Remember to turn it off before the final render.",
            false, kParamGroupDisplay);
  d.boolean(kParamFlipY, "Invert Pin Y Axis",
            "Only needed if a host shows the pin cross-hairs upside down compared to the picture.",
            false, kParamGroupDisplay);

  for (int k = 0; k < kNumPins; ++k) {
    char label[32];
    std::snprintf(label, sizeof(label), "Pin %d", k + 1);
    const std::string grp = pinParamName(k, "");
    d.group(grp, label, k < 2, "One puppet pin.");
    double u, v;
    defaultPinLocation(k, u, v);
    d.boolean(pinParamName(k, "_on"), "Enabled",
              "Turns the pin on. A pin is stuck to the picture where it is when you enable it.", false,
              grp.c_str());
    d.point(pinParamName(k, "_pos"), "Position",
            "Where the pin is. In Setup Mode this also sticks the pin to the picture; otherwise it drags "
            "the picture. Animate it to animate the puppet.",
            u, v, grp.c_str(), true, false);
    d.button(pinParamName(k, "_reset"), "Reset Position",
             "Moves the pin back onto its anchor at the current frame.", grp.c_str());
    d.point(pinParamName(k, "_anchor"), "Anchor",
            "Where the pin is stuck on the undeformed picture (set automatically).", u, v, grp.c_str(),
            false, true);
  }

  OfxPropertySetHandle page = d.define(kOfxParamTypePage, "Controls", "Controls", nullptr, nullptr);
  for (size_t i = 0; i < d.topLevel.size(); ++i)
    setStr(page, kOfxParamPropPageChild, (int)i, d.topLevel[i].c_str());
  return kOfxStatOK;
}

static OfxStatus actionCreateInstance(OfxImageEffectHandle effect) {
  // Only fetch handles here: VEGAS does not allow edit blocks or value changes
  // inside createInstance.
  Instance* inst = new Instance();
  inst->effect = effect;
  gEffect->getParamSet(effect, &inst->paramSet);
  if (gEffect->clipGetHandle(effect, kOfxImageEffectSimpleSourceClipName, &inst->srcClip, nullptr) != kOfxStatOK)
    inst->srcClip = nullptr;
  if (gEffect->clipGetHandle(effect, kOfxImageEffectOutputClipName, &inst->dstClip, nullptr) != kOfxStatOK)
    inst->dstClip = nullptr;
  OfxParamSetHandle ps = inst->paramSet;
  inst->setup = fetchParam(ps, kParamSetup);
  inst->resetAll = fetchParam(ps, kParamResetAll);
  inst->meshArea = fetchParam(ps, kParamMeshArea);
  inst->alphaThreshold = fetchParam(ps, kParamAlphaThreshold);
  inst->density = fetchParam(ps, kParamDensity);
  inst->expansion = fetchParam(ps, kParamExpansion);
  inst->iterations = fetchParam(ps, kParamIterations);
  inst->overlayMesh = fetchParam(ps, kParamOverlayMesh);
  inst->burnOverlay = fetchParam(ps, kParamBurnOverlay);
  inst->flipY = fetchParam(ps, kParamFlipY);
  for (int k = 0; k < kNumPins; ++k) {
    inst->pinOn[k] = fetchParam(ps, pinParamName(k, "_on").c_str());
    inst->pinPos[k] = fetchParam(ps, pinParamName(k, "_pos").c_str());
    inst->pinAnchor[k] = fetchParam(ps, pinParamName(k, "_anchor").c_str());
    inst->pinReset[k] = fetchParam(ps, pinParamName(k, "_reset").c_str());
  }
  OfxPropertySetHandle props = nullptr;
  if (gEffect->getPropertySet(effect, &props) != kOfxStatOK ||
      gProp->propSetPointer(props, kOfxPropInstanceData, 0, inst) != kOfxStatOK) {
    delete inst;
    return kOfxStatFailed;
  }
  return kOfxStatOK;
}

static OfxStatus actionDestroyInstance(OfxImageEffectHandle effect) {
  Instance* inst = getInstance(effect);
  if (inst) {
    OfxPropertySetHandle props = nullptr;
    if (gEffect->getPropertySet(effect, &props) == kOfxStatOK)
      gProp->propSetPointer(props, kOfxPropInstanceData, 0, nullptr);
    delete inst;
  }
  return kOfxStatOK;
}

// Sticks pin k to the picture where it currently is (anchor := position).
static void anchorPinHere(Instance* inst, int k, double t) {
  double u, v;
  if (get2D(inst->pinPos[k], t, u, v)) set2D(inst->pinAnchor[k], t, u, v);
}

// Moves pin k back onto its anchor.
static void resetPin(Instance* inst, int k, double t) {
  double u, v;
  if (get2D(inst->pinAnchor[k], t, u, v)) set2D(inst->pinPos[k], t, u, v);
}

static OfxStatus actionInstanceChanged(OfxImageEffectHandle effect, OfxPropertySetHandle inArgs) {
  Instance* inst = getInstance(effect);
  if (!inst) return kOfxStatReplyDefault;
  if (propString(inArgs, kOfxPropType, 0) != kOfxTypeParameter) return kOfxStatReplyDefault;
  if (propString(inArgs, kOfxPropChangeReason, 0) == kOfxChangeTime) return kOfxStatReplyDefault;
  const std::string name = propString(inArgs, kOfxPropName, 0);
  const double t = propDouble(inArgs, kOfxPropTime, 0, 0.0);

  if (name == kParamResetAll) {
    gParam->paramEditBegin(inst->paramSet, "Reset All Pins");
    for (int k = 0; k < kNumPins; ++k)
      if (getBool(inst->pinOn[k], t, false)) resetPin(inst, k, t);
    gParam->paramEditEnd(inst->paramSet);
    return kOfxStatOK;
  }
  std::string suffix;
  const int k = parsePinParam(name.c_str(), suffix);
  if (k < 0) return kOfxStatReplyDefault;
  if (suffix == "_on") {
    if (getBool(inst->pinOn[k], t, false)) anchorPinHere(inst, k, t);
    return kOfxStatOK;
  }
  if (suffix == "_pos") {
    if (getBool(inst->setup, t, true) && getBool(inst->pinOn[k], t, false)) anchorPinHere(inst, k, t);
    return kOfxStatOK;
  }
  if (suffix == "_reset") {
    resetPin(inst, k, t);
    return kOfxStatOK;
  }
  return kOfxStatReplyDefault;
}

static OfxStatus actionGetRegionsOfInterest(OfxImageEffectHandle effect, OfxPropertySetHandle inArgs,
                                            OfxPropertySetHandle outArgs) {
  Instance* inst = getInstance(effect);
  if (!inst || !inst->srcClip) return kOfxStatReplyDefault;
  // The warp can pull pixels from anywhere in the source.
  OfxRectD rod;
  const double t = propDouble(inArgs, kOfxPropTime, 0, 0.0);
  if (gEffect->clipGetRegionOfDefinition(inst->srcClip, t, &rod) != kOfxStatOK) return kOfxStatReplyDefault;
  const char* name = "OfxImageClipPropRoI_" kOfxImageEffectSimpleSourceClipName;
  gProp->propSetDouble(outArgs, name, 0, rod.x1);
  gProp->propSetDouble(outArgs, name, 1, rod.y1);
  gProp->propSetDouble(outArgs, name, 2, rod.x2);
  gProp->propSetDouble(outArgs, name, 3, rod.y2);
  return kOfxStatOK;
}

static OfxStatus actionIsIdentity(OfxImageEffectHandle effect, OfxPropertySetHandle inArgs,
                                  OfxPropertySetHandle outArgs) {
  Instance* inst = getInstance(effect);
  if (!inst) return kOfxStatReplyDefault;
  const double t = propDouble(inArgs, kOfxPropTime, 0, 0.0);
  const Settings s = readSettings(inst, t);
  if (burnsMesh(s) || burnsPins(s) || (!s.setup && anyPinDisplaced(s))) return kOfxStatReplyDefault;
  gProp->propSetString(outArgs, kOfxPropName, 0, kOfxImageEffectSimpleSourceClipName);
  gProp->propSetDouble(outArgs, kOfxPropTime, 0, t);
  return kOfxStatOK;
}

static OfxStatus actionRender(OfxImageEffectHandle effect, OfxPropertySetHandle inArgs) {
  Instance* inst = getInstance(effect);
  if (!inst || !inst->dstClip) return kOfxStatFailed;
  const double t = propDouble(inArgs, kOfxPropTime, 0, 0.0);
  puppet::RectI window(propInt(inArgs, kOfxImageEffectPropRenderWindow, 0, 0),
                       propInt(inArgs, kOfxImageEffectPropRenderWindow, 1, 0),
                       propInt(inArgs, kOfxImageEffectPropRenderWindow, 2, 0),
                       propInt(inArgs, kOfxImageEffectPropRenderWindow, 3, 0));
  // VEGAS gives the render scale only here, never on the images.
  const double rsx = propDouble(inArgs, kOfxImageEffectPropRenderScale, 0, 1.0);
  const double rsy = propDouble(inArgs, kOfxImageEffectPropRenderScale, 1, rsx);

  ImageHolder dstImg, srcImg;
  if (gEffect->clipGetImage(inst->dstClip, t, nullptr, &dstImg.h) != kOfxStatOK || !dstImg.h) {
    dstImg.h = nullptr;
    return kOfxStatFailed;
  }
  puppet::ImageRef dst;
  OfxStatus st = describeImage(dstImg.h, inst->dstClip, dst);
  if (st != kOfxStatOK) return st;
  if (window.empty()) window = puppet::RectI(dst.x1, dst.y1, dst.x2, dst.y2);

  if (!inst->srcClip || gEffect->clipGetImage(inst->srcClip, t, nullptr, &srcImg.h) != kOfxStatOK || !srcImg.h) {
    srcImg.h = nullptr;
    BandJob clear;
    clear.kind = BandJob::kCopy;
    puppet::ImageRef none = dst;
    none.x2 = none.x1;  // empty source: copyBand just clears
    clear.src = &none;
    clear.dst = &dst;
    clear.window = window;
    runBands(clear);
    return kOfxStatOK;
  }
  puppet::ImageRef src;
  st = describeImage(srcImg.h, inst->srcClip, src);
  if (st != kOfxStatOK) return st;
  if (src.depth != dst.depth || src.nComp != dst.nComp) return kOfxStatErrImageFormat;

  const Settings s = readSettings(inst, t);
  const Geometry g = makeGeometry(inst, t, rsx, rsy, s.flipY);
  const bool deform = !s.setup && anyPinDisplaced(s);
  const bool needMesh = deform || burnsMesh(s) || burnsPins(s) || gHostSupportsOverlays;

  PuppetResult r;
  if (needMesh) computePuppet(s, g, src, deform, r);

  BandJob job;
  job.src = &src;
  job.dst = &dst;
  job.window = window;
  std::vector<puppet::WarpTriangle> tris;
  if (deform && r.meshOk) {
    puppet::prepareTriangles(r.mesh, r.deformed, tris);
    job.kind = BandJob::kWarp;
    job.tris = &tris;
  } else {
    job.kind = BandJob::kCopy;
  }
  runBands(job);

  if (burnsMesh(s) || burnsPins(s)) {
    dst.straightAlpha = src.straightAlpha;
    dst.bgra = src.bgra || dst.bgra;
    puppet::Painter paint(dst, window);
    const double scale = std::max(0.25, std::min(rsx, rsy));
    if (burnsMesh(s) && r.meshOk) puppet::drawMesh(paint, r.mesh, r.deformed, s.setup ? 0.75f : 0.6f);
    if (burnsPins(s)) {
      std::vector<puppet::OverlayPin> pins;
      for (int k = 0; k < kNumPins; ++k) {
        const PinValues& p = s.pins[k];
        // Disabled pins are only shown (grey, numbered) while setting up on a
        // host without overlays, so its cross-hairs can be told apart.
        if (!p.on && !(s.setup && !gHostSupportsOverlays)) continue;
        puppet::OverlayPin op;
        op.number = k + 1;
        op.p = s.setup ? r.pinAnchorPx[k] : r.pinPosPx[k];
        op.state = !p.on ? puppet::kPinDisabled : (r.pinBound[k] ? puppet::kPinActive : puppet::kPinUnbound);
        pins.push_back(op);
      }
      // disabled pins first so active ones are drawn on top
      std::stable_sort(pins.begin(), pins.end(), [](const puppet::OverlayPin& a, const puppet::OverlayPin& b) {
        return (a.state != puppet::kPinDisabled) < (b.state != puppet::kPinDisabled);
      });
      puppet::drawPins(paint, pins, scale);
    }
  }

  if (gHostSupportsOverlays && needMesh) storeSnapshot(inst, t, g, r);
  return kOfxStatOK;
}

////////////////////////////////////////////////////////////////////////////////
// Overlay interact (preview): draw pins + mesh, click to add, drag to move.

#if defined(_WIN32)
#define PUPPET_GLAPI __stdcall
#else
#define PUPPET_GLAPI
#endif

namespace gl {
typedef void(PUPPET_GLAPI* PFN_V_U)(unsigned int);
typedef void(PUPPET_GLAPI* PFN_V)(void);
typedef void(PUPPET_GLAPI* PFN_V_DD)(double, double);
typedef void(PUPPET_GLAPI* PFN_V_FFFF)(float, float, float, float);
typedef void(PUPPET_GLAPI* PFN_V_F)(float);
typedef void(PUPPET_GLAPI* PFN_V_UU)(unsigned int, unsigned int);

enum : unsigned int {
  LINES = 0x0001,
  LINE_LOOP = 0x0002,
  POLYGON = 0x0009,
  BLEND = 0x0BE2,
  LINE_SMOOTH = 0x0B20,
  TEXTURE_2D = 0x0DE1,
  DEPTH_TEST = 0x0B71,
  LIGHTING = 0x0B50,
  SRC_ALPHA = 0x0302,
  ONE_MINUS_SRC_ALPHA = 0x0303,
  CURRENT_BIT = 0x00000001,
  LINE_BIT = 0x00000004,
  ENABLE_BIT = 0x00002000,
  COLOR_BUFFER_BIT = 0x00004000,
};

struct Api {
  bool ok = false;
  PFN_V_U Begin = nullptr, Enable = nullptr, Disable = nullptr, PushAttrib = nullptr;
  PFN_V End = nullptr, PopAttrib = nullptr;
  PFN_V_DD Vertex2d = nullptr;
  PFN_V_FFFF Color4f = nullptr;
  PFN_V_F LineWidth = nullptr;
  PFN_V_UU BlendFunc = nullptr;
};

// OpenGL 1.1 entry points are loaded at run time so the plug-in never fails
// to load because of OpenGL, and so headless hosts work too.
static const Api& api() {
  static Api a;
  static bool tried = false;
  static std::mutex m;
  std::lock_guard<std::mutex> lock(m);
  if (tried) return a;
  tried = true;
#if defined(_WIN32)
  HMODULE lib = GetModuleHandleA("opengl32.dll");
  if (!lib) lib = LoadLibraryA("opengl32.dll");
  if (!lib) return a;
  auto sym = [&](const char* n) { return (void*)GetProcAddress(lib, n); };
#else
  void* lib = dlopen("libGL.so.1", RTLD_LAZY | RTLD_GLOBAL);
  if (!lib) lib = dlopen("libGL.so", RTLD_LAZY | RTLD_GLOBAL);
  if (!lib) return a;
  auto sym = [&](const char* n) { return dlsym(lib, n); };
#endif
  a.Begin = (PFN_V_U)sym("glBegin");
  a.End = (PFN_V)sym("glEnd");
  a.Enable = (PFN_V_U)sym("glEnable");
  a.Disable = (PFN_V_U)sym("glDisable");
  a.PushAttrib = (PFN_V_U)sym("glPushAttrib");
  a.PopAttrib = (PFN_V)sym("glPopAttrib");
  a.Vertex2d = (PFN_V_DD)sym("glVertex2d");
  a.Color4f = (PFN_V_FFFF)sym("glColor4f");
  a.LineWidth = (PFN_V_F)sym("glLineWidth");
  a.BlendFunc = (PFN_V_UU)sym("glBlendFunc");
  a.ok = a.Begin && a.End && a.Enable && a.Disable && a.PushAttrib && a.PopAttrib && a.Vertex2d &&
         a.Color4f && a.LineWidth && a.BlendFunc;
  return a;
}
}  // namespace gl

// Minimal drawing interface implemented with OpenGL (V1) or the Draw Suite (V2).
class OverlayPainter {
 public:
  virtual ~OverlayPainter() {}
  virtual void colour(float r, float g, float b, float a) = 0;
  virtual void width(float w) = 0;
  virtual void lines(const std::vector<OfxPointD>& pts) = 0;  // pairs
  virtual void loop(const std::vector<OfxPointD>& pts) = 0;
  virtual void polygon(const std::vector<OfxPointD>& pts) = 0;
};

class GLPainter : public OverlayPainter {
 public:
  explicit GLPainter(const gl::Api& a) : gl_(a) {
    gl_.PushAttrib(gl::CURRENT_BIT | gl::LINE_BIT | gl::ENABLE_BIT | gl::COLOR_BUFFER_BIT);
    gl_.Disable(gl::TEXTURE_2D);
    gl_.Disable(gl::DEPTH_TEST);
    gl_.Disable(gl::LIGHTING);
    gl_.Enable(gl::BLEND);
    gl_.BlendFunc(gl::SRC_ALPHA, gl::ONE_MINUS_SRC_ALPHA);
    gl_.Enable(gl::LINE_SMOOTH);
  }
  ~GLPainter() override { gl_.PopAttrib(); }
  void colour(float r, float g, float b, float a) override { gl_.Color4f(r, g, b, a); }
  void width(float w) override { gl_.LineWidth(w); }
  void lines(const std::vector<OfxPointD>& pts) override { emit(gl::LINES, pts); }
  void loop(const std::vector<OfxPointD>& pts) override { emit(gl::LINE_LOOP, pts); }
  void polygon(const std::vector<OfxPointD>& pts) override { emit(gl::POLYGON, pts); }

 private:
  void emit(unsigned int mode, const std::vector<OfxPointD>& pts) {
    if (pts.empty()) return;
    gl_.Begin(mode);
    for (const OfxPointD& p : pts) gl_.Vertex2d(p.x, p.y);
    gl_.End();
  }
  const gl::Api& gl_;
};

class DrawSuitePainter : public OverlayPainter {
 public:
  explicit DrawSuitePainter(OfxDrawContextHandle ctx) : ctx_(ctx) {}
  void colour(float r, float g, float b, float a) override {
    const OfxRGBAColourF c = {r, g, b, a};
    gDraw->setColour(ctx_, &c);
  }
  void width(float w) override { gDraw->setLineWidth(ctx_, w); }
  void lines(const std::vector<OfxPointD>& pts) override { emit(kOfxDrawPrimitiveLines, pts); }
  void loop(const std::vector<OfxPointD>& pts) override { emit(kOfxDrawPrimitiveLineLoop, pts); }
  void polygon(const std::vector<OfxPointD>& pts) override { emit(kOfxDrawPrimitivePolygon, pts); }

 private:
  void emit(OfxDrawPrimitive prim, const std::vector<OfxPointD>& pts) {
    if (!pts.empty()) gDraw->draw(ctx_, prim, pts.data(), (int)pts.size());
  }
  OfxDrawContextHandle ctx_;
};

struct InteractData {
  int dragPin = -1;
  bool dragSetup = false;
  bool editOpen = false;
  int selectedPin = -1;
  bool ctrlDown = false;
};

struct InteractContext {
  OfxInteractHandle interact = nullptr;
  OfxPropertySetHandle props = nullptr;
  OfxImageEffectHandle effect = nullptr;
  Instance* inst = nullptr;
  InteractData* data = nullptr;
};

static bool interactContext(const void* handle, OfxPropertySetHandle inArgs, InteractContext& c) {
  c.interact = (OfxInteractHandle)handle;
  if (!gInteract || !c.interact || gInteract->interactGetPropertySet(c.interact, &c.props) != kOfxStatOK)
    return false;
  c.effect = (OfxImageEffectHandle)propPointer(inArgs, kOfxPropEffectInstance, 0);
  if (!c.effect) c.effect = (OfxImageEffectHandle)propPointer(c.props, kOfxPropEffectInstance, 0);
  c.inst = getInstance(c.effect);
  c.data = (InteractData*)propPointer(c.props, kOfxPropInstanceData, 0);
  return c.inst != nullptr;
}

// 7-segment digits for pin numbers (unit cell 0..1 x 0..2).
static void digitSegments(int value, double x, double y, double h, std::vector<OfxPointD>& out) {
  static const unsigned char kSeg[10] = {0x3F, 0x06, 0x5B, 0x4F, 0x66, 0x6D, 0x7D, 0x07, 0x7F, 0x6F};
  const double w = h * 0.5;
  char buf[8];
  std::snprintf(buf, sizeof(buf), "%d", std::max(0, value));
  for (int i = 0; buf[i]; ++i) {
    const unsigned char s = kSeg[buf[i] - '0'];
    const double ox = x + i * w * 1.6;
    const double xs[2] = {ox, ox + w};
    const double ys[3] = {y, y + h * 0.5, y + h};
    auto seg = [&](double ax, double ay, double bx, double by) {
      out.push_back({ax, ay});
      out.push_back({bx, by});
    };
    if (s & 0x01) seg(xs[0], ys[2], xs[1], ys[2]);  // a top
    if (s & 0x02) seg(xs[1], ys[2], xs[1], ys[1]);  // b
    if (s & 0x04) seg(xs[1], ys[1], xs[1], ys[0]);  // c
    if (s & 0x08) seg(xs[0], ys[0], xs[1], ys[0]);  // d bottom
    if (s & 0x10) seg(xs[0], ys[1], xs[0], ys[0]);  // e
    if (s & 0x20) seg(xs[0], ys[2], xs[0], ys[1]);  // f
    if (s & 0x40) seg(xs[0], ys[1], xs[1], ys[1]);  // g middle
  }
}

static std::vector<OfxPointD> circle(double cx, double cy, double rx, double ry, int n = 20) {
  std::vector<OfxPointD> pts;
  pts.reserve(n);
  for (int i = 0; i < n; ++i) {
    const double a = 2.0 * 3.14159265358979323846 * i / n;
    pts.push_back({cx + rx * std::cos(a), cy + ry * std::sin(a)});
  }
  return pts;
}

static OfxStatus interactDraw(const void* handle, OfxPropertySetHandle inArgs, OverlayPainter& paint) {
  InteractContext c;
  if (!interactContext(handle, inArgs, c)) return kOfxStatReplyDefault;
  const double t = propDouble(inArgs, kOfxPropTime, 0, 0.0);
  const double psx = std::max(1e-6, propDouble(inArgs, kOfxInteractPropPixelScale, 0, 1.0));
  const double psy = std::max(1e-6, propDouble(inArgs, kOfxInteractPropPixelScale, 1, psx));
  const Settings s = readSettings(c.inst, t);
  const Geometry g = makeGeometry(c.inst, t, 1.0, 1.0, s.flipY);

  // mesh from the last render at this frame
  if (s.overlayMesh && !s.setup) {
    std::vector<OfxPointD> pts;
    {
      std::lock_guard<std::mutex> lock(c.inst->snapshotMutex);
      const OverlaySnapshot& snap = c.inst->snapshot;
      if (snap.valid && std::fabs(snap.time - t) < 1e-6) {
        pts.reserve(snap.edges.size());
        for (const Vec2& p : snap.edges) pts.push_back({p.x, p.y});
      }
    }
    if (!pts.empty()) {
      paint.width(1.0f);
      paint.colour(0.35f, 0.85f, 1.0f, 0.35f);
      paint.lines(pts);
    }
  }

  bool bound[kNumPins];
  bool haveSnap = false;
  {
    std::lock_guard<std::mutex> lock(c.inst->snapshotMutex);
    haveSnap = c.inst->snapshot.valid && std::fabs(c.inst->snapshot.time - t) < 1e-6;
    for (int k = 0; k < kNumPins; ++k) bound[k] = c.inst->snapshot.pinBound[k];
  }
  const double r = 7.0;
  for (int k = 0; k < kNumPins; ++k) {
    const PinValues& p = s.pins[k];
    if (!p.on) continue;
    const Vec2 cp = g.normToCanonical(p.pu, p.pv);
    const bool unbound = haveSnap && !bound[k];
    const bool selected = c.data && (c.data->selectedPin == k || c.data->dragPin == k);
    // dark halo, filled dot, ring
    paint.colour(0.0f, 0.0f, 0.0f, 0.6f);
    paint.polygon(circle(cp.x, cp.y, (r + 2) * psx, (r + 2) * psy));
    if (unbound) paint.colour(1.0f, 0.25f, 0.2f, 1.0f);
    else if (s.setup) paint.colour(0.35f, 0.85f, 1.0f, 1.0f);
    else paint.colour(1.0f, 0.85f, 0.1f, 1.0f);
    paint.polygon(circle(cp.x, cp.y, r * psx, r * psy));
    paint.width(selected ? 2.5f : 1.5f);
    paint.colour(1.0f, 1.0f, 1.0f, 1.0f);
    paint.loop(circle(cp.x, cp.y, (r + 1) * psx, (r + 1) * psy));
    // number
    std::vector<OfxPointD> seg;
    digitSegments(k + 1, cp.x + (r + 4) * psx, cp.y + (r + 2) * psy, 10.0 * psy, seg);
    std::vector<OfxPointD> shadow = seg;
    for (OfxPointD& q : shadow) {
      q.x += psx;
      q.y -= psy;
    }
    paint.width(2.0f);
    paint.colour(0.0f, 0.0f, 0.0f, 0.8f);
    paint.lines(shadow);
    paint.colour(1.0f, 1.0f, 1.0f, 1.0f);
    paint.lines(seg);
  }
  return kOfxStatOK;
}

static int hitPin(const Settings& s, const Geometry& g, Vec2 pen, double psx, double psy) {
  int best = -1;
  double bestD = 1e300;
  for (int k = 0; k < kNumPins; ++k) {
    if (!s.pins[k].on) continue;
    const Vec2 cp = g.normToCanonical(s.pins[k].pu, s.pins[k].pv);
    const double dx = (pen.x - cp.x) / psx, dy = (pen.y - cp.y) / psy;  // screen pixels
    const double d = dx * dx + dy * dy;
    if (d <= 12.0 * 12.0 && d < bestD) {
      bestD = d;
      best = k;
    }
  }
  return best;
}

static void endDrag(InteractContext& c) {
  if (!c.data) return;
  if (c.data->editOpen) gParam->paramEditEnd(c.inst->paramSet);
  c.data->editOpen = false;
  c.data->dragPin = -1;
}

static OfxStatus interactPen(const char* action, const void* handle, OfxPropertySetHandle inArgs) {
  InteractContext c;
  if (!interactContext(handle, inArgs, c) || !c.data) return kOfxStatReplyDefault;
  const double t = propDouble(inArgs, kOfxPropTime, 0, 0.0);
  const double psx = std::max(1e-6, propDouble(inArgs, kOfxInteractPropPixelScale, 0, 1.0));
  const double psy = std::max(1e-6, propDouble(inArgs, kOfxInteractPropPixelScale, 1, psx));
  const Vec2 pen(propDouble(inArgs, kOfxInteractPropPenPosition, 0, 0.0),
                 propDouble(inArgs, kOfxInteractPropPenPosition, 1, 0.0));
  const Settings s = readSettings(c.inst, t);
  const Geometry g = makeGeometry(c.inst, t, 1.0, 1.0, s.flipY);
  const Vec2 n = g.canonicalToNorm(pen);

  if (std::strcmp(action, kOfxInteractActionPenDown) == 0) {
    endDrag(c);
    int k = hitPin(s, g, pen, psx, psy);
    if (k >= 0 && c.data->ctrlDown) {  // Ctrl+click removes a pin
      setBool(c.inst->pinOn[k], false);
      c.data->selectedPin = -1;
      return kOfxStatOK;
    }
    if (k < 0) {
      // click on the picture: add the first free pin there
      if (n.x < -0.05 || n.x > 1.05 || n.y < -0.05 || n.y > 1.05) return kOfxStatReplyDefault;
      for (int i = 0; i < kNumPins && k < 0; ++i)
        if (!s.pins[i].on && c.inst->pinOn[i] && c.inst->pinPos[i]) k = i;
      if (k < 0) return kOfxStatReplyDefault;  // all pins in use
      gParam->paramEditBegin(c.inst->paramSet, "Add Puppet Pin");
      c.data->editOpen = true;
      set2D(c.inst->pinPos[k], t, n.x, n.y);
      set2D(c.inst->pinAnchor[k], t, n.x, n.y);
      setBool(c.inst->pinOn[k], true);
    } else {
      gParam->paramEditBegin(c.inst->paramSet, "Move Puppet Pin");
      c.data->editOpen = true;
    }
    c.data->dragPin = k;
    c.data->selectedPin = k;
    c.data->dragSetup = s.setup;
    if (gInteract) gInteract->interactRedraw(c.interact);
    return kOfxStatOK;
  }
  if (std::strcmp(action, kOfxInteractActionPenMotion) == 0) {
    const int k = c.data->dragPin;
    if (k < 0) return kOfxStatReplyDefault;
    set2D(c.inst->pinPos[k], t, n.x, n.y);
    if (c.data->dragSetup) set2D(c.inst->pinAnchor[k], t, n.x, n.y);
    return kOfxStatOK;
  }
  if (std::strcmp(action, kOfxInteractActionPenUp) == 0) {
    if (c.data->dragPin < 0) return kOfxStatReplyDefault;
    endDrag(c);
    if (gInteract) gInteract->interactRedraw(c.interact);
    return kOfxStatOK;
  }
  return kOfxStatReplyDefault;
}

static OfxStatus interactKey(const char* action, const void* handle, OfxPropertySetHandle inArgs) {
  InteractContext c;
  if (!interactContext(handle, inArgs, c) || !c.data) return kOfxStatReplyDefault;
  const int key = propInt(inArgs, kOfxPropKeySym, 0, 0);
  const bool down = std::strcmp(action, kOfxInteractActionKeyDown) == 0;
  if (key == kOfxKey_Control_L || key == kOfxKey_Control_R) {
    c.data->ctrlDown = down;
    return kOfxStatReplyDefault;  // let the host see modifiers too
  }
  if (down && (key == kOfxKey_Delete || key == kOfxKey_BackSpace) && c.data->selectedPin >= 0 &&
      c.data->dragPin < 0) {
    setBool(c.inst->pinOn[c.data->selectedPin], false);
    c.data->selectedPin = -1;
    return kOfxStatOK;
  }
  return kOfxStatReplyDefault;
}

static OfxStatus overlayMain(const char* action, const void* handle, OfxPropertySetHandle inArgs, bool v2) {
  if (std::strcmp(action, kOfxActionDescribe) == 0) return kOfxStatOK;
  if (std::strcmp(action, kOfxActionCreateInstance) == 0) {
    OfxPropertySetHandle props = nullptr;
    if (!gInteract || gInteract->interactGetPropertySet((OfxInteractHandle)handle, &props) != kOfxStatOK)
      return kOfxStatFailed;
    InteractData* d = new InteractData();
    if (gProp->propSetPointer(props, kOfxPropInstanceData, 0, d) != kOfxStatOK) {
      delete d;
      return kOfxStatFailed;
    }
    return kOfxStatOK;
  }
  if (std::strcmp(action, kOfxActionDestroyInstance) == 0) {
    OfxPropertySetHandle props = nullptr;
    if (gInteract && gInteract->interactGetPropertySet((OfxInteractHandle)handle, &props) == kOfxStatOK) {
      InteractData* d = (InteractData*)propPointer(props, kOfxPropInstanceData, 0);
      if (d) {
        InteractContext c;
        if (interactContext(handle, nullptr, c) && d->editOpen) gParam->paramEditEnd(c.inst->paramSet);
        gProp->propSetPointer(props, kOfxPropInstanceData, 0, nullptr);
        delete d;
      }
    }
    return kOfxStatOK;
  }
  if (std::strcmp(action, kOfxInteractActionDraw) == 0) {
    if (v2) {
      OfxDrawContextHandle ctx = (OfxDrawContextHandle)propPointer(inArgs, kOfxInteractPropDrawContext, 0);
      if (!ctx || !gDraw) return kOfxStatReplyDefault;
      DrawSuitePainter p(ctx);
      return interactDraw(handle, inArgs, p);
    }
    const gl::Api& a = gl::api();
    if (!a.ok) return kOfxStatReplyDefault;
    GLPainter p(a);
    return interactDraw(handle, inArgs, p);
  }
  if (std::strcmp(action, kOfxInteractActionPenDown) == 0 || std::strcmp(action, kOfxInteractActionPenMotion) == 0 ||
      std::strcmp(action, kOfxInteractActionPenUp) == 0)
    return interactPen(action, handle, inArgs);
  if (std::strcmp(action, kOfxInteractActionKeyDown) == 0 || std::strcmp(action, kOfxInteractActionKeyUp) == 0)
    return interactKey(action, handle, inArgs);
  if (std::strcmp(action, kOfxInteractActionLoseFocus) == 0) {
    InteractContext c;
    if (interactContext(handle, inArgs, c) && c.data) {
      c.data->ctrlDown = false;
      endDrag(c);
    }
    return kOfxStatOK;
  }
  return kOfxStatReplyDefault;
}

static OfxStatus overlayMainV1(const char* action, const void* handle, OfxPropertySetHandle inArgs,
                               OfxPropertySetHandle /*outArgs*/) {
  try {
    return overlayMain(action, handle, inArgs, false);
  } catch (const std::bad_alloc&) {
    return kOfxStatErrMemory;
  } catch (...) {
    return kOfxStatFailed;
  }
}

static OfxStatus overlayMainV2(const char* action, const void* handle, OfxPropertySetHandle inArgs,
                               OfxPropertySetHandle /*outArgs*/) {
  try {
    return overlayMain(action, handle, inArgs, true);
  } catch (const std::bad_alloc&) {
    return kOfxStatErrMemory;
  } catch (...) {
    return kOfxStatFailed;
  }
}

////////////////////////////////////////////////////////////////////////////////
// Entry points

static OfxStatus pluginMain(const char* action, const void* handle, OfxPropertySetHandle inArgs,
                            OfxPropertySetHandle outArgs) {
  if (!action) return kOfxStatErrBadHandle;
  try {
    OfxImageEffectHandle effect = (OfxImageEffectHandle)handle;
    if (std::strcmp(action, kOfxActionLoad) == 0) return actionLoad();
    if (!gProp || !gEffect || !gParam) return kOfxStatErrMissingHostFeature;
    if (std::strcmp(action, kOfxActionUnload) == 0) return kOfxStatOK;
    if (std::strcmp(action, kOfxActionDescribe) == 0) return actionDescribe(effect);
    if (std::strcmp(action, kOfxImageEffectActionDescribeInContext) == 0)
      return actionDescribeInContext(effect, inArgs);
    if (std::strcmp(action, kOfxActionCreateInstance) == 0) return actionCreateInstance(effect);
    if (std::strcmp(action, kOfxActionDestroyInstance) == 0) return actionDestroyInstance(effect);
    if (std::strcmp(action, kOfxImageEffectActionRender) == 0) return actionRender(effect, inArgs);
    if (std::strcmp(action, kOfxImageEffectActionIsIdentity) == 0) return actionIsIdentity(effect, inArgs, outArgs);
    if (std::strcmp(action, kOfxImageEffectActionGetRegionsOfInterest) == 0)
      return actionGetRegionsOfInterest(effect, inArgs, outArgs);
    if (std::strcmp(action, kOfxActionInstanceChanged) == 0) return actionInstanceChanged(effect, inArgs);
    if (std::strcmp(action, kOfxActionPurgeCaches) == 0) {
      Instance* inst = getInstance(effect);
      if (inst) {
        std::lock_guard<std::mutex> lock(inst->snapshotMutex);
        inst->snapshot = OverlaySnapshot();
      }
      return kOfxStatOK;
    }
    return kOfxStatReplyDefault;
  } catch (const std::bad_alloc&) {
    return kOfxStatErrMemory;
  } catch (...) {
    return kOfxStatFailed;
  }
}

static void setHostFunc(OfxHost* host) { gHost = host; }

static OfxPlugin gPlugin = {kOfxImageEffectPluginApi, 1, kPluginIdentifier, kPluginVersionMajor,
                            kPluginVersionMinor, setHostFunc, pluginMain};

extern "C" {
OfxExport int OfxGetNumberOfPlugins(void) { return 1; }
OfxExport OfxPlugin* OfxGetPlugin(int nth) { return nth == 0 ? &gPlugin : nullptr; }
}
