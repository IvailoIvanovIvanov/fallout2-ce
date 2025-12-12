# FSR2 Integration Implementation Checklist

## Phase 1: Scaffolding & Build Integration ✅ COMPLETE

### Core Files
- [x] Create `src/upscaler.h` - Public C API interface
- [x] Create `src/upscaler.cc` - Implementation with FSR2 backend
- [x] Update `CMakeLists.txt` - Add upscaler sources
- [x] Documentation:
  - [x] `docs/full_screen_ai_upscaling_analysis.md` - Architecture overview
  - [x] `docs/fsr_integration_guide.md` - Integration guide
  - [x] `docs/fsr2_cmake_setup.txt` - CMake configuration

### Upscaler Interface
- [x] Enum: `UpscalerMode` (NONE, FSR2)
- [x] Enum: `UpscalerQuality` (QUALITY, BALANCED, PERFORMANCE)
- [x] Enum: `UpscalerState` (UNINITIALIZED, INITIALIZING, READY, ERROR)
- [x] Class: `UpscalerImpl` - Singleton implementation
- [x] C API wrappers - All functions exposed

### Memory Management
- [x] Input buffer allocation (640×480 ARGB8888)
- [x] Output buffer allocation (variable resolution)
- [x] Proper deallocation on shutdown/reconfiguration
- [x] Error checking for allocation failures

### Configuration
- [x] Quality tier setting (quality/balanced/performance)
- [x] Sharpness control (0.0 to 1.0)
- [x] Motion vector enable/disable flag
- [x] Temporal jitter calculation (Halton sequence)

### Error Handling
- [x] Error message buffer (256 bytes)
- [x] `getLastError()` for debugging
- [x] State machine (prevents invalid operations)
- [x] Diagnostic logging with budget tracking

### Status
**✅ READY FOR PHASE 2**
- Compiles cleanly (pending FSR SDK integration)
- All APIs documented
- Scaffolding complete for GPU backend binding

---

## Phase 2: GPU Device Binding 🔧 IN PROGRESS

### Prerequisites
- [ ] Clone FSR SDK 2.1 to `third_party/fsr2`
  ```bash
  git clone https://github.com/GPUOpen-Effects/FidelityFX-SuperResolution.git third_party/fsr2
  git -C third_party/fsr2 checkout v2.1.0
  ```

### CMake Integration
- [ ] Verify FSR2 CMakeLists.txt is present
- [ ] Add FSR2 to main CMakeLists.txt (via add_subdirectory)
- [ ] Test build: `cmake --build . --config Release`
- [ ] Verify `ffx_fsr2` target exists
- [ ] Check `src/upscaler.cc` includes `ffx_fsr2.h` correctly

### Platform Detection
- [ ] Detect DirectX 12 (Windows)
- [ ] Detect Vulkan (Linux/Mac)
- [ ] Create conditional compile flags (FSR2_BACKEND_D3D12 / FSR2_BACKEND_VULKAN)
- [ ] Update `upscaler.cc` with platform-specific headers

### DirectX 12 Backend (Windows)
- [ ] Get `ID3D12Device` from SDL/engine
- [ ] Get `ID3D12CommandQueue` from command line setup
- [ ] Call `ffxGetInterfaceD3D12()` with device/queue
- [ ] Set `FfxFsr2InitializationParameters::backendInterface`
- [ ] Test on Windows VM/machine

### Vulkan Backend (Linux/Mac)
- [ ] Get `VkDevice` from SDL Vulkan backend
- [ ] Get `VkPhysicalDevice` from surface
- [ ] Get `VkQueue` from command pool
- [ ] Call `ffxGetInterfaceVk()` with device/physical/queue
- [ ] Set `FfxFsr2InitializationParameters::backendInterface`
- [ ] Test on Linux/Mac if available

### GPU Resources
- [ ] Create GPU input texture (640×480 RGBA)
- [ ] Create GPU output texture (output resolution)
- [ ] Handle texture format conversions (if needed)
- [ ] Manage texture lifetime (create/destroy with context)

### Dispatch Implementation
- [ ] Fill `FfxFsr2DispatchDescription` structure
- [ ] Set color input texture descriptor
- [ ] Set jitter offset (from `calculateJitter()`)
- [ ] Set sharpness value (from `gSharpness`)
- [ ] Call `ffxFsr2ContextDispatch()`
- [ ] Handle return codes properly

