// VEGAS-specific OFX extension identifiers used by Wron Motion Path.
//
// Only the identifiers this plug-in reads are declared here. They match the
// Sony/MAGIX VEGAS OFX extension header ("ofxSonyVegas.h", 2010) as found in
// public OpenFX forks. The official header is not vendored because of its
// licence notice; when the Wron Transform OFX sources are added, replace this
// file with the VEGAS SDK header that Wron already builds against.
//
// Every use is optional: each property is read with a fallback, so the plug-in
// behaves identically on hosts that do not set them.
#pragma once

// Image property: channel order of the pixels ("RGBA" or "BGRA").
#define kWmpVegasImagePropPixelOrder "OfxImageEffectPropPixelOrder"
#define kWmpVegasPixelOrderRGBA "OfxImagePixelOrderRGBA"
#define kWmpVegasPixelOrderBGRA "OfxImagePixelOrderBGRA"

// BGR-ordered pixel depth labels (never declared as supported by this plug-in,
// recognised defensively if a host reports them anyway).
#define kWmpVegasBitDepthByteBGR "OfxBitDepthByteBGR"
#define kWmpVegasBitDepthShortBGR "OfxBitDepthShortBGR"
#define kWmpVegasBitDepthFloatBGR "OfxBitDepthFloatBGR"

// Render in-arg: string render quality. NOTE: the value
// "OfxImageEffectPropRenderQualityDraft" has the same spelling as the
// standard OFX 1.4 *integer* property kOfxImageEffectPropRenderQualityDraft;
// the two are read separately and never confused.
#define kWmpVegasPropRenderQuality "OfxImageEffectPropRenderQuality"
#define kWmpVegasRenderQualityDraft "OfxImageEffectPropRenderQualityDraft"
#define kWmpVegasRenderQualityPreview "OfxImageEffectPropRenderQualityPreview"
#define kWmpVegasRenderQualityGood "OfxImageEffectPropRenderQualityGood"
#define kWmpVegasRenderQualityBest "OfxImageEffectPropRenderQualityBest"

// Effect instance property: where the effect sits (media/track/event/...).
#define kWmpVegasPropContext "OfxImageEffectPropVegasContext"

// Host names (diagnostics only; behaviour never depends on the host name).
#define kWmpVegasHostNameSony "com.sonycreativesoftware.vegas"
