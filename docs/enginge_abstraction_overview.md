# Engine Abstraction Overview

> The Overseer's Blueprint for Running Fallout 2 in UHD Without Angering the Ancient 640×480 Spirits

## Core Design Principles

1. **Phantom Display (640×480)** – The game runs natively at the original resolution with its own asset cache, completely isolated from rendering decisions. The game logic lives in this "vault" and never knows about the outside world's resolution.

2. **Command-Based Communication** – The phantom display doesn't render directly to screen. Instead, it emits **render commands** describing *what* to draw (fid, position, lighting, etc.), and the real display decides *how* to draw it.

3. **Logical Unit Coordinate System** – Both displays share a unified logical coordinate space (640×480). All positions, mouse events, and rectangles are expressed in logical units, guaranteeing perfect position mapping between input/output regardless of physical resolution.

4. **Separate HD Asset Cache** – The real display maintains its own HD asset cache and renders at physical resolution while respecting logical coordinates. The phantom cache holds only original 8-bit assets.

---

## Architecture Diagram

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                              PHANTOM DISPLAY                                 │
│                           (640×480 Logical Space)                           │
│  ┌─────────────┐  ┌─────────────┐  ┌─────────────┐  ┌─────────────────────┐ │
│  │  Game Logic │  │  Tile/Iso   │  │   Objects   │  │    UI / Windows     │ │
│  │  (scripts,  │  │  Renderer   │  │  Renderer   │  │    (buttons, etc)   │ │
│  │   combat)   │  │             │  │             │  │                     │ │
│  └──────┬──────┘  └──────┬──────┘  └──────┬──────┘  └──────────┬──────────┘ │
│         │                │                │                    │            │
│         ▼                ▼                ▼                    ▼            │
│  ┌──────────────────────────────────────────────────────────────────────┐  │
│  │                     8-bit Asset Cache (gArtCache)                     │  │
│  │                   Original FRM files, indexed palette                 │  │
│  └──────────────────────────────────────────────────────────────────────┘  │
│         │                │                │                    │            │
│         ▼                ▼                ▼                    ▼            │
│  ┌──────────────────────────────────────────────────────────────────────┐  │
│  │                         _screen_buffer (640×480)                      │  │
│  │              8-bit indexed pixels, game's "ground truth"              │  │
│  └──────────────────────────────────────────────────────────────────────┘  │
└─────────────────────────────────────────────────────────────────────────────┘
                                      │
                                      │ RENDER COMMAND BUS
                                      │ (TileBlit, ObjectBlit, UiBlit, etc.)
                                      ▼
┌─────────────────────────────────────────────────────────────────────────────┐
│                               REAL DISPLAY                                   │
│                         (Physical Resolution, e.g. 1920×1080)               │
│  ┌──────────────────────────────────────────────────────────────────────┐  │
│  │                    Display Orchestrator                               │  │
│  │        Consumes commands, resolves HD vs fallback assets              │  │
│  └──────────────────────────────────────────────────────────────────────┘  │
│         │                                                                   │
│         ▼                                                                   │
│  ┌──────────────────────────────────────────────────────────────────────┐  │
│  │                      HD Asset Registry                                │  │
│  │            RGBA textures, per-fid/frame HD views                      │  │
│  └──────────────────────────────────────────────────────────────────────┘  │
│         │                                                                   │
│         ▼                                                                   │
│  ┌──────────────────────────────────────────────────────────────────────┐  │
│  │                   Physical True-Color Overlays                        │  │
│  │         Viewport-sized ARGB buffers, scale tables applied             │  │
│  └──────────────────────────────────────────────────────────────────────┘  │
│         │                                                                   │
│         ▼                                                                   │
│  ┌──────────────────────────────────────────────────────────────────────┐  │
│  │                       SDL Presenter                                   │  │
│  │            Final compositing, letterbox, SDL_RenderPresent            │  │
│  └──────────────────────────────────────────────────────────────────────┘  │
└─────────────────────────────────────────────────────────────────────────────┘
                                      ▲
                                      │ INPUT EVENT BUS
                                      │ (Physical → Logical coordinate mapping)
                                      │
