# Rendering Pipeline Refactoring & Asynchronous Roadmap

## 1. Objective
Transform the current monolithic `upscaler.cc` into a modular, readable, and high-performance rendering pipeline. The goal is to support **only** Anime4K and ML-based (Real-ESRGAN) upscaling, running entirely on the GPU with asynchronous readback to maximize FPS.

## 2. Architectural Principles
*   **Single Responsibility Principle (SRP)**: Split `UpscalerImpl` into dedicated classes for Resource Management, Shader Execution, and Pipeline Orchestration.
*   **GPU-Centric**: Minimize CPU-side pixel manipulation. The pipeline should be: `Upload -> Preprocess (Shader) -> Upscale (ML/Shader) -> Display`.
*   **Asynchronous Execution**: Decouple CPU submission from GPU execution using a Frame N / Frame N-1 double-buffering strategy.

## 3. Refactoring Roadmap

### Phase 1: Cleanup & Simplification (De-clutter)
*   **Goal**: Remove dead weight to make refactoring easier.
*   [ ] **Remove Integer Scaling**: Delete `dispatchIntegerScale`, `UpscalerMode::INTEGER_2X/3X/4X`.
*   [ ] **Remove CPU Post-Processing**: Delete `filterApplyKuwahara` and other CPU-side filters.
*   [ ] **Remove Legacy Options**: Remove unused configuration parameters (e.g., "Motion Vectors" if unused).
*   [ ] **Consolidate Headers**: Clean up `upscaler.h` to expose only the high-level pipeline API.

### Phase 2: Modularization (SRP Extraction)
*   **Goal**: Break `upscaler.cc` into manageable components.
*   [ ] **Create `D3D12Context`**:
    *   Responsibilities: Device creation, Command Queue management, Fence synchronization, Descriptor Heap management.
    *   *Why*: `upscaler.cc` currently manages raw D3D12 pointers manually.
*   [ ] **Create `BufferManager`**:
    *   Responsibilities: Managing `InputBuffer`, `OutputBuffer`, `ReadbackBuffer`.
    *   *Key Feature*: Support for **Double Buffering** (Frame N / Frame N-1) internally.
*   [ ] **Create `ShaderPass` Abstraction**:
    *   Base class for any GPU operation.
    *   Subclasses: `PreprocessingPass` (Blur/HDR), `Anime4kPass`, `PostProcessPass` (Format conversion).

### Phase 3: Pipeline Unification
*   **Goal**: Create a linear, configurable pipeline.
*   [ ] **Define `RenderPipeline` Class**:
    *   Orchestrates the flow: `Input -> Stage 1 -> Stage 2 -> Output`.
    *   Holds the state of the current mode (Anime4K vs ML).
*   [ ] **Implement `PreprocessingPass`**:
    *   Move the current "Blur + HDR" compute shader logic here.
*   [ ] **Implement `MlUpscalePass`**:
    *   Wrap the `UpscalerML` interaction.
    *   Ensure it accepts GPU resources directly (GPU-to-GPU).

### Phase 4: Asynchronous Pipelining (The "Async" Upgrade)
*   **Goal**: Implement the "Frame N / Frame N-1" architecture to unblock the CPU.
*   [ ] **Double Buffer Resources**:
    *   Update `BufferManager` to allocate 2x resources for all GPU buffers.
*   [ ] **Fence Synchronization**:
    *   Implement `WaitForPreviousFrame()` logic.
    *   CPU submits Frame N, then checks if Frame N-1 is ready.
*   [ ] **Async Readback**:
    *   Map the readback buffer for Frame N-1.
    *   If ready, copy to display. If not, wait (or skip).

### Phase 5: Display Integration
*   **Goal**: Ensure the game receives the frame efficiently.
*   [ ] **Optimize `getOutputBuffer`**:
    *   It should return the mapped pointer of Frame N-1 (system memory).
    *   Ensure `svga.cc` handles the pointer correctly without unnecessary copies if possible.

## 4. Proposed Class Structure

```cpp
// Handles D3D12 Device, CommandQueue, and Fences
class D3D12Context {
public:
    void BeginFrame();
    void EndFrame(); // Signals Fence
    void WaitForGpu();
    ID3D12Device* GetDevice();
    ID3D12GraphicsCommandList* GetCommandList();
};

// Manages Input/Output/Readback buffers, handling double-buffering transparently
class ResourceManager {
public:
    void UploadInput(const void* data, int width, int height);
    const void* MapReadback(); // Returns Frame N-1 data
    void SwapBuffers();
};

// Base class for render stages
class IRenderStage {
public:
    virtual void Execute(D3D12Context& ctx, ResourceManager& resources) = 0;
};

// The main coordinator
class RenderPipeline {
    D3D12Context mContext;
    ResourceManager mResources;
    std::vector<std::unique_ptr<IRenderStage>> mStages;
public:
    void Dispatch(const void* inputPixels);
    const void* GetOutput(); // Returns result from previous frame
};
```

## 5. Checklist for Immediate Action
1.  [ ] Create `src/renderer/` directory to house new files.
2.  [ ] Move `upscaler.cc` logic into `src/renderer/RenderPipeline.cc` incrementally.
3.  [ ] Delete Integer Scaling code.
