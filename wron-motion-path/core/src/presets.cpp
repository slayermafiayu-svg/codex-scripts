#include "wmp/presets.h"

#include <cmath>
#include <vector>

namespace wmp {

namespace {

struct Builder {
    double aspect;
    Path path;

    // Arguments in display space (x in frame heights).
    void add(Vec2 p, Vec2 in, Vec2 out, HandleMode mode) {
        PathPoint pt;
        pt.p = {p.x / aspect, p.y};
        pt.in = {in.x / aspect, in.y};
        pt.out = {out.x / aspect, out.y};
        pt.mode = mode;
        pt.id = "p" + std::to_string(path.points.size() + 1);
        path.points.push_back(pt);
    }
};

// Handle length factor for a circular arc of `angle` radians.
double arcHandle(double angle) { return 4.0 / 3.0 * std::tan(angle / 4.0); }

}  // namespace

const char* presetKey(PresetId id) {
    switch (id) {
        case PresetId::Straight: return "straight";
        case PresetId::Arc: return "arc";
        case PresetId::Circle: return "circle";
        case PresetId::Ellipse: return "ellipse";
        case PresetId::SCurve: return "s-curve";
        case PresetId::Zigzag: return "zigzag";
        case PresetId::Spiral: return "spiral";
    }
    return "straight";
}

bool presetFromKey(const std::string& key, PresetId& out) {
    for (int i = 0; i < kPresetCount; ++i) {
        const PresetId id = static_cast<PresetId>(i);
        if (key == presetKey(id)) {
            out = id;
            return true;
        }
    }
    return false;
}

Path makePreset(PresetId id, double aspect) {
    if (!(aspect > 0.0) || !std::isfinite(aspect)) aspect = 16.0 / 9.0;
    Builder b{aspect, {}};
    const double A = aspect;
    const double cx = 0.5 * A, cy = 0.5;
    switch (id) {
        case PresetId::Straight:
            b.add({0.2 * A, cy}, {}, {}, HandleMode::Corner);
            b.add({0.8 * A, cy}, {}, {}, HandleMode::Corner);
            break;

        case PresetId::Arc: {
            // 120° circular arc over a horizontal chord, bulging upward.
            const double halfChord = 0.3 * A;
            const double half = kPi / 3.0;  // 60°
            const double R = halfChord / std::sin(half);
            const double chordY = 0.68;
            const Vec2 c{cx, chordY + R * std::cos(half)};
            const double k = arcHandle(half) * R;
            const double phis[3] = {-half, 0.0, half};
            for (int i = 0; i < 3; ++i) {
                const double f = phis[i];
                const Vec2 p = c + Vec2{std::sin(f), -std::cos(f)} * R;
                const Vec2 tan{std::cos(f), std::sin(f)};
                b.add(p, i == 0 ? Vec2{} : tan * -k, i == 2 ? Vec2{} : tan * k, HandleMode::Smooth);
            }
            break;
        }

        case PresetId::Circle:
        case PresetId::Ellipse: {
            const double rx = id == PresetId::Circle ? 0.3 : 0.36 * A;
            const double ry = 0.3;
            const double k = arcHandle(kPi / 2.0);
            // Starts at the top and runs clockwise on screen (Y down).
            b.add({cx, cy - ry}, {-k * rx, 0}, {k * rx, 0}, HandleMode::Smooth);
            b.add({cx + rx, cy}, {0, -k * ry}, {0, k * ry}, HandleMode::Smooth);
            b.add({cx, cy + ry}, {k * rx, 0}, {-k * rx, 0}, HandleMode::Smooth);
            b.add({cx - rx, cy}, {0, k * ry}, {0, -k * ry}, HandleMode::Smooth);
            b.path.closed = true;
            break;
        }

        case PresetId::SCurve:
            b.add({0.15 * A, 0.75}, {}, {0.22 * A, 0.0}, HandleMode::Smooth);
            b.add({cx, cy}, {-0.07 * A, 0.17}, {0.07 * A, -0.17}, HandleMode::Smooth);
            b.add({0.85 * A, 0.25}, {-0.22 * A, 0.0}, {}, HandleMode::Smooth);
            break;

        case PresetId::Zigzag:
            for (int i = 0; i < 6; ++i) {
                const double x = (0.15 + 0.14 * i) * A;
                b.add({x, (i % 2 == 0) ? 0.65 : 0.35}, {}, {}, HandleMode::Corner);
            }
            break;

        case PresetId::Spiral: {
            // Archimedean spiral r = r0 + bθ, 2.5 turns outward, clockwise on screen.
            const double r0 = 0.03, r1 = 0.4;
            const double turns = 2.5;
            const double thetaEnd = turns * 2.0 * kPi;
            const double bcoef = (r1 - r0) / thetaEnd;
            const int steps = static_cast<int>(turns * 4.0);  // one point per 90°
            const double dTheta = thetaEnd / steps;
            const double k = arcHandle(dTheta);
            for (int i = 0; i <= steps; ++i) {
                const double th = dTheta * i;
                const double r = r0 + bcoef * th;
                const Vec2 dir{std::cos(th), std::sin(th)};
                const Vec2 p = Vec2{cx, cy} + dir * r;
                const Vec2 deriv = dir * bcoef + Vec2{-dir.y, dir.x} * r;  // dp/dθ
                const Vec2 h = deriv * k;
                b.add(p, i == 0 ? Vec2{} : -h, i == steps ? Vec2{} : h, HandleMode::Smooth);
            }
            break;
        }
    }
    return b.path;
}

}  // namespace wmp
