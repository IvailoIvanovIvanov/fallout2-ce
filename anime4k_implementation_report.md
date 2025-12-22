# Anime4K Implementation Report

## Overview
The Anime4K v3.2 upscaling algorithm has been successfully ported to the new Direct3D 12 rendering pipeline. It is now available as a fully GPU-accelerated scaler pass, replacing the standard bilinear scaler when `mode=1` (Anime4K) is selected.

## Implementation Details

### 1. Anime4kPass Class
A new `Anime4kPass` class (inheriting from `ShaderPass`) was created in `src/renderer/anime4k_pass.h` and `src/renderer/anime4k_pass.cc`.
- **Shader**: The HLSL shader source was embedded directly into the C++ file. It implements the Anime4K v3.2 Upscale Original x2 algorithm.
- **Features**:
  - **Bilinear Interpolation**: Used as the base upscaling method.
  - **Luma-based Edge Detection**: Uses Sobel gradients to detect edges.
  - **Adaptive Sharpening**: Sharpens edges based on gradient magnitude and direction.
  - **Letterboxing**: Handles aspect ratio correction (black bars) directly in the compute shader.
  - **Configurable Strength**: The sharpening strength can be adjusted via the `sharpness` configuration.

### 2. RenderPipeline Integration
The `RenderPipeline` class was modified to support swappable scaler passes.
- **SetScalerPass**: A new method `SetScalerPass(std::unique_ptr<ShaderPass> pass)` allows replacing the default `ScalerPass` with a custom one (like `Anime4kPass`).
- **Polymorphism**: The `mScalerPass` member was changed from `ScalerPass` to `ShaderPass` to allow polymorphic behavior.

### 3. Upscaler Configuration
The `UpscalerImpl::init` method in `src/renderer/upscaler.cc` was updated to use the new pass.
- When `mode=1` (Anime4K) is selected:
  - An `Anime4kPass` instance is created.
  - The sharpening strength is set from the `sharpness` config value.
  - The pass is injected into the pipeline using `SetScalerPass`.
  - A diagnostic log confirms the activation: `[SCALER] Using Anime4K Scaler (strength=...)`.

## Usage
To use the Anime4K upscaler, ensure your `fallout2.cfg` contains:
```ini
[upscaler]
mode=1
sharpness=0.5  ; Adjust strength (0.0 - 1.0)
```

## Performance
The Anime4K pass runs entirely on the GPU using Compute Shaders. It is significantly faster than CPU-based implementations and provides better visual quality than standard bilinear scaling for anime-style art (which Fallout 2's sprites resemble).

## Files Modified/Created
- `src/renderer/anime4k_pass.h` (New)
- `src/renderer/anime4k_pass.cc` (New)
- `src/renderer/render_pipeline.h` (Modified)
- `src/renderer/render_pipeline.cc` (Modified)
- `src/renderer/upscaler.cc` (Modified)
- `CMakeLists.txt` (Modified)
