# Fallout 2 CE Renderer Documentation

This directory contains the modern rendering subsystem for Fallout 2 Community Edition. It replaces the legacy SDL-only rendering with a Direct3D 12 pipeline capable of advanced post-processing, including AI upscaling and HDR effects.

## Overview

The renderer is designed as a pipeline of passes that process the game's output (originally a 640x480 8-bit indexed surface) and transform it for modern high-resolution displays.

### Key Features
- **Direct3D 12 Backend**: High-performance graphics API usage.
- **Pipeline Architecture**: Modular pass-based rendering.
- **AI Upscaling**: Integration with DirectML and ONNX Runtime for Real-ESRGAN.
- **Post-Processing**: Blur, HDR, and other shader effects.
- **Double Buffering**: Efficient resource management for smooth frame delivery.

## Core Infrastructure

The core infrastructure manages the GPU resources and the execution context.

- **`d3d12_context`**: Manages the D3D12 device, command queue, command lists, and synchronization (fences). It is the foundation for all GPU operations.
- **`gpu_device`**: A C-style wrapper around D3D12 initialization, likely used for integration with the broader engine or SDL.
- **`gpu_texture`**: Handles creation, upload, and download of GPU textures. It abstracts D3D12 resource management for textures.
- **`buffer_manager`**: Implements a double-buffering scheme for the rendering pipeline. It manages input (CPU->GPU), output (GPU->CPU/Display), and readback buffers.

## Rendering Pipeline

The rendering process is orchestrated by the `RenderPipeline` class.

- **`render_pipeline`**: The main coordinator. It initializes the context, manages buffers, and executes a sequence of `ShaderPass` objects. It handles the flow of data from the game's surface through various effects to the final output.
- **`shader_pass`**: An abstract base class for all rendering passes. Each pass implements `Init`, `Execute`, and `Shutdown`.

## Shader Passes

Specific effects are implemented as classes derived from `ShaderPass`.

- **`blur_filter`**: Implements a Gaussian blur or similar smoothing filter.
- **`hdr_filter`**: Applies High Dynamic Range (HDR) like effects, such as contrast and saturation adjustments.
- **`ml_upscale_pass`**: A specialized pass that uses machine learning models (via `UpscalerML`) to upscale the image.
- **`scaler_pass`**: Handles standard scaling operations, likely for the final stretch to the display resolution or intermediate resizing.

## Upscaling System

The upscaling system is a major component, offering multiple modes from simple integer scaling to advanced AI upscaling.

- **`upscaler`**: The high-level interface for the upscaling system. It manages the state, configuration (modes like Anime4K, Real-ESRGAN, Integer), and integrates with the `RenderPipeline`.
- **`upscaler_ml`**: The backend for ML upscaling. It wraps ONNX Runtime and DirectML to execute neural networks on the GPU.

## Display Management

- **`display_scaler`**: Handles the logic for mapping the game's logical coordinate system (640x480) to the physical display resolution. It manages aspect ratios, letterboxing, and coordinate conversion for mouse input.

## File Descriptions

| File | Description |
|------|-------------|
| `blur_filter.h/cc` | Implements a blur shader pass. |
| `buffer_manager.h/cc` | Manages double-buffered resources for the render pipeline. |
| `d3d12_context.h/cc` | Wraps D3D12 device, command queue, and synchronization. |
| `display_scaler.h/cc` | Manages logical-to-physical coordinate mapping. |
| `gpu_device.h/cc` | C-style interface for GPU device initialization and management. |
| `gpu_texture.h/cc` | Manages GPU texture resources (creation, upload, download). |
| `hdr_filter.h/cc` | Implements an HDR (contrast/saturation) shader pass. |
| `ml_upscale_pass.h/cc` | Shader pass wrapper for the ML upscaler. |
| `render_pipeline.h/cc` | Orchestrates the rendering passes and buffer management. |
| `scaler_pass.h/cc` | Implements a standard image scaling pass. |
| `shader_pass.h` | Abstract base class for rendering passes. |
| `upscaler.h/cc` | Main entry point and manager for the upscaling system. |
| `upscaler_ml.h/cc` | DirectML/ONNX Runtime backend for AI upscaling. |

## Architecture Notes

- **Data Flow**: Game Surface (Indexed) -> `UpscalerImpl` -> `RenderPipeline` -> `BufferManager` (Upload) -> `ShaderPass` chain -> `BufferManager` (Readback/Output).
- **Phantom Display**: The concept of "Phantom Display" refers to the internal 640x480 rendering target which is then processed and upscaled to the actual window resolution.
- **Redundancy**: There seems to be some overlap between `gpu_device`/`gpu_texture` (C-style) and `d3d12_context`/`buffer_manager` (C++ class based). The `RenderPipeline` uses the C++ classes, while `gpu_device` might be legacy or used by other parts of the engine.

## Usage

To use the renderer:
1. Initialize `UpscalerImpl` via `upscalerInit`.
2. Each frame, provide input data using `upscalerSetIndexedInput` or `upscalerSetRgbaInput`.
3. Call `upscalerDispatch` to execute the pipeline.
4. Retrieve the result with `upscalerGetOutputBuffer`.
