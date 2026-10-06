#include "mock_host.h"

#include <dlfcn.h>

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstring>
#include <thread>

#include "ofxMemory.h"
#include "ofxMessage.h"
#include "ofxMultiThread.h"
#include "ofxProperty.h"
#include "wmp/json.h"

namespace mock {

namespace {
Host* gHost = nullptr;
thread_local bool tSpawned = false;
thread_local unsigned int tIndex = 0;

// Per-render state lives with the calling thread, so concurrent renders of
// the same instance never see each other's images.
struct RenderCtx {
    Effect* effect = nullptr;
    std::function<std::unique_ptr<Image>(double, const OfxRectD*)> source;
    Image* output = nullptr;
};
thread_local RenderCtx* tRender = nullptr;

std::mutex gImageMutex;
struct ImageEntry {
    Image* img;
    bool owned;
    std::function<void()> onRelease;
};
std::map<PropertySet*, ImageEntry> gImages;

PropertySet* ps(OfxPropertySetHandle h) { return reinterpret_cast<PropertySet*>(h); }
OfxPropertySetHandle h(PropertySet* p) { return reinterpret_cast<OfxPropertySetHandle>(p); }
Param* prm(OfxParamHandle h) { return reinterpret_cast<Param*>(h); }
ParamSet* pset(OfxParamSetHandle h) { return reinterpret_cast<ParamSet*>(h); }
Effect* eff(OfxImageEffectHandle h) { return reinterpret_cast<Effect*>(h); }
Clip* clp(OfxImageClipHandle h) { return reinterpret_cast<Clip*>(h); }
}  // namespace

// ---------------------------------------------------------------------------
// PropertySet
// ---------------------------------------------------------------------------

int Property::size() const {
    switch (type) {
        case T::Ptr: return static_cast<int>(p.size());
        case T::Str: return static_cast<int>(s.size());
        case T::Dbl: return static_cast<int>(d.size());
        case T::Int: return static_cast<int>(i.size());
    }
    return 0;
}

namespace {
template <typename V>
void putAt(std::vector<V>& vec, int idx, const V& v) {
    if (static_cast<int>(vec.size()) <= idx) vec.resize(static_cast<std::size_t>(idx) + 1);
    vec[static_cast<std::size_t>(idx)] = v;
}
Property& slot(PropertySet& s, const std::string& n, Property::T t) {
    Property& p = s.props[n];
    if (p.size() == 0) p.type = t;
    return p;
}
}  // namespace

void PropertySet::setString(const std::string& n, const std::string& v, int idx) { putAt(slot(*this, n, Property::T::Str).s, idx, v); }
void PropertySet::setDouble(const std::string& n, double v, int idx) { putAt(slot(*this, n, Property::T::Dbl).d, idx, v); }
void PropertySet::setInt(const std::string& n, int v, int idx) { putAt(slot(*this, n, Property::T::Int).i, idx, v); }
void PropertySet::setPointer(const std::string& n, void* v, int idx) { putAt(slot(*this, n, Property::T::Ptr).p, idx, v); }
std::string PropertySet::getString(const std::string& n, int idx) const {
    auto it = props.find(n);
    if (it == props.end() || it->second.type != Property::T::Str || idx >= it->second.size()) return {};
    return it->second.s[static_cast<std::size_t>(idx)];
}
double PropertySet::getDouble(const std::string& n, int idx) const {
    auto it = props.find(n);
    if (it == props.end() || it->second.type != Property::T::Dbl || idx >= it->second.size()) return 0.0;
    return it->second.d[static_cast<std::size_t>(idx)];
}
int PropertySet::getInt(const std::string& n, int idx) const {
    auto it = props.find(n);
    if (it == props.end() || it->second.type != Property::T::Int || idx >= it->second.size()) return 0;
    return it->second.i[static_cast<std::size_t>(idx)];
}
void* PropertySet::getPointer(const std::string& n, int idx) const {
    auto it = props.find(n);
    if (it == props.end() || it->second.type != Property::T::Ptr || idx >= it->second.size()) return nullptr;
    return it->second.p[static_cast<std::size_t>(idx)];
}
int PropertySet::dimension(const std::string& n) const {
    auto it = props.find(n);
    return it == props.end() ? 0 : it->second.size();
}

// ---------------------------------------------------------------------------
// Property suite
// ---------------------------------------------------------------------------

namespace {

template <typename V>
OfxStatus getAt(OfxPropertySetHandle hnd, const char* name, int idx, Property::T t, const std::vector<V> Property::*m,
                V* out) {
    if (!hnd) return kOfxStatErrBadHandle;
    PropertySet* s = ps(hnd);
    auto it = s->props.find(name);
    if (it == s->props.end()) return kOfxStatErrUnknown;
    if (it->second.type != t) return kOfxStatErrValue;
    const auto& vec = it->second.*m;
    if (idx < 0 || idx >= static_cast<int>(vec.size())) return kOfxStatErrBadIndex;
    *out = vec[static_cast<std::size_t>(idx)];
    return kOfxStatOK;
}

OfxStatus propSetPointer(OfxPropertySetHandle p, const char* n, int i, void* v) { ps(p)->setPointer(n, v, i); return kOfxStatOK; }
OfxStatus propSetString(OfxPropertySetHandle p, const char* n, int i, const char* v) { ps(p)->setString(n, v ? v : "", i); return kOfxStatOK; }
OfxStatus propSetDouble(OfxPropertySetHandle p, const char* n, int i, double v) { ps(p)->setDouble(n, v, i); return kOfxStatOK; }
OfxStatus propSetInt(OfxPropertySetHandle p, const char* n, int i, int v) { ps(p)->setInt(n, v, i); return kOfxStatOK; }
OfxStatus propSetPointerN(OfxPropertySetHandle p, const char* n, int c, void* const* v) { for (int i = 0; i < c; ++i) ps(p)->setPointer(n, v[i], i); return kOfxStatOK; }
OfxStatus propSetStringN(OfxPropertySetHandle p, const char* n, int c, const char* const* v) { for (int i = 0; i < c; ++i) ps(p)->setString(n, v[i], i); return kOfxStatOK; }
OfxStatus propSetDoubleN(OfxPropertySetHandle p, const char* n, int c, const double* v) { for (int i = 0; i < c; ++i) ps(p)->setDouble(n, v[i], i); return kOfxStatOK; }
OfxStatus propSetIntN(OfxPropertySetHandle p, const char* n, int c, const int* v) { for (int i = 0; i < c; ++i) ps(p)->setInt(n, v[i], i); return kOfxStatOK; }

OfxStatus propGetPointer(OfxPropertySetHandle p, const char* n, int i, void** v) { return getAt(p, n, i, Property::T::Ptr, &Property::p, v); }
OfxStatus propGetString(OfxPropertySetHandle p, const char* n, int i, char** v) {
    if (!p) return kOfxStatErrBadHandle;
    auto it = ps(p)->props.find(n);
    if (it == ps(p)->props.end()) return kOfxStatErrUnknown;
    if (it->second.type != Property::T::Str) return kOfxStatErrValue;
    if (i < 0 || i >= it->second.size()) return kOfxStatErrBadIndex;
    *v = const_cast<char*>(it->second.s[static_cast<std::size_t>(i)].c_str());
    return kOfxStatOK;
}
OfxStatus propGetDouble(OfxPropertySetHandle p, const char* n, int i, double* v) { return getAt(p, n, i, Property::T::Dbl, &Property::d, v); }
OfxStatus propGetInt(OfxPropertySetHandle p, const char* n, int i, int* v) { return getAt(p, n, i, Property::T::Int, &Property::i, v); }
OfxStatus propGetPointerN(OfxPropertySetHandle p, const char* n, int c, void** v) { for (int i = 0; i < c; ++i) { OfxStatus s = propGetPointer(p, n, i, &v[i]); if (s != kOfxStatOK) return s; } return kOfxStatOK; }
OfxStatus propGetStringN(OfxPropertySetHandle p, const char* n, int c, char** v) { for (int i = 0; i < c; ++i) { OfxStatus s = propGetString(p, n, i, &v[i]); if (s != kOfxStatOK) return s; } return kOfxStatOK; }
OfxStatus propGetDoubleN(OfxPropertySetHandle p, const char* n, int c, double* v) { for (int i = 0; i < c; ++i) { OfxStatus s = propGetDouble(p, n, i, &v[i]); if (s != kOfxStatOK) return s; } return kOfxStatOK; }
OfxStatus propGetIntN(OfxPropertySetHandle p, const char* n, int c, int* v) { for (int i = 0; i < c; ++i) { OfxStatus s = propGetInt(p, n, i, &v[i]); if (s != kOfxStatOK) return s; } return kOfxStatOK; }
OfxStatus propReset(OfxPropertySetHandle p, const char* n) { ps(p)->props.erase(n); return kOfxStatOK; }
OfxStatus propGetDimension(OfxPropertySetHandle p, const char* n, int* c) {
    if (!ps(p)->has(n)) return kOfxStatErrUnknown;
    *c = ps(p)->dimension(n);
    return kOfxStatOK;
}

OfxPropertySuiteV1 makePropertySuite() {
    OfxPropertySuiteV1 s{};
    s.propSetPointer = propSetPointer; s.propSetString = propSetString; s.propSetDouble = propSetDouble; s.propSetInt = propSetInt;
    s.propSetPointerN = propSetPointerN; s.propSetStringN = propSetStringN; s.propSetDoubleN = propSetDoubleN; s.propSetIntN = propSetIntN;
    s.propGetPointer = propGetPointer; s.propGetString = propGetString; s.propGetDouble = propGetDouble; s.propGetInt = propGetInt;
    s.propGetPointerN = propGetPointerN; s.propGetStringN = propGetStringN; s.propGetDoubleN = propGetDoubleN; s.propGetIntN = propGetIntN;
    s.propReset = propReset; s.propGetDimension = propGetDimension;
    return s;
}

}  // namespace

// ---------------------------------------------------------------------------
// Parameters
// ---------------------------------------------------------------------------

bool Param::isNumeric() const {
    return type == kOfxParamTypeDouble || type == kOfxParamTypeDouble2D || type == kOfxParamTypeInteger ||
           type == kOfxParamTypeBoolean || type == kOfxParamTypeChoice;
}
bool Param::isString() const { return type == kOfxParamTypeString; }

std::vector<double> Param::valueAt(double t) const {
    if (keys.empty()) return value;
    if (t <= keys.front().time) return keys.front().v;
    if (t >= keys.back().time) return keys.back().v;
    for (std::size_t k = 0; k + 1 < keys.size(); ++k) {
        const Key& a = keys[k];
        const Key& b = keys[k + 1];
        if (t >= a.time && t <= b.time) {
            const bool stepped = type != kOfxParamTypeDouble && type != kOfxParamTypeDouble2D;
            if (stepped) return (t < b.time) ? a.v : b.v;
            const double f = (b.time > a.time) ? (t - a.time) / (b.time - a.time) : 0.0;
            std::vector<double> out(a.v.size());
            for (std::size_t i = 0; i < out.size(); ++i) out[i] = a.v[i] + f * (b.v[i] - a.v[i]);
            return out;
        }
    }
    return keys.back().v;
}

Param* ParamSet::find(const std::string& name) const {
    for (const auto& p : params)
        if (p->name == name) return p.get();
    return nullptr;
}

namespace {

int dimsOf(const std::string& type) { return type == kOfxParamTypeDouble2D ? 2 : 1; }

void fireChanged(Effect* e, const std::string& name, const char* reason, double time);

OfxStatus paramDefine(OfxParamSetHandle psh, const char* type, const char* name, OfxPropertySetHandle* props) {
    ParamSet* s = pset(psh);
    if (s->find(name)) return kOfxStatErrExists;
    auto p = std::make_unique<Param>();
    p->name = name;
    p->type = type;
    p->dims = dimsOf(type);
    p->props.setString(kOfxParamPropType, type);
    p->props.setString(kOfxPropName, name);
    if (props) *props = h(&p->props);
    s->params.push_back(std::move(p));
    return kOfxStatOK;
}

OfxStatus paramGetHandle(OfxParamSetHandle psh, const char* name, OfxParamHandle* param, OfxPropertySetHandle* props) {
    Param* p = pset(psh)->find(name);
    if (!p) return kOfxStatErrUnknown;
    if (param) *param = reinterpret_cast<OfxParamHandle>(p);
    if (props) *props = h(&p->props);
    return kOfxStatOK;
}

OfxStatus paramSetGetPropertySet(OfxParamSetHandle psh, OfxPropertySetHandle* props) {
    *props = h(&pset(psh)->props);
    return kOfxStatOK;
}

OfxStatus paramGetPropertySet(OfxParamHandle p, OfxPropertySetHandle* props) {
    *props = h(&prm(p)->props);
    return kOfxStatOK;
}

OfxStatus getValueImpl(Param* p, double t, va_list ap) {
    if (p->isString()) {
        // Emulate a host-owned temporary buffer: the pointer is only valid
        // until the next call. Flag overlapping calls on the same parameter.
        const int inFlight = ++p->stringGetsInFlight;
        if (inFlight > 1) ++p->concurrentStringGets;
        p->tempBuffer = p->str;
        char** out = va_arg(ap, char**);
        *out = const_cast<char*>(p->tempBuffer.c_str());
        --p->stringGetsInFlight;
        return kOfxStatOK;
    }
    if (!p->isNumeric()) return kOfxStatErrBadHandle;
    const std::vector<double> v = p->valueAt(t);
    if (p->type == kOfxParamTypeDouble) {
        *va_arg(ap, double*) = v[0];
    } else if (p->type == kOfxParamTypeDouble2D) {
        *va_arg(ap, double*) = v[0];
        *va_arg(ap, double*) = v[1];
    } else {
        *va_arg(ap, int*) = static_cast<int>(std::lround(v[0]));
    }
    return kOfxStatOK;
}

OfxStatus paramGetValue(OfxParamHandle ph, ...) {
    va_list ap;
    va_start(ap, ph);
    const OfxStatus s = getValueImpl(prm(ph), 0.0, ap);
    va_end(ap);
    return s;
}

OfxStatus paramGetValueAtTime(OfxParamHandle ph, OfxTime t, ...) {
    va_list ap;
    va_start(ap, t);
    const OfxStatus s = getValueImpl(prm(ph), t, ap);
    va_end(ap);
    return s;
}

OfxStatus paramUnsupported(OfxParamHandle, OfxTime, ...) { return kOfxStatErrUnsupported; }
OfxStatus paramIntegralUnsupported(OfxParamHandle, OfxTime, OfxTime, ...) { return kOfxStatErrUnsupported; }

Effect* ownerOf(Param* p) {
    // Find the effect owning this parameter (instances only).
    return static_cast<Effect*>(p->props.getPointer("mock.owner"));
}

OfxStatus setValueImpl(Param* p, va_list ap) {
    if (p->isString()) {
        const char* v = va_arg(ap, const char*);
        p->str = v ? v : "";
        p->tempBuffer = "<overwritten by host>";  // any old pointer now shows garbage
    } else if (p->isNumeric()) {
        if (!p->keys.empty()) return kOfxStatErrValue;  // mock: plug-in must not set animated params
        if (p->type == kOfxParamTypeDouble) p->value = {va_arg(ap, double)};
        else if (p->type == kOfxParamTypeDouble2D) {
            const double x = va_arg(ap, double);
            const double y = va_arg(ap, double);
            p->value = {x, y};
        } else {
            p->value = {static_cast<double>(va_arg(ap, int))};
        }
    } else {
        return kOfxStatErrBadHandle;
    }
    Effect* e = ownerOf(p);
    const bool persistent = !p->props.has(kOfxParamPropPersistant) || p->props.getInt(kOfxParamPropPersistant) != 0;
    if (e && persistent && e->params.editDepth == 0) ++e->params.setsOutsideEdit;
    if (e) fireChanged(e, p->name, kOfxChangePluginEdited, 0.0);  // synchronous, like some real hosts
    return kOfxStatOK;
}

OfxStatus paramSetValue(OfxParamHandle ph, ...) {
    va_list ap;
    va_start(ap, ph);
    const OfxStatus s = setValueImpl(prm(ph), ap);
    va_end(ap);
    return s;
}

OfxStatus paramSetValueAtTime(OfxParamHandle, OfxTime, ...) { return kOfxStatErrUnsupported; }

OfxStatus paramGetNumKeys(OfxParamHandle ph, unsigned int* n) {
    *n = static_cast<unsigned int>(prm(ph)->keys.size());
    return kOfxStatOK;
}
OfxStatus paramGetKeyTime(OfxParamHandle ph, unsigned int k, OfxTime* t) {
    if (k >= prm(ph)->keys.size()) return kOfxStatErrBadIndex;
    *t = prm(ph)->keys[k].time;
    return kOfxStatOK;
}
OfxStatus paramGetKeyIndex(OfxParamHandle, OfxTime, int, int*) { return kOfxStatFailed; }
OfxStatus paramDeleteKey(OfxParamHandle, OfxTime) { return kOfxStatErrUnsupported; }
OfxStatus paramDeleteAllKeys(OfxParamHandle) { return kOfxStatErrUnsupported; }
OfxStatus paramCopy(OfxParamHandle, OfxParamHandle, OfxTime, const OfxRangeD*) { return kOfxStatErrUnsupported; }
OfxStatus paramEditBegin(OfxParamSetHandle psh, const char* name) {
    ParamSet* s = pset(psh);
    if (s->editDepth++ == 0) s->editLabels.push_back(name ? name : "");
    return kOfxStatOK;
}
OfxStatus paramEditEnd(OfxParamSetHandle psh) {
    ParamSet* s = pset(psh);
    if (s->editDepth > 0) --s->editDepth;
    return kOfxStatOK;
}

OfxParameterSuiteV1 makeParameterSuite() {
    OfxParameterSuiteV1 s{};
    s.paramDefine = paramDefine;
    s.paramGetHandle = paramGetHandle;
    s.paramSetGetPropertySet = paramSetGetPropertySet;
    s.paramGetPropertySet = paramGetPropertySet;
    s.paramGetValue = paramGetValue;
    s.paramGetValueAtTime = paramGetValueAtTime;
    s.paramGetDerivative = paramUnsupported;
    s.paramGetIntegral = paramIntegralUnsupported;
    s.paramSetValue = paramSetValue;
    s.paramSetValueAtTime = paramSetValueAtTime;
    s.paramGetNumKeys = paramGetNumKeys;
    s.paramGetKeyTime = paramGetKeyTime;
    s.paramGetKeyIndex = paramGetKeyIndex;
    s.paramDeleteKey = paramDeleteKey;
    s.paramDeleteAllKeys = paramDeleteAllKeys;
    s.paramCopy = paramCopy;
    s.paramEditBegin = paramEditBegin;
    s.paramEditEnd = paramEditEnd;
    return s;
}

// ---------------------------------------------------------------------------
// Image effect suite
// ---------------------------------------------------------------------------

OfxStatus getPropertySet(OfxImageEffectHandle e, OfxPropertySetHandle* p) {
    if (!e) return kOfxStatErrBadHandle;
    *p = h(&eff(e)->props);
    return kOfxStatOK;
}
OfxStatus getParamSet(OfxImageEffectHandle e, OfxParamSetHandle* p) {
    *p = reinterpret_cast<OfxParamSetHandle>(&eff(e)->params);
    return kOfxStatOK;
}
OfxStatus clipDefine(OfxImageEffectHandle e, const char* name, OfxPropertySetHandle* p) {
    auto c = std::make_unique<Clip>();
    c->name = name;
    c->owner = eff(e);
    c->props.setString(kOfxPropName, name);
    if (p) *p = h(&c->props);
    eff(e)->clips[name] = std::move(c);
    return kOfxStatOK;
}
OfxStatus clipGetHandle(OfxImageEffectHandle e, const char* name, OfxImageClipHandle* clip, OfxPropertySetHandle* p) {
    auto it = eff(e)->clips.find(name);
    if (it == eff(e)->clips.end()) return kOfxStatErrUnknown;
    *clip = reinterpret_cast<OfxImageClipHandle>(it->second.get());
    if (p) *p = h(&it->second->props);
    return kOfxStatOK;
}
OfxStatus clipGetPropertySet(OfxImageClipHandle c, OfxPropertySetHandle* p) {
    *p = h(&clp(c)->props);
    return kOfxStatOK;
}
OfxStatus clipGetImage(OfxImageClipHandle c, OfxTime t, const OfxRectD* region, OfxPropertySetHandle* out) {
    Clip* clip = clp(c);
    Effect* e = clip->owner;
    if (!tRender || tRender->effect != e) return kOfxStatFailed;  // images only inside a render
    std::unique_ptr<Image> img;
    if (clip->name == kOfxImageEffectOutputClipName) {
        // Output images are owned by the render call: register as non-owned.
        Image* raw = tRender->output;
        std::lock_guard<std::mutex> lock(gImageMutex);
        if (gImages.count(&raw->props)) return kOfxStatErrExists;  // fetched twice without release
        gImages[&raw->props] = {raw, false, {}};
        ++Host::current()->liveImages;
        *out = h(&raw->props);
        return kOfxStatOK;
    }
    if (region) {
        std::lock_guard<std::mutex> lock(e->regionMutex);
        e->sourceRegionsRequested.push_back(*region);
    }
    img = tRender->source(t, region);
    if (!img) return kOfxStatFailed;
    Image* raw = img.release();
    std::lock_guard<std::mutex> lock(gImageMutex);
    gImages[&raw->props] = {raw, true, {}};
    ++Host::current()->liveImages;
    *out = h(&raw->props);
    return kOfxStatOK;
}
OfxStatus clipReleaseImage(OfxPropertySetHandle p) {
    std::lock_guard<std::mutex> lock(gImageMutex);
    auto it = gImages.find(ps(p));
    if (it == gImages.end()) return kOfxStatErrBadHandle;
    if (it->second.owned) delete it->second.img;
    gImages.erase(it);
    --Host::current()->liveImages;
    return kOfxStatOK;
}
OfxStatus clipGetRegionOfDefinition(OfxImageClipHandle c, OfxTime, OfxRectD* r) {
    Effect* e = clp(c)->owner;
    double size[2] = {e->props.getDouble(kOfxImageEffectPropProjectSize, 0),
                      e->props.getDouble(kOfxImageEffectPropProjectSize, 1)};
    *r = {0, 0, size[0], size[1]};
    return kOfxStatOK;
}
int effectAbort(OfxImageEffectHandle e) { return eff(e)->abortFlag.load() ? 1 : 0; }
OfxStatus imageMemoryAlloc(OfxImageEffectHandle, size_t n, OfxImageMemoryHandle* m) {
    *m = reinterpret_cast<OfxImageMemoryHandle>(new unsigned char[n]);
    return kOfxStatOK;
}
OfxStatus imageMemoryFree(OfxImageMemoryHandle m) {
    delete[] reinterpret_cast<unsigned char*>(m);
    return kOfxStatOK;
}
OfxStatus imageMemoryLock(OfxImageMemoryHandle m, void** p) {
    *p = m;
    return kOfxStatOK;
}
OfxStatus imageMemoryUnlock(OfxImageMemoryHandle) { return kOfxStatOK; }

OfxImageEffectSuiteV1 makeEffectSuite() {
    OfxImageEffectSuiteV1 s{};
    s.getPropertySet = getPropertySet;
    s.getParamSet = getParamSet;
    s.clipDefine = clipDefine;
    s.clipGetHandle = clipGetHandle;
    s.clipGetPropertySet = clipGetPropertySet;
    s.clipGetImage = clipGetImage;
    s.clipReleaseImage = clipReleaseImage;
    s.clipGetRegionOfDefinition = clipGetRegionOfDefinition;
    s.abort = effectAbort;
    s.imageMemoryAlloc = imageMemoryAlloc;
    s.imageMemoryFree = imageMemoryFree;
    s.imageMemoryLock = imageMemoryLock;
    s.imageMemoryUnlock = imageMemoryUnlock;
    return s;
}

// ---------------------------------------------------------------------------
// Memory, multithread, message, interact, draw
// ---------------------------------------------------------------------------

OfxStatus memoryAlloc(void*, size_t n, void** p) {
    *p = ::operator new(n);
    return kOfxStatOK;
}
OfxStatus memoryFree(void* p) {
    ::operator delete(p);
    return kOfxStatOK;
}

OfxStatus multiThread(OfxThreadFunctionV1 func, unsigned int n, void* arg) {
    if (tSpawned) return kOfxStatErrExists;
    if (n == 0) n = 4;
    Host* host = Host::current();
    std::vector<std::thread> threads;
    for (unsigned int i = 0; i < n; ++i) {
        threads.emplace_back([=] {
            tSpawned = true;
            tIndex = i;
            const int live = ++host->liveThreads;
            int prev = host->maxThreads.load();
            while (live > prev && !host->maxThreads.compare_exchange_weak(prev, live)) {
            }
            func(i, n, arg);
            --host->liveThreads;
        });
    }
    for (auto& t : threads) t.join();
    return kOfxStatOK;
}
OfxStatus multiThreadNumCPUs(unsigned int* n) {
    *n = 4;
    return kOfxStatOK;
}
OfxStatus multiThreadIndex(unsigned int* i) {
    *i = tIndex;
    return kOfxStatOK;
}
int multiThreadIsSpawnedThread() { return tSpawned ? 1 : 0; }
OfxStatus mutexCreate(OfxMutexHandle* m, int) {
    *m = reinterpret_cast<OfxMutexHandle>(new std::recursive_mutex());
    return kOfxStatOK;
}
OfxStatus mutexDestroy(const OfxMutexHandle m) {
    delete reinterpret_cast<std::recursive_mutex*>(m);
    return kOfxStatOK;
}
OfxStatus mutexLock(const OfxMutexHandle m) {
    reinterpret_cast<std::recursive_mutex*>(m)->lock();
    return kOfxStatOK;
}
OfxStatus mutexUnLock(const OfxMutexHandle m) {
    reinterpret_cast<std::recursive_mutex*>(m)->unlock();
    return kOfxStatOK;
}
OfxStatus mutexTryLock(const OfxMutexHandle m) {
    return reinterpret_cast<std::recursive_mutex*>(m)->try_lock() ? kOfxStatOK : kOfxStatFailed;
}

OfxStatus message(void*, const char* type, const char*, const char* format, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, format);
    std::vsnprintf(buf, sizeof buf, format, ap);
    va_end(ap);
    Host::current()->messages.push_back(std::string(type ? type : "") + ": " + buf);
    return kOfxStatOK;
}

OfxStatus interactSwapBuffers(OfxInteractHandle) { return kOfxStatOK; }
OfxStatus interactRedraw(OfxInteractHandle i) {
    ++reinterpret_cast<Interact*>(i)->redraws;
    return kOfxStatOK;
}
OfxStatus interactGetPropertySet(OfxInteractHandle i, OfxPropertySetHandle* p) {
    *p = h(&reinterpret_cast<Interact*>(i)->props);
    return kOfxStatOK;
}

OfxRGBAColourF gColour{1, 1, 1, 1};
OfxStatus drawGetColour(OfxDrawContextHandle, OfxStandardColour, OfxRGBAColourF* c) {
    *c = {1, 1, 1, 1};
    return kOfxStatOK;
}
OfxStatus drawSetColour(OfxDrawContextHandle, const OfxRGBAColourF* c) {
    gColour = *c;
    return kOfxStatOK;
}
OfxStatus drawSetLineWidth(OfxDrawContextHandle, float) { return kOfxStatOK; }
OfxStatus drawSetLineStipple(OfxDrawContextHandle, OfxDrawLineStipplePattern) { return kOfxStatOK; }
OfxStatus drawDraw(OfxDrawContextHandle, OfxDrawPrimitive prim, const OfxPointD* pts, int n) {
    for (int i = 0; i < n; ++i)
        if (!std::isfinite(pts[i].x) || !std::isfinite(pts[i].y)) return kOfxStatErrValue;
    Host::current()->drawCalls.push_back({prim, n, gColour});
    return kOfxStatOK;
}
OfxStatus drawText(OfxDrawContextHandle, const char*, const OfxPointD*, int) { return kOfxStatOK; }

const void* fetchSuite(OfxPropertySetHandle, const char* name, int version) {
    static const OfxPropertySuiteV1 propSuite = makePropertySuite();
    static const OfxParameterSuiteV1 paramSuite = makeParameterSuite();
    static const OfxImageEffectSuiteV1 effectSuite = makeEffectSuite();
    static const OfxMemorySuiteV1 memSuite{memoryAlloc, memoryFree};
    static const OfxMultiThreadSuiteV1 mtSuite{multiThread, multiThreadNumCPUs, multiThreadIndex, multiThreadIsSpawnedThread,
                                               mutexCreate,  mutexDestroy,       mutexLock,        mutexUnLock,
                                               mutexTryLock};
    static OfxMessageSuiteV1 msgSuite{};
    msgSuite.message = message;
    static const OfxInteractSuiteV1 interactSuite{interactSwapBuffers, interactRedraw, interactGetPropertySet};
    static const OfxDrawSuiteV1 drawSuite{drawGetColour, drawSetColour, drawSetLineWidth, drawSetLineStipple, drawDraw,
                                          drawText};
    const std::string n = name;
    if (version != 1) return nullptr;
    if (n == kOfxPropertySuite) return &propSuite;
    if (n == kOfxParameterSuite) return &paramSuite;
    if (n == kOfxImageEffectSuite) return &effectSuite;
    if (n == kOfxMemorySuite) return &memSuite;
    if (n == kOfxMultiThreadSuite) return Host::current()->provideMultiThread ? &mtSuite : nullptr;
    if (n == kOfxMessageSuite) return &msgSuite;
    if (n == kOfxInteractSuite) return &interactSuite;
    if (n == kOfxDrawSuite) return Host::current()->provideDrawSuite ? &drawSuite : nullptr;
    return nullptr;
}

void fireChanged(Effect* e, const std::string& name, const char* reason, double time) {
    if (e->descriptor || !e->host) return;
    PropertySet in;
    in.setString(kOfxPropType, kOfxTypeParameter);
    in.setString(kOfxPropName, name);
    in.setString(kOfxPropChangeReason, reason);
    in.setDouble(kOfxPropTime, time);
    in.setDouble(kOfxImageEffectPropRenderScale, 1.0, 0);
    in.setDouble(kOfxImageEffectPropRenderScale, 1.0, 1);
    e->host->action(kOfxActionInstanceChanged, e, &in, nullptr);
}

}  // namespace

