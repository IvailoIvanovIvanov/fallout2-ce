# Coordinate System Design

## Overview

The engine uses **two coordinate spaces** to enable upscaling while preserving original game logic:

1. **Logical Coordinates (640×480)** – The phantom display space where all game logic operates
2. **Physical Coordinates (variable)** – The real display space matching the actual window/screen resolution

All coordinate conversions flow through `display_scaler.cc` to maintain a clean abstraction boundary.

---

## Logical Coordinates

**Definition:** Always expressed in 640×480 space, regardless of actual window size.

**Used by:**
- Game logic (movement, AI, pathfinding)
- Object positions (`Object::tile`, screen rects in `_screen_buffer`)
- Input events delivered to game code (`game_mouse`, `input`)
- Render commands (`RenderCommandTileBlit::screenRect`, etc.)
- Window manager virtual screen (`_screen_buffer`, dirty rects)

**Contract:**
```cpp
// Logical space boundaries
const Rect& logicalBounds = displayScalerGetLogicalBounds();
// Always: logicalBounds = {0, 0, 639, 479}

// Example logical coordinate
LogicalCoordinate cursor = {320, 240}; // Center of 640×480 screen
```

**Files that operate in logical space:**
- `game_mouse.cc` – All mouse hit tests, cursor positioning
- `tile.cc` – Tile rendering, hex cursor placement
- `window_manager.cc` – Virtual screen buffer operations
- `object.cc` – Object screen positions
- `art.cc` – Frame blitting to `_screen_buffer`

---

## Physical Coordinates

**Definition:** Window pixel coordinates matching the actual SDL window size (e.g., 1920×1080).

**Used by:**
- SDL event system (`SDL_MouseMotionEvent::x/y`)
- Display output (`SDL_RenderPresent`, texture blitting)
- Viewport calculations (letterboxing, scale tables)

**Contract:**
```cpp
// Physical space boundaries
const Rect& viewport = displayScalerGetPhysicalViewport();
PhysicalSpace space = displayScalerGetPhysicalSpace();
// Example: viewport = {320, 0, 1599, 1079} for 1920×1080 with letterbox
// space.width = 1920, space.height = 1080

// Example physical coordinate
PhysicalCoordinate windowClick = {960, 540}; // Center of 1920×1080 window
```

**Files that operate in physical space:**
- `svga.cc` – SDL presenter, texture management
- `virtual_input.cc` – Raw event capture
- `display_scaler.cc` – Coordinate conversion logic
- `render_display_orchestrator.cc` – Physical overlay compositing

---

## Coordinate Conversion

### Input Flow (Physical → Logical)

```
SDL_Event (physical window coords)
  ↓
virtualInputCaptureEvent() [virtual_input.cc]
  ↓
displayScalerMapPointToVirtual() [display_scaler.cc]
  ↓
mouseDeviceGetData() [mouse.cc]
  ↓
game_mouse / input (logical coords)
```

**Key function:**
```cpp
DisplayScalerVirtualMapping displayScalerMapPointToVirtual(int physicalX, int physicalY);
// Returns:
// - exactX/exactY: sub-pixel logical coordinates (double precision)
// - insideViewport: whether click was inside game area (not letterbox)
// - clamp flags: whether coordinate hit logical bounds
```

### Output Flow (Logical → Physical)

```
Render command (logical rect)
  ↓
Display orchestrator
  ↓
displayScalerGetScaleTable() [display_scaler.cc]
  ↓
Physical overlay buffers (viewport-sized)
  ↓
SDL presenter (physical coords)
```

**Key functions:**
```cpp
Point displayScalerLogicalToPhysical(const Point& logicalPoint);
Rect displayScalerLogicalToPhysical(const Rect& logicalRect);

// Scale tables for efficient batch conversion
const DisplayScalerScaleTable& displayScalerGetScaleTable();
// Provides per-axis span arrays: logical column → physical start/end pixels
```

---

## Scale Factor & Viewport

### Scale Calculation

```cpp
double scale = displayScalerGetScale();
// Computed as: min(physicalWidth / 640.0, physicalHeight / 480.0)
// Example: 1920×1080 → scale = min(3.0, 2.25) = 2.25
//          With integer scaling → floor(2.25) = 2.0

double invScale = displayScalerGetInverseScale();
// invScale = 1.0 / scale, used for physical → logical conversion
```

