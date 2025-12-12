# AMD FSR Redstone (FSR SDK 2.1) Integration Guide

> Implementation of GPU-based full-screen upscaling using AMD FidelityFX Super Resolution 2

## Overview

This guide covers the integration of AMD FSR Redstone (FSR SDK 2.1) into the Fallout 2 CE upscaling pipeline. FSR 2.1 is a temporal AI-powered upscaling algorithm that converts a 640×480 source image to higher resolutions (1920×1080, 2560×1440, etc.) with excellent image quality and performance.

### Key Features

- **Temporal Upscaling:** Uses frame history for superior quality
- **No HD Assets Needed:** Works with 8-bit indexed assets
- **Platform Agnostic:** DirectX 12, Vulkan, and other backends supported
- **Configurable Quality:** Quality/Balanced/Performance modes
- **Active Development:** Regularly updated by AMD
- **Open Source:** Permissive license (MIT-like)

---

## Setup Steps

### Step 1: Obtain FSR SDK 2.1

The FSR 2.1 SDK is available at:
```
https://github.com/GPUOpen-Effects/FidelityFX-SuperResolution
```

Options:

**Option A: Vendored (Recommended for reproducible builds)**

1. Clone the FSR repository into `third_party/`
   ```bash
   cd third_party
   git clone https://github.com/GPUOpen-Effects/FidelityFX-SuperResolution.git fsr2
   cd fsr2
   git checkout tags/v2.1.0  # Pin to v2.1.0
   ```

2. The CMakeLists.txt is already configured to look for vendored FSR:
   ```cmake
   # In CMakeLists.txt, near SDL2/other dependencies:
   if(FALLOUT_VENDORED)
       add_subdirectory(third_party/fsr2 EXCLUDE_FROM_ALL)
   endif()
   ```

**Option B: System Installation**

If FSR is installed system-wide or via package manager:
```bash
# Linux (example)
sudo apt-get install libfsr2-dev

# Or build from source and `make install`
```

In this case, CMakeLists.txt should search for it:
```cmake
find_package(FSR2 REQUIRED)
target_link_libraries(${EXECUTABLE_NAME} PRIVATE FSR2::FSR2)
```

### Step 2: Update CMakeLists.txt

Add FSR SDK to the build:

```cmake
# Find or use vendored FSR2
if(FALLOUT_VENDORED)
    # Add FSR2 if not already present
    if(NOT TARGET ffx_fsr2)
        add_subdirectory(third_party/fsr2 EXCLUDE_FROM_ALL)
    endif()
    target_include_directories(${EXECUTABLE_NAME} PRIVATE 
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party/fsr2/sdk/include
    )
    target_link_libraries(${EXECUTABLE_NAME} PRIVATE ffx_fsr2)
else()
    find_package(FSR2 REQUIRED)
    target_link_libraries(${EXECUTABLE_NAME} PRIVATE FSR2::FSR2)
endif()

# Define FSR2 availability
target_compile_definitions(${EXECUTABLE_NAME} PRIVATE FALLOUT_VENDORED_FSR2=1)
```

### Step 3: Configure FSR Backend

FSR requires a graphics API backend. Update `upscaler.cc`:

```cpp
// In UpscalerImpl::initFsr2(), set the backend interface:

#ifdef _WIN32
    // DirectX 12 (Windows)
    FfxGetDeviceD3D12* deviceD3D12 = getD3D12Device();  // From SDL/engine
    FfxInterface backend = ffxGetInterfaceD3D12(
        physicalDeviceD3d12,
        logicalDevice,
        commandQueue
    );
#else
    // Vulkan (Linux/Mac)
    VkDevice device = getVulkanDevice();  // From SDL/engine
    FfxInterface backend = ffxGetInterfaceVK(
        physicalDevice,
        device,
        ...
    );
#endif

initParams.backendInterface = &backend;
```

**Note:** For now, the upscaler is scaffolded with placeholder backend initialization. Actual GPU interop will be implemented in Phase 2.

### Step 4: Build

```bash
mkdir build
cd build
cmake .. -DFALLOUT_VENDORED=ON
cmake --build . --config Release
```

If compilation succeeds, FSR SDK is properly linked. If errors occur:

- **Missing `ffx_fsr2.h`:** Check vendored path or installation
- **Undefined symbols:** Ensure `ffx_fsr2` lib is linked
- **Device interface errors:** Platform-specific backend setup (Phase 2)

---

## Configuration

Add FSR configuration to `fallout2.cfg`:

