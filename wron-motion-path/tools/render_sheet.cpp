// Renders a contact sheet with the REAL plug-in binary through the mock OFX
// host (not VEGAS): onion-skinned frames for several path/timing setups.
// Output: PAM (P7 RGBA) on stdout; tools/pam_to_png.py converts it to PNG.
// Usage: wmp_render_sheet <path/to/WronMotionPath.ofx> > sheet.pam
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "../ofx/tests/mock_host.h"
#include "wmp/document.h"
#include "wmp/geometry.h"
#include "wmp/presets.h"

namespace {

constexpr int TW = 640, TH = 360;  // tile = frame size

struct Canvas {
    int w, h;
    std::vector<float> px;  // RGBA premultiplied
    Canvas(int w_, int h_) : w(w_), h(h_), px(static_cast<std::size_t>(w_) * h_ * 4, 0.0f) {}
    float* at(int x, int y) { return &px[(static_cast<std::size_t>(y) * w + x) * 4]; }
    void over(int x, int y, const float c[4]) {
        if (x < 0 || y < 0 || x >= w || y >= h) return;
        float* d = at(x, y);
        for (int i = 0; i < 4; ++i) d[i] = c[i] + d[i] * (1.0f - c[3]);
    }
};

// Arrow pointing right, centred in the frame (pixel coords, Y up), premultiplied.
void arrow(double x, double y, double, float c[4]) {
    const double cx = TW / 2.0, cy = TH / 2.0;
    const double dx = x - cx, dy = y - cy;
    const bool body = dx > -26 && dx < 6 && std::fabs(dy) < 7;
    const bool head = dx >= 6 && dx < 26 && std::fabs(dy) < (26 - dx) * 0.9;
    const float a = (body || head) ? 1.0f : 0.0f;
    c[0] = 1.0f * a;
    c[1] = 0.78f * a;
    c[2] = 0.25f * a;
    c[3] = a;
}

void drawPath(Canvas& cv, int ox, int oy, const std::string& json) {
    const wmp::LoadResult r = wmp::loadDocument(json);
    if (!r.usable()) return;
    wmp::PathGeometry g(r.doc.path, {double(TW) / TH, 1e-7});
    const float col[4] = {0.24f * 0.8f, 0.72f * 0.8f, 1.0f * 0.8f, 0.8f};
    const int steps = 2000;
    for (int i = 0; i <= steps; ++i) {
        const wmp::Vec2 n = g.toNormalized(g.sampleAtDistance(g.length() * i / steps, 1, false).pos);
        cv.over(ox + static_cast<int>(n.x * TW), oy + static_cast<int>(n.y * TH), col);
    }
}

void blit(Canvas& cv, int ox, int oy, mock::Image& img, float alphaScale) {
    for (int y = img.y1; y < img.y2; ++y)
        for (int x = img.x1; x < img.x2; ++x) {
            float c[4];
            mock::readPremult(img, x, y, c);
            for (float& v : c) v *= alphaScale;
            cv.over(ox + x, oy + (TH - 1 - y), c);  // OFX rows are bottom-up
        }
}

struct Setup {
    const char* title;
    wmp::PresetId preset;
    std::vector<std::pair<std::string, double>> doubles;
    std::vector<std::pair<std::string, int>> ints;
    std::vector<double> frames;
    double progressPerFrame;
};

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) return 2;
    mock::Host host;
    host.sourcePixel = arrow;
    std::string err;
    if (!host.load(argv[1], err)) {
        std::fprintf(stderr, "%s\n", err.c_str());
        return 1;
    }
    std::vector<Setup> setups = {
        {"constant speed, circle", wmp::PresetId::Circle, {}, {}, {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11}, 1.0 / 12},
        {"orient to path, S-curve", wmp::PresetId::SCurve, {}, {{"wmpOrient", 1}}, {0, 2, 4, 6, 8, 10, 12, 14, 16, 18, 20}, 0.05},
        {"motion blur 180, S-curve", wmp::PresetId::SCurve, {}, {{"wmpOrient", 1}, {"wmpBlur", 1}}, {3, 7, 11, 15}, 0.06},
        {"open loop at the wrap (no streak)", wmp::PresetId::Straight, {}, {{"wmpEndBehavior", 2}, {"wmpBlur", 1}}, {10}, 0.1},
        {"ping-pong, travel direction", wmp::PresetId::Arc, {}, {{"wmpEndBehavior", 3}, {"wmpOrient", 1}}, {0, 3, 6, 9, 12, 15, 18, 21, 24}, 1.0 / 12},
        {"ease in-out, spiral", wmp::PresetId::Spiral, {}, {{"wmpEasing", 3}}, {0, 2, 4, 6, 8, 10, 12, 14, 16, 18, 20}, 0.05},
    };
    const int cols = 3, rows = 2, gap = 8;
    Canvas sheet(cols * TW + (cols + 1) * gap, rows * TH + (rows + 1) * gap);
    for (std::size_t i = 0; i < setups.size(); ++i) {
        const Setup& s = setups[i];
        const int ox = gap + static_cast<int>(i % cols) * (TW + gap), oy = gap + static_cast<int>(i / cols) * (TH + gap);
        for (int y = 0; y < TH; ++y)
            for (int x = 0; x < TW; ++x) {
                const float bg[4] = {0.09f, 0.10f, 0.12f, 1.0f};
                sheet.over(ox + x, oy + y, bg);
            }
        auto e = host.createInstance(TW, TH, 1.0, 30.0, 0, 300);
        host.setInt(e.get(), "wmpPreset", static_cast<int>(s.preset));
        host.pressButton(e.get(), "wmpApplyPreset");
        host.setKeys(e.get(), "wmpProgress", {mock::Key{0.0, {0.0}}, mock::Key{100.0, {100.0 * s.progressPerFrame}}});
        for (const auto& d : s.doubles) host.setDouble(e.get(), d.first, d.second);
        for (const auto& v : s.ints) host.setInt(e.get(), v.first, v.second);
        drawPath(sheet, ox, oy, host.getString(e.get(), "wmpPathData"));
        for (std::size_t f = 0; f < s.frames.size(); ++f) {
            mock::RenderRequest rq;
            rq.time = s.frames[f];
            rq.window[2] = TW;
            rq.window[3] = TH;
            mock::ImageSpec spec;
            spec.width = TW;
            spec.height = TH;
            mock::Image out;
            if (host.render(e.get(), rq, spec, out) != kOfxStatOK) return 1;
            const float fade = 0.35f + 0.65f * static_cast<float>(f + 1) / static_cast<float>(s.frames.size());
            blit(sheet, ox, oy, out, fade);
        }
        host.destroyInstance(e);
        std::fprintf(stderr, "tile %zu: %s\n", i + 1, s.title);
    }
    std::printf("P7\nWIDTH %d\nHEIGHT %d\nDEPTH 4\nMAXVAL 255\nTUPLTYPE RGB_ALPHA\nENDHDR\n", sheet.w, sheet.h);
    std::vector<unsigned char> row(static_cast<std::size_t>(sheet.w) * 4);
    for (int y = 0; y < sheet.h; ++y) {
        for (int x = 0; x < sheet.w; ++x) {
            const float* p = sheet.at(x, y);
            const float a = p[3];
            for (int c = 0; c < 3; ++c)
                row[static_cast<std::size_t>(x) * 4 + c] = static_cast<unsigned char>(std::clamp(a > 0 ? p[c] / a : 0.0f, 0.0f, 1.0f) * 255.0f + 0.5f);
            row[static_cast<std::size_t>(x) * 4 + 3] = static_cast<unsigned char>(std::clamp(a, 0.0f, 1.0f) * 255.0f + 0.5f);
        }
        std::fwrite(row.data(), 1, row.size(), stdout);
    }
    return 0;
}