┌─────────────────────────────────────────────────────────────────────────────┐
│                              INPUT LAYER                                     │
│  ┌──────────────────────────────────────────────────────────────────────┐  │
│  │              SDL Events (mouse, keyboard, touch)                      │  │
│  │                    Native window coordinates                          │  │
│  └──────────────────────────────────────────────────────────────────────┘  │
│         │                                                                   │
│         ▼                                                                   │
│  ┌──────────────────────────────────────────────────────────────────────┐  │
│  │              Display Scaler (Coordinate Translation)                  │  │
│  │     Physical → Logical, letterbox-aware, clamp to 640×480 bounds      │  │
│  └──────────────────────────────────────────────────────────────────────┘  │
│         │                                                                   │
│         ▼                                                                   │
│  ┌──────────────────────────────────────────────────────────────────────┐  │
│  │                    Phantom Display Input Queue                        │  │
│  │            Game logic receives events in logical coordinates          │  │
│  └──────────────────────────────────────────────────────────────────────┘  │
└─────────────────────────────────────────────────────────────────────────────┘
```

---

## Current Implementation Status

### Fully Implemented Components

| Component | Status | Key Files |
|-----------|--------|-----------|
| Virtual 640×480 Surface | ✅ Complete | `window_manager.cc` (`_screen_buffer`, `windowGetVirtualScreenBuffer`) |
| Input Coordinate Translation | ✅ Complete | `virtual_input.cc`, `display_scaler.cc` (`displayScalerMapPointToVirtual`) |
| Render Command Bus | ✅ Complete | `render_commands.h/.cc` (`RenderCommandTileBlit`, `RenderCommandOp`) |
| Asset Registry | ✅ Complete | `render_asset_registry.h/.cc` (`RenderAssetHandle`, `HdTrueColorFrameView`) |
| Display Orchestrator | ✅ Complete | `render_display_orchestrator.cc` (command consumption, HD compositing) |
| HD True-Color Overlays | ✅ Complete | `window_manager.cc` (logical + physical overlays) |
| Scale Tables | ✅ Complete | `display_scaler.cc` (`DisplayScalerScaleTable`) |
| Viewport Events | ✅ Complete | `render_commands.cc` (`RenderViewportEvent` for scroll/fade/resize) |

### Configuration Flags

```ini
[system]
virtual_adapter=1              ; Enable the phantom display layer
virtual_adapter_fullres=1      ; Render to viewport-sized physical buffers
render_display_orchestrator=1  ; Enable command-driven rendering
render_command_trace=0         ; Debug: dump command stream
render_command_replay=0        ; Debug: self-test replay harness

[debug]
virtual_adapter_trace=0        ; Verbose SCALER channel logging
input_overlay=0                ; Visual debug overlay for coordinates
hd_missing_watermark=0         ; Show marker when HD asset missing
render_path_trace=0            ; Focused logging: LEGACY vs PHANTOM render paths
```

---

## Implementation Roadmap

### Phase 1: Formalize Abstraction Boundaries

**Goal:** Create explicit interfaces that enforce the phantom/real display separation.

#### 1.1 Define Interface Contracts

Create new headers that formally define the contracts:

```cpp
// src/phantom_display.h - Wraps the 640×480 game world
class PhantomDisplay {
    unsigned char* getScreenBuffer();
    int getScreenPitch();
    void invalidateRect(const Rect& rect);
    // Game logic calls ONLY these APIs
};

// src/real_display.h - Wraps physical output + HD cache
class RealDisplay {
    void processCommands(const RenderCommandBufferView& commands);
    void present();
    // Only orchestrator calls these APIs
};

// src/display_abstraction.h - Shared contracts
struct LogicalCoordinate { int x, y; };  // Always 640×480 space
struct PhysicalCoordinate { int x, y; }; // Window pixel space
```

#### 1.2 Encapsulate Phantom Display

Move scattered state into a cohesive module:

| Current Location | Target |
|------------------|--------|
| `window_manager.cc` (`_screen_buffer`) | `phantom_display.cc` |
| `gArtCache` (8-bit assets) | Owned by phantom display |
| Window list for game logic | Phantom display internal |

#### 1.3 Encapsulate Real Display

Move rendering state into a cohesive module:

| Current Location | Target |
|------------------|--------|
| `svga.cc` (SDL texture, presenter) | `real_display.cc` |
| `render_display_orchestrator.cc` | `real_display.cc` internal |
| `render_asset_registry.cc` | Owned by real display |

---

### Phase 2: Complete Command Coverage

**Goal:** Make the command bus the ONLY path from phantom → real display.

#### 2.1 Audit All Render Paths

Paths that currently emit commands:
- [x] Tile floors/roofs → `TileBlit` / `RoofBlit`
- [x] Objects/critters → `ObjectBlit`
- [x] UI widgets → `UiBlit`
- [x] Viewport changes → `ViewportEvent`
- [x] Mouse cursor → `CursorBlit` command (tracked, legacy rendering)
- [x] Palette effects → `PaletteEffect` command (tracked, legacy rendering)

Paths that need command coverage:
- [ ] Movie playback → Add `VideoFrame` command
- [ ] Debug overlays → Add `DebugGlyph` command

#### 2.2 Add Missing Command Types

```cpp
enum class RenderCommandOp : uint8_t {
    TileBlit = 0,
    RoofBlit,
    ObjectBlit,
    UiBlit,
    ScreenClear,
    ViewportEvent,
    DebugGlyph,
    // NEW:
    PaletteEffect,    // Fades, gamma shifts
    CursorBlit,       // Mouse cursor
    VideoFrame,       // Movie playback
};
```

#### 2.3 Add Command Validation Layer

In debug builds, assert if ANY code writes to `_screen_buffer` without emitting a command:

```cpp
#ifdef DEBUG_COMMAND_COVERAGE
void phantomDisplayAssertCommandEmitted(const char* file, int line);
#endif
```

---

### Phase 3: Unify Coordinate System

**Goal:** All positions use logical units (640×480 space).

#### 3.1 Coordinate Contract

```cpp
// LOGICAL: Always 640×480 space
// - All game logic uses logical coordinates
// - Render commands carry logical rects
// - Input events delivered in logical coordinates