### Output Readback
- [ ] Copy GPU output texture to CPU buffer
- [ ] Handle GPU→CPU synchronization
- [ ] Maintain output buffer pointer validity
- [ ] Test with various resolutions

### Testing Checklist
- [ ] Compile succeeds on Windows
- [ ] Compile succeeds on Linux (if available)
- [ ] `upscalerInit()` succeeds with valid dimensions
- [ ] `upscalerDispatch()` executes without errors
- [ ] Output buffer contains valid pixel data
- [ ] State machine prevents double-init
- [ ] Reconfiguration handles resize properly

### Estimated Effort
**1-2 weeks** depending on platform availability and FSR SDK documentation clarity

---

## Phase 3: Rendering Pipeline Integration 🔄 NOT STARTED

### Integration Points
- [ ] Modify `src/svga.cc` - `renderPresent()` function
- [ ] Add upscaler dispatch after virtual screen present
- [ ] Route game output through upscaler

### Input Pipeline
- [ ] Create RGBA conversion from indexed buffer
  ```cpp
  // Convert 640x480 indexed -> ARGB8888
  for (int i = 0; i < 640*480; i++) {
      gRgbaBuffer[i] = gPalette[_screen_buffer[i]];
  }
  ```
- [ ] Feed to upscaler: `upscalerSetRgbaInput(gRgbaBuffer)`
- [ ] Or feed indexed directly: `upscalerSetIndexedInput(_screen_buffer, gPalette)`

### Output Pipeline
- [ ] Dispatch upscaler: `upscalerDispatch()`
- [ ] Get upscaled buffer: `upscalerGetOutputBuffer()`
- [ ] Copy to SDL texture for display
- [ ] Handle pitch/stride properly

### Conditional Compilation
- [ ] Add config flag: `system.upscaling_mode` (none/fsr2)
- [ ] Add quality flag: `system.upscaling_quality` (quality/balanced/performance)
- [ ] Add sharpness: `system.upscaling_sharpness` (0.0-1.0)
- [ ] Load from `fallout2.cfg`

### Config Integration
- [ ] Read `upscaling_mode` from config
- [ ] Call `upscalerInit()` with correct dimensions
- [ ] Apply quality/sharpness settings
- [ ] Handle fallback if upscaler unavailable

### Fallback Logic
- [ ] If FSR2 init fails, use legacy integer scaling
- [ ] Log warning but continue running
- [ ] Allow user to disable via config

### Window Resize Handling
- [ ] Detect resolution change in `handleWindowSizeChanged()`
- [ ] Call `upscalerReconfigureOutput()` with new dimensions
- [ ] Verify output buffer reallocated correctly

### Diagnostics
- [ ] Log upscaler stats each frame (optional, budget-limited)
- [ ] Track FSR2 dispatch time
- [ ] Monitor buffer copy time
- [ ] Count dispatch failures

### Testing Checklist
- [ ] Game starts with upscaler enabled
- [ ] Upscaled output appears on screen
- [ ] No visual artifacts or corruption
- [ ] Performance impact acceptable (<3ms per frame)
- [ ] Window resize works smoothly
- [ ] Config flags respected
- [ ] Graceful fallback if upscaler disabled
- [ ] No crashes on long sessions

### Estimated Effort
**1-2 days** once GPU binding complete

---

## Phase 4: Configuration & UI 📊 NOT STARTED

### Settings Integration
- [ ] Add upscaler mode selector (none/fsr2)
- [ ] Add quality tier selector (quality/balanced/performance)
- [ ] Add sharpness slider (0.0-1.0)
- [ ] Add motion vector toggle (enable/disable)

### Config File Updates
- [ ] Document new config keys in sample `fallout2.cfg`
- [ ] Add validation (mode must be valid enum)
- [ ] Add bounds checking (sharpness 0.0-1.0)

### In-Game Menu (Future)
- [ ] Add "Graphics" submenu in main settings
- [ ] Add "Upscaling" section
- [ ] Real-time preview of sharpness changes
- [ ] Display current output resolution

