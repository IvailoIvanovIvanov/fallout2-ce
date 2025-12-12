# FSR2 Implementation Quick Reference

## File Structure

```
src/
├── upscaler.h              # Public C API interface
├── upscaler.cc             # Implementation with FSR2 backend
└── svga.cc                 # Will integrate renderPresent() here

docs/
├── full_screen_ai_upscaling_analysis.md    # Architecture overview
├── fsr_integration_guide.md                # Integration guide
├── fsr2_cmake_setup.txt                    # CMake configuration
└── fsr2_implementation_checklist.md        # This file

third_party/
└── fsr2/                   # FSR SDK 2.1 (to be cloned)
    ├── sdk/include/        # FSR headers
    ├── src/                # FSR source
    └── CMakeLists.txt      # FSR build system
```

## Current Status

✅ **Phase 1 Complete:**
- Upscaler abstraction layer fully implemented
- Memory management in place
- Error handling and diagnostics
- Configuration API
- Ready for GPU device binding

⏳ **Phase 2 In Progress:**
- GPU device interface needs implementation
- FSR SDK 2.1 needs to be integrated
- Platform-specific backend setup

---

## API Quick Reference

### Initialization

```cpp
// Initialize with input/output dimensions and mode
int result = upscalerInit(
    640, 480,                    // Input size (game logical resolution)
    1920, 1080,                  // Output size (physical display)
    UpscalerMode::FSR2           // Use AMD FSR2 backend
);

if (result != 0) {
    printf("Init failed: %s\n", upscalerGetLastError());
}
```

### Per-Frame Rendering

```cpp
// Option A: Feed indexed pixels (automatically converted to RGBA)
upscalerSetIndexedInput(_screen_buffer, gCurrentPalette);

// Option B: Feed pre-converted RGBA
upscalerSetRgbaInput(rgbaBuffer);

// Dispatch upscaling on GPU
upscalerDispatch();

// Get upscaled output
const uint32_t* output = upscalerGetOutputBuffer();
int pitch = upscalerGetOutputPitch();

// Copy to SDL texture
SDL_UpdateTexture(gSdlTexture, nullptr, output, pitch);
```

### Configuration

```cpp
// Set quality tier
upscalerSetQuality(UpscalerQuality::BALANCED);

// Set sharpness (0.0 = none, 1.0 = max)
upscalerSetSharpness(0.5f);

// Enable/disable motion vectors
upscalerSetMotionVectorsEnabled(false);

// Reconfigure output on window resize
upscalerReconfigureOutput(newWidth, newHeight);
```

### Shutdown

```cpp
upscalerShutdown();
```

### Query State

```cpp
// Check if upscaler is available
if (upscalerIsAvailable()) {
    printf("FSR2 is ready\n");
}

// Check current state
UpscalerState state = upscalerGetState();
if (state == UpscalerState::ERROR) {
    printf("Error: %s\n", upscalerGetLastError());
}

// Get actual output dimensions
int w, h;
upscalerGetOutputDimensions(w, h);
printf("Output: %dx%d\n", w, h);
```

---

## Config File (fallout2.cfg)

```ini
[system]
# Upscaling backend: none (disabled), fsr2 (AMD FSR)
upscaling_mode=fsr2

# Quality tier: quality (best), balanced (default), performance (fast)
upscaling_quality=balanced

# Sharpness (0.0 = none, 1.0 = maximum)
upscaling_sharpness=0.5

# Use motion vectors for temporal stability (requires Phase 5)
upscaling_motion_vectors=0

[debug]
# Enable verbose upscaler logging
upscaler_debug_trace=0
```

---

## Integration Points (Code Locations)

### 1. Initialization (main.cc or game.cc)

```cpp
// After SDL window/renderer created:
if (upscalerInit(640, 480, screenGetWidth(), screenGetHeight(), 
                 UpscalerMode::FSR2) != 0) {
    diagnosticsLog(DiagnosticsLevel::Warning, "UPSCALER",
        "FSR2 init failed: %s - falling back to legacy scaling",
        upscalerGetLastError());
}
```

### 2. Rendering Loop (svga.cc - renderPresent())

```cpp
void renderPresent() {
    // ... existing code ...
    
    if (upscalerIsAvailable()) {
        // Path 1: Full-screen upscaling
        upscalerSetIndexedInput(_screen_buffer, gCurrentPalette);
        upscalerDispatch();
        
        const uint32_t* upscaled = upscalerGetOutputBuffer();
        SDL_UpdateTexture(gSdlTexture, nullptr, upscaled, 
                         upscalerGetOutputPitch());
        SDL_RenderClear(gSdlRenderer);
        SDL_RenderCopy(gSdlRenderer, gSdlTexture, nullptr, nullptr);
        SDL_RenderPresent(gSdlRenderer);
    } else {
        // Path 2: Legacy per-asset overlays
        windowPresentVirtualScreen();
        // ... existing renderPresent code ...
    }
}
```

### 3. Window Resize (svga.cc - handleWindowSizeChanged())

```cpp
void handleWindowSizeChanged() {
    int newWidth = screenGetWidth();
    int newHeight = screenGetHeight();
    
    if (upscalerReconfigureOutput(newWidth, newHeight) != 0) {
        diagnosticsLog(DiagnosticsLevel::Warning, "UPSCALER",
            "Failed to reconfigure upscaler: %s",
            upscalerGetLastError());
    }
    
    // ... existing resize code ...
}
```

### 4. Shutdown (main.cc)

```cpp
// Before game exit:
upscalerShutdown();
```

