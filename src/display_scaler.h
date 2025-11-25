#ifndef DISPLAY_SCALER_H
#define DISPLAY_SCALER_H

#include <vector>

#include "geometry.h"

namespace fallout {

struct LogicalSpace {
    int width;
    int height;
};

struct PhysicalSpace {
    int width;
    int height;
};

struct DisplayScalerVirtualMapping {
    double exactX;
    double exactY;
    bool clampedLowX;
    bool clampedHighX;
    bool clampedLowY;
    bool clampedHighY;
    bool insideViewport;
    Rect viewport;
};

struct DisplayScalerAxisScaleTable {
    std::vector<int> starts;
    std::vector<int> ends;
};

struct DisplayScalerScaleTable {
    DisplayScalerAxisScaleTable horizontal;
    DisplayScalerAxisScaleTable vertical;
    Rect viewport;
    double scale;
    bool valid;
    bool hasFractionalScale;
    bool isDownscale;
};

void displayScalerInit(int logicalWidth, int logicalHeight);
void displayScalerSetLogicalSize(int logicalWidth, int logicalHeight);
void displayScalerSetIntegerScaling(bool enabled);
void displayScalerUpdatePhysicalSize(int physicalWidth, int physicalHeight);
const Rect& displayScalerGetLogicalBounds();
const Rect& displayScalerGetPhysicalViewport();
LogicalSpace displayScalerGetLogicalSpace();
PhysicalSpace displayScalerGetPhysicalSpace();
double displayScalerGetScale();
double displayScalerGetInverseScale();
Point displayScalerGetLetterboxOffset();
Point displayScalerLogicalToPhysical(const Point& logicalPoint);
Point displayScalerPhysicalToLogical(const Point& physicalPoint);
Rect displayScalerLogicalToPhysical(const Rect& logicalRect);
Rect displayScalerPhysicalToLogical(const Rect& physicalRect);
DisplayScalerVirtualMapping displayScalerMapPointToVirtual(int physicalX, int physicalY);
const DisplayScalerScaleTable& displayScalerGetScaleTable();

} // namespace fallout

#endif /* DISPLAY_SCALER_H */
