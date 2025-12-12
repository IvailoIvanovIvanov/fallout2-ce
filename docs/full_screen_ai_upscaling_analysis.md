# Full-Screen AI Upscaling Analysis

> Investigating GPU-based Full-Screen Upscaling (FSR/DLSS) as an Alternative to Per-Asset Upscaling

## Executive Summary

**Yes, a full-screen GPU-based upscaling approach using technologies like AMD FSR, NVIDIA DLSS, or open alternatives is both feasible and potentially **superior** to the current per-asset upscaling strategy.** This document details the analysis, benefits, challenges, and implementation roadmap.

---

## Current Architecture Review

### Existing Implementation (Per-Asset Upscaling)

The project currently employs a **dual-display abstraction** with per-asset HD upscaling:

1. **Phantom Display (640×480)** – Game runs at original resolution
   - 8-bit indexed pixels in `_screen_buffer`
   - Original FRM assets in `gArtCache`
   - All game logic isolated in this logical space

2. **Real Display (Variable Resolution)** – Renders upscaled version
   - HD assets registered in `RenderAssetRegistry`
   - Separate overlays (logical + physical) for each window
   - Scale tables (`DisplayScalerScaleTable`) map 640×480 → physical resolution
   - Orchestrator composites per-asset using `render_display_orchestrator.cc`

3. **Assets Upscaling Chain**
   - Tiles: `tileEmitRenderCommand()` → `render_display_orchestrator` finds HD or fallback
   - Objects: `_obj_render_object()` → registered in asset registry
   - UI: `artRender()` → looks up HD variants per fid/frame
   - Fallback: Auto-upscale indexed RGBA when HD missing

### Known Limitations of Per-Asset Approach

1. **Seams & Borders** – Artifacts at asset boundaries due to independent upscaling
2. **Lighting Discontinuities** – Lighting applied per-asset can cause visible transitions
3. **Coordinate Misalignment** – Rounding errors accumulate when upscaling individual sprites
4. **Memory Overhead** – HD registry must maintain separate RGBA for every asset variant
5. **Complexity** – Asset tracking, lifecycle management, and fallback logic are intricate
6. **Dependency on HD Packs** – Upscaling quality depends on having HD assets; fallback is basic bilinear

---

## Full-Screen AI Upscaling Approach

### Proposed Architecture

```
┌─────────────────────────────────────────────────────────┐
│               PHANTOM DISPLAY (640×480)                  │
│  Game logic, original 8-bit rendering, _screen_buffer   │
└────────────────────┬────────────────────────────────────┘
                     │
                     ▼
         ┌───────────────────────────┐
         │  Option 1: Skip Overlays  │
         │  Render 8-bit directly    │
         │  (or keep RGBA overlays   │
         │   only for mixed content) │
         └────────────┬──────────────┘
                      │
                      ▼
         ┌──────────────────────────┐
         │  INTERMEDIATE BUFFER     │
         │  (640×480 RGBA or 8-bit) │
         │  "Source for upscaler"   │
         └────────────┬─────────────┘
                      │
        ┌─────────────┴──────────────┐
        │ AI UPSCALING PASS (GPU)    │
        │                            │
        │ Input: 640×480 source      │
        │ Algorithm: FSR/DLSS/etc    │
        │ Output: Physical resolution│
        │                            │
        │ (or Shaders/compute if     │
        │  using custom approach)    │
        └────────────┬───────────────┘
                     │
                     ▼
         ┌──────────────────────────┐
         │ SDL TEXTURE (upscaled)    │
         │ Display on physical screen│
         └──────────────────────────┘
```

### Why This Works

1. **Unified Image Source** – Composite all layers (tiles, objects, UI) into a single 640×480 buffer *before* upscaling
2. **Coherent Upscaling** – AI upscaler sees the complete scene, not fragmented assets
3. **No Seams** – Since everything is upscaled together, edges align naturally
4. **Lighting Preserved** – Lighting calculations remain per-frame and are upscaled as a unit
5. **Zero Fallback Complexity** – 8-bit indexed can be converted to RGBA once, then upscaled
6. **Natural Anti-Aliasing** – AI algorithms handle edge aliasing that per-asset approaches miss

---

## Technical Feasibility Analysis

### Current State: Suitable Foundations