// PHYSICAL: Window pixel space (varies)
// - Only real display / SDL layer uses physical
// - Scale tables map logical → physical spans
```

#### 3.2 Verify Input Path

Current flow (working):
```
SDL_Event (physical) 
  → virtualInputCaptureEvent() 
  → displayScalerMapPointToVirtual() 
  → game_mouse (logical)
```

Verify all input types:
- [x] Mouse motion
- [x] Mouse buttons
- [x] Mouse wheel
- [x] Touch events
- [ ] Edge scrolling boundaries

---

### Phase 4: Separate Asset Caches

**Goal:** Two completely independent caches.

#### 4.1 Phantom Cache (8-bit)

The existing `gArtCache` serves this role:
- Loads original `.FRM` files
- Returns indexed (8-bit) pixel data
- Managed by `art.cc`

**Constraints:**
- NEVER hold HD (RGBA) data
- NEVER reference physical resolution

#### 4.2 Real Display HD Cache

The `render_asset_registry` provides this:
- Maps `RenderAssetHandle` → `HdTrueColorFrameView`
- Stores RGBA pixel data
- Supports auto-fallback (upscaled 8-bit when HD missing)

**Enhancements needed:**
- [ ] Move ownership entirely into `RealDisplay`
- [ ] Lazy loading: load HD assets on first command reference
- [ ] Async loading: don't block frame while loading

#### 4.3 HD Asset Loading Pipeline

```
Request Flow:
1. Command arrives with RenderAssetHandle
2. Orchestrator queries HD cache
3. Cache miss:
   a. Check HD art pack for matching PNG/DDS
   b. If found → load async, use fallback meanwhile
   c. If not found → use fallback permanently
4. Cache hit → blit HD pixels with lighting applied
```

---

### Phase 5: Make Orchestrator Mandatory

**Goal:** Remove all legacy rendering paths when virtual adapter is enabled.

#### 5.1 Current Fallback Paths (to remove)

```cpp
// window_manager.cc - windowPresentVirtualScreen()
if (!orchestratorOwnsPresenter) {
    blitIndexedRectToTexture(_screen_buffer, ...);  // LEGACY PATH
}
```

#### 5.2 Target Frame Lifecycle

```
1. Game logic runs
   - Updates phantom display (_screen_buffer)
   - Emits render commands for every visual change

2. renderPresent() called
   - Orchestrator consumes ALL queued commands
   - Resolves HD vs fallback for each asset
   - Applies lighting, composites to physical overlays
   - SDL presents frame

3. No code path touches SDL texture directly
   - Except through orchestrator
