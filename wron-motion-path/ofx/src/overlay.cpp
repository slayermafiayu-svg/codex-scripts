#include "overlay.h"

#include <cmath>
#include <string>
#include <vector>

#include "ofxKeySyms.h"
#include "wmp/bezier.h"
#include "wmp/edit.h"

#if defined(WMP_OVERLAY_OPENGL)
#if defined(_WIN32)
#include <windows.h>
#endif
#include <GL/gl.h>
#endif

namespace wmpofx {

namespace {

struct Rgba {
    float r, g, b, a;
};

// Overlay colours: readable on bright and dark video (dark outline under a
// light stroke), selection in amber, current position in red.
constexpr Rgba kOutline{0.0f, 0.0f, 0.0f, 0.65f};
constexpr Rgba kPathColour{0.35f, 0.85f, 1.0f, 1.0f};
constexpr Rgba kAnchorFill{1.0f, 1.0f, 1.0f, 1.0f};
constexpr Rgba kSelected{1.0f, 0.78f, 0.2f, 1.0f};
constexpr Rgba kHandleColour{1.0f, 0.78f, 0.2f, 0.9f};
constexpr Rgba kCurrent{1.0f, 0.3f, 0.3f, 1.0f};

class Painter {
public:
    virtual ~Painter() = default;
    virtual void color(Rgba c) = 0;
    virtual void width(float w) = 0;
    virtual void strip(const std::vector<OfxPointD>& pts, bool loop) = 0;
    virtual void lines(const std::vector<OfxPointD>& pairs) = 0;
    virtual void fill(const std::vector<OfxPointD>& convex) = 0;
};

class DrawSuitePainter final : public Painter {
public:
    DrawSuitePainter(const OfxDrawSuiteV1* s, OfxDrawContextHandle c) : s_(s), c_(c) {}
    void color(Rgba c) override {
        const OfxRGBAColourF col{c.r, c.g, c.b, c.a};
        s_->setColour(c_, &col);
    }
    void width(float w) override { s_->setLineWidth(c_, w); }
    void strip(const std::vector<OfxPointD>& pts, bool loop) override {
        if (pts.size() >= 2)
            s_->draw(c_, loop ? kOfxDrawPrimitiveLineLoop : kOfxDrawPrimitiveLineStrip, pts.data(),
                     static_cast<int>(pts.size()));
    }
    void lines(const std::vector<OfxPointD>& pairs) override {
        if (pairs.size() >= 2) s_->draw(c_, kOfxDrawPrimitiveLines, pairs.data(), static_cast<int>(pairs.size()));
    }
    void fill(const std::vector<OfxPointD>& convex) override {
        if (convex.size() >= 3)
            s_->draw(c_, kOfxDrawPrimitivePolygon, convex.data(), static_cast<int>(convex.size()));
    }

private:
    const OfxDrawSuiteV1* s_;
    OfxDrawContextHandle c_;
};

#if defined(WMP_OVERLAY_OPENGL)
class GLPainter final : public Painter {
public:
    GLPainter() {
        glPushAttrib(GL_COLOR_BUFFER_BIT | GL_LINE_BIT | GL_ENABLE_BIT | GL_CURRENT_BIT);
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        glEnable(GL_LINE_SMOOTH);
        glDisable(GL_TEXTURE_2D);
    }
    ~GLPainter() override { glPopAttrib(); }
    void color(Rgba c) override { glColor4f(c.r, c.g, c.b, c.a); }
    void width(float w) override { glLineWidth(w); }
    void strip(const std::vector<OfxPointD>& pts, bool loop) override { emit(loop ? GL_LINE_LOOP : GL_LINE_STRIP, pts); }
    void lines(const std::vector<OfxPointD>& pairs) override { emit(GL_LINES, pairs); }
    void fill(const std::vector<OfxPointD>& convex) override { emit(GL_POLYGON, convex); }

private:
    static void emit(GLenum mode, const std::vector<OfxPointD>& pts) {
        glBegin(mode);
        for (const OfxPointD& p : pts) glVertex2d(p.x, p.y);
        glEnd();
    }
};
#endif

OfxPointD toCanon(const wmp::FrameRect& f, wmp::Vec2 n) { return {f.x + n.x * f.w, f.y + f.h - n.y * f.h}; }
wmp::Vec2 toN(const wmp::FrameRect& f, const double c[2]) { return {(c[0] - f.x) / f.w, (f.y + f.h - c[1]) / f.h}; }

std::vector<OfxPointD> shape(OfxPointD c, double rx, double ry, wmp::HandleMode mode) {
    std::vector<OfxPointD> pts;
    if (mode == wmp::HandleMode::Corner) {
        pts = {{c.x - rx, c.y - ry}, {c.x + rx, c.y - ry}, {c.x + rx, c.y + ry}, {c.x - rx, c.y + ry}};
    } else if (mode == wmp::HandleMode::Free) {
        const double k = 1.3;
        pts = {{c.x, c.y - ry * k}, {c.x + rx * k, c.y}, {c.x, c.y + ry * k}, {c.x - rx * k, c.y}};
    } else {
        for (int i = 0; i < 12; ++i) {
            const double a = 2.0 * wmp::kPi * i / 12.0;
            pts.push_back({c.x + rx * std::cos(a), c.y + ry * std::sin(a)});
        }
    }
    return pts;
}

void drawPath(Painter& p, const wmp::Path& path, const wmp::FrameRect& f, const double px[2], int selected,
              bool hasCurrent, wmp::Vec2 currentN) {
    const std::size_t n = path.points.size();
    const std::size_t segs = wmp::segmentCount(path);
    std::vector<OfxPointD> poly;
    for (std::size_t s = 0; s < segs; ++s) {
        const wmp::PathPoint& a = path.points[s];
        const wmp::PathPoint& b = path.points[(s + 1) % n];
        const wmp::Cubic c{a.p, a.p + a.out, b.p + b.in, b.p};
        for (int i = (s == 0 ? 0 : 1); i <= 48; ++i) poly.push_back(toCanon(f, c.eval(i / 48.0)));
    }
    if (poly.size() >= 2) {
        p.color(kOutline);
        p.width(3.5f);
        p.strip(poly, false);
        p.color(kPathColour);
        p.width(1.5f);
        p.strip(poly, false);
    }
    const double rx = 4.0 * px[0], ry = 4.0 * px[1];
    // Handles of the selected point.
    if (selected >= 0 && static_cast<std::size_t>(selected) < n) {
        const wmp::PathPoint& sp = path.points[static_cast<std::size_t>(selected)];
        std::vector<OfxPointD> arms;
        for (const wmp::Vec2& h : {sp.in, sp.out}) {
            if (h == wmp::Vec2{}) continue;
            arms.push_back(toCanon(f, sp.p));
            arms.push_back(toCanon(f, sp.p + h));
        }
        if (!arms.empty()) {
            p.color(kOutline);
            p.width(3.0f);
            p.lines(arms);
            p.color(kHandleColour);
            p.width(1.0f);
            p.lines(arms);
            for (std::size_t i = 1; i < arms.size(); i += 2) {
                p.color(kOutline);
                p.fill(shape(arms[i], rx * 0.9, ry * 0.9, wmp::HandleMode::Smooth));
                p.color(kHandleColour);
                p.fill(shape(arms[i], rx * 0.6, ry * 0.6, wmp::HandleMode::Smooth));
            }
        }
    }
    // Anchors: square = corner, circle = smooth, diamond = free.
    for (std::size_t i = 0; i < n; ++i) {
        const OfxPointD c = toCanon(f, path.points[i].p);
        p.color(kOutline);
        p.fill(shape(c, rx * 1.5, ry * 1.5, path.points[i].mode));
        p.color(static_cast<int>(i) == selected ? kSelected : kAnchorFill);
        p.fill(shape(c, rx, ry, path.points[i].mode));
    }
    // Direction tick at the start (first segment direction).
    if (poly.size() >= 3) {
        const OfxPointD a = poly[0], b = poly[2];
        const double dx = (b.x - a.x) / px[0], dy = (b.y - a.y) / px[1];
        const double len = std::hypot(dx, dy);
        if (len > 1e-9) {
            const double ux = dx / len, uy = dy / len;
            const OfxPointD tip{a.x + ux * 18.0 * px[0], a.y + uy * 18.0 * px[1]};
            const OfxPointD l{tip.x - (ux * 6.0 - uy * 4.0) * px[0], tip.y - (uy * 6.0 + ux * 4.0) * px[1]};
            const OfxPointD r{tip.x - (ux * 6.0 + uy * 4.0) * px[0], tip.y - (uy * 6.0 - ux * 4.0) * px[1]};
            p.color(kPathColour);
            p.fill({tip, l, r});
        }
    }
    if (hasCurrent) {
        const OfxPointD c = toCanon(f, currentN);
        const double s = 9.0;
        const std::vector<OfxPointD> cross = {{c.x - s * px[0], c.y}, {c.x + s * px[0], c.y},
                                              {c.x, c.y - s * px[1]}, {c.x, c.y + s * px[1]}};
        p.color(kOutline);
        p.width(3.5f);
        p.lines(cross);
        p.color(kCurrent);
        p.width(1.5f);
        p.lines(cross);
    }
}

struct EventArgs {
    Instance* in = nullptr;
    OverlayState* st = nullptr;
    OfxInteractHandle interact = nullptr;
    double time = 0.0;
    double px[2] = {1.0, 1.0};
};

bool readEventArgs(const void* handle, OfxPropertySetHandle inArgs, EventArgs& ev) {
    ev.interact = static_cast<OfxInteractHandle>(const_cast<void*>(handle));
    OfxImageEffectHandle effect = static_cast<OfxImageEffectHandle>(getPointer(inArgs, kOfxPropEffectInstance));
    if (!effect) {
        OfxPropertySetHandle ip = nullptr;
        if (suites().interact && suites().interact->interactGetPropertySet(ev.interact, &ip) == kOfxStatOK)
            effect = static_cast<OfxImageEffectHandle>(getPointer(ip, kOfxPropEffectInstance));
    }
    ev.in = overlayInstance(effect);
    ev.st = overlayStateOf(ev.in);
    if (!ev.in || !ev.st) return false;
    ev.time = getDouble(inArgs, kOfxPropTime);
    if (!getDoubleN(inArgs, kOfxInteractPropPixelScale, 2, ev.px) || !(ev.px[0] > 0.0) || !(ev.px[1] > 0.0))
        ev.px[0] = ev.px[1] = 1.0;
    return true;
}

void redraw(const EventArgs& ev) {
    if (suites().interact && suites().interact->interactRedraw) suites().interact->interactRedraw(ev.interact);
}

OfxStatus onDraw(const EventArgs& ev, OfxPropertySetHandle inArgs) {
    if (!overlayVisible(ev.in, ev.time)) return kOfxStatOK;
    const wmp::FrameRect f = overlayFrame(ev.in, ev.time);
    wmp::Path path;
    if (ev.st->dragging) {
        path = ev.st->doc.path;
    } else {
        const wmp::LoadResult r = wmp::loadDocument(overlayReadPath(ev.in));
        if (!r.usable()) return kOfxStatOK;
        path = r.doc.path;
        if (ev.st->selected >= static_cast<int>(path.points.size())) ev.st->selected = -1;
    }
    wmp::Vec2 cur;
    const bool hasCur = overlayCurrentPoint(ev.in, ev.time, cur);
    void* ctx = getPointer(inArgs, kOfxInteractPropDrawContext);
    if (ctx && suites().draw) {
        DrawSuitePainter p(suites().draw, static_cast<OfxDrawContextHandle>(ctx));
        drawPath(p, path, f, ev.px, ev.st->selected, hasCur, cur);
        return kOfxStatOK;
    }
#if defined(WMP_OVERLAY_OPENGL)
    GLPainter p;
    drawPath(p, path, f, ev.px, ev.st->selected, hasCur, cur);
    return kOfxStatOK;
#else
    return kOfxStatReplyDefault;
#endif
}

void commit(const EventArgs& ev, const char* label) {
    overlayWritePath(ev.in, wmp::saveDocument(ev.st->doc), label);
}

OfxStatus onPenDown(const EventArgs& ev, OfxPropertySetHandle inArgs) {
    if (!overlayVisible(ev.in, ev.time)) return kOfxStatReplyDefault;
    double pen[2];
    if (!getDoubleN(inArgs, kOfxInteractPropPenPosition, 2, pen)) return kOfxStatReplyDefault;
    const wmp::LoadResult r = wmp::loadDocument(overlayReadPath(ev.in));
    if (!r.usable()) return kOfxStatReplyDefault;  // never overwrite data we cannot read
    OverlayState& st = *ev.st;
    st.doc = r.doc;
    const wmp::FrameRect f = overlayFrame(ev.in, ev.time);
    const double aspect = f.aspect();
    const wmp::Vec2 n = toN(f, pen);
    const double tol = 7.0 * 0.5 * (ev.px[0] + ev.px[1]) / f.h;  // 7 screen px in frame heights
    wmp::Path& path = st.doc.path;
    const wmp::edit::Hit hit = wmp::edit::hitTest(path, aspect, n, tol, st.selected);
    const int tool = overlayTool(ev.in, ev.time);

    if (tool == 2) {  // delete point
        if (hit.kind != wmp::edit::HitKind::Anchor) return kOfxStatReplyDefault;
        wmp::edit::deletePoint(path, hit.index);
        st.selected = -1;
        commit(ev, "Delete Path Point");
        redraw(ev);
        return kOfxStatOK;
    }
    if (tool == 1 || st.ctrl) {  // add point
        if (hit.kind == wmp::edit::HitKind::Segment) {
            st.selected = wmp::edit::insertPoint(path, hit.index, hit.t);
        } else if (hit.kind == wmp::edit::HitKind::None) {
            wmp::PathPoint np;
            np.p = n;
            np.id = wmp::edit::nextPointId(path);
            // Append after the selected end point, else after the last point.
            const bool atStart = !path.closed && st.selected == 0 && path.points.size() > 1;
            if (atStart) {
                path.points.insert(path.points.begin(), np);
                st.selected = 0;
            } else {
                path.points.push_back(np);
                st.selected = static_cast<int>(path.points.size()) - 1;
            }
        } else {
            return kOfxStatReplyDefault;
        }
        commit(ev, "Add Path Point");
        redraw(ev);
        return kOfxStatOK;
    }
    switch (hit.kind) {
        case wmp::edit::HitKind::Anchor:
            st.selected = hit.index;
            st.dragging = true;
            st.dragKind = static_cast<int>(hit.kind);
            st.index = hit.index;
            st.grabOffsetN = path.points[static_cast<std::size_t>(hit.index)].p - n;
            st.changed = false;
            redraw(ev);
            return kOfxStatOK;
        case wmp::edit::HitKind::InHandle:
        case wmp::edit::HitKind::OutHandle:
            st.dragging = true;
            st.dragKind = static_cast<int>(hit.kind);
            st.index = hit.index;
            st.changed = false;
            return kOfxStatOK;
        default:
            if (st.selected != -1) {
                st.selected = -1;
                redraw(ev);
            }
            return kOfxStatReplyDefault;
    }
}

OfxStatus onPenMotion(const EventArgs& ev, OfxPropertySetHandle inArgs) {
    OverlayState& st = *ev.st;
    if (!st.dragging) return kOfxStatReplyDefault;
    double pen[2];
    if (!getDoubleN(inArgs, kOfxInteractPropPenPosition, 2, pen)) return kOfxStatReplyDefault;
    const wmp::FrameRect f = overlayFrame(ev.in, ev.time);
    const wmp::Vec2 n = toN(f, pen);
    const auto kind = static_cast<wmp::edit::HitKind>(st.dragKind);
    if (kind == wmp::edit::HitKind::Anchor) wmp::edit::moveAnchor(st.doc.path, st.index, n + st.grabOffsetN);
    else
        wmp::edit::moveHandle(st.doc.path, st.index, kind == wmp::edit::HitKind::OutHandle, n, f.aspect(), st.alt);
    st.changed = true;
    redraw(ev);
    return kOfxStatOK;
}

OfxStatus onPenUp(const EventArgs& ev) {
    OverlayState& st = *ev.st;
    if (!st.dragging) return kOfxStatReplyDefault;
    st.dragging = false;
    if (st.changed) commit(ev, st.dragKind == static_cast<int>(wmp::edit::HitKind::Anchor) ? "Move Path Point"
                                                                                            : "Edit Path Handle");
    st.changed = false;
    redraw(ev);
    return kOfxStatOK;
}

OfxStatus onKey(const EventArgs& ev, OfxPropertySetHandle inArgs, bool down) {
    OverlayState& st = *ev.st;
    const int key = getInt(inArgs, kOfxPropKeySym);
    if (key == kOfxKey_Control_L || key == kOfxKey_Control_R) st.ctrl = down;
    if (key == kOfxKey_Alt_L || key == kOfxKey_Alt_R) st.alt = down;
    if (!down) return kOfxStatReplyDefault;
    if (key == kOfxKey_Escape && st.dragging) {  // cancel the drag, nothing written
        st.dragging = false;
        st.changed = false;
        redraw(ev);
        return kOfxStatOK;
    }
    if ((key == kOfxKey_Delete || key == kOfxKey_BackSpace) && st.selected >= 0 && !st.dragging) {
        const wmp::LoadResult r = wmp::loadDocument(overlayReadPath(ev.in));
        if (!r.usable() || st.selected >= static_cast<int>(r.doc.path.points.size())) return kOfxStatReplyDefault;
        st.doc = r.doc;
        wmp::edit::deletePoint(st.doc.path, st.selected);
        st.selected = -1;
        commit(ev, "Delete Path Point");
        redraw(ev);
        return kOfxStatOK;
    }
    return kOfxStatReplyDefault;  // never swallow host shortcuts we do not use
}

}  // namespace

bool overlayCanDraw() {
#if defined(WMP_OVERLAY_OPENGL)
    return true;
#else
    return suites().draw != nullptr;
#endif
}

OfxStatus overlayMain(const char* action, const void* handle, OfxPropertySetHandle inArgs,
                      OfxPropertySetHandle /*outArgs*/) {
    try {
        const std::string a = action ? action : "";
        if (a == kOfxActionDescribe || a == kOfxActionCreateInstance || a == kOfxActionDestroyInstance)
            return kOfxStatOK;
        EventArgs ev;
        if (!readEventArgs(handle, inArgs, ev)) return kOfxStatReplyDefault;
        if (a == kOfxInteractActionDraw) return onDraw(ev, inArgs);
        if (a == kOfxInteractActionPenDown) return onPenDown(ev, inArgs);
        if (a == kOfxInteractActionPenMotion) return onPenMotion(ev, inArgs);
        if (a == kOfxInteractActionPenUp) return onPenUp(ev);
        if (a == kOfxInteractActionKeyDown) return onKey(ev, inArgs, true);
        if (a == kOfxInteractActionKeyUp) return onKey(ev, inArgs, false);
        if (a == kOfxInteractActionLoseFocus) {
            ev.st->ctrl = ev.st->alt = false;
            return kOfxStatOK;
        }
        return kOfxStatReplyDefault;
    } catch (...) {
        return kOfxStatFailed;
    }
}

}  // namespace wmpofx
