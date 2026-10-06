// Wron Motion Path — OFX identifiers.
//
// FROZEN once released: projects store parameter values by these names and
// choice values by index. Never rename or reorder; only append. The
// ofx_param_freeze test fails if this list changes unintentionally.
#pragma once

namespace wmpofx {

// Plug-in identity. The identifier must be unique across all installed OFX
// plug-ins (VEGAS 2026 Build 143 fixed identifier-hash collisions) and must
// never change after the first public release.
// TODO(wron): confirm the reverse-domain prefix used by Wron Transform OFX.
inline constexpr const char* kPluginIdentifier = "com.wronsvp.ofx.MotionPath";
inline constexpr int kPluginVersionMajor = 0;  // 0.x = developer builds only
inline constexpr int kPluginVersionMinor = 1;
inline constexpr const char* kPluginGrouping = "WronSVP";

// Clips
inline constexpr const char* kClipSource = "Source";
inline constexpr const char* kClipOutput = "Output";

// Groups
inline constexpr const char* kGrpPath = "wmpGrpPath";
inline constexpr const char* kGrpTiming = "wmpGrpTiming";
inline constexpr const char* kGrpTransform = "wmpGrpTransform";
inline constexpr const char* kGrpOrientation = "wmpGrpOrientation";
inline constexpr const char* kGrpBlur = "wmpGrpBlur";
inline constexpr const char* kGrpPresets = "wmpGrpPresets";

// Path
inline constexpr const char* kPathData = "wmpPathData";        // string, secret, persistent (versioned JSON)
inline constexpr const char* kPathInfo = "wmpPathInfo";        // read-only label, not persistent
inline constexpr const char* kReversePath = "wmpReversePath";  // push button
inline constexpr const char* kShowOverlay = "wmpShowOverlay";  // bool: draw the path in the host preview
inline constexpr const char* kOverlayTool = "wmpOverlayTool";  // 0 edit points, 1 add point, 2 delete point

// Timing
inline constexpr const char* kProgress = "wmpProgress";
inline constexpr const char* kStartOffset = "wmpStartOffset";
inline constexpr const char* kReverse = "wmpReverse";
inline constexpr const char* kEndBehavior = "wmpEndBehavior";  // 0 clamp, 1 extend, 2 loop, 3 ping-pong
inline constexpr const char* kSpeedMode = "wmpSpeedMode";      // 0 constant speed, 1 equal time per segment
inline constexpr const char* kEasing = "wmpEasing";            // 0 linear, 1 in, 2 out, 3 in-out, 4 back out, 5 custom
inline constexpr const char* kEaseP1 = "wmpEaseP1";
inline constexpr const char* kEaseP2 = "wmpEaseP2";

// Transform
inline constexpr const char* kPivot = "wmpPivot";
inline constexpr const char* kOffset = "wmpOffset";
inline constexpr const char* kUniformScale = "wmpUniformScale";
inline constexpr const char* kScale = "wmpScale";
inline constexpr const char* kRotation = "wmpRotation";
inline constexpr const char* kOpacity = "wmpOpacity";

// Orientation
inline constexpr const char* kOrient = "wmpOrient";
inline constexpr const char* kOrientOffset = "wmpOrientOffset";
inline constexpr const char* kOrientMode = "wmpOrientMode";    // 0 travel direction, 1 path direction
inline constexpr const char* kOrientSmooth = "wmpOrientSmooth";

// Motion blur
inline constexpr const char* kBlur = "wmpBlur";
inline constexpr const char* kShutterAngle = "wmpShutterAngle";
inline constexpr const char* kShutterPhase = "wmpShutterPhase";
inline constexpr const char* kBlurAdaptive = "wmpBlurAdaptive";
inline constexpr const char* kBlurSamples = "wmpBlurSamples";
inline constexpr const char* kBlurMaxSamples = "wmpBlurMaxSamples";
inline constexpr const char* kBlurPreview = "wmpBlurPreview";  // 0 full, 1 reduced, 2 off

// Diagnostics
inline constexpr const char* kGrpDiagnostics = "wmpGrpDiagnostics";
inline constexpr const char* kHostInfo = "wmpHostInfo";        // read-only label, not persistent

// Presets
inline constexpr const char* kPreset = "wmpPreset";            // order = wmp::PresetId
inline constexpr const char* kApplyPreset = "wmpApplyPreset";  // push button

}  // namespace wmpofx
