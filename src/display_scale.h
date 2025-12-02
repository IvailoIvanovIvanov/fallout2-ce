#ifndef DISPLAY_SCALE_H
#define DISPLAY_SCALE_H

namespace fallout {

// Initializes the logical/physical mapping. Call once after window creation
// (provide logical game resolution and actual window size in pixels).
void displayScaleInit(int logicalWidth, int logicalHeight, int windowWidth, int windowHeight);

// Preferred integer UI scale (1, 2, 3, 4 ...). Always >= 1.
int displayGetScale();

// Optional float scale if you later support non-integer scaling.
float displayGetDpiScale();

// Coordinate conversion helpers.
int logicalToPhysicalX(int x);
int logicalToPhysicalY(int y);
int physicalToLogicalX(int x);
int physicalToLogicalY(int y);

struct DsRect { int x; int y; int w; int h; };
DsRect logicalRectToPhysicalRect(DsRect r);
DsRect physicalRectToLogicalRect(DsRect r);

} // namespace fallout

#endif // DISPLAY_SCALE_H
