// Wron Motion Path — affine transforms and coordinate-space mapping.
//
// Spaces:
//   N  normalized frame coords, Y down, (0.5,0.5) = centre      (user facing)
//   D  display space = (u * aspect, v), isotropic, frame heights (geometry)
//   C  OFX canonical coords, Y up, origin bottom-left, PAR-corrected (host)
//   P  OFX pixel coords: P = (C.x * renderScale.x / PAR, C.y * renderScale.y)
#pragma once

#include <cmath>

#include "wmp/vec2.h"

namespace wmp {

// x' = a*x + b*y + tx ; y' = c*x + d*y + ty
struct Affine {
    double a = 1.0, b = 0.0, c = 0.0, d = 1.0, tx = 0.0, ty = 0.0;

    static Affine identity() { return {}; }
    static Affine translate(const Vec2& t) { return {1, 0, 0, 1, t.x, t.y}; }
    static Affine scale(double sx, double sy) { return {sx, 0, 0, sy, 0, 0}; }
    // Rotation by `rad` in a Y-DOWN space: positive turns +X toward +Y, i.e.
    // clockwise on screen.
    static Affine rotate(double rad) {
        const double cs = std::cos(rad), sn = std::sin(rad);
        return {cs, -sn, sn, cs, 0, 0};
    }

    Vec2 apply(const Vec2& p) const { return {a * p.x + b * p.y + tx, c * p.x + d * p.y + ty}; }
    Vec2 applyLinear(const Vec2& v) const { return {a * v.x + b * v.y, c * v.x + d * v.y}; }
    double det() const { return a * d - b * c; }

    // this ∘ o  (apply o first)
    Affine operator*(const Affine& o) const {
        return {a * o.a + b * o.c, a * o.b + b * o.d, c * o.a + d * o.c, c * o.b + d * o.d,
                a * o.tx + b * o.ty + tx, c * o.tx + d * o.ty + ty};
    }

    bool inverse(Affine& out) const {
        const double dt = det();
        if (!(std::fabs(dt) > 1e-300) || !std::isfinite(dt)) return false;
        const double id = 1.0 / dt;
        out.a = d * id;
        out.b = -b * id;
        out.c = -c * id;
        out.d = a * id;
        out.tx = -(out.a * tx + out.b * ty);
        out.ty = -(out.c * tx + out.d * ty);
        return true;
    }

    // Largest singular value of the linear part (max stretch).
    double maxStretch() const {
        const double e = 0.5 * (a + d), f = 0.5 * (a - d), g = 0.5 * (c + b), h = 0.5 * (c - b);
        return std::hypot(e, h) + std::hypot(f, g);
    }

    bool isFinite() const {
        return std::isfinite(a) && std::isfinite(b) && std::isfinite(c) && std::isfinite(d) && std::isfinite(tx) &&
               std::isfinite(ty);
    }
};

// Project frame in canonical coordinates (OFX kOfxImageEffectPropProjectOffset
// / ProjectSize). Canonical units already include the pixel aspect ratio.
struct FrameRect {
    double x = 0.0, y = 0.0, w = 1920.0, h = 1080.0;
    double aspect() const { return h > 0.0 ? w / h : 1.0; }
    bool valid() const { return std::isfinite(x) && std::isfinite(y) && w > 0.0 && h > 0.0; }
};

// D space -> canonical.
inline Affine displayToCanonical(const FrameRect& f) { return {f.h, 0, 0, -f.h, f.x, f.y + f.h}; }
// canonical -> D space.
inline Affine canonicalToDisplay(const FrameRect& f) {
    return {1.0 / f.h, 0, 0, -1.0 / f.h, -f.x / f.h, (f.y + f.h) / f.h};
}

// canonical -> pixel at a render scale / PAR.
inline Affine canonicalToPixel(double renderScaleX, double renderScaleY, double par) {
    return Affine::scale(renderScaleX / par, renderScaleY);
}

}  // namespace wmp
