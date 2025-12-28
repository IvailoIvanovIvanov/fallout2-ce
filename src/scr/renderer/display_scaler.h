#ifndef DISPLAY_SCALER_H
#define DISPLAY_SCALER_H

/**
 * @file display_scaler.h
 * @brief Display coordinate transformation and scaling utilities.
 *
 * Provides functions for mapping between logical (game) coordinates
 * and physical (screen) coordinates, handling aspect ratio preservation
 * with letterboxing/pillarboxing.
 */

#include <vector>
#include "../../geometry.h"

namespace fallout {

//-----------------------------------------------------------------------------
// Coordinate Space Types
//-----------------------------------------------------------------------------

/**
 * @struct LogicalSpace
 * @brief Represents the game's internal resolution (e.g., 640x480).
 */
struct LogicalSpace {
    int width;
    int height;
};

/**
 * @struct PhysicalSpace
 * @brief Represents the actual screen/window resolution.
 */
struct PhysicalSpace {
    int width;
    int height;
};

/**
 * @struct DisplayScalerVirtualMapping
 * @brief Result of mapping a physical point to logical coordinates.
 *
 * Contains the exact floating-point logical coordinates and edge detection
 * flags for mouse/input handling.
 */
struct DisplayScalerVirtualMapping {
    double exactX;          ///< Exact X coordinate in logical space
    double exactY;          ///< Exact Y coordinate in logical space
    bool clampedLowX;       ///< True if point was clamped to left edge
    bool clampedHighX;      ///< True if point was clamped to right edge
    bool clampedLowY;       ///< True if point was clamped to top edge
    bool clampedHighY;      ///< True if point was clamped to bottom edge
    bool insideViewport;    ///< True if point is within the game viewport
    Rect viewport;          ///< The physical viewport rectangle
};

//-----------------------------------------------------------------------------
// Initialization
//-----------------------------------------------------------------------------

/**
 * @brief Initializes the display scaler with the logical (game) resolution.
 * @param logicalWidth Game's internal width in pixels.
 * @param logicalHeight Game's internal height in pixels.
 */
void displayScalerInit(int logicalWidth, int logicalHeight);

/**
 * @brief Updates the logical size (typically when game resolution changes).
 */
void displayScalerSetLogicalSize(int logicalWidth, int logicalHeight);

/**
 * @brief Updates the physical size (window or screen resolution).
 */
void displayScalerUpdatePhysicalSize(int physicalWidth, int physicalHeight);

//-----------------------------------------------------------------------------
// Accessors
//-----------------------------------------------------------------------------

/**
 * @brief Returns the logical coordinate bounds (0,0 to width-1,height-1).
 */
const Rect& displayScalerGetLogicalBounds();

/**
 * @brief Returns the physical viewport rectangle where the game is rendered.
 */
const Rect& displayScalerGetPhysicalViewport();

/**
 * @brief Returns the current logical space dimensions.
 */
LogicalSpace displayScalerGetLogicalSpace();

/**
 * @brief Returns the current physical space dimensions.
 */
PhysicalSpace displayScalerGetPhysicalSpace();

/**
 * @brief Returns the scale factor from logical to physical coordinates.
 */
double displayScalerGetScale();

/**
 * @brief Returns the inverse scale factor (physical to logical).
 */
double displayScalerGetInverseScale();

/**
 * @brief Returns the letterbox/pillarbox offset in physical pixels.
 */
Point displayScalerGetLetterboxOffset();

//-----------------------------------------------------------------------------
// Coordinate Transformation
//-----------------------------------------------------------------------------

/**
 * @brief Converts a logical point to physical screen coordinates.
 */
Point displayScalerLogicalToPhysical(const Point& logicalPoint);

/**
 * @brief Converts a physical screen point to logical game coordinates.
 */
Point displayScalerPhysicalToLogical(const Point& physicalPoint);

/**
 * @brief Converts a logical rectangle to physical screen coordinates.
 */
Rect displayScalerLogicalToPhysical(const Rect& logicalRect);

/**
 * @brief Converts a physical screen rectangle to logical game coordinates.
 */
Rect displayScalerPhysicalToLogical(const Rect& physicalRect);

/**
 * @brief Maps a physical screen point to virtual coordinates with edge detection.
 *
 * Used for mouse input handling to detect when cursor is at screen edges
 * (for edge scrolling) or outside the viewport.
 */
DisplayScalerVirtualMapping displayScalerMapPointToVirtual(int physicalX, int physicalY);

} // namespace fallout

#endif /* DISPLAY_SCALER_H */