### Viewport & Letterboxing

The viewport is the subset of physical space where the game renders (excludes letterbox bars):

```cpp
const Rect& viewport = displayScalerGetPhysicalViewport();
// Example for 1920×1080 at 2.0 scale:
//   Viewport width  = 640 * 2.0 = 1280
//   Viewport height = 480 * 2.0 = 960
//   Letterbox offset X = (1920 - 1280) / 2 = 320
//   Letterbox offset Y = (1080 - 960) / 2 = 60
//   viewport = {320, 60, 1599, 1019}
```

### Scale Tables (Stage 5)

For efficient pixel expansion without SDL interpolation:

```cpp
const DisplayScalerScaleTable& table = displayScalerGetScaleTable();

// Logical column 0 → physical pixels [320, 321] (2px wide at 2.0 scale)
int startX = table.horizontal.starts[0];  // 320
int endX   = table.horizontal.ends[0];    // 321

// Used by blitIndexedRectToTexture and HD overlay compositing
```

---

## Edge Cases & Constraints

### Letterbox Clicks

Clicks in the letterbox area (outside viewport) are:
- Clamped to logical bounds `[0, 639] × [0, 479]`
- Marked `insideViewport = false` in `DisplayScalerVirtualMapping`
- Still delivered to game logic (e.g., for edge scrolling)

### Sub-Pixel Precision

Logical coordinates can be fractional:
```cpp
DisplayScalerVirtualMapping mapping = displayScalerMapPointToVirtual(961, 541);
// mapping.exactX = 320.5 (between logical pixels 320 and 321)
// Game code rounds to int: int(round(320.5)) = 320 or 321
```

### Integer Scaling vs. Smooth Scaling

```cpp
displayScalerSetIntegerScaling(true);  // Enforces floor(scale)
displayScalerSetIntegerScaling(false); // Allows fractional scale
// Controlled by user setting; affects viewport size/letterbox
```

---

## Common Pitfalls

### ❌ Don't Mix Coordinate Spaces

```cpp
// BAD: Using physical SDL coords directly for game logic
SDL_GetMouseState(&x, &y);
int tile = tileFromScreenXY(x, y, elevation); // WRONG! Needs logical coords

// GOOD: Convert first
DisplayScalerVirtualMapping mapping = displayScalerMapPointToVirtual(x, y);
int logicalX = static_cast<int>(round(mapping.exactX));
int logicalY = static_cast<int>(round(mapping.exactY));
int tile = tileFromScreenXY(logicalX, logicalY, elevation);
```

### ❌ Don't Bypass the Scaler

```cpp
// BAD: Manual scaling math
int physicalX = logicalX * 2; // Assumes 2.0 scale, ignores letterbox

// GOOD: Use scaler APIs
Point physical = displayScalerLogicalToPhysical({logicalX, logicalY});
```

### ❌ Don't Store Physical Coords in Game State

```cpp
// BAD: Saving physical coordinates in save files
saveData.cursorX = physicalMouseX; // Breaks on resolution change

// GOOD: Always store logical
saveData.cursorX = logicalMouseX; // Portable across resolutions
```

---

## Verification Checklist

When adding new input/output code:

- [ ] All SDL input events go through `virtualInputCaptureEvent()`
- [ ] All mouse positions passed to game logic are in logical space
- [ ] All render commands use logical `Rect` and positions
- [ ] Physical coordinates are ONLY used in presenter/SDL layer
- [ ] No direct scaling math (always use `display_scaler` APIs)
- [ ] New coordinates documented as `LogicalCoordinate` or `PhysicalCoordinate`

---

## Related Files

| File | Purpose |
|------|---------|
| `display_scaler.h/.cc` | Core coordinate conversion logic |
| `virtual_input.h/.cc` | Physical input capture and translation |
| `display_abstraction.h` | Coordinate type definitions |
| `window_manager.cc` | Logical screen buffer management |
| `svga.cc` | Physical presenter and SDL texture |
| `render_display_orchestrator.cc` | Physical overlay compositing |

---

## Future Work

- [ ] Add debug mode that visualizes coordinate spaces (overlay grid)
- [ ] Extend coordinate types with stronger type safety (C++ wrappers)
- [ ] Add runtime assertions for coordinate range violations
- [ ] Profile coordinate conversion hotspots for optimization
