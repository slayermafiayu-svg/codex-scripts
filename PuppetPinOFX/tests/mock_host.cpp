/*
  A small but strict OpenFX host used to test the built Puppet Pin plug-in
  without VEGAS. It loads the .ofx, runs the same action sequence a host
  runs (load, describe, describe in context, create instance, instance
  changed, is identity, render, overlay pen events, destroy, unload) and
  checks the results. It behaves like VEGAS where that matters:

    * 8-bit images are BGRA and say so with OfxImageEffectPropPixelOrder,
    * the render scale is only in the render arguments (not on images),
    * edit blocks during createInstance are reported as errors,
    * properties that do not exist return kOfxStatErrUnknown.

  Usage: mock_host <path to PuppetPin.ofx> [output directory for PPM images]
*/
#include "ofxCore.h"
#include "ofxDrawSuite.h"
#include "ofxImageEffect.h"
#include "ofxInteract.h"
#include "ofxKeySyms.h"
#include "ofxMemory.h"
#include "ofxMessage.h"
#include "ofxMultiThread.h"
#include "ofxOld.h"
#include "ofxParam.h"
#include "ofxProperty.h"

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#endif

static int gFailures = 0;
#define CHECK(cond)                                                     \
  do {                                                                  \
    if (!(cond)) {                                                      \
      std::printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);     \
      ++gFailures;                                                      \
    }                                                                   \
  } while (0)

////////////////////////////////////////////////////////////////////////////////
// Properties

struct Prop {
  char type = 0;  // 's', 'i', 'd', 'p'
  std::vector<std::string> s;
  std::vector<int> i;
  std::vector<double> d;
  std::vector<void*> p;
  int dim() const {
    return type == 's' ? (int)s.size() : type == 'i' ? (int)i.size() : type == 'd' ? (int)d.size() : (int)p.size();
  }
};

struct OfxPropertySetStruct {
  std::map<std::string, Prop> props;
  bool readOnly = false;
  bool has(const std::string& n) const { return props.count(n) != 0; }
  // host-side convenience
  void setS(const std::string& n, const std::string& v, int idx = 0) {
    Prop& p = props[n];
    p.type = 's';
    if ((int)p.s.size() <= idx) p.s.resize(idx + 1);
    p.s[idx] = v;
  }
  void setI(const std::string& n, int v, int idx = 0) {
    Prop& p = props[n];
    p.type = 'i';
    if ((int)p.i.size() <= idx) p.i.resize(idx + 1);
    p.i[idx] = v;
  }
  void setD(const std::string& n, double v, int idx = 0) {
    Prop& p = props[n];
    p.type = 'd';
    if ((int)p.d.size() <= idx) p.d.resize(idx + 1);
    p.d[idx] = v;
  }
  void setP(const std::string& n, void* v, int idx = 0) {
    Prop& p = props[n];
    p.type = 'p';
    if ((int)p.p.size() <= idx) p.p.resize(idx + 1);
    p.p[idx] = v;
  }
  std::string getS(const std::string& n, int idx = 0) const {
    auto it = props.find(n);
    return it != props.end() && it->second.type == 's' && idx < (int)it->second.s.size() ? it->second.s[idx] : "";
  }
  int getI(const std::string& n, int idx = 0, int def = 0) const {
    auto it = props.find(n);
    return it != props.end() && it->second.type == 'i' && idx < (int)it->second.i.size() ? it->second.i[idx] : def;
  }
  double getD(const std::string& n, int idx = 0, double def = 0) const {
    auto it = props.find(n);
    return it != props.end() && it->second.type == 'd' && idx < (int)it->second.d.size() ? it->second.d[idx] : def;
  }
  void* getP(const std::string& n, int idx = 0) const {
    auto it = props.find(n);
    return it != props.end() && it->second.type == 'p' && idx < (int)it->second.p.size() ? it->second.p[idx] : nullptr;
  }
};
typedef OfxPropertySetStruct PropSet;

static std::mutex gPropMutex;

template <class T>
static OfxStatus propSet(OfxPropertySetHandle h, const char* name, int idx, char type, T v,
                         std::vector<T> Prop::*member) {
  if (!h || !name || idx < 0) return kOfxStatErrBadHandle;
  std::lock_guard<std::mutex> lock(gPropMutex);
  PropSet* ps = (PropSet*)h;
  if (ps->readOnly) return kOfxStatErrValue;
  Prop& p = ps->props[name];
  if (p.type && p.type != type) return kOfxStatErrValue;
  p.type = type;
  if ((int)(p.*member).size() <= idx) (p.*member).resize(idx + 1);
  (p.*member)[idx] = v;
  return kOfxStatOK;
}
template <class T>
static OfxStatus propGet(OfxPropertySetHandle h, const char* name, int idx, char type, T* v,
                         std::vector<T> Prop::*member) {
  if (!h || !name || !v) return kOfxStatErrBadHandle;
  std::lock_guard<std::mutex> lock(gPropMutex);
  PropSet* ps = (PropSet*)h;
  auto it = ps->props.find(name);
  if (it == ps->props.end()) return kOfxStatErrUnknown;
  if (it->second.type != type) return kOfxStatErrValue;
  if (idx < 0 || idx >= (int)(it->second.*member).size()) return kOfxStatErrBadIndex;
  *v = (it->second.*member)[idx];
  return kOfxStatOK;
}

static OfxStatus pSetPointer(OfxPropertySetHandle h, const char* n, int i, void* v) { return propSet<void*>(h, n, i, 'p', v, &Prop::p); }
static OfxStatus pSetString(OfxPropertySetHandle h, const char* n, int i, const char* v) {
  if (!v) return kOfxStatErrValue;
  return propSet<std::string>(h, n, i, 's', std::string(v), &Prop::s);
}
static OfxStatus pSetDouble(OfxPropertySetHandle h, const char* n, int i, double v) { return propSet<double>(h, n, i, 'd', v, &Prop::d); }
static OfxStatus pSetInt(OfxPropertySetHandle h, const char* n, int i, int v) { return propSet<int>(h, n, i, 'i', v, &Prop::i); }
static OfxStatus pSetPointerN(OfxPropertySetHandle h, const char* n, int c, void* const* v) {
  for (int i = 0; i < c; ++i) if (OfxStatus s = pSetPointer(h, n, i, v[i])) return s;
  return kOfxStatOK;
}
static OfxStatus pSetStringN(OfxPropertySetHandle h, const char* n, int c, const char* const* v) {
  for (int i = 0; i < c; ++i) if (OfxStatus s = pSetString(h, n, i, v[i])) return s;
  return kOfxStatOK;
}
static OfxStatus pSetDoubleN(OfxPropertySetHandle h, const char* n, int c, const double* v) {
  for (int i = 0; i < c; ++i) if (OfxStatus s = pSetDouble(h, n, i, v[i])) return s;
  return kOfxStatOK;
}
static OfxStatus pSetIntN(OfxPropertySetHandle h, const char* n, int c, const int* v) {
  for (int i = 0; i < c; ++i) if (OfxStatus s = pSetInt(h, n, i, v[i])) return s;
  return kOfxStatOK;
}
static OfxStatus pGetPointer(OfxPropertySetHandle h, const char* n, int i, void** v) { return propGet<void*>(h, n, i, 'p', v, &Prop::p); }
static OfxStatus pGetString(OfxPropertySetHandle h, const char* n, int i, char** v) {
  if (!h || !n || !v) return kOfxStatErrBadHandle;
  std::lock_guard<std::mutex> lock(gPropMutex);
  PropSet* ps = (PropSet*)h;
  auto it = ps->props.find(n);
  if (it == ps->props.end()) return kOfxStatErrUnknown;
  if (it->second.type != 's') return kOfxStatErrValue;
  if (i < 0 || i >= (int)it->second.s.size()) return kOfxStatErrBadIndex;
  *v = (char*)it->second.s[i].c_str();
  return kOfxStatOK;
}
static OfxStatus pGetDouble(OfxPropertySetHandle h, const char* n, int i, double* v) { return propGet<double>(h, n, i, 'd', v, &Prop::d); }
static OfxStatus pGetInt(OfxPropertySetHandle h, const char* n, int i, int* v) { return propGet<int>(h, n, i, 'i', v, &Prop::i); }
static OfxStatus pGetPointerN(OfxPropertySetHandle h, const char* n, int c, void** v) {
  for (int i = 0; i < c; ++i) if (OfxStatus s = pGetPointer(h, n, i, &v[i])) return s;
  return kOfxStatOK;
}
static OfxStatus pGetStringN(OfxPropertySetHandle h, const char* n, int c, char** v) {
  for (int i = 0; i < c; ++i) if (OfxStatus s = pGetString(h, n, i, &v[i])) return s;
  return kOfxStatOK;
}
static OfxStatus pGetDoubleN(OfxPropertySetHandle h, const char* n, int c, double* v) {
  for (int i = 0; i < c; ++i) if (OfxStatus s = pGetDouble(h, n, i, &v[i])) return s;
  return kOfxStatOK;
}
static OfxStatus pGetIntN(OfxPropertySetHandle h, const char* n, int c, int* v) {
  for (int i = 0; i < c; ++i) if (OfxStatus s = pGetInt(h, n, i, &v[i])) return s;
  return kOfxStatOK;
}
static OfxStatus pReset(OfxPropertySetHandle h, const char* n) {
  if (!h || !n) return kOfxStatErrBadHandle;
  std::lock_guard<std::mutex> lock(gPropMutex);
  PropSet* ps = (PropSet*)h;
  if (!ps->has(n)) return kOfxStatErrUnknown;
  ps->props.erase(n);
  return kOfxStatOK;
}
static OfxStatus pGetDimension(OfxPropertySetHandle h, const char* n, int* c) {
  if (!h || !n || !c) return kOfxStatErrBadHandle;
  std::lock_guard<std::mutex> lock(gPropMutex);
  PropSet* ps = (PropSet*)h;
  auto it = ps->props.find(n);
  if (it == ps->props.end()) return kOfxStatErrUnknown;
  *c = it->second.dim();
  return kOfxStatOK;
}

