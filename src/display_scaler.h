#ifndef DISPLAY_SCALER_H
#define DISPLAY_SCALER_H

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
int displayScalerGetDefaultLogicalWidth();
int displayScalerGetDefaultLogicalHeight();
double displayScalerGetLogicalScaleX();
double displayScalerGetLogicalScaleY();

} // namespace fallout

#endif /* DISPLAY_SCALER_H */
