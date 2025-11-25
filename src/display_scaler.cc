#include "display_scaler.h"

#include <algorithm>
#include <cmath>

#include "diagnostics.h"
#include <fstream>
#include <iomanip>

namespace fallout {

namespace {

constexpr double kScaleEpsilon = 1.0e-4;

constexpr int kDefaultLogicalWidth = 640;
constexpr int kDefaultLogicalHeight = 480;

LogicalSpace gLogicalSpace = { kDefaultLogicalWidth, kDefaultLogicalHeight };
PhysicalSpace gPhysicalSpace = { kDefaultLogicalWidth, kDefaultLogicalHeight };
Rect gLogicalBounds = { 0, 0, kDefaultLogicalWidth - 1, kDefaultLogicalHeight - 1 };
Rect gPhysicalViewport = { 0, 0, kDefaultLogicalWidth - 1, kDefaultLogicalHeight - 1 };
Point gLetterboxOffset = { 0, 0 };
double gScale = 1.0;
double gInvScale = 1.0;
bool gUseIntegerScaling = false;
DisplayScalerScaleTable gScaleTable = {};
bool gScaleTablesDirty = true;

static void logScalerState(const char* reason)
{
    if (!diagnosticsWouldLog(DiagnosticsLevel::Info)) {
        return;
    }

    const int viewportWidth = gPhysicalViewport.right - gPhysicalViewport.left + 1;
    const int viewportHeight = gPhysicalViewport.bottom - gPhysicalViewport.top + 1;

    diagnosticsLog(
        DiagnosticsLevel::Info,
        "SCALER",
        "%s logical=%dx%d physical=%dx%d viewport=(%d,%d %dx%d) letterbox=%d,%d scale=%.4f invScale=%.4f integer=%d",
        reason != nullptr ? reason : "update",
        gLogicalSpace.width,
        gLogicalSpace.height,
        gPhysicalSpace.width,
        gPhysicalSpace.height,
        gPhysicalViewport.left,
        gPhysicalViewport.top,
        viewportWidth,
        viewportHeight,
        gLetterboxOffset.x,
        gLetterboxOffset.y,
        gScale,
        gInvScale,
        gUseIntegerScaling ? 1 : 0);
}

static void rebuildScaleTables()
{
    const int logicalWidth = std::max(1, gLogicalSpace.width);
    const int logicalHeight = std::max(1, gLogicalSpace.height);
    const int physicalRightBound = std::max(0, gPhysicalSpace.width - 1);
    const int physicalBottomBound = std::max(0, gPhysicalSpace.height - 1);

    gScaleTable.horizontal.starts.resize(logicalWidth);
    gScaleTable.horizontal.ends.resize(logicalWidth);
    gScaleTable.vertical.starts.resize(logicalHeight);
    gScaleTable.vertical.ends.resize(logicalHeight);

    for (int x = 0; x < logicalWidth; x++) {
        const double startEdge = static_cast<double>(gPhysicalViewport.left) + static_cast<double>(x) * gScale;
        const double endEdge = static_cast<double>(gPhysicalViewport.left) + static_cast<double>(x + 1) * gScale;

        int start = std::clamp(static_cast<int>(std::floor(startEdge)), 0, physicalRightBound);
        int end = std::clamp(static_cast<int>(std::ceil(endEdge) - 1), 0, physicalRightBound);
        if (end < start) {
            end = start;
        }

        gScaleTable.horizontal.starts[x] = start;
        gScaleTable.horizontal.ends[x] = end;
    }

    for (int y = 0; y < logicalHeight; y++) {
        const double startEdge = static_cast<double>(gPhysicalViewport.top) + static_cast<double>(y) * gScale;
        const double endEdge = static_cast<double>(gPhysicalViewport.top) + static_cast<double>(y + 1) * gScale;

        int start = std::clamp(static_cast<int>(std::floor(startEdge)), 0, physicalBottomBound);
        int end = std::clamp(static_cast<int>(std::ceil(endEdge) - 1), 0, physicalBottomBound);
        if (end < start) {
            end = start;
        }

        gScaleTable.vertical.starts[y] = start;
        gScaleTable.vertical.ends[y] = end;
    }

    gScaleTable.viewport = gPhysicalViewport;
    gScaleTable.scale = gScale;
    gScaleTable.hasFractionalScale = std::abs(gScale - std::round(gScale)) > kScaleEpsilon;
    gScaleTable.isDownscale = gScale < 1.0 - kScaleEpsilon;
    gScaleTable.valid = true;
    gScaleTablesDirty = false;
}

static void updateViewport()
{
    if (gLogicalSpace.width <= 0 || gLogicalSpace.height <= 0) {
        gLogicalSpace.width = kDefaultLogicalWidth;
        gLogicalSpace.height = kDefaultLogicalHeight;
    }

    if (gPhysicalSpace.width <= 0) {
        gPhysicalSpace.width = kDefaultLogicalWidth;
    }

    if (gPhysicalSpace.height <= 0) {
        gPhysicalSpace.height = kDefaultLogicalHeight;
    }

    const double scaleX = static_cast<double>(gPhysicalSpace.width) / static_cast<double>(gLogicalSpace.width);
    const double scaleY = static_cast<double>(gPhysicalSpace.height) / static_cast<double>(gLogicalSpace.height);

    gScale = std::min(scaleX, scaleY);
    if (gUseIntegerScaling && gScale >= 1.0) {
        gScale = std::floor(gScale);
    }
    if (gScale <= 0.0) {
        gScale = 1.0;
    }

    gInvScale = 1.0 / gScale;

    const int viewportWidth = std::max(1, static_cast<int>(std::round(gLogicalSpace.width * gScale)));
    const int viewportHeight = std::max(1, static_cast<int>(std::round(gLogicalSpace.height * gScale)));

    gLetterboxOffset.x = (gPhysicalSpace.width - viewportWidth) / 2;
    gLetterboxOffset.y = (gPhysicalSpace.height - viewportHeight) / 2;

    gPhysicalViewport.left = gLetterboxOffset.x;
    gPhysicalViewport.top = gLetterboxOffset.y;
    gPhysicalViewport.right = gPhysicalViewport.left + viewportWidth - 1;
    gPhysicalViewport.bottom = gPhysicalViewport.top + viewportHeight - 1;

    gLogicalBounds.left = 0;
    gLogicalBounds.top = 0;
    gLogicalBounds.right = gLogicalSpace.width - 1;
    gLogicalBounds.bottom = gLogicalSpace.height - 1;

    gScaleTablesDirty = true;
    rebuildScaleTables();
    logScalerState("viewport");
}

static int clampToLogicalX(double value)
{
    const int rounded = static_cast<int>(std::round(value));
    return std::clamp(rounded, gLogicalBounds.left, gLogicalBounds.right);
}

static int clampToLogicalY(double value)
{
    const int rounded = static_cast<int>(std::round(value));
    return std::clamp(rounded, gLogicalBounds.top, gLogicalBounds.bottom);
}

static int clampToPhysicalX(double value)
{
    const int rounded = static_cast<int>(std::round(value));
    return std::clamp(rounded, 0, gPhysicalSpace.width - 1);
}

static int clampToPhysicalY(double value)
{
    const int rounded = static_cast<int>(std::round(value));
    return std::clamp(rounded, 0, gPhysicalSpace.height - 1);
}

} // namespace

void displayScalerInit(int logicalWidth, int logicalHeight)
{
    gLogicalSpace.width = std::max(1, logicalWidth);
    gLogicalSpace.height = std::max(1, logicalHeight);
    gPhysicalSpace.width = std::max(gPhysicalSpace.width, gLogicalSpace.width);
    gPhysicalSpace.height = std::max(gPhysicalSpace.height, gLogicalSpace.height);
    updateViewport();
}

void displayScalerSetLogicalSize(int logicalWidth, int logicalHeight)
{
    gLogicalSpace.width = std::max(1, logicalWidth);
    gLogicalSpace.height = std::max(1, logicalHeight);
    updateViewport();
}

void displayScalerSetIntegerScaling(bool enabled)
{
    gUseIntegerScaling = enabled;
    updateViewport();
}

void displayScalerUpdatePhysicalSize(int physicalWidth, int physicalHeight)
{
    gPhysicalSpace.width = std::max(1, physicalWidth);
    gPhysicalSpace.height = std::max(1, physicalHeight);
    updateViewport();
}

const Rect& displayScalerGetLogicalBounds()
{
    return gLogicalBounds;
}

const Rect& displayScalerGetPhysicalViewport()
{
    return gPhysicalViewport;
}

LogicalSpace displayScalerGetLogicalSpace()
{
    return gLogicalSpace;
}

PhysicalSpace displayScalerGetPhysicalSpace()
{
    return gPhysicalSpace;
}

double displayScalerGetScale()
{
    return gScale;
}

double displayScalerGetInverseScale()
{
    return gInvScale;
}

Point displayScalerGetLetterboxOffset()
{
    return gLetterboxOffset;
}

Point displayScalerLogicalToPhysical(const Point& logicalPoint)
{
    Point result;
    result.x = clampToPhysicalX(gPhysicalViewport.left + logicalPoint.x * gScale);
    result.y = clampToPhysicalY(gPhysicalViewport.top + logicalPoint.y * gScale);
    return result;
}

Point displayScalerPhysicalToLogical(const Point& physicalPoint)
{
    const double localX = static_cast<double>(physicalPoint.x - gPhysicalViewport.left);
    const double localY = static_cast<double>(physicalPoint.y - gPhysicalViewport.top);

    Point result;
    result.x = clampToLogicalX(localX * gInvScale);
    result.y = clampToLogicalY(localY * gInvScale);
    return result;
}

Rect displayScalerLogicalToPhysical(const Rect& logicalRect)
{
    const double leftEdge = gPhysicalViewport.left + logicalRect.left * gScale;
    const double topEdge = gPhysicalViewport.top + logicalRect.top * gScale;
    const double rightEdge = gPhysicalViewport.left + (logicalRect.right + 1) * gScale;
    const double bottomEdge = gPhysicalViewport.top + (logicalRect.bottom + 1) * gScale;

    Rect result;
    const int physicalRightBound = std::max(0, gPhysicalSpace.width - 1);
    const int physicalBottomBound = std::max(0, gPhysicalSpace.height - 1);

    result.left = std::clamp(static_cast<int>(std::floor(leftEdge)), 0, physicalRightBound);
    result.top = std::clamp(static_cast<int>(std::floor(topEdge)), 0, physicalBottomBound);
    result.right = std::clamp(static_cast<int>(std::ceil(rightEdge) - 1), 0, physicalRightBound);
    result.bottom = std::clamp(static_cast<int>(std::ceil(bottomEdge) - 1), 0, physicalBottomBound);

    if (result.left > result.right) {
        result.left = result.right;
    }

    if (result.top > result.bottom) {
        result.top = result.bottom;
    }

    return result;
}

Rect displayScalerPhysicalToLogical(const Rect& physicalRect)
{
    Point topLeft = { physicalRect.left, physicalRect.top };
    Point bottomRight = { physicalRect.right, physicalRect.bottom };

    Point logicalTopLeft = displayScalerPhysicalToLogical(topLeft);
    Point logicalBottomRight = displayScalerPhysicalToLogical(bottomRight);

    Rect result;
    result.left = std::min(logicalTopLeft.x, logicalBottomRight.x);
    result.top = std::min(logicalTopLeft.y, logicalBottomRight.y);
    result.right = std::max(logicalTopLeft.x, logicalBottomRight.x);
    result.bottom = std::max(logicalTopLeft.y, logicalBottomRight.y);
    return result;
}

DisplayScalerVirtualMapping displayScalerMapPointToVirtual(int physicalX, int physicalY)
{
    DisplayScalerVirtualMapping mapping = {};
    mapping.viewport = gPhysicalViewport;

    const Rect& viewport = gPhysicalViewport;
    const LogicalSpace logicalSpace = gLogicalSpace;

    const double logicalMaxX = static_cast<double>(logicalSpace.width - 1);
    const double logicalMaxY = static_cast<double>(logicalSpace.height - 1);

    const bool hitViewportLeft = physicalX <= viewport.left;
    const bool hitViewportRight = physicalX >= viewport.right;
    const bool hitViewportTop = physicalY <= viewport.top;
    const bool hitViewportBottom = physicalY >= viewport.bottom;
    mapping.insideViewport = !hitViewportLeft && !hitViewportRight && !hitViewportTop && !hitViewportBottom;

    const double localX = static_cast<double>(physicalX - viewport.left);
    const double localY = static_cast<double>(physicalY - viewport.top);

    const double rawLogicalExactX = localX * gInvScale;
    const double rawLogicalExactY = localY * gInvScale;

    mapping.exactX = std::clamp(rawLogicalExactX, 0.0, logicalMaxX);
    mapping.exactY = std::clamp(rawLogicalExactY, 0.0, logicalMaxY);

    mapping.clampedLowX = mapping.exactX <= 0.0;
    mapping.clampedHighX = mapping.exactX >= logicalMaxX;
    mapping.clampedLowY = mapping.exactY <= 0.0;
    mapping.clampedHighY = mapping.exactY >= logicalMaxY;

    if (hitViewportLeft) {
        mapping.exactX = 0.0;
        mapping.clampedLowX = true;
        mapping.clampedHighX = false;
    } else if (hitViewportRight) {
        mapping.exactX = logicalMaxX;
        mapping.clampedHighX = true;
        mapping.clampedLowX = false;
    }

    if (hitViewportTop) {
        mapping.exactY = 0.0;
        mapping.clampedLowY = true;
        mapping.clampedHighY = false;
    } else if (hitViewportBottom) {
        mapping.exactY = logicalMaxY;
        mapping.clampedHighY = true;
        mapping.clampedLowY = false;
    }

    return mapping;
}

const DisplayScalerScaleTable& displayScalerGetScaleTable()
{
    if (gScaleTablesDirty) {
        rebuildScaleTables();
    }

    return gScaleTable;
}

} // namespace fallout