static OfxPropertySuiteV1 gPropSuite = {pSetPointer,  pSetString,  pSetDouble,  pSetInt,     pSetPointerN,
                                        pSetStringN,  pSetDoubleN, pSetIntN,    pGetPointer, pGetString,
                                        pGetDouble,   pGetInt,     pGetPointerN, pGetStringN, pGetDoubleN,
                                        pGetIntN,     pReset,      pGetDimension};

////////////////////////////////////////////////////////////////////////////////
// Parameters

struct OfxParamStruct {
  std::string name, type;
  PropSet props;
  std::vector<double> value;                       // static value
  std::map<double, std::vector<double>> keys;      // keyframes
  int dims() const {
    if (type == kOfxParamTypeDouble2D || type == kOfxParamTypeInteger2D) return 2;
    if (type == kOfxParamTypeDouble || type == kOfxParamTypeInteger || type == kOfxParamTypeBoolean ||
        type == kOfxParamTypeChoice)
      return 1;
    return 0;
  }
  bool isInt() const {
    return type == kOfxParamTypeInteger || type == kOfxParamTypeBoolean || type == kOfxParamTypeChoice ||
           type == kOfxParamTypeInteger2D;
  }
  std::vector<double> at(double t) const {
    if (keys.empty()) return value;
    if (t <= keys.begin()->first) return keys.begin()->second;
    if (t >= keys.rbegin()->first) return keys.rbegin()->second;
    auto hi = keys.lower_bound(t);
    auto lo = std::prev(hi);
    const double a = (t - lo->first) / (hi->first - lo->first);
    std::vector<double> r(lo->second.size());
    for (size_t i = 0; i < r.size(); ++i) r[i] = lo->second[i] + a * (hi->second[i] - lo->second[i]);
    return r;
  }
};
typedef OfxParamStruct Param;

struct OfxParamSetStruct {
  std::vector<std::unique_ptr<Param>> list;
  std::map<std::string, Param*> byName;
  PropSet props;
  int editDepth = 0;
  int editBegins = 0;
};
typedef OfxParamSetStruct ParamSet;

static bool gInCreateInstance = false;
static int gEditBlockInCreateInstance = 0;
static int gSetValueOutsideAllowed = 0;
static bool gSetValueAllowed = false;  // true during instanceChanged / interact actions

static OfxStatus paramDefine(OfxParamSetHandle ps, const char* type, const char* name, OfxPropertySetHandle* props) {
  if (!ps || !type || !name) return kOfxStatErrBadHandle;
  ParamSet* s = (ParamSet*)ps;
  if (s->byName.count(name)) return kOfxStatErrExists;
  std::unique_ptr<Param> p(new Param());
  p->name = name;
  p->type = type;
  p->props.setS(kOfxParamPropType, type);
  p->props.setS(kOfxPropName, name);
  if (props) *props = (OfxPropertySetHandle)&p->props;
  s->byName[name] = p.get();
  s->list.push_back(std::move(p));
  return kOfxStatOK;
}
static OfxStatus paramGetHandle(OfxParamSetHandle ps, const char* name, OfxParamHandle* h, OfxPropertySetHandle* props) {
  if (!ps || !name || !h) return kOfxStatErrBadHandle;
  ParamSet* s = (ParamSet*)ps;
  auto it = s->byName.find(name);
  if (it == s->byName.end()) return kOfxStatErrUnknown;
  *h = (OfxParamHandle)it->second;
  if (props) *props = (OfxPropertySetHandle)&it->second->props;
  return kOfxStatOK;
}
static OfxStatus paramSetGetPropertySet(OfxParamSetHandle ps, OfxPropertySetHandle* props) {
  if (!ps || !props) return kOfxStatErrBadHandle;
  *props = (OfxPropertySetHandle)&((ParamSet*)ps)->props;
  return kOfxStatOK;
}
static OfxStatus paramGetPropertySet(OfxParamHandle h, OfxPropertySetHandle* props) {
  if (!h || !props) return kOfxStatErrBadHandle;
  *props = (OfxPropertySetHandle)&((Param*)h)->props;
  return kOfxStatOK;
}
static OfxStatus getValues(Param* p, double t, va_list ap) {
  const int n = p->dims();
  if (n == 0) return kOfxStatErrBadHandle;
  const std::vector<double> v = p->at(t);
  if ((int)v.size() < n) return kOfxStatFailed;
  for (int i = 0; i < n; ++i) {
    if (p->isInt()) *va_arg(ap, int*) = (int)std::lround(v[i]);
    else *va_arg(ap, double*) = v[i];
  }
  return kOfxStatOK;
}
static OfxStatus paramGetValue(OfxParamHandle h, ...) {
  if (!h) return kOfxStatErrBadHandle;
  va_list ap;
  va_start(ap, h);
  OfxStatus s = getValues((Param*)h, 0.0, ap);
  va_end(ap);
  return s;
}
static OfxStatus paramGetValueAtTime(OfxParamHandle h, OfxTime t, ...) {
  if (!h) return kOfxStatErrBadHandle;
  va_list ap;
  va_start(ap, t);
  OfxStatus s = getValues((Param*)h, t, ap);
  va_end(ap);
  return s;
}
static OfxStatus paramGetDerivative(OfxParamHandle, OfxTime, ...) { return kOfxStatErrUnsupported; }
static OfxStatus paramGetIntegral(OfxParamHandle, OfxTime, OfxTime, ...) { return kOfxStatErrUnsupported; }
static std::vector<double> readArgs(Param* p, va_list ap) {
  std::vector<double> v(p->dims());
  for (size_t i = 0; i < v.size(); ++i) v[i] = p->isInt() ? (double)va_arg(ap, int) : va_arg(ap, double);
  return v;
}
static OfxStatus paramSetValue(OfxParamHandle h, ...) {
  if (!h) return kOfxStatErrBadHandle;
  if (!gSetValueAllowed) ++gSetValueOutsideAllowed;
  Param* p = (Param*)h;
  if (p->dims() == 0) return kOfxStatErrBadHandle;
  va_list ap;
  va_start(ap, h);
  p->value = readArgs(p, ap);
  va_end(ap);
  return kOfxStatOK;
}
static OfxStatus paramSetValueAtTime(OfxParamHandle h, OfxTime t, ...) {
  if (!h) return kOfxStatErrBadHandle;
  if (!gSetValueAllowed) ++gSetValueOutsideAllowed;
  Param* p = (Param*)h;
  if (p->dims() == 0) return kOfxStatErrBadHandle;
  va_list ap;
  va_start(ap, t);
  p->keys[t] = readArgs(p, ap);
  va_end(ap);
  return kOfxStatOK;
}
static OfxStatus paramGetNumKeys(OfxParamHandle h, unsigned int* n) {
  if (!h || !n) return kOfxStatErrBadHandle;
  *n = (unsigned int)((Param*)h)->keys.size();
  return kOfxStatOK;
}
static OfxStatus paramGetKeyTime(OfxParamHandle h, unsigned int nth, OfxTime* t) {
  if (!h || !t) return kOfxStatErrBadHandle;
  Param* p = (Param*)h;
  if (nth >= p->keys.size()) return kOfxStatErrBadIndex;
  auto it = p->keys.begin();
  std::advance(it, nth);
  *t = it->first;
  return kOfxStatOK;
}
static OfxStatus paramGetKeyIndex(OfxParamHandle, OfxTime, int, int*) { return kOfxStatFailed; }
static OfxStatus paramDeleteKey(OfxParamHandle h, OfxTime t) {
  if (!h) return kOfxStatErrBadHandle;
  return ((Param*)h)->keys.erase(t) ? kOfxStatOK : kOfxStatErrBadIndex;
}
static OfxStatus paramDeleteAllKeys(OfxParamHandle h) {
  if (!h) return kOfxStatErrBadHandle;
  ((Param*)h)->keys.clear();
  return kOfxStatOK;
}
static OfxStatus paramCopy(OfxParamHandle, OfxParamHandle, OfxTime, const OfxRangeD*) { return kOfxStatErrUnsupported; }
static OfxStatus paramEditBegin(OfxParamSetHandle ps, const char*) {
  if (!ps) return kOfxStatErrBadHandle;
  if (gInCreateInstance) ++gEditBlockInCreateInstance;  // VEGAS crashes here
  ((ParamSet*)ps)->editDepth++;
  ((ParamSet*)ps)->editBegins++;
  return kOfxStatOK;
}
static OfxStatus paramEditEnd(OfxParamSetHandle ps) {
  if (!ps) return kOfxStatErrBadHandle;
  if (((ParamSet*)ps)->editDepth <= 0) return kOfxStatFailed;
  ((ParamSet*)ps)->editDepth--;
  return kOfxStatOK;
}

