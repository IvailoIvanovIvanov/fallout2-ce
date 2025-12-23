# Anime4K GAN UUL Implementation Report

## Overview
The `Anime4K_Upscale_GAN_x4_UUL.glsl` shader has been integrated into the engine. This shader is significantly more complex than previous versions, involving multiple passes, texture upscaling, and complex binding chains.

## Key Changes

### 1. Shader Parsing (`ParseMPVShader`)
- **Directives Supported**:
    - `//!HOOK MAIN`: Defines a new pass.
    - `//!BIND <Texture>`: Binds a texture to the pass.
    - `//!SAVE <Texture>`: Saves the output of the pass to a named texture.
    - `//!WIDTH <Ref> <Mult>` / `//!HEIGHT <Ref> <Mult>`: Defines the output size of the pass, supporting multipliers (e.g., `conv0ups.w 4 *`).
- **Logic**: The parser now correctly calculates texture sizes based on references and multipliers, which is essential for the 4x upscaling passes.

### 2. Execution Logic (`Execute`)
- **Texture Management**:
    - Implemented a local `sizes` map to track the dimensions of every texture (INPUT, MAIN, HOOKED, and intermediates).
    - This ensures that when a pass binds a texture (like `conv1ups` which is 4x), the shader receives the correct size uniforms (`_size`, `_pt`).
- **Binding Chain**:
    - `HOOKED` correctly tracks the output of the last pass that wrote to `MAIN`.
    - `MAIN` correctly updates its size in the tracking map when written to.
- **Uniforms**:
    - Correctly sets `_size` (width, height) and `_pt` (pixel size) for every bound texture.

### 3. Data Structures
- Updated `Anime4kPass::Pass` struct to include `width` and `height` for each pass.
- Updated `Anime4kPass` class to handle the new parsing and execution flow.

## Verification
- The shader file `data/shaders/Anime4K_Upscale_GAN_x4_UUL.glsl` is present.
- The C++ implementation in `src/renderer/anime4k_pass.cc` matches the shader's requirements (mpv/libplacebo syntax).

## Next Steps
- Compile and run the engine.
- Verify that the shader loads without errors.
- Check visual output for correct 4x upscaling.
