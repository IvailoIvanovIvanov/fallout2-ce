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

Paths that need command coverage:
- [ ] Palette effects (fades, gamma) → Add `PaletteEffect` command
- [ ] Mouse cursor → Add `CursorBlit` command
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

#### 6.1 Diagnostic Channels

```
SCALER - Scale table, viewport, present stats
VA_TRACE - Virtual adapter dirty rects
RENDERTRACE - Individual blit operations
```

#### 6.2 Coverage Metrics

Add per-frame metrics:
- Commands emitted vs. direct writes (should be 100% / 0%)
- HD cache hit rate
- Fallback usage count
- Frame time breakdown (phantom vs. orchestrator)

#### 6.3 Test Scenarios

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


