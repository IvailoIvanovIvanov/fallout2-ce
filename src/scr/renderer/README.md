# Fallout 2 CE Renderer Documentation

This directory contains the modern rendering subsystem for Fallout 2 Community Edition. It replaces the legacy SDL-only rendering with an OpenGL pipeline capable of advanced post-processing, including AI-inspired shader upscaling and HDR effects.

## Overview

The renderer is designed as a pipeline of passes that process the game's output (originally a 640x480 8-bit indexed surface) and transform it for modern high-resolution displays.

### Key Features
- **OpenGL Backend**: Cross-platform high-performance graphics API usage.
- **Pipeline Architecture**: Modular pass-based rendering.
- **Shader-based Upscaling**: Integration with Anime4K for high-quality upscaling.
- **Post-Processing**: Blur, HDR, and other shader effects.
- **Double Buffering**: Efficient resource management for smooth frame delivery.

## Core Infrastructure

The core infrastructure manages the GPU resources and the execution context.

- **`opengl_context`**: Manages the OpenGL context, shaders, and synchronization. It is the foundation for all GPU operations.
- **`buffer_manager`**: Implements a double-buffering scheme for the rendering pipeline. It manages input (CPU->GPU), output (GPU->CPU/Display), and readback buffers.

## Rendering Pipeline

The rendering process is orchestrated by the `RenderPipeline` class.

- **`render_pipeline`**: The main coordinator. It initializes the context, manages buffers, and executes a sequence of `ShaderPass` objects. It handles the flow of data from the game's surface through various effects to the final output.
- **`shader_pass`**: An abstract base class for all rendering passes. Each pass implements `Init`, `Execute`, and `Shutdown`.

## Shader Passes

Specific effects are implemented as classes derived from `ShaderPass`.

- **`blur_filter`**: Implements a Gaussian blur or similar smoothing filter.
- **`hdr_filter`**: Applies High Dynamic Range (HDR) like effects, such as contrast and saturation adjustments.
- **`anime4k_pass`**: A specialized pass that uses Anime4K shaders for high-quality upscaling.
- **`scaler_pass`**: Handles standard scaling operations, likely for the final stretch to the display resolution or intermediate resizing.

## Upscaling System

The upscaling system is a major component, offering multiple modes from simple integer scaling to advanced shader-based upscaling.

- **`upscaler`**: The high-level interface for the upscaling system. It manages the state, configuration (modes like Anime4K, Integer), and integrates with the `RenderPipeline`.

## Display Management

- **`display_scaler`**: Handles the logic for mapping the game's logical coordinate system (640x480) to the physical display resolution. It manages aspect ratios, letterboxing, and coordinate conversion for mouse input.

## File Descriptions

| File | Description |
|------|-------------|
| `blur_filter.h/cc` | Implements a blur shader pass. |
| `buffer_manager.h/cc` | Manages double-buffered resources for the render pipeline. |
| `display_scaler.h/cc` | Manages logical-to-physical coordinate mapping. |
| `hdr_filter.h/cc` | Implements an HDR (contrast/saturation) shader pass. |
| `anime4k_pass.h/cc` | Shader pass wrapper for the Anime4K upscaler. |
| `render_pipeline.h/cc` | Orchestrates the rendering passes and buffer management. |
| `scaler_pass.h/cc` | Implements a standard image scaling pass. |
| `shader_pass.h` | Abstract base class for rendering passes. |
| `upscaler.h/cc` | Main entry point and manager for the upscaling system. |
| `opengl_context.h/cc` | Wraps OpenGL context and resource management. |

## Architecture Notes

- **Data Flow**: Game Surface (Indexed) -> `UpscalerImpl` -> `RenderPipeline` -> `BufferManager` (Upload) -> `ShaderPass` chain -> `BufferManager` (Readback/Output).
- **Phantom Display**: The concept of "Phantom Display" refers to the internal 640x480 rendering target which is then processed and upscaled to the actual window resolution.
- **Pass Order**: The rendering pipeline applies effects in this order:
  1. **Blur Filter** (optional, pre-processing): Smooths edges at the 640x480 native resolution
  2. **HDR Filter** (enabled by default): Enhances colors, contrast, and deepens blacks at 640x480
  3. **Anime4K Upscaler** (default mode): Uses Anime4K shaders to upscale the processed 640x480 to target resolution
  4. **Scaler Pass** (final): Stretches the result to the physical display size

## Usage

To use the renderer:
1. Initialize `UpscalerImpl` via `upscalerInit`.
2. Each frame, provide input data using `upscalerSetIndexedInput` or `upscalerSetRgbaInput`.
3. Call `upscalerDispatch` to execute the pipeline.
4. Retrieve the result with `upscalerGetOutputBuffer`.

### Configuration

The renderer is configured to use **Anime4K upscaling by default** with HDR enhancement.

**Default Pipeline Order:**
1. Blur Filter (if enabled via `upscaler_edge_smoothing=1`)
2. HDR Filter (enabled by default)
3. Anime4K Upscaler (default mode)
4. Final scaler to display resolution

**HDR Filter Settings** (enabled by default):
- **Saturation**: 1.5 (vivid colors)
- **Contrast**: 1.3 (enhanced contrast)
- **Black Crush Threshold**: 0.05 (affects darker pixels)
- **Black Crush Strength**: 1.5 (deep blacks)

Override in `fallout2.cfg`:
```ini
# Upscaler mode: 0=NONE, 1=ANIME4K, 2=REAL_ESRGAN (default)
upscaler_mode=2

# HDR settings
upscaler_hdr_enable=1
upscaler_hdr_saturation=1.5
upscaler_hdr_contrast=1.3
upscaler_black_crush_threshold=0.05
upscaler_black_crush_strength=1.5

# Edge smoothing (optional pre-processing blur)
upscaler_edge_smoothing=0
upscaler_smoothing_strength=0.6
```