// ---------------------------------------------------------------------------
// Host
// ---------------------------------------------------------------------------

Host* Host::current() { return gHost; }

Host::Host() { gHost = this; }

Host::~Host() {
    unload();
    if (gHost == this) gHost = nullptr;
}

bool Host::load(const std::string& path, std::string& error) {
    lib_ = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!lib_) {
        error = dlerror();
        return false;
    }
    using CountFn = int (*)();
    using GetFn = OfxPlugin* (*)(int);
    auto count = reinterpret_cast<CountFn>(dlsym(lib_, "OfxGetNumberOfPlugins"));
    auto get = reinterpret_cast<GetFn>(dlsym(lib_, "OfxGetPlugin"));
    if (!count || !get || count() < 1) {
        error = "missing OFX entry points";
        return false;
    }
    plugin_ = get(0);
    hostProps_.setString(kOfxPropName, hostName);
    hostProps_.setString(kOfxPropLabel, "Wron mock OFX host");
    hostProps_.setString(kOfxPropVersionLabel, "test");
    hostProps_.setInt(kOfxImageEffectHostPropIsBackground, 0);
    hostProps_.setInt(kOfxImageEffectPropSupportsOverlays, supportsOverlays ? 1 : 0);
    hostProps_.setInt(kOfxImageEffectPropSupportsMultiResolution, 1);
    hostProps_.setInt(kOfxImageEffectPropSupportsTiles, 1);
    hostProps_.setInt(kOfxImageEffectPropTemporalClipAccess, 0);
    hostProps_.setString(kOfxImageEffectPropSupportedContexts, kOfxImageEffectContextFilter);
    ofxHost_.host = h(&hostProps_);
    ofxHost_.fetchSuite = fetchSuite;
    plugin_->setHost(&ofxHost_);
    if (action(kOfxActionLoad, nullptr, nullptr, nullptr) != kOfxStatOK) {
        error = "load action failed";
        return false;
    }
    descriptor_ = std::make_unique<Effect>();
    descriptor_->host = this;
    if (action(kOfxActionDescribe, descriptor_.get(), nullptr, nullptr) != kOfxStatOK) {
        error = "describe failed";
        return false;
    }
    context_ = std::make_unique<Effect>();
    context_->host = this;
    context_->props = descriptor_->props;
    PropertySet in;
    in.setString(kOfxImageEffectPropContext, kOfxImageEffectContextFilter);
    if (action(kOfxImageEffectActionDescribeInContext, context_.get(), &in, nullptr) != kOfxStatOK) {
        error = "describe in context failed";
        return false;
    }
    return true;
}