static OfxParameterSuiteV1 gParamSuite = {paramDefine,       paramGetHandle,     paramSetGetPropertySet,
                                          paramGetPropertySet, paramGetValue,    paramGetValueAtTime,
                                          paramGetDerivative, paramGetIntegral,  paramSetValue,
                                          paramSetValueAtTime, paramGetNumKeys,  paramGetKeyTime,
                                          paramGetKeyIndex,  paramDeleteKey,     paramDeleteAllKeys,
                                          paramCopy,         paramEditBegin,     paramEditEnd};

////////////////////////////////////////////////////////////////////////////////
// Images, clips, effects

struct Image {
  int x1, y1, x2, y2, nComp, bpc;
  std::vector<unsigned char> buf;
  bool flip = false;  // top-down storage: negative row bytes
  std::string depth, comps, premult;
  bool bgra = false;
  std::ptrdiff_t rowBytes() const { return (std::ptrdiff_t)(x2 - x1) * nComp * bpc * (flip ? -1 : 1); }
  unsigned char* origin() {
    const std::ptrdiff_t rb = (std::ptrdiff_t)(x2 - x1) * nComp * bpc;
    return flip ? buf.data() + (y2 - y1 - 1) * rb : buf.data();
  }
  unsigned char* at(int x, int y) { return origin() + (std::ptrdiff_t)(y - y1) * rowBytes() + (std::ptrdiff_t)(x - x1) * nComp * bpc; }
  float get(int x, int y, int c) {
    unsigned char* p = at(x, y) + c * bpc;
    if (bpc == 1) return *p / 255.0f;
    if (bpc == 2) return *(uint16_t*)p / 65535.0f;
    return *(float*)p;
  }
  void put(int x, int y, int c, float v) {
    unsigned char* p = at(x, y) + c * bpc;
    v = std::max(0.0f, v);
    if (bpc == 1) *p = (unsigned char)std::lround(std::min(1.0f, v) * 255);
    else if (bpc == 2) *(uint16_t*)p = (uint16_t)std::lround(std::min(1.0f, v) * 65535);
    else *(float*)p = v;
  }
};

static Image makeImage(int x1, int y1, int x2, int y2, const std::string& depth, const std::string& comps) {
  Image im;
  im.x1 = x1; im.y1 = y1; im.x2 = x2; im.y2 = y2;
  im.depth = depth;
  im.comps = comps;
  im.nComp = comps == kOfxImageComponentRGBA ? 4 : comps == kOfxImageComponentRGB ? 3 : 1;
  im.bpc = depth == kOfxBitDepthByte ? 1 : depth == kOfxBitDepthShort ? 2 : 4;
  im.buf.assign((size_t)(x2 - x1) * (y2 - y1) * im.nComp * im.bpc, 0);
  im.premult = kOfxImagePreMultiplied;
  im.bgra = depth == kOfxBitDepthByte && im.nComp == 4;  // like VEGAS
  return im;
}

struct OfxImageClipStruct {
  std::string name;
  PropSet props;
  Image* image = nullptr;  // what clipGetImage returns
};
typedef OfxImageClipStruct Clip;

struct OfxImageEffectStruct {
  PropSet props;
  ParamSet params;
  std::map<std::string, std::unique_ptr<Clip>> clips;
};
typedef OfxImageEffectStruct Effect;

static int gOutstandingImages = 0;

static OfxStatus getPropertySet(OfxImageEffectHandle e, OfxPropertySetHandle* p) {
  if (!e || !p) return kOfxStatErrBadHandle;
  *p = (OfxPropertySetHandle) & ((Effect*)e)->props;
  return kOfxStatOK;
}
static OfxStatus getParamSet(OfxImageEffectHandle e, OfxParamSetHandle* p) {
  if (!e || !p) return kOfxStatErrBadHandle;
  *p = (OfxParamSetHandle) & ((Effect*)e)->params;
  return kOfxStatOK;
}
static OfxStatus clipDefine(OfxImageEffectHandle e, const char* name, OfxPropertySetHandle* p) {
  if (!e || !name) return kOfxStatErrBadHandle;
  Effect* fx = (Effect*)e;
  std::unique_ptr<Clip> c(new Clip());
  c->name = name;
  if (p) *p = (OfxPropertySetHandle)&c->props;
  fx->clips[name] = std::move(c);
  return kOfxStatOK;
}
static OfxStatus clipGetHandle(OfxImageEffectHandle e, const char* name, OfxImageClipHandle* c, OfxPropertySetHandle* p) {
  if (!e || !name || !c) return kOfxStatErrBadHandle;
  Effect* fx = (Effect*)e;
  auto it = fx->clips.find(name);
  if (it == fx->clips.end()) return kOfxStatErrUnknown;
  *c = (OfxImageClipHandle)it->second.get();
  if (p) *p = (OfxPropertySetHandle)&it->second->props;
  return kOfxStatOK;
}
static OfxStatus clipGetPropertySet(OfxImageClipHandle c, OfxPropertySetHandle* p) {
  if (!c || !p) return kOfxStatErrBadHandle;
  *p = (OfxPropertySetHandle) & ((Clip*)c)->props;
  return kOfxStatOK;
}
static OfxStatus clipGetImage(OfxImageClipHandle c, OfxTime, const OfxRectD*, OfxPropertySetHandle* out) {
  if (!c || !out) return kOfxStatErrBadHandle;
  Clip* clip = (Clip*)c;
  if (!clip->image) return kOfxStatFailed;
  Image* im = clip->image;
  PropSet* li = new PropSet();
  li->setP(kOfxImagePropData, im->origin());
  li->setI(kOfxImagePropBounds, im->x1, 0);
  li->setI(kOfxImagePropBounds, im->y1, 1);
  li->setI(kOfxImagePropBounds, im->x2, 2);
  li->setI(kOfxImagePropBounds, im->y2, 3);
  li->setI(kOfxImagePropRegionOfDefinition, im->x1, 0);
  li->setI(kOfxImagePropRegionOfDefinition, im->y1, 1);
  li->setI(kOfxImagePropRegionOfDefinition, im->x2, 2);
  li->setI(kOfxImagePropRegionOfDefinition, im->y2, 3);
  li->setI(kOfxImagePropRowBytes, (int)im->rowBytes());
  li->setS(kOfxImageEffectPropPixelDepth, im->depth);
  li->setS(kOfxImageEffectPropComponents, im->comps);
  li->setS(kOfxImageEffectPropPreMultiplication, im->premult);
  li->setD(kOfxImagePropPixelAspectRatio, 1.0);
  li->setS(kOfxImagePropField, kOfxImageFieldNone);
  li->setS(kOfxImagePropUniqueIdentifier, "img");
  if (im->bgra) li->setS("OfxImageEffectPropPixelOrder", "OfxImagePixelOrderBGRA");
  // like VEGAS: no kOfxImageEffectPropRenderScale on images
  li->readOnly = true;
  ++gOutstandingImages;
  *out = (OfxPropertySetHandle)li;
  return kOfxStatOK;
}
static OfxStatus clipReleaseImage(OfxPropertySetHandle h) {
  if (!h) return kOfxStatErrBadHandle;
  --gOutstandingImages;
  delete (PropSet*)h;
  return kOfxStatOK;
}
static OfxStatus clipGetRegionOfDefinition(OfxImageClipHandle c, OfxTime, OfxRectD* r) {
  if (!c || !r) return kOfxStatErrBadHandle;
  Clip* clip = (Clip*)c;
  r->x1 = clip->props.getD("mock.rod", 0);
  r->y1 = clip->props.getD("mock.rod", 1);
  r->x2 = clip->props.getD("mock.rod", 2);
  r->y2 = clip->props.getD("mock.rod", 3);
  return kOfxStatOK;
}
static int abortFn(OfxImageEffectHandle) { return 0; }
static OfxStatus imageMemoryAlloc(OfxImageEffectHandle, size_t, OfxImageMemoryHandle*) { return kOfxStatErrMemory; }
static OfxStatus imageMemoryFree(OfxImageMemoryHandle) { return kOfxStatErrBadHandle; }
static OfxStatus imageMemoryLock(OfxImageMemoryHandle, void**) { return kOfxStatErrBadHandle; }
static OfxStatus imageMemoryUnlock(OfxImageMemoryHandle) { return kOfxStatErrBadHandle; }