The existing codebase provides strong foundations:

| Component | Current Status | Reusability |
|-----------|---|---|
| **Phantom Display** | ✅ Fully implemented | Stays unchanged – is the upscaler's input source |
| **640×480 Buffer** | ✅ `_screen_buffer` working | Perfect intermediate representation |
| **Scale Tables** | ✅ `DisplayScalerScaleTable` ready | Can drive viewport computation, not per-asset upscaling |
| **SDL Renderer** | ✅ SDL 2.0 in place | Handles final composite to physical screen |
| **Coordinate Translation** | ✅ `display_scaler.cc` complete | Input mapping unchanged; output mapping simplified |
| **Overlay System** | ⚠️ Can be optional | Can simplify or remove per-asset overlays |

### What Changes

| Change | Impact | Effort |
|--------|--------|--------|
| **Remove Per-Asset HD Registry Dependency** | Significant but optional – registry still works for mixed content | Medium |
| **Render to Intermediate 640×480 Buffer** | Already done in `_screen_buffer` | None – already exists |
| **Upscaler Integration Point** | New: After `renderPresent()` presentation setup | Low-Medium |
| **Shader/Filter Setup** | Depends on upscaler choice | Medium-High |
| **Viewport Scaling Logic** | Simplifies: no per-asset scale tables, just output mapping | Medium |

---

## Implementation Options

### Option A: AMD FSR 2 Integration (Recommended for Windows)

**What:** AMD FidelityFX Super Resolution 2  
**Pros:**
- Open-source, permissive license
- Works on any GPU (AMD, NVIDIA, Intel)
- Temporal upscaling (uses motion for quality)
- Active development & community

**Integration Point:**
```cpp
// In svga.cc, after renderPresent() setup:

1. Create FSR2 context (1920×1080 → chosen output)
2. Each frame:
   a. Copy _screen_buffer (640×480) to FSR input texture
   b. Compute motion vectors (from frame-to-frame diff or game state)
   c. Call FSR2 upscale() → outputs to SDL texture
   d. SDL_RenderCopy → to screen

// Pseudo-code:
FsrContext* upscaler = FsrCreateContext(640, 480, physicalWidth, physicalHeight);
while (gameRunning) {
    // Phantom display renders to _screen_buffer (unchanged)
    // ...
    
    // New upscaling step (replaces old presenter logic)
    copyScreenBufferToFsrInput(upscaler, _screen_buffer);
    FsrDispatch(upscaler);  // GPU-side upscaling
    copyFsrOutputToSdlTexture(upscaler, gSdlTextureSurface);
    
    renderPresent();  // SDL handles final output
}
```

**Challenges:**
- Requires FSR SDK integration (redistributable or source)
- Motion vector calculation (can use simple frame diff or zero vectors for initial)
- Temporal artifacts if motion vectors are poor

### Option B: NVIDIA DLSS 3

**What:** NVIDIA Deep Learning Super Sampling  
**Pros:**
- Industry-leading image quality
- Handles motion excellently
- Frame generation bonus (DLSS 3)

**Cons:**
- NVIDIA-only (locks out AMD/Intel users)
- License restrictions (require official registration)
- Less suitable for indie/retro projects

**Verdict:** Less ideal for a portable multi-GPU project, but technically feasible.

### Option C: Intel XeSS

**What:** Intel Xe Super Sampling  
**Pros:**
- Works on Intel Arc + some other GPUs
- Open-source drivers

**Cons:**
- Smaller market coverage than FSR
- Less mature

**Verdict:** Good fallback but not primary target.

### Option D: Custom Shader-Based Upscaling

**What:** Custom compute shader using established algorithms (bicubic, Lanczos, edge-aware)  
**Pros:**
- Full control, no external dependency
- Can optimize for pixel-art games specifically
- Works on all GPUs

**Cons:**
- More development effort
- Won't match AI quality of FSR/DLSS
- Need GLSL/HLSL expertise

**Verdict:** Consider as fallback or if external SDK integration is blocked.

---

## Recommended Approach: Hybrid Strategy

### Phase 1: Full-Screen RGBA Upscaling (Foundation)

