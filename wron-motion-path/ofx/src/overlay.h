// Wron Motion Path — OFX overlay interact (direct path editing in the host's
// video preview).
//
// Used only when the host reports kOfxImageEffectPropSupportsOverlays and the
// plug-in can draw (OFX 1.5 Draw Suite via OverlayInteractV2, or legacy
// OpenGL via OverlayInteractV1). VEGAS Pro 2026 ships OFX interacts for its
// own Crop/PiP plug-ins; third-party interact support is NOT verified on a
// real host from this repository.
//
// Editing rules come from wmp::edit (shared with the web path editor). During
// a drag the edited path lives only in OverlayState and is drawn from there;
// the parameter is written once on pen-up inside paramEditBegin/End, so a
// drag is one undo step and the host does not re-render on every mouse move.
#pragma once

#include <string>

#include "ofx_util.h"
#include "wmp/document.h"
#include "wmp/transform.h"

namespace wmpofx {

struct Instance;

struct OverlayState {
    bool dragging = false;
    int dragKind = 0;   // wmp::edit::HitKind
    int index = -1;
    int selected = -1;
    bool changed = false;
    wmp::PathDocument doc;  // working copy while dragging
    wmp::Vec2 grabOffsetN;
    // Modifier keys, tracked from key events (hosts are not required to send
    // them; every action is also reachable through the "Overlay Tool" choice).
    bool ctrl = false, alt = false;
};

bool overlayCanDraw();
OfxStatus overlayMain(const char* action, const void* handle, OfxPropertySetHandle inArgs,
                      OfxPropertySetHandle outArgs);

// Implemented in plugin.cpp.
Instance* overlayInstance(OfxImageEffectHandle effect);
OverlayState* overlayStateOf(Instance* in);
std::string overlayReadPath(Instance* in);
void overlayWritePath(Instance* in, const std::string& json, const char* undoLabel);
wmp::FrameRect overlayFrame(Instance* in, double time);
bool overlayCurrentPoint(Instance* in, double time, wmp::Vec2& pathPointN);
int overlayTool(Instance* in, double time);  // 0 edit, 1 add point, 2 delete point
bool overlayVisible(Instance* in, double time);

}  // namespace wmpofx