static OfxImageEffectSuiteV1 gEffectSuite = {getPropertySet, getParamSet,        clipDefine,
                                             clipGetHandle,  clipGetPropertySet, clipGetImage,
                                             clipReleaseImage, clipGetRegionOfDefinition, abortFn,
                                             imageMemoryAlloc, imageMemoryFree,   imageMemoryLock,
                                             imageMemoryUnlock};

////////////////////////////////////////////////////////////////////////////////
// Multithread, interact, message and memory suites

static thread_local unsigned int tThreadIndex = 0;
static thread_local bool tSpawned = false;
static int gMultiThreadCalls = 0;
static OfxStatus mtMultiThread(OfxThreadFunctionV1 func, unsigned int n, void* arg) {
  if (!func) return kOfxStatErrBadHandle;
  ++gMultiThreadCalls;
  std::vector<std::thread> threads;
  for (unsigned int i = 1; i < n; ++i)
    threads.emplace_back([=]() {
      tThreadIndex = i;
      tSpawned = true;
      func(i, n, arg);
    });
  func(0, n, arg);
  for (auto& t : threads) t.join();
  return kOfxStatOK;
}
static OfxStatus mtNumCPUs(unsigned int* n) {
  *n = 4;
  return kOfxStatOK;
}
static OfxStatus mtIndex(unsigned int* i) {
  *i = tThreadIndex;
  return kOfxStatOK;
}
static int mtIsSpawned(void) { return tSpawned ? 1 : 0; }
static OfxStatus mtMutexCreate(OfxMutexHandle* m, int) {
  *m = (OfxMutexHandle) new std::recursive_mutex();
  return kOfxStatOK;
}
static OfxStatus mtMutexDestroy(const OfxMutexHandle m) {
  delete (std::recursive_mutex*)m;
  return kOfxStatOK;
}
static OfxStatus mtMutexLock(const OfxMutexHandle m) {
  ((std::recursive_mutex*)m)->lock();
  return kOfxStatOK;
}
static OfxStatus mtMutexUnlock(const OfxMutexHandle m) {
  ((std::recursive_mutex*)m)->unlock();
  return kOfxStatOK;
}
static OfxStatus mtMutexTryLock(const OfxMutexHandle m) {
  return ((std::recursive_mutex*)m)->try_lock() ? kOfxStatOK : kOfxStatFailed;
}
static OfxMultiThreadSuiteV1 gThreadSuite = {mtMultiThread, mtNumCPUs,     mtIndex,        mtIsSpawned,
                                             mtMutexCreate, mtMutexDestroy, mtMutexLock, mtMutexUnlock,
                                             mtMutexTryLock};

struct InteractInstance {
  PropSet props;
};
static int gRedraws = 0;
static OfxStatus iSwap(OfxInteractHandle) { return kOfxStatOK; }
static OfxStatus iRedraw(OfxInteractHandle) {
  ++gRedraws;
  return kOfxStatOK;
}
static OfxStatus iGetProps(OfxInteractHandle h, OfxPropertySetHandle* p) {
  if (!h || !p) return kOfxStatErrBadHandle;
  *p = (OfxPropertySetHandle) & ((InteractInstance*)h)->props;
  return kOfxStatOK;
}
static OfxInteractSuiteV1 gInteractSuite = {iSwap, iRedraw, iGetProps};

static OfxStatus msgMessage(void*, const char* type, const char*, const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  std::printf("  [plugin message %s] ", type ? type : "");
  std::vprintf(fmt, ap);
  std::printf("\n");
  va_end(ap);
  return kOfxStatOK;
}
static OfxMessageSuiteV1 gMessageSuite = {msgMessage};

static OfxStatus memAlloc(void*, size_t n, void** p) {
  *p = std::malloc(n);
  return *p ? kOfxStatOK : kOfxStatErrMemory;
}
static OfxStatus memFree(void* p) {
  std::free(p);
  return kOfxStatOK;
}
static OfxMemorySuiteV1 gMemorySuite = {memAlloc, memFree};

////////////////////////////////////////////////////////////////////////////////
// Host

static PropSet gHostProps;
static const void* fetchSuite(OfxPropertySetHandle, const char* name, int version) {
  if (version != 1) return nullptr;
  if (!std::strcmp(name, kOfxPropertySuite)) return &gPropSuite;
  if (!std::strcmp(name, kOfxImageEffectSuite)) return &gEffectSuite;
  if (!std::strcmp(name, kOfxParameterSuite)) return &gParamSuite;
  if (!std::strcmp(name, kOfxMultiThreadSuite)) return &gThreadSuite;
  if (!std::strcmp(name, kOfxInteractSuite)) return &gInteractSuite;
  if (!std::strcmp(name, kOfxMessageSuite)) return &gMessageSuite;
  if (!std::strcmp(name, kOfxMemorySuite)) return &gMemorySuite;
  return nullptr;  // no draw suite, like VEGAS
}
static OfxHost gHost = {(OfxPropertySetHandle)&gHostProps, fetchSuite};

////////////////////////////////////////////////////////////////////////////////
// Test driver

typedef OfxPlugin* (*GetPluginFn)(int);
typedef int (*GetCountFn)(void);

static OfxPlugin* gPlugin = nullptr;

static OfxStatus call(const char* action, const void* handle, PropSet* in = nullptr, PropSet* out = nullptr) {
  return gPlugin->mainEntry(action, handle, (OfxPropertySetHandle)in, (OfxPropertySetHandle)out);
}

static Param* param(Effect& fx, const std::string& name) {
  auto it = fx.params.byName.find(name);
  return it == fx.params.byName.end() ? nullptr : it->second;
}

