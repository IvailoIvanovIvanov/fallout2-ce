# Engine Abstraction Overview

> The Overseer's Blueprint for Running Fallout 2 in UHD with Modern Rendering

## Core Design Principles

1. **Phantom Display (640×480)** – The game runs natively at the original resolution with its own asset cache, completely isolated from rendering decisions. The game logic lives in this "vault" and never knows about the outside world's resolution.

2. **GPU-Centric Pipeline** – The 640x480 buffer is uploaded to the GPU, where all scaling, post-processing (Blur, HDR), and upscaling (Anime4K, ML) happen via Compute Shaders.

3. **Logical Unit Coordinate System** – Both displays share a unified logical coordinate space (640×480). All positions, mouse events, and rectangles are expressed in logical units, guaranteeing perfect position mapping between input/output regardless of physical resolution.

---

## Architecture Diagram

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                              PHANTOM DISPLAY                                 │
│                           (640×480 Logical Space)                           │
│  ┌─────────────┐  ┌─────────────┐  ┌─────────────┐  ┌─────────────────────┐ │
│  │  Game Logic │  │  Tile/Iso   │  │   Objects   │  │    UI / Windows     │ │
│  │  (scripts,  │  │  Renderer   │  │  Renderer   │  │    (buttons, etc)   │ │
│  │   combat)   │  │             │  │             │  │                     │ │
│  └──────┬──────┘  └──────┬──────┘  └──────┬──────┘  └──────────┬──────────┘ │
│         │                │                │                    │            │
│         ▼                ▼                ▼                    ▼            │
│  ┌──────────────────────────────────────────────────────────────────────┐  │
│  │                     8-bit Asset Cache (gArtCache)                     │  │
│  │                   Original FRM files, indexed palette                 │  │
│  └──────────────────────────────────────────────────────────────────────┘  │
│         │                │                │                    │            │
│         ▼                ▼                ▼                    ▼            │
│  ┌──────────────────────────────────────────────────────────────────────┐  │
│  │                         _screen_buffer (640×480)                      │  │
│  │              8-bit indexed pixels, game's "ground truth"              │  │
│  └──────────────────────────────────────────────────────────────────────┘  │
└─────────────────────────────────────────────────────────────────────────────┘
                                      │
                                      │ Direct Memory Access / Upload
                                      ▼
┌─────────────────────────────────────────────────────────────────────────────┐
│                               RENDER PIPELINE                                │
│                         (Physical Resolution, e.g. 2560x1440)               │
│  ┌──────────────────────────────────────────────────────────────────────┐  │
│  │                        Buffer Manager                                 │  │
│  │        Manages Input (640x480) and Output (Physical) Buffers          │  │
│  └──────────────────────────────────────────────────────────────────────┘  │
│         │                                                                   │
│         ▼                                                                   │
│  ┌──────────────────────────────────────────────────────────────────────┐  │
│  │                      Preprocessing Pass                               │  │
│  │            Compute Shader: Blur -> HDR -> Letterbox Scale             │  │
│  └──────────────────────────────────────────────────────────────────────┘  │
│         │                                                                   │
│         ▼                                                                   │
│  ┌──────────────────────────────────────────────────────────────────────┐  │
│  │                       ML Upscale Pass                                 │  │
│  │            (Optional) Real-ESRGAN / Anime4K Upscaling                 │  │
│  └──────────────────────────────────────────────────────────────────────┘  │
│         │                                                                   │
│         ▼                                                                   │
│  ┌──────────────────────────────────────────────────────────────────────┐  │
│  │                       SDL Presenter                                   │  │
│  │            Final compositing, SDL_RenderPresent                       │  │
│  └──────────────────────────────────────────────────────────────────────┘  │
└─────────────────────────────────────────────────────────────────────────────┘
                                      ▲
                                      │ INPUT EVENT BUS
                                      │ (Physical → Logical coordinate mapping)
                                      │
┌─────────────────────────────────────────────────────────────────────────────┐
│                              INPUT LAYER                                     │
│  ┌──────────────────────────────────────────────────────────────────────┐  │
│  │              SDL Events (mouse, keyboard, touch)                      │  │
│  │                    Native window coordinates                          │  │
│  └──────────────────────────────────────────────────────────────────────┘  │
│         │                                                                   │
│         ▼                                                                   │
│  ┌──────────────────────────────────────────────────────────────────────┐  │
│  │              Display Scaler (Coordinate Translation)                  │  │
│  │     Physical → Logical, letterbox-aware, clamp to 640×480 bounds      │  │
│  └──────────────────────────────────────────────────────────────────────┘  │
│         │                                                                   │
│         ▼                                                                   │
│  ┌──────────────────────────────────────────────────────────────────────┐  │
│  │                    Phantom Display Input Queue                        │  │
│  │            Game logic receives events in logical coordinates          │  │
│  └──────────────────────────────────────────────────────────────────────┘  │
└─────────────────────────────────────────────────────────────────────────────┘
```

## Component Responsibilities

### 1. Phantom Display (Legacy Engine)
*   **Role**: Runs the original Fallout 2 engine logic.
*   **Resolution**: Fixed at 640x480 (or configured logical resolution).
*   **Output**: Writes 8-bit indexed pixels to `_screen_buffer`.
*   **Ignorance**: Knows nothing about the physical screen, shaders, or upscaling.

### 2. Render Pipeline (`src/renderer/`)
*   **Role**: Modern D3D12-based rendering pipeline.
*   **BufferManager**: Handles double-buffered GPU resources to allow async CPU/GPU execution.
*   **PreprocessingPass**:
    *   Takes 640x480 input.
    *   Applies Blur (on original pixels).
    *   Applies HDR/Color Correction.
    *   Scales to Physical Resolution (preserving aspect ratio with letterboxing).
*   **MlUpscalePass**: Applies AI upscaling if enabled.

### 3. Display Scaler (`src/renderer/display_scaler.cc`)
*   **Role**: Pure math utility for coordinate systems.
*   **Input**: Physical mouse coordinates (from SDL).
*   **Output**: Logical game coordinates (640x480).
*   **Logic**: Inverse of the PreprocessingPass scaling logic (accounts for scale factor and letterbox offsets).

## Directory Structure (`src/renderer/`)

*   `RenderPipeline.cc`: Main coordinator.
*   `BufferManager.cc`: Resource management.
*   `D3D12Context.cc`: Low-level D3D12 wrapper.
*   `PreprocessingPass.cc`: Shader logic.
*   `MlUpscalePass.cc`: ML integration.
*   `display_scaler.cc`: Coordinate math.


