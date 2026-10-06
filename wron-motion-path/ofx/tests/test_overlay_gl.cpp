// Draws the overlay through the legacy OpenGL path (OverlayInteractV1) into an
// off-screen OSMesa context and checks the pixels. Mock host, not VEGAS.
#include <GL/osmesa.h>

#include <cmath>
#include <cstdio>
#include <vector>

#include "../../core/tests/test.h"
#include "mock_host.h"

namespace {
constexpr int W = 640, H = 360;

bool lit(const std::vector<unsigned char>& px, int x, int y) {
    const unsigned char* p = &px[(static_cast<std::size_t>(y) * W + x) * 4];
    return p[0] + p[1] + p[2] > 60;
}
}  // namespace

TEST(overlay_opengl_draws_path_and_points) {
    mock::Host host;
    host.provideDrawSuite = false;  // force the OpenGL (V1) entry point
    std::string err;
    CHECK(host.load(WMP_PLUGIN_PATH, err));
    if (!host.plugin()) return;
    CHECK(host.descriptor()->props.getPointer(kOfxImageEffectPluginPropOverlayInteractV1) != nullptr);
    auto e = host.createInstance(W, H, 1.0, 30.0, 0, 100);

    OSMesaContext ctx = OSMesaCreateContextExt(OSMESA_RGBA, 16, 0, 0, nullptr);
    CHECK(ctx != nullptr);
    std::vector<unsigned char> buffer(static_cast<std::size_t>(W) * H * 4, 0);
    CHECK(OSMesaMakeCurrent(ctx, buffer.data(), GL_UNSIGNED_BYTE, W, H));
    // Hosts set up canonical coordinates for overlay drawing.
    glViewport(0, 0, W, H);
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrtho(0, W, 0, H, -1, 1);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);

    auto it = host.createInteract(e.get());
    mock::PropertySet in;
    in.setDouble(kOfxPropTime, 0);
    in.setDouble(kOfxInteractPropPixelScale, 1.0, 0);
    in.setDouble(kOfxInteractPropPixelScale, 1.0, 1);
    CHECK(host.interactAction(it.get(), kOfxInteractActionDraw, in) == kOfxStatOK);
    glFinish();

    // Default path: straight line at y = 180 from x = 128 to 512 (OSMesa rows are bottom-up like GL).
    int litOnLine = 0;
    for (int x = 140; x < 500; x += 10) litOnLine += lit(buffer, x, 180) ? 1 : 0;
    CHECK(litOnLine >= 34);
    CHECK(lit(buffer, 128, 180));   // start anchor
    CHECK(lit(buffer, 512, 180));   // end anchor
    CHECK(!lit(buffer, 320, 60));   // nothing drawn elsewhere
    CHECK(!lit(buffer, 50, 300));
    std::printf("  lit samples on the path: %d/36\n", litOnLine);

    OSMesaDestroyContext(ctx);
    host.destroyInstance(e);
}