static void instanceChanged(Effect& fx, const std::string& name, double t) {
  PropSet in;
  in.setS(kOfxPropType, kOfxTypeParameter);
  in.setS(kOfxPropName, name);
  in.setS(kOfxPropChangeReason, kOfxChangeUserEdited);
  in.setD(kOfxPropTime, t);
  in.setD(kOfxImageEffectPropRenderScale, 1.0, 0);
  in.setD(kOfxImageEffectPropRenderScale, 1.0, 1);
  gSetValueAllowed = true;
  call(kOfxActionBeginInstanceChanged, &fx, &in);
  OfxStatus s = call(kOfxActionInstanceChanged, &fx, &in);
  CHECK(s == kOfxStatOK || s == kOfxStatReplyDefault);
  call(kOfxActionEndInstanceChanged, &fx, &in);
  gSetValueAllowed = false;
}

// user edits: set the value like the host UI would, then notify
static void userSet(Effect& fx, const std::string& name, std::vector<double> v, double t = 0.0) {
  Param* p = param(fx, name);
  CHECK(p != nullptr);
  if (!p) return;
  if (!p->keys.empty()) p->keys[t] = v;
  else p->value = v;
  instanceChanged(fx, name, t);
}

static std::vector<double> valueOf(Effect& fx, const std::string& name, double t = 0.0) {
  Param* p = param(fx, name);
  return p ? p->at(t) : std::vector<double>();
}

static void writePPM(Image& im, const std::string& path) {
  FILE* f = std::fopen(path.c_str(), "wb");
  if (!f) return;
  const int w = im.x2 - im.x1, h = im.y2 - im.y1;
  std::fprintf(f, "P6\n%d %d\n255\n", w, h);
  for (int y = im.y2 - 1; y >= im.y1; --y)
    for (int x = im.x1; x < im.x2; ++x) {
      unsigned char rgb[3];
      // composite over a grey checkerboard so transparency is visible
      const float bg = (((x / 8) + (y / 8)) & 1) ? 0.35f : 0.25f;
      const float a = im.nComp == 4 ? im.get(x, y, 3) : 1.0f;
      for (int c = 0; c < 3; ++c) {
        int ch = im.nComp == 1 ? 0 : (im.bgra ? 2 - c : c);
        float v = im.nComp == 1 ? im.get(x, y, 0) : im.get(x, y, ch);
        if (im.nComp == 4 && im.premult == kOfxImageUnPreMultiplied) v *= a;
        v = v + bg * (1.0f - (im.nComp == 1 ? 1.0f : a));
        rgb[c] = (unsigned char)std::lround(std::max(0.0f, std::min(1.0f, v)) * 255);
      }
      std::fwrite(rgb, 1, 3, f);
    }
  std::fclose(f);
}

// A "character": a horizontal bar with a colour gradient, on transparency.
static void paintCharacter(Image& im, double scale) {
  for (int y = im.y1; y < im.y2; ++y)
    for (int x = im.x1; x < im.x2; ++x) {
      const double fx = (x + 0.5) / scale, fy = (y + 0.5) / scale;
      const bool bar = fx > 40 && fx < 280 && fy > 75 && fy < 105;
      const bool head = std::hypot(fx - 60, fy - 90) < 28;
      const float a = (bar || head) ? 1.0f : 0.0f;
      const float r = (float)(fx / 320.0), g = (float)(fy / 180.0), b = 0.8f;
      if (im.nComp == 1) {
        im.put(x, y, 0, a);
        continue;
      }
      const float rgb[3] = {r * a, g * a, b * a};
      for (int c = 0; c < 3; ++c) im.put(x, y, im.bgra ? 2 - c : c, rgb[c]);
      if (im.nComp == 4) im.put(x, y, 3, a);
    }
}

struct Run {
  std::unique_ptr<Effect> fx;
  Image src, dst;
};

static void setupInstanceEffect(Effect& desc, Effect& fx, double projW, double projH) {
  // copy parameter descriptors, initialise values from defaults
  for (auto& p : desc.params.list) {
    OfxPropertySetHandle props = nullptr;
    paramDefine((OfxParamSetHandle)&fx.params, p->type.c_str(), p->name.c_str(), &props);
    Param* np = fx.params.byName[p->name];
    np->props = p->props;
    const int n = np->dims();
    np->value.assign(n, 0.0);
    for (int i = 0; i < n; ++i)
      np->value[i] = np->isInt() ? (double)np->props.getI(kOfxParamPropDefault, i, 0)
                                 : np->props.getD(kOfxParamPropDefault, i, 0.0);
  }
  fx.props.setS(kOfxImageEffectPropContext, kOfxImageEffectContextFilter);
  fx.props.setD(kOfxImageEffectPropProjectSize, projW, 0);
  fx.props.setD(kOfxImageEffectPropProjectSize, projH, 1);
  fx.props.setD(kOfxImageEffectPropProjectOffset, 0.0, 0);
  fx.props.setD(kOfxImageEffectPropProjectOffset, 0.0, 1);
  fx.props.setD(kOfxImageEffectPropProjectExtent, projW, 0);
  fx.props.setD(kOfxImageEffectPropProjectExtent, projH, 1);
  fx.props.setD(kOfxImageEffectPropProjectPixelAspectRatio, 1.0);
  fx.props.setD(kOfxImageEffectPropFrameRate, 30.0);
  for (const char* name : {kOfxImageEffectSimpleSourceClipName, kOfxImageEffectOutputClipName}) {
    clipDefine((OfxImageEffectHandle)&fx, name, nullptr);
    Clip* c = fx.clips[name].get();
    c->props = desc.clips[name]->props;
    c->props.setS(kOfxPropName, name);
    c->props.setD(kOfxImagePropPixelAspectRatio, 1.0);
    c->props.setI(kOfxImageClipPropConnected, 1);
    c->props.setD("mock.rod", 0.0, 0);
    c->props.setD("mock.rod", 0.0, 1);
    c->props.setD("mock.rod", projW, 2);
    c->props.setD("mock.rod", projH, 3);
    c->props.setS("OfxImageEffectPropPixelOrder", "OfxImagePixelOrderBGRA");
  }
}

#define kQuality "OfxImageEffectPropRenderQuality"
#define kQualityPreview "OfxImageEffectPropRenderQualityPreview"
#define kQualityBest "OfxImageEffectPropRenderQualityBest"

// quality: like VEGAS, Preview for the preview window, Best for a final render.
static OfxStatus render(Effect& fx, double t, double rs, const char* quality = kQualityBest,
                        const char* field = kOfxImageFieldNone) {
  Clip* dst = fx.clips[kOfxImageEffectOutputClipName].get();
  PropSet in;
  in.setD(kOfxPropTime, t);
  in.setS(kOfxImageEffectPropFieldToRender, field);
  in.setI(kOfxImageEffectPropRenderWindow, dst->image->x1, 0);
  in.setI(kOfxImageEffectPropRenderWindow, dst->image->y1, 1);
  in.setI(kOfxImageEffectPropRenderWindow, dst->image->x2, 2);
  in.setI(kOfxImageEffectPropRenderWindow, dst->image->y2, 3);
  in.setD(kOfxImageEffectPropRenderScale, rs, 0);
  in.setD(kOfxImageEffectPropRenderScale, rs, 1);
  in.setS(kQuality, quality);
  const bool preview = std::strcmp(quality, kQualityPreview) == 0;
  in.setI(kOfxImageEffectPropSequentialRenderStatus, preview ? 0 : 1);
  in.setI(kOfxImageEffectPropInteractiveRenderStatus, preview ? 1 : 0);
  call(kOfxImageEffectActionBeginSequenceRender, &fx, &in);
  const OfxStatus s = call(kOfxImageEffectActionRender, &fx, &in);
  call(kOfxImageEffectActionEndSequenceRender, &fx, &in);
  CHECK(gOutstandingImages == 0);
  return s;
}

static bool isIdentity(Effect& fx, double t, const char* quality = nullptr) {
  PropSet in, out;
  in.setD(kOfxPropTime, t);
  if (quality) in.setS(kQuality, quality);
  in.setS(kOfxImageEffectPropFieldToRender, kOfxImageFieldNone);
  in.setI(kOfxImageEffectPropRenderWindow, 0, 0);
  in.setI(kOfxImageEffectPropRenderWindow, 0, 1);
  in.setI(kOfxImageEffectPropRenderWindow, 320, 2);
  in.setI(kOfxImageEffectPropRenderWindow, 180, 3);
  in.setD(kOfxImageEffectPropRenderScale, 1.0, 0);
  in.setD(kOfxImageEffectPropRenderScale, 1.0, 1);
  out.setS(kOfxPropName, "");
  out.setD(kOfxPropTime, t);
  const OfxStatus s = call(kOfxImageEffectActionIsIdentity, &fx, &in, &out);
  if (s == kOfxStatOK) CHECK(out.getS(kOfxPropName) == kOfxImageEffectSimpleSourceClipName);
  return s == kOfxStatOK;
}