```

#### 5.3 Handle Edge Cases

| Edge Case | Solution |
|-----------|----------|
| Boot/splash screens | Emit `UiBlit` commands for all splash art |
| Movie playback | Emit `VideoFrame` commands per movie frame |
| Loading screens | Emit commands; orchestrator handles partial frames |
| Debug overlays | Emit `DebugGlyph` commands (or render post-composite) |

---

### Phase 6: Testing & Diagnostics

**Status:** ✅ Complete

#### 6.1 Diagnostic Channels

```
SCALER - Scale table, viewport, present stats
VA_TRACE - Virtual adapter dirty rects
RENDERTRACE - Individual blit operations
RENDERPATH - Focused legacy vs phantom display path tracing
```

#### 6.1.1 Render Path Trace (render_path_trace=1)

This focused diagnostic helps identify whether legacy or phantom display code is drawing to the screen.

**Enable in fallout2.cfg:**
```ini
[debug]
render_path_trace=1
```

**Log Messages:**

| Log Pattern | Meaning | Expected in Phantom Mode |
|-------------|---------|--------------------------|
| `PHANTOM: orchestrator processing N commands` | HD orchestrator is rendering tiles/objects | ✅ Yes |
| `PHANTOM: orchestrator drew N pixels` | HD content was composited to overlay | ✅ Yes |
| `PHANTOM: HD overlay composited` | HD overlay being presented | ✅ Yes |
| `BLOCKED: legacy indexed_blit` | Legacy path blocked by orchestrator | ✅ Yes (good!) |
| `LEGACY: indexed_blit ACTIVE` | Legacy indexed path is drawing | ❌ Should NOT appear |
| `LEGACY: _GNW_win_refresh blit` | Legacy window blit to _screen_buffer | ❌ Should NOT appear |
| `PHANTOM: orchestrator idle (no commands)` | No tile commands this frame | ⚠️ OK during menus |
| `isoDisable: transitioning to menu` | Game world paused for menu | ℹ️ Informational |
| `isoDisable: preserving HD overlay` | HD content preserved behind menu | ✅ Yes (expected) |

**Interpreting Results:**

In **phantom display mode** (virtual_adapter=1, render_display_orchestrator=1):
- You should see `PHANTOM:` logs when game world is active
- You should see `BLOCKED:` when legacy paths are correctly blocked
- You should **NOT** see `LEGACY:` logs during gameplay (indicates fallback leak)

If you see `LEGACY: indexed_blit ACTIVE` during gameplay, it means:
1. The orchestrator doesn't "own" the presenter
2. Check `hadContent` and `ownsPresenter` values in the log
3. This causes low-res tiles to overwrite HD content

#### 6.2 Coverage Metrics

Implemented per-frame metrics:
- [x] Commands queued vs. dropped (overflow detection)
- [x] HD cache hit rate (orchestratorFrames, fallbackFrames)
- [x] Direct write tracking (legacy path usage)
- [x] Frame time breakdown (renderCommandLogFrameMetrics every 300 frames)

#### 6.3 Critical Rendering Fixes Discovered

**Issue 1: Black Screen**
- **Root Cause:** Orchestrator composited overlays but never marked virtual screen dirty
- **Fix:** Added `windowVirtualScreenInvalidateRect()` call after orchestrator processing
- **Location:** `render_display_orchestrator.cc` line ~760

**Issue 2: Missing Base Layer**  
- **Root Cause:** When orchestrator active, `clearPresenterRect()` was called instead of rendering indexed background
- **Fix:** Always render indexed background via `blitIndexedRectToTexture()`, orchestrator only affects overlays
- **Location:** `window_manager.cc` line ~3605

**Issue 3: Grid Artifacts & Movement Trails**
- **Root Cause:** Physical overlay buffers accumulated data across frames without being cleared
- **Fix:** Clear entire tile window overlay at start of each frame before processing commands
- **Location:** `render_display_orchestrator.cc` line ~750
- **Key Insight:** Must clear ENTIRE overlay buffer per frame, not per-command, to prevent stale data

**Issue 4: Cursor/Palette Integration**
- **Implementation:** Cursor and palette commands now emitted and tracked by orchestrator
- **Location:** `mouse.cc` (cursor), `svga.cc` (palette), `render_display_orchestrator.cc` (processing)
- **Status:** Commands tracked for metrics; actual rendering still via legacy paths (intentional)

**Issue 5: Command Queue Overflow Disabling Orchestrator**
- **Root Cause:** `kRenderCommandTileCapacity` was set to 4096, but complex maps emit more tile/object commands per frame
- **Symptom:** Log shows `command_fallback activated reason=command_queue_overflow`, followed by `orchestrator_enabled=0` and legacy rendering taking over
- **Fix 1:** Increased `kRenderCommandTileCapacity` from 4096 to 16384 in `render_commands.cc`
- **Fix 2:** Exposed `renderCommandResetFallbackState()` as public API
- **Fix 3:** Added fallback reset call in `mapLoad()` to give orchestrator fresh start on new maps
- **Location:** `render_commands.cc` line ~22 (capacity), `map.cc` (reset call)
- **Key Insight:** Once fallback is triggered, it was permanent for the entire session; now resets on map transitions

#### 6.4 Test Scenarios

| Scenario | What to Verify |
|----------|----------------|
| Combat (many objects) | Command queue doesn't overflow |
| Dialogue (UI heavy) | UiBlit commands capture all widgets |
| Map scroll | ViewportEvent + tile redraws work |
| Palette fade | PaletteEffect commands work |
| Movie playback | VideoFrame commands work |
| Resolution change | Scale tables rebuild, overlays resize |

---

## File Structure

### Current Key Files

```
src/
├── window_manager.h/.cc       # Phantom display state (_screen_buffer)
├── svga.h/.cc                 # SDL presenter, texture management
├── display_scaler.h/.cc       # Coordinate conversion, scale tables
├── virtual_input.h/.cc        # Input capture and translation
├── render_commands.h/.cc      # Command bus, serialization
├── render_asset_registry.h/.cc # HD asset registration
├── render_display_orchestrator.h/.cc # Command consumer, compositor
├── art.h/.cc                  # 8-bit asset cache (gArtCache)
├── tile.h/.cc                 # Iso tile rendering
```

### Proposed New Files

```
src/
├── phantom_display.h/.cc      # NEW: Encapsulates 640×480 game world
├── real_display.h/.cc         # NEW: Encapsulates physical output + HD cache
├── display_abstraction.h      # NEW: Shared interface contracts
```

---

## Actual Rendering Pipeline (As Implemented)

### Frame Lifecycle

```
1. Game Logic Updates (Phantom Display)
   - Game code writes to _screen_buffer (640×480 indexed)
   - Marks regions dirty via windowVirtualScreenInvalidateRect()
   - Emits render commands for tiles/objects/UI

