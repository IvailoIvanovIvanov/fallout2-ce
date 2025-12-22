# Phantom Screen Implementation Report

## Overview
We have successfully refactored the rendering pipeline to implement the "Phantom Screen" architecture. This ensures that all filter effects (Blur, HDR, ML Upscaling) operate on the native 640x480 resolution, preventing scaling artifacts and ensuring correct pixel processing. A dedicated `ScalerPass` now handles the final upscaling and aspect ratio correction.

## Changes Implemented

### 1. Phantom Screen Architecture
- **Intermediate Buffers**: The pipeline now maintains two internal 640x480 buffers.
- **1:1 Processing**: All filters (`BlurFilter`, `HdrFilter`, `MlUpscalePass`) are initialized and executed at 640x480 resolution.
- **Ping-Pong Execution**: The pipeline swaps between these two buffers for each filter pass.

### 2. ScalerPass
- **New Class**: `src/renderer/scaler_pass.h` / `.cc`
- **Function**: Takes the final 640x480 image and scales it to the window resolution.
- **Features**:
    - Implements Aspect Ratio Correction (Letterboxing).
    - Uses Nearest Neighbor scaling (configurable in shader) for sharp pixels.
    - Adds black bars where necessary.

### 3. Interface Updates
- **ShaderPass**: `Execute` method updated to `void Execute(D3D12Context& context, ID3D12Resource* input, ID3D12Resource* output)`.
- **Decoupling**: Filters no longer depend on `BufferManager` for their execution, allowing flexible chaining of resources.

### 4. Build System
- Added `src/renderer/scaler_pass.cc` and `src/renderer/scaler_pass.h` to `CMakeLists.txt`.

## Verification
- **Build Status**: Successful.
- **Linker Errors**: Resolved.

## Next Steps
- **Testing**: Run the game to verify the visual output.
- **Tuning**: Adjust filter parameters (Blur strength, HDR saturation) as they now operate on fewer pixels (640x480 vs Window Size), which might make them appear stronger.