---

## Testing Checklist

### Build & Compilation
- [ ] Add FSR SDK to `third_party/fsr2`
- [ ] Update CMakeLists.txt to include FSR
- [ ] Compile: `cmake --build . --config Release`
- [ ] No linker errors related to FSR

### Functionality
- [ ] `upscalerInit()` returns 0 (success)
- [ ] `upscalerIsAvailable()` returns true
- [ ] `upscalerDispatch()` completes without error
- [ ] Output buffer contains valid pixel data
- [ ] No visual corruption or artifacts

### Visual Quality
- [ ] Upscaled image appears sharper than integer scaling
- [ ] No seams or borders visible
- [ ] Lighting smooth and consistent
- [ ] No temporal flicker on static scenes

### Performance
- [ ] Frame time acceptable (< 5ms for upscaling)
- [ ] No stuttering or hitching
- [ ] Stable FPS (60 or target)
- [ ] GPU utilization reasonable (< 50%)

### Configuration
- [ ] Config file settings respected
- [ ] Can disable upscaler via config
- [ ] Quality/sharpness changes apply
- [ ] Window resize handled smoothly

### Error Handling
- [ ] Invalid dimensions rejected
- [ ] Missing GPU device handled gracefully
- [ ] State machine prevents invalid transitions
- [ ] Error messages helpful for debugging

---

## Diagnostic Logging

Enable detailed logging in `fallout2.cfg`:

```ini
[debug]
upscaler_debug_trace=1
```

Expected log output:
```
[INFO] UPSCALER: Upscaler mode: FSR2 (AMD FidelityFX Super Resolution 2)
[INFO] UPSCALER: Allocated input buffer: 640x480 (1228800 bytes)
[INFO] UPSCALER: Allocated output buffer: 1920x1080 (8294400 bytes)
[INFO] UPSCALER: FSR2 context initialized: 640x480 -> 1920x1080
[INFO] UPSCALER: Upscaler initialized successfully
[INFO] UPSCALER: Upscaler quality set to: 1 (scale: 66.67%)
[INFO] UPSCALER: Upscaler sharpness set to: 0.50
```

---

## Troubleshooting

### Build Fails with "ffx_fsr2.h not found"

**Cause:** FSR SDK not in `third_party/fsr2`

**Fix:**
```bash
cd third_party
git clone https://github.com/GPUOpen-Effects/FidelityFX-SuperResolution.git fsr2
cd fsr2 && git checkout v2.1.0
```

### upscalerInit() Returns -1

**Cause:** GPU device not initialized or invalid dimensions

**Fix:**
1. Ensure SDL renderer created before `upscalerInit()`
2. Check dimensions: input must be 640×480, output >= 320×240
3. Check error: `printf("%s\n", upscalerGetLastError())`

### Upscaled Image Looks Blurry

**Cause:** Sharpness too low

**Fix:**
```ini
upscaling_sharpness=0.7  ; Increase sharpness
```

### Output Doesn't Appear

**Cause:** SDL texture not updated with upscaler output

**Fix:**
```cpp
const uint32_t* output = upscalerGetOutputBuffer();
int pitch = upscalerGetOutputPitch();
SDL_UpdateTexture(gSdlTexture, nullptr, output, pitch);
```

### Performance Impact Too High

**Cause:** Output resolution too high or GPU underpowered

**Fix:**
1. Lower output resolution
2. Try `upscaling_quality=performance` mode
3. Profile GPU time to find bottleneck
4. Consider GPU-side input conversion (future optimization)

---

## Development Tips

### Incremental Testing

1. **Test compilation** before trying to run
2. **Test initialization** before dispatch
3. **Test dispatch** on simple input before complex scenes
4. **Test output** by comparing upscaled vs. expected
5. **Test performance** at target resolution

### Debug Output

```cpp
// Add temporary debug logging
diagnosticsLog(DiagnosticsLevel::Info, "UPSCALER",
    "Debug: input_buffer=%p output_buffer=%p pitch=%d",
    gInputBuffer, gOutputBuffer, gOutputPitch);
```

### Memory Leak Detection

Run under valgrind or Visual Studio memory debugger:
```bash
valgrind --leak-check=full ./fallout2-ce
```

Verify:
- Input buffer freed on shutdown
- Output buffer freed on shutdown
- No accumulation of allocations

### GPU Profiling

Use vendor tools to measure FSR dispatch time:
- **NVIDIA:** NVIDIA Nsight
- **AMD:** Radeon GPU Profiler
- **Intel:** Intel Graphics Performance Analyzers

---

## References

- **Upscaler Header:** `src/upscaler.h` (all APIs documented)
- **Upscaler Implementation:** `src/upscaler.cc` (see code comments)
- **Integration Guide:** `docs/fsr_integration_guide.md`
- **FSR Documentation:** https://gpuopen.com/learn/fidelityfx-super-resolution-2/
- **Architecture Analysis:** `docs/full_screen_ai_upscaling_analysis.md`

---

## Next Steps

1. ✅ Clone FSR SDK to `third_party/fsr2`
2. ⏳ Update CMakeLists.txt for FSR2 build
3. ⏳ Test compilation
4. ⏳ Implement GPU device interface
5. ⏳ Integrate with renderPresent()
6. ⏳ Test on target hardware
7. ⏳ Measure performance and visual quality
8. ⏳ Add configuration UI
9. ⏳ Implement motion vector support (optional)
10. ⏳ Deprecate old per-asset upscaling

Ready to move forward! 🚀