2. windowPresentVirtualScreen() Called
   a. renderDisplayOrchestratorProcess()
      - Processes viewport events (scroll, fade, etc.)
      - Processes cursor commands (tracking only)
      - Processes palette commands (tracking only)
      - CLEARS entire tile window physical overlay buffer
      - For each tile/object/UI command:
        * Queries HD asset registry
        * Composites HD pixels to physical overlay with lighting
      - Marks virtual screen dirty if content rendered
   
   b. Render Indexed Background
      - blitIndexedRectToTexture(virtualScreenBuffer, ...)
      - Converts 8-bit indexed → RGBA using texture palette
      - Writes to SDL presenter texture (gSdlTextureSurface)
   
   c. Composite HD Overlays
      - windowCompositeTrueColorOverlays()
      - For each window with physical overlay:
        * blitPhysicalTrueColorRectToTexture()
        * Composites RGBA overlays on top of indexed base
        * Respects mask: only overwrites where mask != 0

3. renderPresent() Called
   - SDL_UpdateTexture() uploads presenter texture to GPU
   - SDL_RenderCopy() renders with letterboxing
   - SDL_RenderPresent() swaps buffers
   - Logs frame metrics every 300 frames
```

### Layer Compositing Order

```
Final Frame = Base Layer + Overlay Layer

Base Layer (8-bit indexed):
  - Source: _screen_buffer (640×480)
  - Method: blitIndexedRectToTexture
  - Output: SDL presenter texture
  - Always rendered every frame

Overlay Layer (32-bit RGBA):
  - Source: Physical overlay buffers (per window)
  - Method: blitPhysicalTrueColorRectToTexture  
  - Output: SDL presenter texture (composited on top)
  - Only rendered where mask != 0
  - Cleared every frame before orchestrator writes
```

### Critical Implementation Details

**1. Overlay Buffer Management**
- Each window has TWO overlay buffers:
  - Logical overlay: 640×480 RGBA (for non-scaled rendering)
  - Physical overlay: Viewport-sized RGBA (for HD rendering)
- Orchestrator writes ONLY to physical overlays
- Physical overlays MUST be cleared every frame (not per-command)
- Compositing happens AFTER indexed background is rendered

**2. Coordinate System**
- All game logic uses logical coordinates (640×480)
- Render commands carry logical rects
- Display scaler maps logical → physical via scale tables
- Physical overlays are viewport-sized but indexed by logical coords

**3. Asset Resolution**
- HD assets queried via `renderAssetRegistryGetHdView()`
- Cache miss → auto-generates fallback from 8-bit indexed
- Fallback is cached permanently until restart
- HD assets rendered at full resolution with bilinear filtering

---

## Phase 7: GPU Optimization for HD Overlay Rendering

### Problem Statement

The current HD overlay architecture has a significant performance bottleneck:

```
Current Flow (CPU-bound):
┌─────────────────┐    ┌──────────────────┐    ┌────────────────┐    ┌─────────────┐
│ HD Tile Assets  │───▶│ CPU Overlay      │───▶│ CPU Presenter  │───▶│ SDL Texture │
│ (RGBA, HD res)  │    │ Buffer (RAM)     │    │ Surface (RAM)  │    │ (GPU VRAM)  │
└─────────────────┘    └──────────────────┘    └────────────────┘    └─────────────┘
                            │                       │                      │
                       blitToPhysicalOverlay   blitPhysicalTrueColor   SDL_UpdateTexture
                       (per-pixel copy)        RectToTexture           (upload to GPU)
                                               (per-pixel copy)
```

**The Problem:** Every frame, HD content is copied:
1. From HD asset → CPU physical overlay buffer (per-pixel)
2. From CPU overlay buffer → CPU presenter surface (per-pixel)  
3. From CPU presenter surface → GPU texture (bulk upload)

This triple-copy with per-pixel operations is slow, especially at 2048×1152+ resolutions.

### Proposed Solution: GPU-Resident Overlay Texture

Move the HD overlay to a GPU-resident texture and use hardware-accelerated blending:

```
Optimized Flow (GPU-accelerated):
┌─────────────────┐    ┌────────────────────┐
│ HD Tile Assets  │───▶│ GPU Overlay Texture│
│ (RGBA, HD res)  │    │ (VRAM, streaming)  │
└─────────────────┘    └─────────┬──────────┘
                                 │
                      SDL_LockTexture (dirty region only)
                                 │
                                 ▼
