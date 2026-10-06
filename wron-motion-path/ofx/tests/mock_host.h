// Minimal OFX host used to exercise the plug-in binary end to end on Linux.
//
// It implements the OFX 1.x suites the plug-in uses (property, image effect,
// parameter with keyframes, memory, multithread, message, interact, draw),
// loads the real .ofx with dlopen and drives the real actions. It also counts
// outstanding images and live threads so tests can prove nothing leaks.
//
// THIS IS NOT VEGAS. Passing these tests shows the plug-in follows the OFX
// contract as written; VEGAS Pro 2026 behaviour must still be verified on the
// real host (see docs/03-HOST-VERIFICATION.md).
#pragma once

#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "ofxCore.h"
#include "ofxDrawSuite.h"
#include "ofxImageEffect.h"
#include "ofxInteract.h"
#include "ofxParam.h"

namespace mock {

struct Property {
    enum class T { Ptr, Str, Dbl, Int } type = T::Int;
    std::vector<void*> p;
    std::vector<std::string> s;
    std::vector<double> d;
    std::vector<int> i;
    int size() const;
};

struct PropertySet {
    std::map<std::string, Property> props;
    void setString(const std::string& n, const std::string& v, int idx = 0);
    void setDouble(const std::string& n, double v, int idx = 0);
    void setInt(const std::string& n, int v, int idx = 0);
    void setPointer(const std::string& n, void* v, int idx = 0);
    std::string getString(const std::string& n, int idx = 0) const;
    double getDouble(const std::string& n, int idx = 0) const;
    int getInt(const std::string& n, int idx = 0) const;
    void* getPointer(const std::string& n, int idx = 0) const;
    bool has(const std::string& n) const { return props.count(n) != 0; }
    int dimension(const std::string& n) const;
};

struct Key {
    double time;
    std::vector<double> v;
};

struct Param {
    std::string name, type;
    PropertySet props;
    int dims = 1;
    std::vector<double> value;  // numeric value (static)
    std::string str;            // string value
    std::vector<Key> keys;      // numeric animation, sorted by time
    std::string tempBuffer;     // emulates a host temp buffer returned by string gets
    std::atomic<int> stringGetsInFlight{0};
    std::atomic<int> concurrentStringGets{0};
    bool isNumeric() const;
    bool isString() const;
    std::vector<double> valueAt(double t) const;
};

struct ParamSet {
    PropertySet props;
    std::vector<std::unique_ptr<Param>> params;
    Param* find(const std::string& name) const;
    int editDepth = 0;
    std::vector<std::string> editLabels;   // completed edit blocks
    int setsOutsideEdit = 0;               // plug-in writes not wrapped in an edit block
};

struct Image {
    PropertySet props;
    std::vector<unsigned char> data;
    int x1 = 0, y1 = 0, x2 = 0, y2 = 0;
    int bpp = 16;
    std::string depth = kOfxBitDepthFloat;
    std::string premult = kOfxImagePreMultiplied;
    bool bgra = false;
    float* f(int x, int y) {
        return reinterpret_cast<float*>(data.data()) + (static_cast<std::size_t>(y - y1) * (x2 - x1) + (x - x1)) * 4;
    }
    unsigned char* b(int x, int y) {
        return data.data() + (static_cast<std::size_t>(y - y1) * (x2 - x1) + (x - x1)) * 4;
    }
};

struct Effect;

struct Clip {
    std::string name;
    PropertySet props;
    Effect* owner = nullptr;
};

class Host;

// Reads a pixel of any test image format as premultiplied float RGBA.
void readPremult(Image& img, int x, int y, float out[4]);

struct Effect {
    bool descriptor = true;
    Host* host = nullptr;
    PropertySet props;
    ParamSet params;
    std::map<std::string, std::unique_ptr<Clip>> clips;
    std::atomic<bool> abortFlag{false};
    std::vector<OfxRectD> sourceRegionsRequested;
    std::mutex regionMutex;
};

struct DrawCall {
    OfxDrawPrimitive primitive;
    int count;
    OfxRGBAColourF colour;
};

struct Interact {
    PropertySet props;
    Effect* effect = nullptr;
    std::atomic<int> redraws{0};
};

struct ImageSpec {
    int width = 0, height = 0;        // full-resolution frame size in pixels
    double renderScale = 1.0;
    std::string depth = kOfxBitDepthFloat;
    std::string premult = kOfxImagePreMultiplied;
    bool bgra = false;                // VEGAS pixel-order extension
};

struct RenderRequest {
    double time = 0.0;
    int window[4] = {0, 0, 0, 0};      // pixel coords at render scale
    double renderScale = 1.0;
    std::string field = kOfxImageFieldNone;
    std::string vegasQuality;          // VEGAS render-quality extension ("" = not set)
};

class Host {
public:
    // Configuration (before load()).
    std::string hostName = "org.example.wmp.mockhost";
    bool supportsOverlays = true;
    bool provideDrawSuite = true;
    bool provideMultiThread = true;