### Diagnostics Display
- [ ] Overlay showing:
  - [ ] "Upscaling: FSR2 (Balanced)"
  - [ ] "Input: 640×480 → Output: 1920×1080"
  - [ ] Dispatch time (ms)
- [ ] Optional: FSR quality indicator

### Visual Comparison Mode
- [ ] Toggle: Full-screen vs. Upscaled vs. 1:1 Pixel
- [ ] Allow side-by-side comparison during gameplay
- [ ] Measure visual difference (SSIM, PSNR) - optional

### Documentation
- [ ] User guide: How to enable/configure upscaling
- [ ] Troubleshooting: Common issues and fixes
- [ ] Performance tips: Quality vs. speed trade-offs

### Estimated Effort
**2-3 days** depending on UI framework complexity

---

## Phase 5: Motion Estimation (Optional) 🎬 NOT STARTED

### Purpose
- Improve temporal stability of FSR2
- Reduce flicker/shimmer on static scenes
- Better handle fast motion (combat scenes)

### Approaches

#### Option A: Frame Differencing (Simple)
- [ ] Compare current frame to previous frame
- [ ] Compute per-block motion vectors
- [ ] Advantages: No game state knowledge needed
- [ ] Disadvantages: May miss large motions, slower
- **Estimated effort:** 1 day

#### Option B: Scroll Delta Tracking (Better)
- [ ] Hook into `tileWindowScroll()` or viewport events
- [ ] Extract X/Y scroll deltas
- [ ] Convert to motion vector map
- [ ] Feed to FSR2
- [ ] Advantages: Perfect for map panning
- [ ] Disadvantages: Requires game state hooks
- **Estimated effort:** 1-2 days

#### Option C: Reactive Map (Advanced)
- [ ] Mark UI regions as "reactive" (important details)
- [ ] Mark static regions (less important)
- [ ] FSR2 preserves sharp details in reactive areas
- [ ] Advantages: Best overall quality
- [ ] Disadvantages: Most complex
- **Estimated effort:** 2-3 days

### Implementation Plan
- [ ] Implement Option B (scroll delta tracking)
- [ ] Test temporal stability
- [ ] If needed, upgrade to Option C (reactive maps)
- [ ] Profile performance impact

### Testing Checklist
- [ ] Map scrolling: No shimmer/flicker
- [ ] Combat movement: Smooth temporal coherence
- [ ] Still scenes: Stable without jitter
- [ ] UI interaction: No temporal artifacts

### Estimated Effort
**1-2 days** for basic motion, 2-3 days for reactive maps

---

## Phase 6: Deprecation & Cleanup (Long-term) ♻️ NOT STARTED

### Prerequisites
- Full-screen upscaling must be proven stable
- Visual quality must exceed or match per-asset approach
- Performance must be acceptable

### Deprecation Steps

#### Step 1: Disable Per-Asset Overlays
- [ ] Set `render_display_orchestrator` to use upscaler path only
- [ ] Keep `RenderAssetRegistry` for compatibility (stub)
- [ ] Remove per-window true-color overlay allocation

#### Step 2: Remove Orchestrator
- [ ] Delete `src/render_display_orchestrator.cc`
- [ ] Delete `src/render_display_orchestrator.h`
- [ ] Remove render command bus if not needed elsewhere

#### Step 3: Simplify Coordinate Translation
- [ ] Remove per-asset scale table logic
- [ ] Keep only output viewport scale table
- [ ] Simplify `display_scaler.cc`

#### Step 4: Clean Asset Registry
- [ ] Keep `RenderAssetRegistry` as optional/stub
- [ ] Remove HD asset tracking
- [ ] Remove fallback RGBA generation
- [ ] Remove lifecycle management

### Migration Safety
- [ ] Maintain config flag to force legacy mode (regression testing)
- [ ] Keep per-asset code paths in case needed
- [ ] Add warnings if legacy mode used in production

### Final Cleanup
- [ ] Remove unused headers
- [ ] Remove dead code paths
- [ ] Update documentation

### Estimated Effort
**1 week** after full-screen proven stable

---

## Master Timeline