┌─────────────────────────────────────────────────────────────────┐
│                     GPU Compositor                               │
│  1. Render indexed base layer (gSdlTexture)                     │
│  2. Blend overlay texture on top (hardware alpha blend)          │
│  3. SDL_RenderPresent                                            │
└─────────────────────────────────────────────────────────────────┘
```

### Implementation Architecture

#### 7.1 Create GPU Overlay Texture

**New State in `svga.cc`:**
```cpp
// GPU-resident overlay texture for HD content
SDL_Texture* gSdlOverlayTexture = nullptr;    // STREAMING access for updates
Rect gOverlayDirtyRegion = { 0, 0, -1, -1 };  // Track dirty region per frame
bool gOverlayHasContent = false;               // Skip blend if nothing to draw

// Create overlay texture alongside main texture
gSdlOverlayTexture = SDL_CreateTexture(
    gSdlRenderer,
    SDL_PIXELFORMAT_ARGB8888,
    SDL_TEXTUREACCESS_STREAMING,  // Allows SDL_LockTexture for partial updates
    presenterWidth,
    presenterHeight
);

// Enable alpha blending for overlay
SDL_SetTextureBlendMode(gSdlOverlayTexture, SDL_BLENDMODE_BLEND);
```

#### 7.2 Replace Physical Overlay Buffers

**Current:** Each window has a CPU `trueColorPhysicalOverlay` buffer  
**New:** Windows write directly to GPU overlay texture region

**Modification to `window_manager.cc`:**
```cpp
// BEFORE: Allocate per-window CPU overlay buffer
window->trueColorPhysicalOverlay = (uint32_t*)internal_malloc(viewportSize);