int main(int argc, char** argv) {
  if (argc < 2) {
    std::fprintf(stderr, "usage: mock_host <PuppetPin.ofx> [--no-overlays] [output dir]\n");
    return 2;
  }
  std::string outDir;
  bool overlays = true;
  for (int i = 2; i < argc; ++i) {
    if (std::strcmp(argv[i], "--no-overlays") == 0) overlays = false;
    else outDir = argv[i];
  }
  std::printf("host %s overlays\n", overlays ? "with" : "without");
#if defined(_WIN32)
  HMODULE lib = LoadLibraryA(argv[1]);
  if (!lib) {
    std::printf("FAIL: cannot load %s (error %lu)\n", argv[1], GetLastError());
    return 1;
  }
  GetPluginFn getPlugin = (GetPluginFn)GetProcAddress(lib, "OfxGetPlugin");
  GetCountFn getCount = (GetCountFn)GetProcAddress(lib, "OfxGetNumberOfPlugins");
#else
  void* lib = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
  if (!lib) {
    std::printf("FAIL: cannot load %s: %s\n", argv[1], dlerror());
    return 1;
  }
  GetPluginFn getPlugin = (GetPluginFn)dlsym(lib, "OfxGetPlugin");
  GetCountFn getCount = (GetCountFn)dlsym(lib, "OfxGetNumberOfPlugins");
#endif
  if (!getPlugin || !getCount) {
    std::printf("FAIL: missing OfxGetPlugin / OfxGetNumberOfPlugins exports\n");
    return 1;
  }
  CHECK(getCount() == 1);
  gPlugin = getPlugin(0);
  CHECK(gPlugin != nullptr);
  if (!gPlugin) return 1;
  CHECK(getPlugin(1) == nullptr);
  std::printf("plugin: %s %u.%u (%s v%d)\n", gPlugin->pluginIdentifier, gPlugin->pluginVersionMajor,
              gPlugin->pluginVersionMinor, gPlugin->pluginApi, gPlugin->apiVersion);
  CHECK(std::strcmp(gPlugin->pluginApi, kOfxImageEffectPluginApi) == 0);
  CHECK(gPlugin->apiVersion == 1);

  // host description, VEGAS-like
  gHostProps.setS(kOfxPropName, "com.sonycreativesoftware.vegas");
  gHostProps.setS(kOfxPropLabel, "VEGAS Pro (mock)");
  gHostProps.setI(kOfxImageEffectHostPropIsBackground, 0);
  gHostProps.setI(kOfxImageEffectPropSupportsOverlays, overlays ? 1 : 0);
  gHostProps.setI(kOfxImageEffectPropSupportsMultiResolution, 0);
  gHostProps.setI(kOfxImageEffectPropSupportsTiles, 0);
  gHostProps.setI(kOfxImageEffectPropTemporalClipAccess, 1);
  gHostProps.setS(kOfxImageEffectPropSupportedComponents, kOfxImageComponentRGBA, 0);
  gHostProps.setS(kOfxImageEffectPropSupportedComponents, kOfxImageComponentAlpha, 1);
  gHostProps.setS(kOfxImageEffectPropSupportedPixelDepths, kOfxBitDepthByte, 0);
  gHostProps.setS(kOfxImageEffectPropSupportedPixelDepths, kOfxBitDepthFloat, 1);
  gHostProps.setI(kOfxParamHostPropMaxParameters, 1000);
  gPlugin->setHost(&gHost);

  std::printf("load / describe\n");
  CHECK(call(kOfxActionLoad, nullptr) == kOfxStatOK);
  Effect desc;
  CHECK(call(kOfxActionDescribe, &desc) == kOfxStatOK);
  CHECK(desc.props.getS(kOfxPropLabel) == "Puppet Pin");
  CHECK(desc.props.getS(kOfxImageEffectPluginPropGrouping) == "Distort");
  CHECK(desc.props.getS(kOfxImageEffectPropSupportedContexts, 0) == kOfxImageEffectContextFilter);
  CHECK(desc.props.getS(kOfxImageEffectPropSupportedPixelDepths, 0) == kOfxBitDepthByte);
  CHECK(desc.props.getS(kOfxImageEffectPropSupportedPixelDepths, 2) == kOfxBitDepthFloat);
  CHECK(desc.props.getI(kOfxImageEffectPropSupportsTiles, 0, 1) == 0);
  OfxPluginEntryPoint* overlay = (OfxPluginEntryPoint*)desc.props.getP(kOfxImageEffectPluginPropOverlayInteractV1);
  CHECK(overlay != nullptr);
  CHECK(desc.props.getP(kOfxImageEffectPluginPropOverlayInteractV2) == nullptr);  // no draw suite here

  std::printf("describe in context (filter)\n");
  PropSet ctx;
  ctx.setS(kOfxImageEffectPropContext, kOfxImageEffectContextFilter);
  CHECK(call(kOfxImageEffectActionDescribeInContext, &desc, &ctx) == kOfxStatOK);
  CHECK(desc.clips.count(kOfxImageEffectSimpleSourceClipName) == 1);
  CHECK(desc.clips.count(kOfxImageEffectOutputClipName) == 1);
  int pins = 0, secret = 0;
  for (auto& p : desc.params.list) {
    if (p->name.size() > 4 && p->name.compare(p->name.size() - 4, 4, "_pos") == 0) {
      ++pins;
      CHECK(p->type == kOfxParamTypeDouble2D);
      CHECK(p->props.getS(kOfxParamPropDoubleType) == kOfxParamDoubleTypeNormalisedXYAbsolute);
      CHECK(p->props.getI(kOfxParamPropAnimates, 0, 0) == 1);
    }
    if (p->props.getI(kOfxParamPropSecret, 0, 0)) ++secret;
    // every parent must exist and be a group
    const std::string parent = p->props.getS(kOfxParamPropParent);
    if (!parent.empty()) {
      CHECK(desc.params.byName.count(parent) == 1);
      if (desc.params.byName.count(parent)) CHECK(desc.params.byName[parent]->type == kOfxParamTypeGroup);
    }
  }
  std::printf("  %zu parameters, %d pins, %d hidden\n", desc.params.list.size(), pins, secret);
  CHECK(pins == 20 && secret == 20);
  CHECK(desc.params.list.size() < 1000);  // VEGAS limit

  std::printf("create instance\n");
  const int W = 320, H = 180;
  Effect fx;
  setupInstanceEffect(desc, fx, W, H);
  gInCreateInstance = true;
  CHECK(call(kOfxActionCreateInstance, &fx) == kOfxStatOK);
  gInCreateInstance = false;
  CHECK(gEditBlockInCreateInstance == 0);
  CHECK(gSetValueOutsideAllowed == 0);
  CHECK(fx.props.getP(kOfxPropInstanceData) != nullptr);

  // images (8-bit BGRA like VEGAS)
  Image src = makeImage(0, 0, W, H, kOfxBitDepthByte, kOfxImageComponentRGBA);
  Image dst = makeImage(0, 0, W, H, kOfxBitDepthByte, kOfxImageComponentRGBA);
  paintCharacter(src, 1.0);
  fx.clips[kOfxImageEffectSimpleSourceClipName]->image = &src;
  fx.clips[kOfxImageEffectOutputClipName]->image = &dst;
  if (!outDir.empty()) writePPM(src, outDir + "/0_source.ppm");

  auto isSecret = [&](const char* name) { return param(fx, name)->props.getI(kOfxParamPropSecret, 0, 0) != 0; };
  auto isEnabled = [&](const char* name) { return param(fx, name)->props.getI(kOfxParamPropEnabled, 0, 1) != 0; };

  std::printf("part 1 - mesh mode: only mesh settings are shown\n");
  CHECK(valueOf(fx, "mode")[0] == 0.0);
  for (const char* n : {"meshGroup", "meshArea", "alphaThreshold", "meshDensity", "expansion"})
    CHECK(!isSecret(n) && isEnabled(n));
  for (const char* n : {"animateGroup", "rigidity", "animatePins", "overlayMesh", "resetAllPins", "pin01_reset"})
    CHECK(isSecret(n) && !isEnabled(n));
  CHECK(!isSecret("pin01_on") && !isSecret("pin01_pos") && isSecret("pin01_anchor"));

  std::printf("mesh mode render (mesh burned in)\n");
  CHECK(!isIdentity(fx, 0));
  CHECK(render(fx, 0, 1.0) == kOfxStatOK);
  if (!outDir.empty()) writePPM(dst, outDir + (overlays ? "/1_setup.ppm" : "/1_setup_no_overlays.ppm"));
  {
    // picture undeformed: an interior pixel away from mesh lines keeps its colour
    int same = 0, total = 0;
    for (int y = 0; y < H; ++y)
      for (int x = 0; x < W; ++x) {
        ++total;
        same += std::memcmp(src.at(x, y), dst.at(x, y), 4) == 0;
      }
    CHECK(same > total * 0.8);
    CHECK(same < total);  // ... but the mesh is drawn
    // without host overlays the (disabled, grey) pins are burned in too:
    // pin 1's default spot is (0.2, 0.8) of the frame
    const unsigned char* c = dst.at((int)(0.2 * W), (int)(0.8 * H));
    const bool grey = std::abs((int)c[0] - (int)c[1]) < 8 && std::abs((int)c[1] - (int)c[2]) < 8 && c[3] > 100;
    CHECK(grey == !overlays);
  }

  std::printf("place pins in setup mode\n");
  // pin 1 on the head, pin 2 in the middle of the bar, pin 3 at the bar tip
  const double n1[2] = {60.0 / W, 90.0 / H}, n2[2] = {150.0 / W, 90.0 / H}, n3[2] = {270.0 / W, 90.0 / H};
  userSet(fx, "pin01_on", {1});
  userSet(fx, "pin01_pos", {n1[0], n1[1]});
  userSet(fx, "pin02_on", {1});
  userSet(fx, "pin02_pos", {n2[0], n2[1]});
  userSet(fx, "pin03_on", {1});
  userSet(fx, "pin03_pos", {n3[0], n3[1]});
  CHECK(std::fabs(valueOf(fx, "pin01_anchor")[0] - n1[0]) < 1e-12);
  CHECK(std::fabs(valueOf(fx, "pin03_anchor")[1] - n3[1]) < 1e-12);
  CHECK(fx.params.editDepth == 0);

  std::printf("part 2 - animate mode: mesh settings hidden, animate settings shown\n");
  userSet(fx, "mode", {1});
  for (const char* n : {"meshGroup", "meshArea", "alphaThreshold", "meshDensity", "expansion"})
    CHECK(isSecret(n) && !isEnabled(n));
  for (const char* n : {"animateGroup", "rigidity", "animatePins", "overlayMesh", "resetAllPins", "pin01_reset"})
    CHECK(!isSecret(n) && isEnabled(n));
  CHECK(!isSecret("pin01_pos") && isSecret("pin01_anchor"));
  // nothing moved yet: a final render is the source, the preview still shows the pins
  CHECK(isIdentity(fx, 0, kQualityBest));
  CHECK(!isIdentity(fx, 0, kQualityPreview));
  CHECK(!isIdentity(fx, 0));  // quality unknown: pins may have to be drawn

  std::printf("animate: keyframe pin 3 upwards\n");
  Param* p3 = param(fx, "pin03_pos");
  p3->keys[0.0] = {n3[0], n3[1]};
  p3->keys[10.0] = {200.0 / W, 160.0 / H};
  instanceChanged(fx, "pin03_pos", 10.0);
  // moving a pin outside setup mode must not move its anchor
  CHECK(std::fabs(valueOf(fx, "pin03_anchor")[0] - n3[0]) < 1e-12);
  CHECK(isIdentity(fx, 0.0, kQualityBest));
  CHECK(!isIdentity(fx, 10.0, kQualityBest));
  CHECK(render(fx, 10.0, 1.0) == kOfxStatOK);
  if (!outDir.empty()) writePPM(dst, outDir + "/2_deformed.ppm");
  {
    // preview render: same bent picture plus the pins, and no mesh
    std::vector<unsigned char> finalRender = dst.buf;
    CHECK(render(fx, 10.0, 1.0, kQualityPreview) == kOfxStatOK);
    if (!outDir.empty()) writePPM(dst, outDir + "/2_deformed_preview.ppm");
    int diff = 0;
    for (size_t i = 0; i < finalRender.size(); i += 4) diff += std::memcmp(&finalRender[i], &dst.buf[i], 4) != 0;
    std::printf("  preview differs from render in %d pixels (pin markers)\n", diff);
    CHECK(diff > 100 && diff < 3000);  // three pins, no mesh lines
    // pin 1 sits on the head: yellow marker (BGRA)
    const unsigned char* c = dst.at(60, 90);
    CHECK(c[2] > 200 && c[1] > 170 && c[0] < 80);
    userSet(fx, "animatePins", {2});  // Never
    CHECK(render(fx, 10.0, 1.0, kQualityPreview) == kOfxStatOK);
    CHECK(std::memcmp(finalRender.data(), dst.buf.data(), finalRender.size()) == 0);
    userSet(fx, "animatePins", {0});
    CHECK(render(fx, 10.0, 1.0) == kOfxStatOK);
  }
  {
    // the bar tip followed the pin: around (200,160) is now opaque, the old tip is not
    CHECK(dst.get(200, 160, 3) > 0.9f);
    CHECK(dst.get(270, 90, 3) < 0.1f);
    // the head (pinned) stayed put and kept its colour
    CHECK(std::memcmp(src.at(60, 90), dst.at(60, 90), 4) == 0 ||
          std::abs((int)src.at(60, 90)[0] - (int)dst.at(60, 90)[0]) <= 2);
    // colour travelled with the tip: (200,160) shows the colour that was at the tip
    const float expectR = 270.0f / 320.0f;  // red channel of the source gradient at x=270
    CHECK(std::fabs(dst.get(200, 160, 2) - expectR) < 0.06f);  // BGRA: red is channel 2
  }

  std::printf("half resolution preview render (render scale 0.5)\n");
  {
    Image s2 = makeImage(0, 0, W / 2, H / 2, kOfxBitDepthByte, kOfxImageComponentRGBA);
    Image d2 = makeImage(0, 0, W / 2, H / 2, kOfxBitDepthByte, kOfxImageComponentRGBA);
    paintCharacter(s2, 0.5);
    fx.clips[kOfxImageEffectSimpleSourceClipName]->image = &s2;
    fx.clips[kOfxImageEffectOutputClipName]->image = &d2;
    CHECK(render(fx, 10.0, 0.5) == kOfxStatOK);
    CHECK(d2.get(100, 80, 3) > 0.9f);
    CHECK(d2.get(135, 45, 3) < 0.1f);
    if (!outDir.empty()) writePPM(d2, outDir + "/3_half.ppm");
  }

  std::printf("other pixel formats\n");
  struct Fmt {
    const char* depth;
    const char* comps;
    bool flip;
    const char* premult;
  } fmts[] = {
      {kOfxBitDepthFloat, kOfxImageComponentRGBA, false, kOfxImagePreMultiplied},
      {kOfxBitDepthFloat, kOfxImageComponentRGBA, true, kOfxImageUnPreMultiplied},
      {kOfxBitDepthShort, kOfxImageComponentRGBA, false, kOfxImagePreMultiplied},
      {kOfxBitDepthByte, kOfxImageComponentRGBA, true, kOfxImageUnPreMultiplied},
      {kOfxBitDepthByte, kOfxImageComponentRGB, false, kOfxImageOpaque},
      {kOfxBitDepthFloat, kOfxImageComponentAlpha, false, kOfxImagePreMultiplied},
  };
  for (const Fmt& f : fmts) {
    Image s2 = makeImage(0, 0, W, H, f.depth, f.comps);
    Image d2 = makeImage(0, 0, W, H, f.depth, f.comps);
    s2.flip = d2.flip = f.flip;
    s2.premult = d2.premult = f.premult;
    paintCharacter(s2, 1.0);
    fx.clips[kOfxImageEffectSimpleSourceClipName]->image = &s2;
    fx.clips[kOfxImageEffectOutputClipName]->image = &d2;
    const OfxStatus st = render(fx, 10.0, 1.0);
    std::printf("  %s %s %s %s -> %d\n", f.depth, f.comps, f.flip ? "top-down" : "bottom-up", f.premult, st);
    CHECK(st == kOfxStatOK);
    if (s2.nComp != 3) {
      CHECK(d2.get(200, 160, s2.nComp == 4 ? 3 : 0) > 0.9f);
      CHECK(d2.get(270, 90, s2.nComp == 4 ? 3 : 0) < 0.1f);
    } else {
      // RGB has no alpha: the whole frame is the surface, the bar still moves
      CHECK(st == kOfxStatOK);
    }
  }
  // mismatched source/output formats are refused, not crashed on
  {
    Image s2 = makeImage(0, 0, W, H, kOfxBitDepthFloat, kOfxImageComponentRGBA);
    Image d2 = makeImage(0, 0, W, H, kOfxBitDepthByte, kOfxImageComponentRGBA);
    fx.clips[kOfxImageEffectSimpleSourceClipName]->image = &s2;
    fx.clips[kOfxImageEffectOutputClipName]->image = &d2;
    CHECK(render(fx, 10.0, 1.0) == kOfxStatErrImageFormat);
  }
  fx.clips[kOfxImageEffectSimpleSourceClipName]->image = &src;
  fx.clips[kOfxImageEffectOutputClipName]->image = &dst;

  std::printf("whole frame mesh area + burned overlay\n");
  userSet(fx, "meshArea", {1});
  userSet(fx, "burnOverlay", {1});
  CHECK(render(fx, 10.0, 1.0) == kOfxStatOK);
  if (!outDir.empty()) writePPM(dst, outDir + "/4_wholeframe_overlay.ppm");
  userSet(fx, "burnOverlay", {0});
  userSet(fx, "meshArea", {0});

  std::printf("reset buttons\n");
  userSet(fx, "pin03_reset", {}, 10.0);
  CHECK(std::fabs(valueOf(fx, "pin03_pos", 10.0)[0] - n3[0]) < 1e-12);
  p3->keys[10.0] = {200.0 / W, 160.0 / H};
  userSet(fx, "resetAllPins", {}, 10.0);
  CHECK(std::fabs(valueOf(fx, "pin03_pos", 10.0)[1] - n3[1]) < 1e-12);
  CHECK(fx.params.editDepth == 0);
  p3->keys[10.0] = {200.0 / W, 160.0 / H};

  std::printf("enabling a pin anchors it where it is\n");
  userSet(fx, "pin04_pos", {0.5, 0.5});  // disabled: anchor untouched
  const double a4 = valueOf(fx, "pin04_anchor")[0];
  CHECK(std::fabs(a4 - 0.5) > 1e-6);
  userSet(fx, "pin04_on", {1});
  CHECK(std::fabs(valueOf(fx, "pin04_anchor")[0] - 0.5) < 1e-12);
  userSet(fx, "pin04_on", {0});

  std::printf("overlay interact\n");
  if (overlays) {
    PropSet idesc;
    CHECK(overlay(kOfxActionDescribe, &idesc, nullptr, nullptr) == kOfxStatOK);
    InteractInstance ii;
    ii.props.setP(kOfxPropEffectInstance, &fx);
    ii.props.setD(kOfxInteractPropPixelScale, 1.0, 0);
    ii.props.setD(kOfxInteractPropPixelScale, 1.0, 1);
    CHECK(overlay(kOfxActionCreateInstance, &ii, nullptr, nullptr) == kOfxStatOK);
    auto penArgs = [&](double x, double y, double t) {
      PropSet in;
      in.setP(kOfxPropEffectInstance, &fx);
      in.setD(kOfxInteractPropPixelScale, 1.0, 0);
      in.setD(kOfxInteractPropPixelScale, 1.0, 1);
      in.setD(kOfxPropTime, t);
      in.setD(kOfxImageEffectPropRenderScale, 1.0, 0);
      in.setD(kOfxImageEffectPropRenderScale, 1.0, 1);
      in.setD(kOfxInteractPropPenPosition, x, 0);
      in.setD(kOfxInteractPropPenPosition, y, 1);
      in.setI(kOfxInteractPropPenViewportPosition, (int)x, 0);
      in.setI(kOfxInteractPropPenViewportPosition, (int)y, 1);
      in.setD(kOfxInteractPropPenPressure, 1.0);
      return in;
    };
    gSetValueAllowed = true;
    // draw: no OpenGL in this test host, must not crash
    {
      PropSet in = penArgs(0, 0, 10.0);
      in.setD(kOfxInteractPropBackgroundColour, 0.0, 0);
      const OfxStatus s = overlay(kOfxInteractActionDraw, &ii, (OfxPropertySetHandle)&in, nullptr);
      CHECK(s == kOfxStatOK || s == kOfxStatReplyDefault);
    }
    // drag pin 3 (currently at its keyframed spot (200,160) at t=10) to (220,150)
    PropSet down = penArgs(201, 159, 10.0);
    CHECK(overlay(kOfxInteractActionPenDown, &ii, (OfxPropertySetHandle)&down, nullptr) == kOfxStatOK);
    PropSet move = penArgs(220, 150, 10.0);
    CHECK(overlay(kOfxInteractActionPenMotion, &ii, (OfxPropertySetHandle)&move, nullptr) == kOfxStatOK);
    PropSet up = penArgs(220, 150, 10.0);
    CHECK(overlay(kOfxInteractActionPenUp, &ii, (OfxPropertySetHandle)&up, nullptr) == kOfxStatOK);
    CHECK(std::fabs(valueOf(fx, "pin03_pos", 10.0)[0] - 220.0 / W) < 1e-9);
    CHECK(std::fabs(valueOf(fx, "pin03_anchor")[0] - n3[0]) < 1e-12);  // not in setup: anchor stays
    CHECK(fx.params.editDepth == 0);
    // click on empty picture adds the first free pin (pin 4) there
    PropSet add = penArgs(100, 60, 10.0);
    CHECK(overlay(kOfxInteractActionPenDown, &ii, (OfxPropertySetHandle)&add, nullptr) == kOfxStatOK);
    CHECK(overlay(kOfxInteractActionPenUp, &ii, (OfxPropertySetHandle)&add, nullptr) == kOfxStatOK);
    CHECK(valueOf(fx, "pin04_on")[0] == 1.0);
    CHECK(std::fabs(valueOf(fx, "pin04_anchor")[0] - 100.0 / W) < 1e-9);
    CHECK(std::fabs(valueOf(fx, "pin04_pos")[1] - 60.0 / H) < 1e-9);
    // Delete removes the selected pin
    PropSet key;
    key.setP(kOfxPropEffectInstance, &fx);
    key.setD(kOfxPropTime, 10.0);
    key.setI(kOfxPropKeySym, kOfxKey_Delete);
    key.setS(kOfxPropKeyString, "");
    CHECK(overlay(kOfxInteractActionKeyDown, &ii, (OfxPropertySetHandle)&key, nullptr) == kOfxStatOK);
    CHECK(valueOf(fx, "pin04_on")[0] == 0.0);
    // clicks far outside the frame are left to the host
    PropSet outside = penArgs(-500, -500, 10.0);
    CHECK(overlay(kOfxInteractActionPenDown, &ii, (OfxPropertySetHandle)&outside, nullptr) == kOfxStatReplyDefault);
    gSetValueAllowed = false;
    CHECK(overlay(kOfxActionDestroyInstance, &ii, nullptr, nullptr) == kOfxStatOK);
    CHECK(fx.params.editDepth == 0);
  }

  std::printf("purge caches / destroy / unload\n");
  CHECK(call(kOfxActionPurgeCaches, &fx) == kOfxStatOK);
  CHECK(call(kOfxActionDestroyInstance, &fx) == kOfxStatOK);
  CHECK(call(kOfxActionUnload, nullptr) == kOfxStatOK);
  CHECK(gOutstandingImages == 0);
  CHECK(gMultiThreadCalls > 0);
  CHECK(gSetValueOutsideAllowed == 0);

  if (gFailures) {
    std::printf("%d check(s) FAILED\n", gFailures);
    return 1;
  }
  std::printf("mock host: all checks passed\n");
  return 0;
}