void Host::unload() {
    if (plugin_) action(kOfxActionUnload, nullptr, nullptr, nullptr);
    plugin_ = nullptr;
    descriptor_.reset();
    context_.reset();
    if (lib_) dlclose(lib_);
    lib_ = nullptr;
}

OfxStatus Host::action(const char* name, void* handle, PropertySet* in, PropertySet* out) {
    return plugin_->mainEntry(name, handle, in ? h(in) : nullptr, out ? h(out) : nullptr);
}

std::unique_ptr<Effect> Host::createInstance(int width, int height, double par, double fps, double frameStart,
                                             double frameEnd) {
    auto e = std::make_unique<Effect>();
    e->descriptor = false;
    e->host = this;
    e->props = context_->props;
    e->props.setString(kOfxImageEffectPropContext, kOfxImageEffectContextFilter);
    e->props.setInt(kOfxPropIsInteractive, 1);
    e->props.setDouble(kOfxImageEffectPropProjectSize, width * par, 0);
    e->props.setDouble(kOfxImageEffectPropProjectSize, height, 1);
    e->props.setDouble(kOfxImageEffectPropProjectOffset, 0, 0);
    e->props.setDouble(kOfxImageEffectPropProjectOffset, 0, 1);
    e->props.setDouble(kOfxImageEffectPropProjectExtent, width * par, 0);
    e->props.setDouble(kOfxImageEffectPropProjectExtent, height, 1);
    e->props.setDouble(kOfxImageEffectPropProjectPixelAspectRatio, par);
    e->props.setDouble(kOfxImageEffectPropFrameRate, fps);
    e->props.setDouble(kOfxImageEffectPropFrameRange, frameStart, 0);
    e->props.setDouble(kOfxImageEffectPropFrameRange, frameEnd, 1);
    e->props.setPointer(kOfxPropInstanceData, nullptr);
    for (const auto& c : context_->clips) {
        auto clip = std::make_unique<Clip>(*c.second);
        clip->owner = e.get();
        clip->props.setString(kOfxImageEffectPropComponents, kOfxImageComponentRGBA);
        clip->props.setString(kOfxImageEffectPropPixelDepth, kOfxBitDepthFloat);
        clip->props.setString(kOfxImageEffectPropPreMultiplication, kOfxImagePreMultiplied);
        clip->props.setDouble(kOfxImagePropPixelAspectRatio, par);
        clip->props.setInt(kOfxImageClipPropConnected, 1);
        clip->props.setDouble(kOfxImageEffectPropFrameRate, fps);
        e->clips[c.first] = std::move(clip);
    }
    for (const auto& dp : context_->params.params) {
        auto p = std::make_unique<Param>();
        p->name = dp->name;
        p->type = dp->type;
        p->dims = dp->dims;
        p->props = dp->props;
        p->props.setPointer("mock.owner", e.get());
        if (p->isString()) {
            p->str = dp->props.getString(kOfxParamPropDefault);
        } else if (p->type == kOfxParamTypeDouble || p->type == kOfxParamTypeDouble2D) {
            for (int i = 0; i < p->dims; ++i) p->value.push_back(dp->props.getDouble(kOfxParamPropDefault, i));
        } else if (p->isNumeric()) {
            p->value = {static_cast<double>(dp->props.getInt(kOfxParamPropDefault))};
        }
        e->params.params.push_back(std::move(p));
    }
    if (action(kOfxActionCreateInstance, e.get(), nullptr, nullptr) != kOfxStatOK) return nullptr;
    return e;
}