    Host();
    ~Host();
    bool load(const std::string& pluginPath, std::string& error);
    void unload();

    OfxPlugin* plugin() const { return plugin_; }
    Effect* descriptor() { return descriptor_.get(); }
    Effect* contextDescriptor() { return context_.get(); }

    std::unique_ptr<Effect> createInstance(int width, int height, double par, double fps,
                                           double frameStart, double frameEnd);
    void destroyInstance(std::unique_ptr<Effect>& e);

    OfxStatus action(const char* name, void* handle, PropertySet* in, PropertySet* out);

    // Renders into a newly allocated output image covering the whole frame.
    OfxStatus render(Effect* e, const RenderRequest& rq, const ImageSpec& spec, Image& out);

    // Parameter helpers for tests (emulate user edits; fire instanceChanged).
    void setDouble(Effect* e, const std::string& name, double v);
    void setDouble2(Effect* e, const std::string& name, double x, double y);
    void setInt(Effect* e, const std::string& name, int v);
    void setString(Effect* e, const std::string& name, const std::string& v);
    void setKeys(Effect* e, const std::string& name, const std::vector<Key>& keys);
    std::string getString(Effect* e, const std::string& name);
    void pressButton(Effect* e, const std::string& name, double time = 0.0);

    // Project save / load and event copy emulation.
    std::string saveParams(Effect* e) const;
    void loadParams(Effect* e, const std::string& saved);

    // Source content: premultiplied RGBA at a full-resolution pixel position
    // (x, y in OFX pixel coords, Y up) and time. Point-sampled at pixel centres.
    std::function<void(double x, double y, double time, float rgba[4])> sourcePixel;
    // When true, source images are cropped to the region the plug-in asked for
    // (proves the region-of-interest requests are sufficient).
    bool cropSourceToRequest = false;
    // Mimic hosts that ask isIdentity first and copy the source themselves.
    bool askIdentityFirst = true;

    // Overlay helpers.
    std::unique_ptr<Interact> createInteract(Effect* e);
    OfxStatus interactAction(Interact* it, const char* action, PropertySet& in);
    std::vector<DrawCall> drawCalls;

    // Accounting.
    std::atomic<int> liveImages{0};
    std::atomic<int> liveThreads{0};
    std::atomic<int> maxThreads{0};
    std::vector<std::string> messages;

    static Host* current();
    bool lastWasIdentity() const { return lastWasIdentity_.load(); }

private:
    std::atomic<bool> lastWasIdentity_{false};
    void* lib_ = nullptr;
    OfxPlugin* plugin_ = nullptr;
    OfxHost ofxHost_{};
    PropertySet hostProps_;
    std::unique_ptr<Effect> descriptor_, context_;
    std::unique_ptr<Image> makeImage(const ImageSpec& spec, double time, const OfxRectD* region, bool source,
                                     double par);
};

}  // namespace mock