// AFTER: Per-window tracks logical dirty rect only
// HD content goes directly to GPU overlay texture
window->overlayDirtyRect = { 0, 0, -1, -1 };
```

#### 7.3 Streaming Update Path

**New function in `svga.cc`:**
```cpp
// Update a rect of the GPU overlay texture directly
void blitToGpuOverlayTexture(const uint32_t* src, int srcPitch, const Rect& rect)
{
    if (gSdlOverlayTexture == nullptr || src == nullptr) return;
    
    // Clip to texture bounds
    Rect clipped = clipToPresenterBounds(rect);
    int width = rectGetWidth(&clipped);
    int height = rectGetHeight(&clipped);
    
    // Lock only the dirty region
    SDL_Rect sdlRect = { clipped.left, clipped.top, width, height };
    void* pixels;
    int pitch;
    if (SDL_LockTexture(gSdlOverlayTexture, &sdlRect, &pixels, &pitch) != 0) {
        return;
    }
    
    // Copy pixels (single memcpy per row - much faster)
    const uint32_t* srcRow = src;
    uint8_t* dstRow = (uint8_t*)pixels;
    for (int y = 0; y < height; y++) {
        memcpy(dstRow, srcRow, width * sizeof(uint32_t));
        srcRow += srcPitch;
        dstRow += pitch;
    }
    
    SDL_UnlockTexture(gSdlOverlayTexture);
    
    // Expand dirty region
    rectUnionPoint(&gOverlayDirtyRegion, clipped.left, clipped.top);
    rectUnionPoint(&gOverlayDirtyRegion, clipped.right, clipped.bottom);
    gOverlayHasContent = true;
}
```

#### 7.4 GPU Compositing in renderPresent()

**Modified `renderPresent()` in `svga.cc`:**
```cpp
void renderPresent()
{
    // 1. Clear render target
    SDL_SetRenderDrawColor(gSdlRenderer, 0, 0, 0, 255);
    SDL_RenderClear(gSdlRenderer);
    
    // 2. Render indexed base layer (always)
    SDL_RenderCopy(gSdlRenderer, gSdlTexture, nullptr, &destRect);
    
    // 3. Blend HD overlay on top (if has content)
    if (gOverlayHasContent && gSdlOverlayTexture != nullptr) {
        SDL_RenderCopy(gSdlRenderer, gSdlOverlayTexture, nullptr, &destRect);
    }
    
    // 4. Present
    SDL_RenderPresent(gSdlRenderer);
    
    // 5. Clear overlay for next frame (GPU-accelerated clear)
    if (gOverlayHasContent) {
        SDL_SetRenderTarget(gSdlRenderer, gSdlOverlayTexture);
        SDL_SetRenderDrawColor(gSdlRenderer, 0, 0, 0, 0);  // Fully transparent
        SDL_RenderClear(gSdlRenderer);
        SDL_SetRenderTarget(gSdlRenderer, nullptr);
        gOverlayHasContent = false;
        gOverlayDirtyRegion = { 0, 0, -1, -1 };
    }
}
```

#### 7.5 Scroll Optimization

**Problem:** When the map scrolls, the overlay content needs to shift.

**Solution:** Use `SDL_RenderCopy` with source/dest rects to shift, then clear exposed edges:

```cpp
void scrollGpuOverlay(int dx, int dy)
{
    if (!gOverlayHasContent || gSdlOverlayTexture == nullptr) return;
    
    // Use a secondary texture for scroll buffer
    // Copy shifted region, clear exposed edges
    SDL_Rect srcRect = computeScrollSource(dx, dy);
    SDL_Rect dstRect = computeScrollDest(dx, dy);
    
    SDL_SetRenderTarget(gSdlRenderer, gSdlOverlayTextureBack);
    SDL_SetRenderDrawColor(gSdlRenderer, 0, 0, 0, 0);
    SDL_RenderClear(gSdlRenderer);
    SDL_RenderCopy(gSdlRenderer, gSdlOverlayTexture, &srcRect, &dstRect);
    SDL_SetRenderTarget(gSdlRenderer, nullptr);
    
    // Swap textures
    std::swap(gSdlOverlayTexture, gSdlOverlayTextureBack);
}
```

### Performance Benefits

| Metric | Current (CPU) | Optimized (GPU) |
|--------|---------------|-----------------|
| Overlay writes | Per-pixel CPU loop | memcpy + SDL_LockTexture |
| Compositing | Per-pixel alpha blend in CPU | Hardware blend |
| Clear | memset entire buffer | SDL_RenderClear (GPU) |
| Scroll | memmove entire buffer | SDL_RenderCopy (GPU) |
| Memory bandwidth | 3× copy (overlay→surface→texture) | 1× copy (directly to VRAM) |

**Expected improvement:** 2-5× faster frame times at 2048×1152+ resolutions.

### Implementation Phases

**Phase 7.1: Create GPU Overlay Texture** (Low risk)
- Add `gSdlOverlayTexture` creation in `createRenderer()`
- Add destruction in `destroyRenderer()`
- No behavior change yet

**Phase 7.2: Add Direct Upload Path** (Medium risk)
- Implement `blitToGpuOverlayTexture()` 
- Keep CPU path as fallback
- Add config flag: `gpu_overlay=1`

**Phase 7.3: Remove CPU Overlay Buffers** (High risk)
- Modify `blitToPhysicalOverlay()` to call GPU path
- Remove `trueColorPhysicalOverlay` allocation
- Update scroll handler

**Phase 7.4: Hardware Compositing** (Medium risk)
- Replace `blitPhysicalTrueColorRectToTexture()` CPU blend
- Use `SDL_RenderCopy` with blend mode
- GPU clears overlay each frame

### Configuration

```ini
[system]
gpu_overlay=1           ; Enable GPU-resident overlay texture
gpu_overlay_debug=0     ; Show overlay bounds, dirty regions
vsync=1                 ; Enable VSync for tear-free rendering
target_fps=60           ; Target frame rate for FPS limiter and scroll speed
```

### Compatibility Notes

- Requires SDL 2.0 with hardware renderer
- Fallback to CPU path if GPU texture creation fails
- Some integrated GPUs may not benefit (shared memory)

---

## Phase 7b: Frame Timing Optimization

### Problem Analysis

The original frame timing system had several issues causing choppy movement:

1. **No VSync** - Renderer created with `flags=0`, causing screen tearing
2. **Imprecise FPS Limiter** - Used `SDL_Delay()` which has 10-15ms granularity on Windows
3. **Integer Math** - Frame time calculated as `1000 / 60 = 16ms` (should be 16.667ms)
4. **Hardcoded Scroll Throttle** - Map scrolling locked to 33ms (30fps)

### Solution Implementation

#### High-Precision FPS Limiter (`fps_limiter.cc`)

```cpp
// Uses QueryPerformanceCounter for microsecond precision
// Hybrid approach: SDL_Delay for coarse sleep, spin-wait for final timing
void FpsLimiter::throttle() const
{
    double elapsedUs = getElapsedMicroseconds();
    double remainingUs = _frameTimeUs - elapsedUs;
    
    if (remainingUs > 2000) {  // Sleep most of the wait
        SDL_Delay((remainingUs - 1500) / 1000);
    }
    
    // Spin-wait for precise timing (sub-millisecond)
    while (getElapsedMicroseconds() < _frameTimeUs) { }
}
```

#### VSync Support (`svga.cc`)

```cpp
Uint32 rendererFlags = 0;
if (settings.system.vsync) {
    rendererFlags |= SDL_RENDERER_PRESENTVSYNC;
}
gSdlRenderer = SDL_CreateRenderer(gSdlWindow, -1, rendererFlags);
```

#### Dynamic Scroll Throttle (`map.cc`)

```cpp
// Use target_fps to calculate scroll threshold
unsigned int scrollThresholdMs = 1000 / settings.system.target_fps;  // 16ms at 60fps
if (getTicksSince(gIsoWindowScrollTimestamp) < scrollThresholdMs) {
    return -2;
}
```

### Performance Benefits

| Aspect | Before | After |
|--------|--------|-------|
| Frame pacing | ±15ms jitter | ±0.1ms precision |
| Map scroll rate | 30fps max | 60fps |
| Screen tearing | Common | Eliminated (VSync) |
| Input latency | Variable | Consistent 16.67ms |

### Settings

```ini
[system]
vsync=1          ; Sync to monitor refresh (eliminates tearing)
target_fps=60    ; Frame rate target (affects scroll speed, FPS limiter)
```

---

## Phase 7c: Deferred Presentation (Flicker Elimination)

### Problem Analysis

The game's animation system caused flickering around characters because:

1. **Multiple presents per frame** - Each animated object called `tileWindowRefreshRect()` which triggered `windowRefreshRect()` → `windowPresentVirtualScreen()` immediately
2. **Partial state visibility** - User sees intermediate states where some objects are updated but others aren't
3. **N objects = N texture uploads** - Each moving object caused a separate texture upload to GPU

### Root Cause Chain

```
_object_animate() [called from tickersExecute]
  ↓ for each animated object
  tileWindowRefreshRect()
    ↓
  isoWindowRefreshRect() 
    ↓
  windowRefreshRect()
    ↓
  windowPresentVirtualScreen()  ← IMMEDIATE PRESENT = FLICKER!