void Host::destroyInstance(std::unique_ptr<Effect>& e) {
    if (!e) return;
    action(kOfxActionDestroyInstance, e.get(), nullptr, nullptr);
    e.reset();
}

namespace {

void writePixel(Image& img, int x, int y, const float premult[4]) {
    float c[4] = {premult[0], premult[1], premult[2], premult[3]};
    if (img.premult == kOfxImageUnPreMultiplied && c[3] > 0.0f) {
        c[0] /= c[3];
        c[1] /= c[3];
        c[2] /= c[3];
    }
    if (img.premult == kOfxImageOpaque) c[3] = 1.0f;
    if (img.bgra) std::swap(c[0], c[2]);
    const std::size_t idx = (static_cast<std::size_t>(y - img.y1) * (img.x2 - img.x1) + (x - img.x1)) * 4;
    if (img.depth == kOfxBitDepthFloat) {
        float* p = reinterpret_cast<float*>(img.data.data()) + idx;
        for (int i = 0; i < 4; ++i) p[i] = c[i];
    } else if (img.depth == kOfxBitDepthShort) {
        std::uint16_t* p = reinterpret_cast<std::uint16_t*>(img.data.data()) + idx;
        for (int i = 0; i < 4; ++i) p[i] = static_cast<std::uint16_t>(std::clamp(c[i], 0.0f, 1.0f) * 65535.0f + 0.5f);
    } else {
        unsigned char* p = img.data.data() + idx;
        for (int i = 0; i < 4; ++i) p[i] = static_cast<unsigned char>(std::clamp(c[i], 0.0f, 1.0f) * 255.0f + 0.5f);
    }
}

void fillProps(Image& img, double rs, double par, double time) {
    img.props.setPointer(kOfxImagePropData, img.data.data());
    img.props.setInt(kOfxImagePropRowBytes, (img.x2 - img.x1) * img.bpp);
    const int b[4] = {img.x1, img.y1, img.x2, img.y2};
    for (int i = 0; i < 4; ++i) {
        img.props.setInt(kOfxImagePropBounds, b[i], i);
        img.props.setInt(kOfxImagePropRegionOfDefinition, b[i], i);
    }
    img.props.setString(kOfxImageEffectPropPixelDepth, img.depth);
    img.props.setString(kOfxImageEffectPropComponents, kOfxImageComponentRGBA);
    img.props.setString(kOfxImageEffectPropPreMultiplication, img.premult);
    img.props.setDouble(kOfxImagePropPixelAspectRatio, par);
    img.props.setDouble(kOfxImageEffectPropRenderScale, rs, 0);
    img.props.setDouble(kOfxImageEffectPropRenderScale, rs, 1);
    img.props.setString(kOfxImagePropField, kOfxImageFieldNone);
    img.props.setString(kOfxImagePropUniqueIdentifier, std::to_string(time));
    if (img.bgra) img.props.setString("OfxImageEffectPropPixelOrder", "OfxImagePixelOrderBGRA");
}

}  // namespace

