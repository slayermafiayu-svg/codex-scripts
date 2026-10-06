#include "ofx_util.h"

#include <cstring>

#include "vegas_ext.h"

namespace wmpofx {

HostSuites& suites() {
    static HostSuites s;
    return s;
}

std::string getString(OfxPropertySetHandle p, const char* name, int index, const char* def) {
    char* v = nullptr;
    if (!p || suites().prop->propGetString(p, name, index, &v) != kOfxStatOK || !v) return def;
    return v;
}

double getDouble(OfxPropertySetHandle p, const char* name, int index, double def) {
    double v = def;
    if (!p || suites().prop->propGetDouble(p, name, index, &v) != kOfxStatOK) return def;
    return v;
}

int getInt(OfxPropertySetHandle p, const char* name, int index, int def) {
    int v = def;
    if (!p || suites().prop->propGetInt(p, name, index, &v) != kOfxStatOK) return def;
    return v;
}

void* getPointer(OfxPropertySetHandle p, const char* name, int index) {
    void* v = nullptr;
    if (!p || suites().prop->propGetPointer(p, name, index, &v) != kOfxStatOK) return nullptr;
    return v;
}

bool getDoubleN(OfxPropertySetHandle p, const char* name, int count, double* out) {
    return p && suites().prop->propGetDoubleN(p, name, count, out) == kOfxStatOK;
}

bool getIntN(OfxPropertySetHandle p, const char* name, int count, int* out) {
    return p && suites().prop->propGetIntN(p, name, count, out) == kOfxStatOK;
}

void setString(OfxPropertySetHandle p, const char* name, const char* v, int index) {
    if (p) suites().prop->propSetString(p, name, index, v);
}
void setDouble(OfxPropertySetHandle p, const char* name, double v, int index) {
    if (p) suites().prop->propSetDouble(p, name, index, v);
}
void setInt(OfxPropertySetHandle p, const char* name, int v, int index) {
    if (p) suites().prop->propSetInt(p, name, index, v);
}
void setPointer(OfxPropertySetHandle p, const char* name, void* v, int index) {
    if (p) suites().prop->propSetPointer(p, name, index, v);
}
void setDoubleN(OfxPropertySetHandle p, const char* name, int count, const double* v) {
    if (p) suites().prop->propSetDoubleN(p, name, count, v);
}

bool imageView(OfxPropertySetHandle img, wmp::ImageView& v, double& par, std::string& error) {
    v = wmp::ImageView();
    v.data = getPointer(img, kOfxImagePropData);
    v.rowBytes = getInt(img, kOfxImagePropRowBytes);
    int b[4] = {0, 0, 0, 0};
    if (!getIntN(img, kOfxImagePropBounds, 4, b)) {
        error = "image has no bounds";
        return false;
    }
    v.bounds = {b[0], b[1], b[2], b[3]};
    const std::string comps = getString(img, kOfxImageEffectPropComponents);
    if (comps != kOfxImageComponentRGBA) {
        error = "unsupported components: " + comps;
        return false;
    }
    const std::string depth = getString(img, kOfxImageEffectPropPixelDepth);
    bool bgrDepth = false;
    if (depth == kOfxBitDepthByte) v.depth = wmp::PixelDepth::U8;
    else if (depth == kOfxBitDepthShort) v.depth = wmp::PixelDepth::U16;
    else if (depth == kOfxBitDepthFloat) v.depth = wmp::PixelDepth::F32;
    else if (depth == kWmpVegasBitDepthByteBGR) { v.depth = wmp::PixelDepth::U8; bgrDepth = true; }
    else if (depth == kWmpVegasBitDepthShortBGR) { v.depth = wmp::PixelDepth::U16; bgrDepth = true; }
    else if (depth == kWmpVegasBitDepthFloatBGR) { v.depth = wmp::PixelDepth::F32; bgrDepth = true; }
    else {
        error = "unsupported pixel depth: " + depth;
        return false;
    }
    const std::string order = getString(img, kWmpVegasImagePropPixelOrder, 0, kWmpVegasPixelOrderRGBA);
    v.order = (bgrDepth || order == kWmpVegasPixelOrderBGRA) ? wmp::ChannelOrder::BGRA : wmp::ChannelOrder::RGBA;
    const std::string pm = getString(img, kOfxImageEffectPropPreMultiplication, 0, kOfxImagePreMultiplied);
    if (pm == kOfxImageOpaque) v.alpha = wmp::AlphaMode::Opaque;
    else if (pm == kOfxImageUnPreMultiplied) v.alpha = wmp::AlphaMode::Straight;
    else v.alpha = wmp::AlphaMode::Premultiplied;
    par = getDouble(img, kOfxImagePropPixelAspectRatio, 0, 1.0);
    if (!(par > 0.0)) par = 1.0;
    if (!v.data && !v.bounds.empty()) {
        error = "image has no data";
        return false;
    }
    const long long minRow = static_cast<long long>(v.bounds.width()) * v.bytesPerPixel();
    if (!v.bounds.empty() && (v.rowBytes < 0 ? -v.rowBytes : v.rowBytes) < minRow) {
        error = "row bytes smaller than a row";
        return false;
    }
    return true;
}

ImageHandle::ImageHandle(OfxImageClipHandle clip, OfxTime time, const OfxRectD* region) {
    if (!clip) return;
    if (suites().effect->clipGetImage(clip, time, region, &props_) != kOfxStatOK) props_ = nullptr;
}

ImageHandle::~ImageHandle() {
    if (props_) suites().effect->clipReleaseImage(props_);
}

}  // namespace wmpofx