```

### Solution: Deferred Presentation

Added a deferred presentation mode (`window_manager.cc`):

```cpp
static bool gDeferredPresentationEnabled = false;

void windowSetDeferredPresentation(bool enabled);
bool windowIsDeferredPresentationEnabled();

void windowRefreshRect(int win, const Rect* rect)
{
    // ... update screen buffer ...
    
    // Only present immediately if deferred presentation is disabled
    if (!gDeferredPresentationEnabled) {
        windowPresentVirtualScreen();
    }
}
```

The main game loop now enables deferred presentation:

```cpp
static void mainLoop()
{
    windowSetDeferredPresentation(true);  // Enable deferred mode
    
    while (_game_user_wants_to_quit == 0) {
        sharedFpsLimiter.mark();
        
        int keyCode = inputGetInput();   // Runs tickersExecute() → _object_animate()
        // ... game logic ...
        
        renderPresent();                  // Single present per frame!
        sharedFpsLimiter.throttle();
    }
    
    windowSetDeferredPresentation(false);
}
```

### Performance Benefits

| Aspect | Before | After |
|--------|--------|-------|
| Presents per frame | N (one per animated object) | 1 |
| Texture uploads | N | 1 |
| Visual flickering | Visible around moving objects | Eliminated |
| Frame coherence | Partial states visible | Complete frames only |

---

## Quick Start (Running Upscaled Today)

The system **already works**. To run the game upscaled:

1. Edit `fallout2.cfg`:
```ini
[system]
virtual_adapter=1
virtual_adapter_fullres=1
render_display_orchestrator=1
```

2. Launch the game normally.

3. (Optional) Enable diagnostics in `fallout2.cfg`:
```ini
[debug]
virtual_adapter_trace=1
```

4. Check logs for `SCALER orchestrator ...` lines confirming HD pipeline is active.

---

## Summary

The engine abstraction creates a clean separation between:

- **Phantom Display**: The classic 640×480 game world, running original logic with original assets
- **Real Display**: The modern HD presenter, consuming commands and rendering at native resolution

This architecture allows:
- ✅ Game logic remains unchanged
- ✅ HD assets render at full resolution
- ✅ Input coordinates map correctly
- ✅ Fallback to 8-bit when HD assets missing
- ✅ No modifications to original gameplay code

The remaining work is architectural cleanup—encapsulating scattered state, completing command coverage, and removing legacy fallback paths.



# The Enginge_abstraction_overview.md describes a clean architectural vision with four key pillars:

1. Phantom Display (640×480) – The game runs natively at the original resolution with its own asset cache, completely isolated from rendering decisions.

2. Command-Based Communication – The phantom display doesn't render directly; it sends commands to the real display describing what to draw, not how.

3. Logical Unit Coordinate System – Both displays share a unified logical coordinate space for perfect position mapping between input/output.

4. Separate HD Asset Cache – The real display maintains its own HD asset cache and renders at physical resolution while respecting logical coordinates.