void readPremult(Image& img, int x, int y, float out[4]) {
    const std::size_t idx = (static_cast<std::size_t>(y - img.y1) * (img.x2 - img.x1) + (x - img.x1)) * 4;
    float c[4];
    if (img.depth == kOfxBitDepthFloat) {
        const float* p = reinterpret_cast<const float*>(img.data.data()) + idx;
        for (int i = 0; i < 4; ++i) c[i] = p[i];
    } else if (img.depth == kOfxBitDepthShort) {
        const std::uint16_t* p = reinterpret_cast<const std::uint16_t*>(img.data.data()) + idx;
        for (int i = 0; i < 4; ++i) c[i] = p[i] / 65535.0f;
    } else {
        const unsigned char* p = img.data.data() + idx;
        for (int i = 0; i < 4; ++i) c[i] = p[i] / 255.0f;
    }
    if (img.bgra) std::swap(c[0], c[2]);
    if (img.premult == kOfxImageUnPreMultiplied) {
        c[0] *= c[3];
        c[1] *= c[3];
        c[2] *= c[3];
    }
    for (int i = 0; i < 4; ++i) out[i] = c[i];
}

std::unique_ptr<Image> Host::makeImage(const ImageSpec& spec, double time, const OfxRectD* region, bool source,
                                       double par) {
    auto img = std::make_unique<Image>();
    img->depth = spec.depth;
    img->premult = spec.premult;
    img->bgra = spec.bgra;
    img->bpp = spec.depth == kOfxBitDepthFloat ? 16 : spec.depth == kOfxBitDepthShort ? 8 : 4;
    img->x1 = 0;
    img->y1 = 0;
    img->x2 = static_cast<int>(std::lround(spec.width * spec.renderScale));
    img->y2 = static_cast<int>(std::lround(spec.height * spec.renderScale));
    if (source && region && cropSourceToRequest) {
        // region is canonical; pixel = canonical * rs / PAR (x), * rs (y).
        const double parX = par;
        img->x1 = std::max(img->x1, static_cast<int>(std::floor(region->x1 * spec.renderScale / parX)));
        img->y1 = std::max(img->y1, static_cast<int>(std::floor(region->y1 * spec.renderScale)));
        img->x2 = std::min(img->x2, static_cast<int>(std::ceil(region->x2 * spec.renderScale / parX)));
        img->y2 = std::min(img->y2, static_cast<int>(std::ceil(region->y2 * spec.renderScale)));
        if (img->x2 < img->x1) img->x2 = img->x1;
        if (img->y2 < img->y1) img->y2 = img->y1;
    }
    img->data.assign(static_cast<std::size_t>(img->x2 - img->x1) * (img->y2 - img->y1) * img->bpp, 0);
    if (source && sourcePixel) {
        for (int y = img->y1; y < img->y2; ++y)
            for (int x = img->x1; x < img->x2; ++x) {
                float c[4] = {0, 0, 0, 0};
                sourcePixel((x + 0.5) / spec.renderScale, (y + 0.5) / spec.renderScale, time, c);
                writePixel(*img, x, y, c);
            }
    }
    fillProps(*img, spec.renderScale, par, time);
    return img;
}

