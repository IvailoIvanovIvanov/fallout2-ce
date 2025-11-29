# Test Scenarios for Engine Abstraction

## Overview

This document defines test scenarios to validate the engine abstraction architecture (Phases 1-5) and ensure the phantom/real display separation works correctly across all game systems.

## Configuration

All tests should run with the following `fallout2.cfg` settings:

```ini
[system]
virtual_adapter=1
virtual_adapter_fullres=1
render_display_orchestrator=1

[debug]
virtual_adapter_trace=1  ; Optional: Enable for detailed logging
```

## Test Scenarios

### 1. Combat (Many Objects)

**Objective:** Verify command queue handles high object density without overflow

**Steps:**
1. Start combat with 6+ NPCs/critters visible
2. Trigger animations (attacks, movement, explosions)
3. Monitor frame rate and command queue stats

**Validation:**
- ✅ No command queue overflow (`gRenderCommandStats.dropped == 0`)
- ✅ All objects render correctly at physical resolution
- ✅ Frame rate remains stable (≥30 FPS)
- ✅ Check logs for: `SCALER orchestrator frame=...` with `commands >100`
- ✅ No `command_fallback activated` warnings

**Metrics to Check:**
```
SCALER orchestrator ... commands=[high count] tile_hd=... obj_hd=[many] obj_fallback=[low]
```

---

### 2. Dialogue (UI Heavy)

**Objective:** Verify UiBlit commands capture all UI widgets correctly

**Steps:**
1. Enter dialogue with NPC
2. Navigate dialogue options (hover, select)
3. Open character sheet/inventory during dialogue
4. Close and reopen windows

**Validation:**
- ✅ All UI elements render at correct positions
- ✅ Mouse hover highlights work correctly
- ✅ Text rendering sharp at physical resolution
- ✅ Coordinate translation accurate (click targets match visuals)
- ✅ Check logs for: `ui_hd=[many]` in orchestrator stats

**Metrics to Check:**
```
SCALER orchestrator ... ui_hd=[20+] ui_fallback=[0]
VA_TRACE present_dirty ... (should show UI window dirty rects)
```

---

### 3. Map Scroll

**Objective:** Verify ViewportEvent + tile redraws work correctly

**Steps:**
1. Scroll map in all 8 directions using edge scrolling
2. Scroll rapidly, then stop abruptly
3. Use mousewheel zoom (if enabled)
4. Fast-travel to different map, check initial render

**Validation:**
- ✅ Tiles redraw smoothly during scroll
- ✅ No tearing or artifacts at viewport edges
- ✅ ViewportEvent commands logged: `SCALER viewport=...`
- ✅ Scale tables update correctly on zoom
- ✅ Input coordinates remain accurate after scroll

**Metrics to Check:**
```
SCALER viewport rect=(...) scale=... logical_bounds=... physical_viewport=...
SCALER orchestrator ... tile_hd=[high] (during scroll)
```

---

### 4. Palette Fade (Phase 2)

**Objective:** Verify PaletteEffect commands work (when implemented)

**Steps:**
1. Trigger fade-out (sleep, unconscious, end-turn)
2. Trigger fade-in (wake up, new map load)
3. Apply gamma/brightness changes in settings

**Validation:**
- ✅ Fade transitions smooth
- ✅ PaletteEffect commands emitted (check with `render_command_trace=1`)
- ✅ HD assets fade correctly (not just indexed buffer)
- ✅ No color banding or artifacts

**Metrics to Check:**
```
SCALER palette_effect type=[fade_out/fade_in] duration=...
```

**Note:** If PaletteEffect commands not yet implemented, verify legacy palette path doesn't corrupt HD rendering.

---

### 5. Movie Playback (Phase 2)

**Objective:** Verify VideoFrame commands work (when implemented)

**Steps:**
1. Play intro movie
2. Play in-game cutscene (death, level-up)
3. Skip movie mid-playback

