// Wron Motion Path — thin helpers over the raw OFX C API.
#pragma once

#include <string>

#include "ofxCore.h"
#include "ofxDrawSuite.h"
#include "ofxImageEffect.h"
#include "ofxInteract.h"
#include "ofxMemory.h"
#include "ofxMessage.h"
#include "ofxMultiThread.h"
#include "ofxParam.h"
#include "ofxProperty.h"
#include "wmp/render.h"

namespace wmpofx {

// Host-provided function tables. These are process-wide by nature (one host)
// and hold no per-instance or path state.
struct HostSuites {
    OfxHost* host = nullptr;
    const OfxPropertySuiteV1* prop = nullptr;
    const OfxImageEffectSuiteV1* effect = nullptr;
    const OfxParameterSuiteV1* param = nullptr;
    const OfxMemorySuiteV1* memory = nullptr;
    const OfxMultiThreadSuiteV1* thread = nullptr;
    const OfxMessageSuiteV1* message = nullptr;
    const OfxInteractSuiteV1* interact = nullptr;
    const OfxDrawSuiteV1* draw = nullptr;
    bool supportsOverlays = false;
    std::string hostName, hostLabel, hostVersion;
};

HostSuites& suites();

// Property helpers: return `def` when the host does not set the property.
std::string getString(OfxPropertySetHandle p, const char* name, int index = 0, const char* def = "");
double getDouble(OfxPropertySetHandle p, const char* name, int index = 0, double def = 0.0);
int getInt(OfxPropertySetHandle p, const char* name, int index = 0, int def = 0);
void* getPointer(OfxPropertySetHandle p, const char* name, int index = 0);
bool getDoubleN(OfxPropertySetHandle p, const char* name, int count, double* out);
bool getIntN(OfxPropertySetHandle p, const char* name, int count, int* out);

void setString(OfxPropertySetHandle p, const char* name, const char* v, int index = 0);
void setDouble(OfxPropertySetHandle p, const char* name, double v, int index = 0);
void setInt(OfxPropertySetHandle p, const char* name, int v, int index = 0);
void setPointer(OfxPropertySetHandle p, const char* name, void* v, int index = 0);
void setDoubleN(OfxPropertySetHandle p, const char* name, int count, const double* v);

// Describes a fetched image. Returns false (with a reason) for formats the
// renderer does not handle, so render fails cleanly instead of misreading.
bool imageView(OfxPropertySetHandle image, wmp::ImageView& out, double& par, std::string& error);

// RAII wrapper for clipGetImage / clipReleaseImage.
class ImageHandle {
public:
    ImageHandle() = default;
    ImageHandle(OfxImageClipHandle clip, OfxTime time, const OfxRectD* region);
    ~ImageHandle();
    ImageHandle(const ImageHandle&) = delete;
    ImageHandle& operator=(const ImageHandle&) = delete;
    OfxPropertySetHandle props() const { return props_; }
    explicit operator bool() const { return props_ != nullptr; }

private:
    OfxPropertySetHandle props_ = nullptr;
};

}  // namespace wmpofx