OfxStatus Host::render(Effect* e, const RenderRequest& rq, const ImageSpec& spec, Image& out) {
    double par = e->props.getDouble(kOfxImageEffectPropProjectPixelAspectRatio);
    if (!(par > 0.0)) par = 1.0;
    {
        // Clip format follows the requested spec; only written when it changes
        // (concurrent renders of one instance use the same format).
        static std::mutex clipMutex;
        std::lock_guard<std::mutex> lock(clipMutex);
        for (auto& c : e->clips) {
            if (c.second->props.getString(kOfxImageEffectPropPixelDepth) != spec.depth)
                c.second->props.setString(kOfxImageEffectPropPixelDepth, spec.depth);
            if (c.second->props.getString(kOfxImageEffectPropPreMultiplication) != spec.premult)
                c.second->props.setString(kOfxImageEffectPropPreMultiplication, spec.premult);
        }
    }
    // Output buffer: whole frame, filled with a sentinel to catch writes
    // outside the render window.
    out = Image();
    out.depth = spec.depth;
    out.premult = spec.premult;
    out.bgra = spec.bgra;
    out.bpp = spec.depth == kOfxBitDepthFloat ? 16 : spec.depth == kOfxBitDepthShort ? 8 : 4;
    out.x2 = static_cast<int>(std::lround(spec.width * spec.renderScale));
    out.y2 = static_cast<int>(std::lround(spec.height * spec.renderScale));
    out.data.assign(static_cast<std::size_t>(out.x2) * out.y2 * out.bpp, 0x5A);
    fillProps(out, spec.renderScale, par, rq.time);
    RenderCtx ctx;
    ctx.effect = e;
    ctx.output = &out;
    ctx.source = [this, spec, par](double t, const OfxRectD* region) { return makeImage(spec, t, region, true, par); };
    struct Scope {
        RenderCtx* prev;
        explicit Scope(RenderCtx* c) : prev(tRender) { tRender = c; }
        ~Scope() { tRender = prev; }
    } scope(&ctx);
    PropertySet in;
    in.setDouble(kOfxPropTime, rq.time);
    in.setString(kOfxImageEffectPropFieldToRender, rq.field);
    for (int i = 0; i < 4; ++i) in.setInt(kOfxImageEffectPropRenderWindow, rq.window[i], i);
    in.setDouble(kOfxImageEffectPropRenderScale, rq.renderScale, 0);
    in.setDouble(kOfxImageEffectPropRenderScale, rq.renderScale, 1);
    in.setInt(kOfxImageEffectPropSequentialRenderStatus, 0);
    in.setInt(kOfxImageEffectPropInteractiveRenderStatus, 1);
    if (!rq.vegasQuality.empty()) in.setString("OfxImageEffectPropRenderQuality", rq.vegasQuality);
    if (askIdentityFirst) {
        PropertySet idOut;
        idOut.setString(kOfxPropName, "");
        idOut.setDouble(kOfxPropTime, rq.time);
        if (action(kOfxImageEffectActionIsIdentity, e, &in, &idOut) == kOfxStatOK) {
            // Host copies the named clip itself.
            auto src = makeImage(spec, idOut.getDouble(kOfxPropTime), nullptr, true, par);
            for (int y = rq.window[1]; y < rq.window[3]; ++y)
                for (int x = rq.window[0]; x < rq.window[2]; ++x)
                    std::memcpy(out.data.data() + (static_cast<std::size_t>(y) * out.x2 + x) * out.bpp,
                                src->data.data() + (static_cast<std::size_t>(y) * src->x2 + x) * src->bpp,
                                static_cast<std::size_t>(out.bpp));
            lastWasIdentity_ = true;
            return kOfxStatOK;
        }
    }
    lastWasIdentity_ = false;
    return action(kOfxImageEffectActionRender, e, &in, nullptr);
}

