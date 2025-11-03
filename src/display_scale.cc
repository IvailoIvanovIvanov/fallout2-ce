#include "display_scale.h"

#include <algorithm>

namespace fallout {

static int g_logicalW = 640;
static int g_logicalH = 480;
static int g_windowW = 640;
static int g_windowH = 480;
static int g_scale = 1;
static float g_dpiScale = 1.0f;

void displayScaleInit(int logicalWidth, int logicalHeight, int windowWidth, int windowHeight)
{
    g_logicalW = std::max(1, logicalWidth);
    g_logicalH = std::max(1, logicalHeight);
    g_windowW = std::max(1, windowWidth);
    g_windowH = std::max(1, windowHeight);

    int scaleX = g_windowW / g_logicalW;
    int scaleY = g_windowH / g_logicalH;
    int scale = std::min(scaleX, scaleY);
    if (scale < 1) scale = 1;
    g_scale = scale;

    g_dpiScale = static_cast<float>(g_windowW) / static_cast<float>(g_logicalW);
}

int displayGetScale()
{
    return g_scale;
}

float displayGetDpiScale()
{
    return g_dpiScale;
}

int logicalToPhysicalX(int x) { return x * g_scale; }
int logicalToPhysicalY(int y) { return y * g_scale; }
int physicalToLogicalX(int x) { return x / g_scale; }
int physicalToLogicalY(int y) { return y / g_scale; }

DsRect logicalRectToPhysicalRect(DsRect r)
{
    DsRect out;
    out.x = r.x * g_scale;
    out.y = r.y * g_scale;
    out.w = r.w * g_scale;
    out.h = r.h * g_scale;
    return out;
}

DsRect physicalRectToLogicalRect(DsRect r)
{
    DsRect out;
    out.x = r.x / g_scale;
    out.y = r.y / g_scale;
    out.w = r.w / g_scale;
    out.h = r.h / g_scale;
    return out;
}

} // namespace fallout
