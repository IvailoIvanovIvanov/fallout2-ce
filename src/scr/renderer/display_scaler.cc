#include "display_scaler.h"

#include <algorithm>
#include <cmath>
#include "logger.h"

namespace fallout {

namespace {

//-----------------------------------------------------------------------------
// Constants
//-----------------------------------------------------------------------------

constexpr int kDefaultLogicalWidth = 640;
constexpr int kDefaultLogicalHeight = 480;

//-----------------------------------------------------------------------------
// Global State
//-----------------------------------------------------------------------------

LogicalSpace gLogicalSpace = {kDefaultLogicalWidth, kDefaultLogicalHeight};
PhysicalSpace gPhysicalSpace = {kDefaultLogicalWidth, kDefaultLogicalHeight};
Rect gLogicalBounds = {0, 0, kDefaultLogicalWidth - 1, kDefaultLogicalHeight - 1};
Rect gPhysicalViewport = {0, 0, kDefaultLogicalWidth - 1, kDefaultLogicalHeight - 1};
Point gLetterboxOffset = {0, 0};
double gScale = 1.0;
double gInvScale = 1.0;

//-----------------------------------------------------------------------------
// Internal Helper Functions
//-----------------------------------------------------------------------------

/**
 * @brief Logs the current scaler state for debugging.
 */
void logScalerState(const char* reason) {
    const int viewportWidth = gPhysicalViewport.right - gPhysicalViewport.left + 1;
    const int viewportHeight = gPhysicalViewport.bottom - gPhysicalViewport.top + 1;

    renderer::Logger::Log(
        renderer::LogLevel::Info,
        "SCALER: %s logical=%dx%d physical=%dx%d viewport=(%d,%d %dx%d) scale=%.4f",
        reason != nullptr ? reason : "update",
        gLogicalSpace.width, gLogicalSpace.height,
        gPhysicalSpace.width, gPhysicalSpace.height,
        gPhysicalViewport.left, gPhysicalViewport.top,
        viewportWidth, viewportHeight,
        gScale);
}

/**
 * @brief Ensures dimensions are valid, applying defaults if needed.
 */
void validateDimensions() {
    if (gLogicalSpace.width <= 0) gLogicalSpace.width = kDefaultLogicalWidth;
    if (gLogicalSpace.height <= 0) gLogicalSpace.height = kDefaultLogicalHeight;
    if (gPhysicalSpace.width <= 0) gPhysicalSpace.width = kDefaultLogicalWidth;
    if (gPhysicalSpace.height <= 0) gPhysicalSpace.height = kDefaultLogicalHeight;
}

/**
 * @brief Calculates the uniform scale factor to fit logical in physical.
 */
void calculateScale() {
    const double scaleX = static_cast<double>(gPhysicalSpace.width) / gLogicalSpace.width;
    const double scaleY = static_cast<double>(gPhysicalSpace.height) / gLogicalSpace.height;
    gScale = std::min(scaleX, scaleY);
    if (gScale <= 0.0) gScale = 1.0;
    gInvScale = 1.0 / gScale;
}

/**
 * @brief Calculates viewport position and letterbox offset.
 */
void calculateViewport() {
    const int viewportWidth = std::max(1, static_cast<int>(std::round(gLogicalSpace.width * gScale)));
    const int viewportHeight = std::max(1, static_cast<int>(std::round(gLogicalSpace.height * gScale)));

    gLetterboxOffset.x = (gPhysicalSpace.width - viewportWidth) / 2;
    gLetterboxOffset.y = (gPhysicalSpace.height - viewportHeight) / 2;

    gPhysicalViewport.left = gLetterboxOffset.x;
    gPhysicalViewport.top = gLetterboxOffset.y;
    gPhysicalViewport.right = gPhysicalViewport.left + viewportWidth - 1;
    gPhysicalViewport.bottom = gPhysicalViewport.top + viewportHeight - 1;
}

/**
 * @brief Updates logical bounds based on current logical space.
 */
void updateLogicalBounds() {
    gLogicalBounds.left = 0;
    gLogicalBounds.top = 0;
    gLogicalBounds.right = gLogicalSpace.width - 1;
    gLogicalBounds.bottom = gLogicalSpace.height - 1;
}

/**
 * @brief Recalculates all viewport and scaling parameters.
 */
void updateViewport() {
    validateDimensions();
    calculateScale();
    calculateViewport();
    updateLogicalBounds();
    logScalerState("viewport");
}

//-----------------------------------------------------------------------------
// Clamping Helpers
//-----------------------------------------------------------------------------

int clampToLogicalX(double value) {
    return std::clamp(static_cast<int>(std::round(value)), 
                      gLogicalBounds.left, gLogicalBounds.right);
}

int clampToLogicalY(double value) {
    return std::clamp(static_cast<int>(std::round(value)), 
                      gLogicalBounds.top, gLogicalBounds.bottom);
}

int clampToPhysicalX(double value) {
    return std::clamp(static_cast<int>(std::round(value)), 0, gPhysicalSpace.width - 1);
}

int clampToPhysicalY(double value) {
    return std::clamp(static_cast<int>(std::round(value)), 0, gPhysicalSpace.height - 1);
}

} // namespace

//-----------------------------------------------------------------------------
// Initialization Functions
//-----------------------------------------------------------------------------

void displayScalerInit(int logicalWidth, int logicalHeight) {
    gLogicalSpace.width = std::max(1, logicalWidth);
    gLogicalSpace.height = std::max(1, logicalHeight);
    gPhysicalSpace.width = std::max(gPhysicalSpace.width, gLogicalSpace.width);
    gPhysicalSpace.height = std::max(gPhysicalSpace.height, gLogicalSpace.height);
    updateViewport();
}

void displayScalerSetLogicalSize(int logicalWidth, int logicalHeight) {
    gLogicalSpace.width = std::max(1, logicalWidth);
    gLogicalSpace.height = std::max(1, logicalHeight);
    updateViewport();
}

void displayScalerUpdatePhysicalSize(int physicalWidth, int physicalHeight) {
    gPhysicalSpace.width = std::max(1, physicalWidth);
    gPhysicalSpace.height = std::max(1, physicalHeight);
    updateViewport();
}

//-----------------------------------------------------------------------------
// Accessor Functions
//-----------------------------------------------------------------------------

const Rect& displayScalerGetLogicalBounds() {
    return gLogicalBounds;
}

const Rect& displayScalerGetPhysicalViewport() {
    return gPhysicalViewport;
}

LogicalSpace displayScalerGetLogicalSpace() {
    return gLogicalSpace;
}

PhysicalSpace displayScalerGetPhysicalSpace() {
    return gPhysicalSpace;
}

double displayScalerGetScale() {
    return gScale;
}

double displayScalerGetInverseScale() {
    return gInvScale;
}

Point displayScalerGetLetterboxOffset() {
    return gLetterboxOffset;
}

//-----------------------------------------------------------------------------
// Point Transformation Functions
//-----------------------------------------------------------------------------

Point displayScalerLogicalToPhysical(const Point& logicalPoint) {
    Point result;
    result.x = clampToPhysicalX(gPhysicalViewport.left + logicalPoint.x * gScale);
    result.y = clampToPhysicalY(gPhysicalViewport.top + logicalPoint.y * gScale);
    return result;
}

Point displayScalerPhysicalToLogical(const Point& physicalPoint) {
    const double localX = static_cast<double>(physicalPoint.x - gPhysicalViewport.left);
    const double localY = static_cast<double>(physicalPoint.y - gPhysicalViewport.top);

    Point result;
    result.x = clampToLogicalX(localX * gInvScale);
    result.y = clampToLogicalY(localY * gInvScale);
    return result;
}

//-----------------------------------------------------------------------------
// Rectangle Transformation Functions
//-----------------------------------------------------------------------------

Rect displayScalerLogicalToPhysical(const Rect& logicalRect) {
    const double leftEdge = gPhysicalViewport.left + logicalRect.left * gScale;
    const double topEdge = gPhysicalViewport.top + logicalRect.top * gScale;
    const double rightEdge = gPhysicalViewport.left + (logicalRect.right + 1) * gScale;
    const double bottomEdge = gPhysicalViewport.top + (logicalRect.bottom + 1) * gScale;

    const int physicalRightBound = std::max(0, gPhysicalSpace.width - 1);
    const int physicalBottomBound = std::max(0, gPhysicalSpace.height - 1);

    Rect result;
    result.left = std::clamp(static_cast<int>(std::floor(leftEdge)), 0, physicalRightBound);
    result.top = std::clamp(static_cast<int>(std::floor(topEdge)), 0, physicalBottomBound);
    result.right = std::clamp(static_cast<int>(std::ceil(rightEdge) - 1), 0, physicalRightBound);
    result.bottom = std::clamp(static_cast<int>(std::ceil(bottomEdge) - 1), 0, physicalBottomBound);

    // Ensure valid rectangle (left <= right, top <= bottom)
    if (result.left > result.right) result.left = result.right;
    if (result.top > result.bottom) result.top = result.bottom;

    return result;
}

Rect displayScalerPhysicalToLogical(const Rect& physicalRect) {
    Point logicalTopLeft = displayScalerPhysicalToLogical(Point{physicalRect.left, physicalRect.top});
    Point logicalBottomRight = displayScalerPhysicalToLogical(Point{physicalRect.right, physicalRect.bottom});

    Rect result;
    result.left = std::min(logicalTopLeft.x, logicalBottomRight.x);
    result.top = std::min(logicalTopLeft.y, logicalBottomRight.y);
    result.right = std::max(logicalTopLeft.x, logicalBottomRight.x);
    result.bottom = std::max(logicalTopLeft.y, logicalBottomRight.y);
    return result;
}

//-----------------------------------------------------------------------------
// Virtual Mapping (for Mouse Input)
//-----------------------------------------------------------------------------

DisplayScalerVirtualMapping displayScalerMapPointToVirtual(int physicalX, int physicalY) {
    DisplayScalerVirtualMapping mapping = {};
    mapping.viewport = gPhysicalViewport;

    const double logicalMaxX = static_cast<double>(gLogicalSpace.width - 1);
    const double logicalMaxY = static_cast<double>(gLogicalSpace.height - 1);

    // Edge detection
    const bool hitLeft = physicalX <= gPhysicalViewport.left;
    const bool hitRight = physicalX >= gPhysicalViewport.right;
    const bool hitTop = physicalY <= gPhysicalViewport.top;
    const bool hitBottom = physicalY >= gPhysicalViewport.bottom;
    
    mapping.insideViewport = !hitLeft && !hitRight && !hitTop && !hitBottom;

    // Calculate local coordinates within viewport
    const double localX = static_cast<double>(physicalX - gPhysicalViewport.left);
    const double localY = static_cast<double>(physicalY - gPhysicalViewport.top);

    // Convert to logical coordinates
    mapping.exactX = std::clamp(localX * gInvScale, 0.0, logicalMaxX);
    mapping.exactY = std::clamp(localY * gInvScale, 0.0, logicalMaxY);

    // Set edge flags
    mapping.clampedLowX = mapping.exactX <= 0.0;
    mapping.clampedHighX = mapping.exactX >= logicalMaxX;
    mapping.clampedLowY = mapping.exactY <= 0.0;
    mapping.clampedHighY = mapping.exactY >= logicalMaxY;

    // Override for viewport edge cases
    if (hitLeft) {
        mapping.exactX = 0.0;
        mapping.clampedLowX = true;
        mapping.clampedHighX = false;
    } else if (hitRight) {
        mapping.exactX = logicalMaxX;
        mapping.clampedHighX = true;
        mapping.clampedLowX = false;
    }

    if (hitTop) {
        mapping.exactY = 0.0;
        mapping.clampedLowY = true;
        mapping.clampedHighY = false;
    } else if (hitBottom) {
        mapping.exactY = logicalMaxY;
        mapping.clampedHighY = true;
        mapping.clampedLowY = false;
    }

    return mapping;
}

} // namespace fallout