void Host::setDouble(Effect* e, const std::string& name, double v) {
    Param* p = e->params.find(name);
    p->keys.clear();
    p->value = {v};
    fireChanged(e, name, kOfxChangeUserEdited, 0.0);
}
void Host::setDouble2(Effect* e, const std::string& name, double x, double y) {
    Param* p = e->params.find(name);
    p->keys.clear();
    p->value = {x, y};
    fireChanged(e, name, kOfxChangeUserEdited, 0.0);
}
void Host::setInt(Effect* e, const std::string& name, int v) {
    Param* p = e->params.find(name);
    p->keys.clear();
    p->value = {static_cast<double>(v)};
    fireChanged(e, name, kOfxChangeUserEdited, 0.0);
}
void Host::setString(Effect* e, const std::string& name, const std::string& v) {
    Param* p = e->params.find(name);
    p->str = v;
    fireChanged(e, name, kOfxChangeUserEdited, 0.0);
}
void Host::setKeys(Effect* e, const std::string& name, const std::vector<Key>& keys) {
    Param* p = e->params.find(name);
    p->keys = keys;
    std::sort(p->keys.begin(), p->keys.end(), [](const Key& a, const Key& b) { return a.time < b.time; });
    fireChanged(e, name, kOfxChangeUserEdited, 0.0);
}
std::string Host::getString(Effect* e, const std::string& name) { return e->params.find(name)->str; }
void Host::pressButton(Effect* e, const std::string& name, double time) {
    fireChanged(e, name, kOfxChangeUserEdited, time);
}