**Validation:**
- ✅ Video renders at correct aspect ratio
- ✅ No black bars or stretching artifacts
- ✅ Audio sync maintained
- ✅ VideoFrame commands emitted (if implemented)
- ✅ Clean return to game after playback

**Metrics to Check:**
```
SCALER video_frame frame=[n] size=... format=...
```

**Note:** If VideoFrame not implemented, verify legacy movie playback doesn't break orchestrator state.

---

### 6. Resolution Change

**Objective:** Verify scale tables rebuild and overlays resize correctly

**Steps:**
1. Start game at 1920×1080
2. Change window size (if windowed mode supported)
3. Toggle fullscreen
4. Change to 2560×1440 or ultrawide resolution

**Validation:**
- ✅ Scale tables rebuild: `SCALER viewport rect=... scale=[new]`
- ✅ Physical overlays resize to match new viewport
- ✅ Logical coordinate system remains 640×480
- ✅ Input mapping updates correctly (test mouse clicks)
- ✅ No memory leaks or texture allocation failures

**Metrics to Check:**
```
SCALER viewport rect=(0,0 [new_width]x[new_height]) scale=[ratio] logical_bounds=(0,0 640x480)
RENDERER texture_upload ... (should succeed after resize)
```

---

## Stress Tests

### 7. Command Queue Overflow

**Objective:** Verify fallback mechanism activates safely on queue overflow

**Setup:** Modify `kRenderCommandTileCapacity` to artificially low value (e.g., 16)

**Steps:**
1. Load complex map with many tiles/objects
2. Trigger intensive rendering (combat + UI)

**Validation:**
- ✅ `command_fallback activated reason=command_queue_overflow` logged
- ✅ Game continues running (no crash)
- ✅ Rendering falls back to indexed path gracefully
- ✅ Stats show: `gRenderCommandStats.dropped > 0`

---

### 8. HD Cache Stress

**Objective:** Verify fallback generation when HD assets missing

**Setup:** Delete/rename HD art files for specific FIDs

**Steps:**
1. Trigger rendering of assets without HD equivalents
2. Mix HD and non-HD assets in same frame

**Validation:**
- ✅ Fallback auto-generated: `SCALER asset_registry fallback fid=...`
- ✅ Upscaled indexed assets render correctly
- ✅ HD cache hit rate logged: `artTrueColorStatsLog` shows hits/misses
- ✅ No black sprites or missing textures

**Metrics to Check:**
```
SCALER asset_registry fallback fid=[n] frame=[n] rot=[n] variant=[n]
SCALER hd_cache_stats requests=[n] hits=[n] misses=[n] hit_rate=[%]
```

---

## Diagnostic Commands

### Enable All Logging
```ini
[debug]
virtual_adapter_trace=1
diagnostics_level=2  ; 0=Off, 1=Info, 2=Trace
```

### Frame Metrics Logging
Call `renderCommandLogFrameMetrics()` every N frames to log:
- Commands emitted vs direct writes (should be 100% / 0%)
- Orchestrator frame count
- Fallback usage count

### Command Dump Hotkey
Press `Ctrl+F8` to dump last frame's command stream to file:
- Validates command capture is complete
- Useful for debugging missing render ops

---

## Acceptance Criteria

For Phase 6 completion, all test scenarios must pass with:

1. **Zero command queue overflows** in normal gameplay
2. **100% command coverage** (no direct writes when orchestrator active)
3. **>95% HD cache hit rate** with full HD art pack installed
4. **Stable frame rate** (≥30 FPS) on target hardware
5. **No coordinate translation errors** (input clicks match visuals)
6. **Clean orchestrator handoff** (no legacy path fallbacks except catastrophic errors)

---

## Known Limitations (Phase 6)

- **PaletteEffect commands:** Implemented but not wired to palette system yet
- **CursorBlit commands:** Implemented but not wired to mouse cursor rendering
- **VideoFrame commands:** Implemented but movie playback uses legacy path
- **Lazy loading:** HD assets loaded at map start, not on-demand (Phase 4 future work)

These are tracked for future phases but do not block Phase 6 acceptance.