1. **Consolidate to Single 640×480 RGBA Buffer**
   ```cpp
   // Create intermediate RGBA buffer (only once, not per-window)
   unsigned char* gScreenBufferRgba = allocate(640 * 480 * 4);  // ARGB8888
   
   // Convert 8-bit indexed → RGBA on each frame (before upscaling)
   convertIndexedToRgba(_screen_buffer, gScreenBufferRgba, gCurrentPalette);
   ```

2. **Simplify Overlay Logic**
   - Option A: Remove per-asset overlays entirely (use upscaler only)
   - Option B: Keep overlays only for UI that needs pixel-perfect alignment
   - Option C: Render overlays into the 640×480 RGBA buffer before upscaling

3. **Set Up Upscaler Input**
   ```cpp
   // svga.cc: New function
   void upscalerUpdateFrame() {
       // Copy gScreenBufferRgba to upscaler input texture
       // Trigger upscaling
       // Copy result to gSdlTextureSurface or direct to SDL texture
   }
   ```

### Phase 2: FSR 2 Integration

1. **Add FSR SDK**
   - Clone or download FSR 2 source
   - Link against `ffx_fsr2` library (or static build)

2. **Initialize Upscaler**
   ```cpp
   FfxFsr2Context fsr2Context;
   FfxFsr2CreateContextDesc createDesc = {
       .displaySize = {physicalWidth, physicalHeight},
       .maxUpscaleSize = {physicalWidth, physicalHeight},
       .backendInterface = getBackendInterface(),  // DirectX 12, Vulkan, etc.
   };
   ffxFsr2ContextCreate(&fsr2Context, &createDesc);
   ```

3. **Dispatch Each Frame**
   ```cpp
   FfxFsr2DispatchDescription dispatchDesc = {
       .color = inputTexture,  // 640×480 RGBA
       .depth = nullptr,  // Optional: can provide for better temporal coherence
       .motionVectors = nullptr,  // Can compute from frame delta
       .exposure = nullptr,
       .reactive = nullptr,
       .transparencyAndComposition = nullptr,
       .jitterOffset = {frameJitterX, frameJitterY},
       .motionVectorScale = {1.0f, 1.0f},
       .renderSize = {640, 480},
       .enableSharpening = true,
       .sharpness = 0.5f,
   };
   ffxFsr2ContextDispatch(&fsr2Context, &dispatchDesc);
   ```

4. **Extract Output**
   ```cpp
   // Copy upscaled result (stored in output texture) to SDL
   // Update gSdlTextureSurface with upscaled pixels
   ```

### Phase 3: Fallback & Configuration

1. **Config Options**
   ```ini
   [system]
   upscaling_mode=fsr2          ; fsr2, custom, none (legacy integer scaling)
   upscaling_quality=balanced   ; quality, balanced, performance
   upscaling_sharpness=0.5      ; 0.0 (none) to 1.0 (max)
   
   [debug]
   upscaler_debug_overlay=0     ; Show upscaler quality metrics
   ```

2. **Graceful Fallback**
   ```cpp
   if (upscalerInit() != FSX_OK) {
       debugPrint("FSR2 init failed, falling back to integer scaling");
       useIntegerScaling = true;
   }
   ```

---

## Benefits Over Current Per-Asset Approach

### 1. **Eliminated Seams & Borders**
- ✅ No asset boundaries because everything is upscaled as one image
- ✅ Edges naturally anti-aliased by AI algorithms
- ✅ Critter outlines align with tile boundaries perfectly

### 2. **Simplified Lighting**
- ✅ Lighting baked into 640×480 image before upscaling
- ✅ No per-asset lighting adjustments needed
- ✅ Consistent lighting across entire viewport

### 3. **Reduced Memory Overhead**
- ✅ No per-asset HD registry required (optional, can deprecate)
- ✅ Single 640×480 + upscaler working buffers
- ✅ Eliminates fallback RGBA auto-generation storage

### 4. **Reduced Complexity**
- ✅ No asset tracking, lifecycle, or registration
- ✅ No dimension validation or fallback promotion logic
- ✅ Simpler orchestrator (or can be removed entirely)
- ✅ Cleaner code paths (fewer branches)

### 5. **Better Quality**
- ✅ AI algorithms see entire scene (global context)
- ✅ Motion estimation can leverage temporal coherence
- ✅ Handles edge cases (thin lines, text) better
- ✅ Works equally well for all content (no HD packs needed)