std::string Host::saveParams(Effect* e) const {
    using wmp::json::Value;
    Value root = Value::object();
    for (const auto& p : e->params.params) {
        if (p->props.has(kOfxParamPropPersistant) && p->props.getInt(kOfxParamPropPersistant) == 0) continue;
        if (!p->isNumeric() && !p->isString()) continue;
        Value v = Value::object();
        if (p->isString()) {
            v.set("s", Value::string(p->str));
        } else {
            Value val = Value::array();
            for (double d : p->value) val.push(Value::number(d));
            v.set("v", val);
            Value keys = Value::array();
            for (const Key& k : p->keys) {
                Value kv = Value::array();
                kv.push(Value::number(k.time));
                for (double d : k.v) kv.push(Value::number(d));
                keys.push(kv);
            }
            v.set("k", keys);
        }
        root.set(p->name, v);
    }
    return wmp::json::serialize(root);
}

void Host::loadParams(Effect* e, const std::string& saved) {
    wmp::json::Value root;
    if (!wmp::json::parse(saved, root)) return;
    for (const auto& m : root.members()) {
        Param* p = e->params.find(m.first);
        if (!p) continue;
        if (const auto* s = m.second.find("s")) p->str = s->asString();
        if (const auto* v = m.second.find("v")) {
            p->value.clear();
            for (const auto& d : v->items()) p->value.push_back(d.asNumber());
        }
        if (const auto* k = m.second.find("k")) {
            p->keys.clear();
            for (const auto& kv : k->items()) {
                Key key;
                key.time = kv.items()[0].asNumber();
                for (std::size_t i = 1; i < kv.items().size(); ++i) key.v.push_back(kv.items()[i].asNumber());
                p->keys.push_back(key);
            }
        }
    }
    // Hosts notify after loading a project.
    fireChanged(e, "wmpPathData", kOfxChangeUserEdited, 0.0);
}

std::unique_ptr<Interact> Host::createInteract(Effect* e) {
    auto it = std::make_unique<Interact>();
    it->effect = e;
    it->props.setPointer(kOfxPropEffectInstance, e);
    return it;
}

OfxStatus Host::interactAction(Interact* it, const char* name, PropertySet& in) {
    void* entry = descriptor_->props.getPointer(provideDrawSuite ? kOfxImageEffectPluginPropOverlayInteractV2
                                                                 : kOfxImageEffectPluginPropOverlayInteractV1);
    if (!entry) return kOfxStatErrMissingHostFeature;
    in.setPointer(kOfxPropEffectInstance, it->effect);
    auto fn = reinterpret_cast<OfxPluginEntryPoint*>(entry);
    return fn(name, it, h(&in), nullptr);
}

}  // namespace mock
