# OpenGL Migration Plan

## Objective
Migrate the rendering backend from Direct3D 12 to OpenGL to allow direct usage of GLSL shader files (specifically for Anime4K) and remove the dependency on DirectX.

## Current Architecture
*   **Windowing**: SDL2
*   **Rendering**: Direct3D 12 (Compute Shaders for upscaling)
*   **Shaders**: HLSL (compiled at runtime or embedded)
*   **Key Classes**:
    *   `GpuDevice` / `D3D12Context`: Manages D3D12 device, command queue, and resources.
    *   `RenderPipeline`: Orchestrates the rendering passes.
    *   `Anime4kPass`, `ScalerPass`: Specific rendering passes using D3D12 commands.

## Migration Roadmap

### Phase 1: Abstraction Layer
Create a hardware-agnostic interface for GPU operations.

1.  **Define `GpuContext` Interface**:
    *   Abstract base class in `src/renderer/gpu_context.h`.
    *   Methods: `Init()`, `CreateTexture()`, `CreateBuffer()`, `Dispatch()`, `BeginFrame()`, `EndFrame()`.
2.  **Refactor `D3D12Context`**:
    *   Make it inherit from `GpuContext`.
    *   Ensure existing functionality works through the interface.

### Phase 2: OpenGL Implementation
Implement the OpenGL backend.

1.  **Build System (`CMakeLists.txt`)**:
    *   Add `opengl32.lib` (Windows) or `-lGL` (Linux/Mac) to linker settings.
    *   Add `glew` or `glad` for OpenGL extension loading (if needed, or use SDL's loader).
2.  **Create `OpenGLContext`**:
    *   Implement `GpuContext`.
    *   Use `SDL_GL_CreateContext` for initialization.
    *   Implement `CreateTexture` using `glGenTextures` / `glTexStorage2D`.
    *   Implement `Dispatch` using `glDispatchCompute`.
3.  **Shader Management**:
    *   Implement a GLSL shader loader that reads `.glsl` files directly from the `data/shaders` directory.
    *   Compile and link Compute Shaders at runtime.

### Phase 3: Pipeline Integration
Switch the engine to use the new backend.

1.  **Update `RenderPipeline`**:
    *   Hold a `std::unique_ptr<GpuContext>` instead of `D3D12Context`.
    *   Instantiate `OpenGLContext` or `D3D12Context` based on configuration (`fallout2.cfg`).
2.  **Port Shader Passes**:
    *   Refactor `Anime4kPass`, `ScalerPass`, etc., to use the `GpuContext` API.
    *   Ensure they can handle both HLSL (for D3D12) and GLSL (for OpenGL) sources, or switch entirely to GLSL if D3D12 is dropped.

### Phase 4: GLSL Integration
Enable direct editing of GLSL files.

1.  **Asset Loading**:
    *   Ensure `.glsl` files are copied to the build output directory.
2.  **Hot Reloading (Optional)**:
    *   Watch for file changes and recompile shaders at runtime for rapid iteration.

## Benefits
*   **Cross-Platform**: OpenGL works on Linux, macOS, and Windows (D3D12 is Windows-only).
*   **Direct GLSL Usage**: No need to port shaders to HLSL manually.
*   **Simplicity**: OpenGL state machine can be simpler for this specific use case (compute post-processing) than D3D12's explicit management.

## Estimated Effort
*   **High**: Requires rewriting the core rendering loop and all shader passes.
*   **Risk**: Performance differences, driver compatibility issues.

## Immediate Next Steps
1.  Create `src/renderer/gpu_context.h` interface.
2.  Create `src/renderer/opengl_context.h` stub.
3.  Update `CMakeLists.txt` to link OpenGL.