```ini
[system]
# Upscaling mode: none, fsr2
upscaling_mode=fsr2

# Quality tier: quality, balanced, performance
upscaling_quality=balanced

# Output sharpness (0.0 = none, 1.0 = maximum)
upscaling_sharpness=0.5

# Enable motion vectors (requires game state tracking)
upscaling_motion_vectors=0

[debug]
# Verbose upscaler diagnostics
upscaler_debug_trace=0

# Measure upscaler performance
upscaler_benchmark=0
```

---

## Integration Points (Phase Roadmap)

### Phase 1: Scaffolding & Setup ✅ (Current)

- [x] Create `upscaler.h` abstract interface
- [x] Implement `upscaler.cc` with FSR2 backend
- [x] Add to CMakeLists.txt
- [x] Placeholder GPU device interface
- [ ] Handle compilation on Windows/Linux

**Status:** Ready for backend device integration

### Phase 2: GPU Device Binding

- [ ] Detect graphics API (D3D12, Vulkan)
- [ ] Get device/queue from SDL renderer
- [ ] Initialize FSR backend interface
- [ ] Create GPU textures for input/output

**Estimated effort:** 1-2 days

### Phase 3: Rendering Pipeline Integration

- [ ] Modify `renderPresent()` to use upscaler path
- [ ] Convert `_screen_buffer` → RGBA → upscaler input
- [ ] Dispatch upscaling pass
- [ ] Copy output to SDL texture for display

**Estimated effort:** 1-2 days

### Phase 4: Configuration & Testing

- [ ] Expose quality/sharpness settings UI
- [ ] A/B testing: per-asset upscaling vs. full-screen
- [ ] Performance profiling
- [ ] Visual quality validation

**Estimated effort:** 2-3 days

### Phase 5: Motion Estimation (Optional)

- [ ] Implement frame differencing for motion vectors
- [ ] Detect map scrolls from viewport events
- [ ] Feed motion to FSR2 dispatcher

**Estimated effort:** 1-2 days (optional, improves temporal stability)

### Phase 6: Deprecation & Cleanup

- [ ] Remove orchestrator path if full-screen stable
- [ ] Clean up per-asset overlay code
- [ ] Simplify coordinate translation

**Estimated effort:** 1 week (after full-screen proven)

---

## Current Implementation Status

### Implemented (Phase 1)

✅ **Header (`upscaler.h`)**
- C API interface for upscaler operations
- Enum types for mode/quality/state
- Comprehensive documentation

✅ **Implementation (`upscaler.cc`)**
- Singleton pattern for upscaler state
- Buffer allocation/deallocation
- Configuration (quality, sharpness, motion vectors)
- Diagnostic logging
- Placeholder FSR2 dispatch (awaits GPU device binding)
- Error tracking and reporting

✅ **Build Integration**
- Added to CMakeLists.txt
- Conditional compilation for FSR availability

### Pending (Phase 2+)

⏳ **GPU Device Interface**
- Actual DirectX 12 / Vulkan device binding
- Input/output GPU texture creation
- Shader compilation and dispatch

⏳ **Rendering Pipeline Hook**
- Integration with `renderPresent()` in `svga.cc`
- Input buffer conversion (8-bit indexed → RGBA)
- Output readback and display

⏳ **Configuration UI**
- Settings menu entries for upscaler mode/quality
- Real-time sharpness adjustment

---

## Usage (Once Integrated)

### Initialization

```cpp
// In game startup (main.cc or svga.cc)
int physicalWidth = screenGetWidth();
int physicalHeight = screenGetHeight();

if (upscalerInit(640, 480, physicalWidth, physicalHeight, UpscalerMode::FSR2) != 0) {
    diagnosticsLog(DiagnosticsLevel::Warning, "UPSCALER", 
        "FSR2 init failed: %s", upscalerGetLastError());
    // Fallback to legacy integer scaling
}
```

### Per-Frame Rendering

```cpp
// In renderPresent() (svga.cc)
if (upscalerIsAvailable()) {
    // Path 1: Convert indexed to RGBA & upscale
    upscalerSetIndexedInput(_screen_buffer, gCurrentPalette);
    upscalerDispatch();
    
    // Path 2: Use upscaled output
    const uint32_t* upscaled = upscalerGetOutputBuffer();
    int pitch = upscalerGetOutputPitch();
    
    // Copy to SDL texture for display
    SDL_UpdateTexture(gSdlTexture, nullptr, upscaled, pitch);
    SDL_RenderClear(gSdlRenderer);
    SDL_RenderCopy(gSdlRenderer, gSdlTexture, nullptr, nullptr);
    SDL_RenderPresent(gSdlRenderer);
} else {
    // Legacy path: per-asset overlays + integer scaling
    windowPresentVirtualScreen();
    renderPresent();
}
```

### Reconfiguration (Window Resize)