### 6. **Flexible Quality Tiers**
- ✅ Same code path, adjust quality via sharpness/mode config
- ✅ Supports FSR's quality/balanced/performance modes
- ✅ Future: easy to add DLSS or custom upscalers

---

## Challenges & Mitigations

### Challenge 1: Temporal Artifacts (Motion Estimation)

**Problem:** FSR2 requires motion vectors for best temporal coherence. Fallout uses memmove for map scrolling, which produces no game-object motion vectors.

**Solutions:**
- ✅ **Frame Differencing:** Compare consecutive 640×480 frames, estimate motion (brute-force or simple block matching)
- ✅ **Zero Vectors + Jitter:** FSR2 can work with zero motion vectors + temporal jitter (quality degraded but stable)
- ✅ **Fixed Motion Hints:** When map scrolls, inject scroll delta as motion (detect in `tileWindowScroll` or viewport events)
- ✅ **Fallback Mode:** Degraded FSR quality but still better than per-asset upscaling

**Recommendation:** Start with frame differencing, upgrade to game-state hints later.

### Challenge 2: UI Pixel-Perfect Alignment

**Problem:** Some UI elements (tooltips, exact cursor positions) may blur slightly after AI upscaling.

**Solutions:**
- ✅ Render UI separately at physical resolution (keep overlays for UI only)
- ✅ Use FSR2's reactive textures to mark UI regions → preserve sharpness
- ✅ Accept slight blur as quality trade-off (most players won't notice)

**Recommendation:** Mark UI regions with reactive maps, let FSR preserve fine details.

### Challenge 3: Integration Complexity (FSR SDK)

**Problem:** Adding FSR 2 requires linking against external library & handling multiple backends (DX12, Vulkan, etc.).

**Solutions:**
- ✅ **Use Pre-built Binaries:** FSR 2 ships with static libs for common backends
- ✅ **Start with One Backend:** Support SDL's chosen backend (likely DX11/DX12 on Windows)
- ✅ **Containerize Upscaler:** Wrap FSR in a clean C++ interface, isolate integration points
- ✅ **Custom Shader Fallback:** Implement basic bicubic upscaling as fallback if FSR unavailable

**Recommendation:** Abstract upscaler behind interface, start with FSR 2 + DirectX, add Vulkan later if needed.

### Challenge 4: Retro Aesthetic vs. AI Sharpness

**Problem:** FSR's sharpening may over-smooth the retro pixel-art look some players prefer.

**Solutions:**
- ✅ Configurable sharpness: 0.0 (none) to 1.0 (max)
- ✅ Separate mode: `upscaling_mode=none` → pure integer scaling
- ✅ Custom upscalers for pixel art (nnedi3, xBRZ) as alternatives
- ✅ Community feedback: let modders choose preferred upscaler

**Recommendation:** Expose sharpness slider in options, default to balanced (0.5).

---

## Migration Path: From Per-Asset to Full-Screen

### Step 1: Parallel Systems (Low Risk)

Keep existing overlay system, add full-screen upscaler **alongside**:

```cpp
// In renderPresent():
if (gUpscalerMode == UPSCALER_NONE) {
    // Legacy path: per-asset overlays
    windowPresentVirtualScreen();
    renderPresent();
} else if (gUpscalerMode == UPSCALER_FSR2) {
    // New path: full-screen upscaling
    convertScreenBufferToRgba();
    upscalerDispatch();
    renderPresent();
}
```

**Benefit:** Can enable full-screen upscaling via config flag without touching orchestrator.

### Step 2: Remove HD Registry Dependency (Medium Risk)

Once full-screen upscaler is stable, deprecate per-asset overlays:

```cpp
// Turn off orchestrator, skip asset registry lookups
// Delete: render_display_orchestrator.cc (or stub it)
// Delete: RenderAssetRegistry lifecycle code
// Keep: basic structure in case we add mixed content later
```

### Step 3: Simplify Input Pipeline (Medium Risk)

Reduce coordinate translation complexity:

```cpp
// Old: per-asset scale tables
// New: single output scale table (640×480 → physical resolution)
// Upscaler handles all internal scaling
```

### Step 4: Clean Up (Optional)

Once stable, remove legacy code:

```cpp
// Delete: unused overlay allocation in windowCreate
// Delete: window-level per-window true-color compositing
// Delete: asset tracking in art.cc
```

---

## Proof of Concept Implementation

### Minimal Working Example

```cpp
// upscaler.h
#pragma once

namespace fallout {

enum class UpscalerMode {
    NONE,      // Integer scaling only
    FSR2,      // AMD FidelityFX Super Resolution 2
    CUSTOM,    // Shader-based upscaling
};

class Upscaler {
public:
    static Upscaler* getInstance();
    
    bool init(int inputWidth, int inputHeight, int outputWidth, int outputHeight);
    void shutdown();
    
    void setMode(UpscalerMode mode);
    bool dispatch(unsigned char* inputRgba, unsigned char* outputRgba);
    
    void setSharpness(float value);  // 0.0 to 1.0
    void setQuality(const char* mode);  // "quality", "balanced", "performance"
    
private:
    Upscaler() = default;
    
    bool initFsr2();
    bool dispatchFsr2();
    
    UpscalerMode gMode = UpscalerMode::NONE;
    // ... FSR2 context, input/output textures, etc.
};

}  // namespace fallout
```

```cpp
// In svga.cc renderPresent() replacement:
void renderPresentWithUpscaling() {
    Upscaler* upscaler = Upscaler::getInstance();
    
    // Step 1: Convert 640×480 indexed to RGBA
    unsigned char* rgbaBuffer = (unsigned char*)malloc(640 * 480 * 4);
    convertIndexedToRgba(_screen_buffer, rgbaBuffer, gCurrentPalette);
    
    // Step 2: Upscale to physical resolution
    unsigned char* outputBuffer = (unsigned char*)malloc(screenWidth * screenHeight * 4);
    upscaler->dispatch(rgbaBuffer, outputBuffer);
    
    // Step 3: Copy to SDL texture
    SDL_UpdateTexture(gSdlTexture, nullptr, outputBuffer, screenWidth * 4);
    SDL_RenderClear(gSdlRenderer);
    SDL_RenderCopy(gSdlRenderer, gSdlTexture, nullptr, nullptr);
    SDL_RenderPresent(gSdlRenderer);
    
    free(rgbaBuffer);
    free(outputBuffer);
}
```

---

## Comparison Table: Per-Asset vs. Full-Screen

| Aspect | Per-Asset (Current) | Full-Screen AI (Proposed) |
|--------|---|---|
| **Seam Artifacts** | ⚠️ Common | ✅ None |
| **Lighting Consistency** | ⚠️ Per-asset | ✅ Global |
| **Memory Usage** | ⚠️ High (registry) | ✅ Low (1 buffer) |
| **Code Complexity** | ⚠️ High (orchestrator, registry) | ✅ Low (upscaler abstraction) |
| **HD Asset Dependency** | ⚠️ Requires packs | ✅ None needed |
| **Quality (without HD packs)** | ⚠️ Basic bilinear | ✅ AI-driven |
| **Fallback Performance** | ⚠️ Degraded but works | ✅ Integer scale fallback |
| **GPU Utilization** | ⚠️ Per-sprite compositing | ✅ Single upscaling pass |
| **Temporal Stability** | ✅ Stable | ⚠️ Needs motion vectors |
| **UI Pixel-Perfect** | ✅ Possible | ⚠️ May blur slightly |

---

## Recommendations

### For Short-Term (1-2 Sprints)

1. **Research & Prototype:**
   - Build minimal upscaler abstraction (`upscaler.h`)
   - Integrate FSR 2 SDK (or evaluate alternatives)
   - Test with 640×480 → 1920×1080 upscaling on sample frames
   - Measure frame time impact

2. **Parallel Testing:**
   - Keep current orchestrator working
   - Add upscaler as config option
   - Compare visual quality side-by-side

### For Medium-Term (3-4 Sprints)

3. **Refine Upscaler:**
   - Implement frame differencing for motion
   - Add sharpness/quality config
   - Test across multiple scenes (combat, menus, UI)
   - Profile performance on various GPUs

4. **Graceful Fallback:**
   - Fallback to integer scaling if upscaler unavailable
   - Config option to force legacy per-asset mode for regression testing

### For Long-Term (5+ Sprints)

5. **Deprecate Per-Asset Overlays:**
   - Remove orchestrator once full-screen is stable
   - Delete unnecessary asset tracking code
   - Simplify coordinate translation

6. **Future Upscalers:**
   - Add support for DLSS (NVIDIA)
   - Explore XeSS (Intel)
   - Custom pixel-art upscalers (nnedi3, xBRZ)

---

## Conclusion

**A full-screen GPU-based upscaling approach is not only feasible but strongly recommended as a **superior alternative** to the current per-asset upscaling strategy.**

### Key Takeaways

1. ✅ **Eliminates core issues:** seams, lighting discontinuities, memory overhead
2. ✅ **Simpler architecture:** remove orchestrator, asset registry, complex lifecycle
3. ✅ **Better quality:** AI algorithms see entire scene, natural anti-aliasing
4. ✅ **Flexible:** easy to add multiple upscaler backends (FSR2, DLSS, custom)
5. ✅ **Risk-manageable:** can prototype in parallel with existing system

### Recommended First Step

**Implement FSR 2 integration** as the primary upscaling backend:
- Open-source, permissive license
- Works across AMD/NVIDIA/Intel GPUs
- Industry-proven quality
- Active development & community support
- Can be integrated incrementally alongside existing code

The effort to implement is moderate (~1-2 weeks for a solid prototype) and the benefits are substantial.

---

## Implementation Status (Phases 1-4)

### ✅ Phase 1: D3D12 Renderer Integration
- Modified `svga.cc` to use Direct3D 12 renderer instead of OpenGL
- Changed SDL hint from `"opengl"` to `"direct3d12"`
- Removed `SDL_WINDOW_OPENGL` flag (incompatible with Direct3D)
- **Status:** COMPLETE - Build successful

### ✅ Phase 2: GPU Device Binding Module (`gpu_device.h/cc`)
- Created `gpuDeviceInit()` - Initializes D3D12 device and command queue
- Created `gpuDeviceGetDevice()` - Returns ID3D12Device pointer
- Created `gpuDeviceGetCommandQueue()` - Returns ID3D12CommandQueue pointer
- Created command allocator and command list factory functions
- GPU synchronization primitives (fence, wait for GPU)
- **Status:** COMPLETE - Build successful - Device initialization ready

### ✅ Phase 3: GPU Device Integration with Upscaler
- Updated `upscaler.cc` to include `gpu_device.h`
- Added GPU context member variables to `UpscalerImpl` class
- Updated `initFsr2()` to query GPU device and create command allocator
- Updated `shutdownFsr2()` to clean up GPU resources
- **Status:** COMPLETE - Build successful - GPU context acquisition working

### ✅ Phase 4: GPU Texture Management (`gpu_texture.h/cc`)
- Created `gpuTextureCreate()` - D3D12 texture resource allocation
- Created `gpuTextureRelease()` - Resource cleanup
- Created `gpuTextureUpload()` - CPU→GPU buffer transfers
- Created `gpuTextureDownload()` - GPU→CPU readback (Phase 5)
- Integrated GPU textures into upscaler workflow
- Updated `dispatchFsr2()` to upload input texture to GPU
- **Status:** COMPLETE - Build successful - Texture creation infrastructure ready

### ⏳ Phase 5: FSR2 Context Creation & Dispatch (In Progress)
- TODO: Implement `ffxCreateContext()` call in `initFsr2()`
- TODO: Bind GPU textures to FSR2 input/output
- TODO: Implement `ffxDispatch()` call in `dispatchFsr2()`
- TODO: Complete GPU→CPU readback for output texture
- TODO: Verify FSR2 compute shader execution on GPU

### 📋 Phase 6: Performance & Quality Tuning
- Measure GPU utilization and throughput
- Tune FSR2 jitter and sharpness parameters
- Optimize texture transfer pipeline
- Profile CPU/GPU synchronization overhead

---

## References

- **AMD FidelityFX Super Resolution 2:** https://github.com/GPUOpen-Effects/FidelityFX-SuperResolution
- **Display Abstraction Overview:** `docs/Enginge_abstraction_overview.md`
- **Asset Architecture:** `docs/asset_architecture.md`
- **Virtual Adapter Plan:** `docs/virtual_adapter_plan.md`