| Phase | Task | Effort | Status |
|-------|------|--------|--------|
| 1 | Scaffolding & Build | 1 day | ✅ COMPLETE |
| 2 | GPU Device Binding | 1-2 weeks | 🔧 IN PROGRESS |
| 3 | Rendering Pipeline | 1-2 days | ⏳ BLOCKED (needs Phase 2) |
| 4 | Configuration & UI | 2-3 days | ⏳ BLOCKED (needs Phase 3) |
| 5 | Motion Estimation | 1-2 days | ⏳ OPTIONAL |
| 6 | Deprecation & Cleanup | 1 week | ⏳ FUTURE |
| | **TOTAL** | **3-4 weeks** | |

---

## Success Criteria

### Phase 1 ✅
- [x] Compiles without errors
- [x] APIs fully documented
- [x] Ready for GPU binding

### Phase 2
- [ ] FSR2 successfully initializes
- [ ] Dispatch executes without GPU errors
- [ ] Output buffer contains valid upscaled pixels
- [ ] Works on both Windows and Linux (if available)

### Phase 3
- [ ] Game renders with upscaler enabled
- [ ] Visual quality superior to per-asset approach
- [ ] No seams, borders, or lighting artifacts
- [ ] Performance impact < 5ms per frame

### Phase 4
- [ ] Config flags work correctly
- [ ] Quality/sharpness changes apply in real-time
- [ ] UI shows clear upscaling status
- [ ] Users can easily enable/disable

### Phase 5
- [ ] Temporal stability improved (no flicker)
- [ ] Motion estimation doesn't cause regression
- [ ] Minimal performance impact

### Phase 6
- [ ] Per-asset code cleanly removed
- [ ] No memory leaks
- [ ] Codebase simpler and more maintainable
- [ ] Legacy mode still available for testing

---

## Known Issues & Workarounds

### Current Limitations (Phase 1)
1. GPU device interface not yet implemented
   - **Workaround:** Add FSR SDK and complete Phase 2

2. No input/output GPU textures created
   - **Workaround:** Implement GPU resource creation in Phase 2

3. FSR2 dispatch is a skeleton (no actual upscaling)
   - **Workaround:** Complete GPU device binding first

### Expected Challenges (Phase 2+)
1. **Graphics API Complexity**
   - DirectX 12 / Vulkan device setup is error-prone
   - Recommend starting with one platform
   - **Solution:** Isolate backend in separate module

2. **Memory Synchronization**
   - GPU↔CPU sync needs careful handling
   - May cause stalls if not done right
   - **Solution:** Use persistent mapped buffers or staging textures

3. **Temporal Artifacts**
   - FSR2 needs motion vectors for best results
   - Zero-vector mode is acceptable but not perfect
   - **Solution:** Implement Phase 5 (motion estimation) if issues occur

4. **Performance Regression**
   - GPU upscaling slower than expected
   - **Solution:** Profile GPU time, optimize input conversion (GPU-side)

---

## References

- **FSR 2.1 GitHub:** https://github.com/GPUOpen-Effects/FidelityFX-SuperResolution
- **FSR Redstone Announcement:** https://gpuopen.com/learn/welcome-amd-fsr-redstone-amd-fsr-sdk-21-available/
- **FSR Documentation:** https://gpuopen.com/learn/fidelityfx-super-resolution-2/
- **DirectX 12 Guide:** https://learn.microsoft.com/en-us/windows/win32/direct3d12/
- **Vulkan Guide:** https://vulkan.lunarg.com/
- **Upscaling Architecture:** `docs/full_screen_ai_upscaling_analysis.md`
- **Integration Guide:** `docs/fsr_integration_guide.md`

---

## Next Immediate Steps

1. **Download FSR SDK 2.1**
   ```bash
   cd third_party
   git clone https://github.com/GPUOpen-Effects/FidelityFX-SuperResolution.git fsr2
   cd fsr2 && git checkout v2.1.0
   ```

2. **Test Build**
   ```bash
   cmake .. -DFALLOUT_VENDORED=ON
   cmake --build . --config Release
   ```

3. **Implement GPU Backend** (Phase 2)
   - Detect graphics API (DirectX 12 vs Vulkan)
   - Get device/queue from SDL/engine
   - Initialize FSR backend interface
   - Create GPU textures

4. **Validate**
   - Verify `upscalerInit()` succeeds
   - Check output buffer has valid data
   - Test on actual GPU hardware

Ready to proceed! 🚀