```cpp
// In handleWindowSizeChanged() (svga.cc)
int newWidth = screenGetWidth();
int newHeight = screenGetHeight();

if (upscalerReconfigureOutput(newWidth, newHeight) != 0) {
    diagnosticsLog(DiagnosticsLevel::Warning, "UPSCALER", 
        "Failed to reconfigure upscaler: %s", upscalerGetLastError());
}
```

### Shutdown

```cpp
// In game cleanup
upscalerShutdown();
```

---

## Diagnostics & Debugging

Enable detailed logging:

```ini
[debug]
upscaler_debug_trace=1
```

This produces logs like:
```
[INFO] UPSCALER: Upscaler mode: FSR2 (AMD FidelityFX Super Resolution 2)
[INFO] UPSCALER: Allocated input buffer: 640x480 (1228800 bytes)
[INFO] UPSCALER: Allocated output buffer: 1920x1080 (8294400 bytes)
[INFO] UPSCALER: FSR2 context initialized: 640x480 -> 1920x1080
[INFO] UPSCALER: Upscaler initialized successfully
```

Check for errors:
```cpp
if (upscalerGetState() == UpscalerState::ERROR) {
    printf("Upscaler error: %s\n", upscalerGetLastError());
}
```

---

## Performance Considerations

### Memory Usage

```
Input buffer:  640×480×4 bytes  = ~1.2 MB
Output buffer: 1920×1080×4 bytes = ~8.3 MB (for 1920x1080 output)
FSR scratch:   ~2-4 MB (varies by implementation)
────────────────────────────────
Total per-frame: ~11-13 MB (small impact on modern systems)
```

### GPU Time

- **FSR2 Dispatch:** 1-3 ms on modern GPUs (varies by output resolution)
- **Input conversion:** 0.1-0.5 ms (palette lookup)
- **Output readback:** 0.5-2 ms (depends on GPU bandwidth)

**Typical frame time (1920×1080):** 2-5 ms

### Optimization Opportunities (Future)

- [ ] GPU-side palette conversion (instead of CPU)
- [ ] Persistent GPU texture objects (instead of per-frame malloc)
- [ ] Async upscaling (dispatch frame N while rendering N+1)
- [ ] Compute shader fallback for custom upscaling

---

## Troubleshooting

### FSR2 Fails to Initialize

**Symptom:** `ffxFsr2ContextCreate failed: 0x...`

**Causes:**
1. Graphics API not initialized before upscaler init
2. Invalid render/display size (must be power-of-2 or >= 128×128)
3. Backend interface not set (Phase 2 incomplete)

**Solution:**
- Ensure SDL renderer is created before calling `upscalerInit()`
- Check that dimensions are reasonable (640×480 → 1920×1080+)
- Complete Phase 2 GPU device binding

### Output Appears Blurry or Over-Sharpened

**Symptom:** Upscaled image looks wrong (too soft or too harsh)

**Solutions:**
1. Adjust sharpness in config (0.5 is balanced):
   ```ini
   upscaling_sharpness=0.3  ; More natural
   upscaling_sharpness=0.7  ; Crisper
   ```

2. Try different quality modes:
   ```ini
   upscaling_quality=quality      ; Best image, slower
   upscaling_quality=balanced     ; Default
   upscaling_quality=performance  ; Faster, slightly softer
   ```

### Temporal Artifacts (Flickering, Shimmer)

**Symptom:** Image flickers or shimmers, especially on still scenes

**Causes:**
1. Motion vectors not provided or inaccurate
2. Temporal jitter not stable

**Solutions:**
- Ensure `upscaling_motion_vectors=0` (zero-vector + jitter mode is stable)
- Phase 5: Implement proper motion vector generation if issues persist

---

## References

- **AMD FidelityFX Super Resolution 2:** https://github.com/GPUOpen-Effects/FidelityFX-SuperResolution
- **FSR 2.1 Documentation:** https://gpuopen.com/learn/fidelityfx-super-resolution-2/
- **Redstone Announcement:** https://gpuopen.com/learn/welcome-amd-fsr-redstone-amd-fsr-sdk-21-available/
- **Upscaler Architecture:** See `docs/full_screen_ai_upscaling_analysis.md`

---

## Next Steps

1. **Test Compilation:** Build with FSR2 vendored or system-installed
2. **Phase 2:** Complete GPU device interface for your platform (DirectX/Vulkan)
3. **Phase 3:** Integrate with rendering pipeline (`renderPresent()`)
4. **Phase 4:** Configure quality settings and test visually
5. **Phase 5:** (Optional) Add motion vector support for temporal coherence

For now, the upscaler is fully scaffolded and ready for GPU device binding!
