# libplacebo Integration Migration Guide

## Overview

This document provides a comprehensive guide to migrating the Fallout 2 CE renderer from the current custom OpenGL/Vulkan implementation to **libplacebo** - a high-quality GPU-accelerated image/video processing library. libplacebo powers mpv and provides industry-standard upscalers, color management, and HDR support.

### Benefits of Migration

| Feature | Current Implementation | libplacebo |
|---------|----------------------|------------|
| **Upscaling** | Custom Anime4K shaders | EWA Lanczos, Jinc, Spline, Bicubic, and 20+ filters |
| **HDR** | Custom HDR filter with basic tonemap | Full HDR10/HLG/Dolby Vision support with proper gamut mapping |
| **Color Management** | Basic saturation/contrast adjustments | ICC profiles, color primaries, transfer functions |
| **Debanding** | Not implemented | Built-in advanced debanding |
| **Shader Hooks** | Custom MPV shader parser | Native mpv hook support |
| **Maintenance** | Manual shader maintenance | Active library with regular updates |
| **Cross-platform** | OpenGL + partial Vulkan | Vulkan, OpenGL, D3D11 backends |

---

## Prerequisites

### Using the Pre-built mingw Package

The pre-built package at `C:\Users\User\Downloads\mingw-w64-x86_64-libplacebo-7.351.0-1-any.pkg` contains:

```
mingw64/
├── bin/
│   └── libplacebo-351.dll          # Runtime DLL
├── include/
│   └── libplacebo/                  # Headers
│       ├── vulkan.h                 # Vulkan backend
│       ├── opengl.h                 # OpenGL backend  
│       ├── renderer.h               # High-level rendering API
│       ├── colorspace.h             # Color management
│       ├── filters.h                # Built-in scalers
│       ├── shaders/
│       │   ├── sampling.h           # Sampling algorithms
│       │   ├── colorspace.h         # Color conversion shaders
│       │   └── custom.h             # Custom shader hooks
│       └── ...
├── lib/
│   ├── libplacebo.a                 # Static library
│   ├── libplacebo.dll.a             # Import library
│   └── pkgconfig/libplacebo.pc
└── share/licenses/libplacebo/LICENSE
```

### Package Features (from config.h)

```c
#define PL_HAVE_VULKAN 1        // ✅ Vulkan backend
#define PL_HAVE_OPENGL 1        // ✅ OpenGL backend
#define PL_HAVE_D3D11 1         // ✅ D3D11 backend
#define PL_HAVE_SHADERC 1       // ✅ Runtime shader compilation
#define PL_HAVE_LCMS 1          // ✅ ICC color management
#define PL_HAVE_VK_PROC_ADDR 1  // ✅ Vulkan function loading
#define PL_HAVE_DOVI 1          // ✅ Dolby Vision support
```

### Build Compatibility Considerations

⚠️ **IMPORTANT**: This mingw package is built with GCC/MinGW. Your project uses MSVC (Visual Studio). There may be ABI incompatibilities:

**Option A: Use MSVC-compatible build (Recommended)**
- Build libplacebo from source using MSVC
- Use vcpkg: `vcpkg install libplacebo[vulkan]`

**Option B: Use the mingw package**
- Requires linking against mingw runtime libraries
- May have C++ ABI issues (std::string, exceptions)
- Works if calling only C API functions

**Recommendation**: Use **vcpkg** for proper MSVC compatibility:
```powershell
vcpkg install libplacebo[vulkan,shaderc,lcms] --triplet=x64-windows
```

---

## Building libplacebo from Source (MSVC)

If vcpkg doesn't work or you need a custom build, here's how to build libplacebo from source with MSVC:

### Prerequisites

1. **Visual Studio 2019 or 2022** with C++ workload
2. **Vulkan SDK** (latest version from https://vulkan.lunarg.com/)
3. **Python 3.x** with meson and ninja:
   ```powershell
   pip install meson ninja
   ```
4. **pkg-config** (from MSYS2 or chocolatey):
   ```powershell
   choco install pkgconfiglite
   ```

### Build Steps

```powershell
# Clone libplacebo
git clone --recursive https://github.com/haasn/libplacebo.git
cd libplacebo

# Create build directory
mkdir build
cd build

# Configure with meson (from VS Developer Command Prompt)
meson setup .. --buildtype=release `
    -Dvulkan=enabled `
    -Dd3d11=enabled `
    -Dopengl=enabled `
    -Dshaderc=enabled `
    -Dlcms=disabled `
    -Ddemos=false `
    -Dtests=false

# Build
ninja

# Install to prefix
meson install --destdir=C:\libplacebo-install
```

### Alternative: Use the mingw Package with C Interface Only

The mingw package **can work** with MSVC if you only use the C API (no C++ classes across the boundary). libplacebo's public API is entirely C, so this should be safe:

```cpp
// This is safe - pure C API
#include <libplacebo/vulkan.h>
pl_log log = pl_log_create(PL_API_VER, &params);  // OK

// This would be problematic - but libplacebo doesn't do this
// std::string func();  // ABI incompatible
```

To use the mingw package:

1. Copy headers to your project's `third_party/libplacebo/include/`
2. Copy `libplacebo.dll.a` to `third_party/libplacebo/lib/`
3. Copy `libplacebo-351.dll` to your output directory
4. Set `LIBPLACEBO_ROOT` environment variable or configure CMake

---

## Architecture Comparison

### Current Architecture

```
┌────────────────────────────────────────────────────────────────┐
│                     RenderPipeline                              │
├────────────────────────────────────────────────────────────────┤
│  ┌─────────────────┐    ┌────────────────────────────────────┐ │
│  │  GpuContext     │───▶│  OpenGLContext / VulkanContext    │ │
│  │  (Abstract)     │    │  (Texture, Shader, Present)       │ │
│  └─────────────────┘    └────────────────────────────────────┘ │
│          │                                                      │
│          ▼                                                      │
│  ┌─────────────────┐    ┌────────────────────────────────────┐ │
│  │  ShaderPass     │───▶│  GenericShaderPass / ScalerPass   │ │
│  │  (Abstract)     │    │  (Custom compute shaders)         │ │
│  └─────────────────┘    └────────────────────────────────────┘ │
│          │                                                      │
│          ▼                                                      │
│  ┌─────────────────────────────────────────────────────────┐   │
│  │  Custom GLSL Shaders (data/shaders/)                    │   │
│  │  - Anime4K passes                                       │   │
│  │  - HDR filter                                           │   │
│  │  - Scaler                                               │   │
│  └─────────────────────────────────────────────────────────┘   │
└────────────────────────────────────────────────────────────────┘
```

### New libplacebo Architecture

```
┌────────────────────────────────────────────────────────────────┐
│                   PlaceboRenderPipeline                         │
├────────────────────────────────────────────────────────────────┤
│  ┌─────────────────┐                                           │
│  │   pl_vulkan     │ ◄── Vulkan GPU context                   │
│  │   pl_gpu        │     (handles all GPU resources)          │
│  └─────────────────┘                                           │
│          │                                                      │
│          ▼                                                      │
│  ┌─────────────────┐                                           │
│  │  pl_swapchain   │ ◄── Window presentation                  │
│  │                 │     (automatic HDR detection)            │
│  └─────────────────┘                                           │
│          │                                                      │
│          ▼                                                      │
│  ┌─────────────────┐    ┌────────────────────────────────────┐ │
│  │  pl_renderer    │───▶│  Built-in processing:              │ │
│  │                 │    │  - Upscaling (EWA Lanczos, etc)    │ │
│  │                 │    │  - Color mapping (HDR/SDR)         │ │
│  │                 │    │  - Debanding                       │ │
│  │                 │    │  - Dithering                       │ │
│  │                 │    │  - Custom hooks                    │ │
│  └─────────────────┘    └────────────────────────────────────┘ │
└────────────────────────────────────────────────────────────────┘
```

---

## Step-by-Step Migration

### Phase 1: CMake Integration

#### 1.1 Add libplacebo to CMakeLists.txt

```cmake
# Option 1: vcpkg (recommended for MSVC)
find_package(libplacebo CONFIG REQUIRED)
target_link_libraries(${EXECUTABLE_NAME} libplacebo::libplacebo)

# Option 2: pkg-config
find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBPLACEBO REQUIRED libplacebo)
target_link_libraries(${EXECUTABLE_NAME} ${LIBPLACEBO_LIBRARIES})
target_include_directories(${EXECUTABLE_NAME} PRIVATE ${LIBPLACEBO_INCLUDE_DIRS})

# Option 3: Manual linking (for pre-built package)
set(LIBPLACEBO_ROOT "C:/path/to/libplacebo")
target_include_directories(${EXECUTABLE_NAME} PRIVATE "${LIBPLACEBO_ROOT}/include")
target_link_libraries(${EXECUTABLE_NAME} "${LIBPLACEBO_ROOT}/lib/libplacebo.dll.a")
# Copy DLL to output
add_custom_command(TARGET ${EXECUTABLE_NAME} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
    "${LIBPLACEBO_ROOT}/bin/libplacebo-351.dll"
    "$<TARGET_FILE_DIR:${EXECUTABLE_NAME}>"
)
```

#### 1.2 Updated CMakeLists.txt Section

Add after the Vulkan section in CMakeLists.txt:

```cmake
# libplacebo for advanced rendering
option(FALLOUT_USE_LIBPLACEBO "Use libplacebo for upscaling and HDR" ON)

if(FALLOUT_USE_LIBPLACEBO)
    find_package(libplacebo CONFIG QUIET)
    if(libplacebo_FOUND)
        message(STATUS "libplacebo found")
        target_compile_definitions(${EXECUTABLE_NAME} PRIVATE FALLOUT_HAVE_LIBPLACEBO=1)
        target_link_libraries(${EXECUTABLE_NAME} libplacebo::libplacebo)
    else()
        message(WARNING "libplacebo not found - falling back to custom renderer")
        target_compile_definitions(${EXECUTABLE_NAME} PRIVATE FALLOUT_HAVE_LIBPLACEBO=0)
    endif()
else()
    target_compile_definitions(${EXECUTABLE_NAME} PRIVATE FALLOUT_HAVE_LIBPLACEBO=0)
endif()
```

---

### Phase 2: Create libplacebo Context Wrapper

#### 2.1 New Header: `src/scr/renderer/placebo_context.h`

```cpp
#ifndef FALLOUT_RENDERER_PLACEBO_CONTEXT_H
#define FALLOUT_RENDERER_PLACEBO_CONTEXT_H

#include "gpu_context.h"

#if FALLOUT_HAVE_LIBPLACEBO

#include <libplacebo/vulkan.h>
#include <libplacebo/renderer.h>
#include <libplacebo/shaders/colorspace.h>
#include <libplacebo/utils/upload.h>

struct SDL_Window;

namespace fallout {
namespace renderer {

/**
 * @brief Upscaler algorithm selection
 */
enum class PlaceboUpscaler {
    BILINEAR,           // Fast, low quality
    BICUBIC,            // Good balance
    LANCZOS,            // Sharp, may ring
    EWA_LANCZOS,        // High quality polar (recommended)
    EWA_LANCZOSSHARP,   // Sharper variant
    SPLINE36,           // Smooth, no ringing
    SPLINE64,           // Higher quality spline
    MITCHELL,           // Balanced, no ringing
    CATMULL_ROM,        // Sharp spline
    OVERSAMPLE,         // Pixel art mode
};

/**
 * @brief HDR/Color processing mode
 */
enum class PlaceboColorMode {
    PASSTHROUGH,        // No color processing
    SDR_ENHANCE,        // Enhanced SDR (saturation, contrast)
    HDR10,              // HDR10 output
    HDR_TONEMAP_SDR,    // HDR to SDR tonemapping
};

/**
 * @struct PlaceboConfig
 * @brief Configuration for libplacebo rendering
 */
struct PlaceboConfig {
    PlaceboUpscaler upscaler = PlaceboUpscaler::EWA_LANCZOS;
    PlaceboColorMode colorMode = PlaceboColorMode::SDR_ENHANCE;
    
    // Upscaling options
    float antiringing = 0.5f;       // 0.0 - 1.0
    bool debanding = true;
    float debandThreshold = 3.0f;
    float debandGrain = 4.0f;
    
    // Color options  
    float saturation = 1.2f;        // 1.0 = normal
    float contrast = 1.1f;          // 1.0 = normal
    float brightness = 0.0f;        // 0.0 = normal
    float gamma = 1.0f;             // 1.0 = normal
    
    // HDR options
    float peakNits = 1000.0f;       // Display peak brightness
    float paperWhiteNits = 203.0f;  // Reference white level
    bool hdrPassthrough = false;    // Pass HDR content through
};

/**
 * @class PlaceboContext
 * @brief libplacebo-based GPU context with Vulkan backend
 */
class PlaceboContext : public GpuContext {
public:
    PlaceboContext(SDL_Window* window, int screenWidth, int screenHeight);
    ~PlaceboContext() override;

    // GpuContext interface
    bool Init() override;
    void Shutdown() override;
    
    void* CreateTexture(const TextureDesc& desc, const void* initialData = nullptr) override;
    void DestroyTexture(void* textureHandle) override;
    
    bool CreateComputeShader(const std::string& source, void** outShader) override;
    void Dispatch(void* shader, int x, int y, int z) override;
    
    void BindTexture(int slot, void* textureHandle) override;
    void BindUnorderedAccessView(int slot, void* textureHandle, 
                                  TextureFormat format = TextureFormat::RGBA8) override;
    void SetConstants(int slot, const void* data, int size) override;
    
    void BeginFrame() override;
    void EndFrame() override;
    bool Reconfigure(int width, int height) override;
    void Present(void* textureHandle, int srcWidth, int srcHeight,
                 int windowWidth, int windowHeight) override;
    
    void UpdateTexture(void* textureHandle, const void* data, int width, int height) override;
    void ReadbackTexture(void* textureHandle, void* data, int size) override;

    // libplacebo-specific interface
    void SetConfig(const PlaceboConfig& config);
    PlaceboConfig& GetConfig() { return mConfig; }
    
    /**
     * @brief Renders source texture to output with upscaling and effects
     * @param srcTexture Source texture (game frame)
     * @param srcWidth Source width
     * @param srcHeight Source height  
     */
    void RenderFrame(void* srcTexture, int srcWidth, int srcHeight);
    
    /**
     * @brief Checks if HDR output is active
     */
    bool IsHDRActive() const;
    
    /**
     * @brief Gets the list of available upscaler names
     */
    static std::vector<std::string> GetAvailableUpscalers();

private:
    bool CreateVulkanInstance();
    bool CreateVulkanDevice();
    bool CreateSwapchain();
    bool CreateRenderer();
    void UpdateRenderParams();
    
    const pl_filter_config* GetFilterConfig(PlaceboUpscaler upscaler);
    void ApplyColorAdjustments(pl_color_adjustment* adj);
    
    SDL_Window* mWindow = nullptr;
    int mScreenWidth = 0;
    int mScreenHeight = 0;
    
    // libplacebo objects
    pl_log mLog = nullptr;
    pl_vk_inst mVkInst = nullptr;
    pl_vulkan mVulkan = nullptr;
    pl_swapchain mSwapchain = nullptr;
    pl_renderer mRenderer = nullptr;
    pl_tex mUploadTex = nullptr;  // For uploading game frames
    
    // Render parameters
    struct pl_render_params mRenderParams;
    struct pl_deband_params mDebandParams;
    struct pl_sigmoid_params mSigmoidParams;
    struct pl_color_adjustment mColorAdj;
    struct pl_color_map_params mColorMapParams;
    struct pl_dither_params mDitherParams;
    
    PlaceboConfig mConfig;
    bool mConfigDirty = true;
};

} // namespace renderer
} // namespace fallout

#else // !FALLOUT_HAVE_LIBPLACEBO

// Stub when libplacebo not available
namespace fallout {
namespace renderer {

class PlaceboContext : public GpuContext {
public:
    PlaceboContext(void*, int, int) {}
    bool Init() override { return false; }
    void Shutdown() override {}
    void* CreateTexture(const TextureDesc&, const void* = nullptr) override { return nullptr; }
    void DestroyTexture(void*) override {}
    bool CreateComputeShader(const std::string&, void**) override { return false; }
    void Dispatch(void*, int, int, int) override {}
    void BindTexture(int, void*) override {}
    void BindUnorderedAccessView(int, void*, TextureFormat = TextureFormat::RGBA8) override {}
    void SetConstants(int, const void*, int) override {}
    void BeginFrame() override {}
    void EndFrame() override {}
    void Present(void*, int, int, int, int) override {}
    void UpdateTexture(void*, const void*, int, int) override {}
    void ReadbackTexture(void*, void*, int) override {}
};

} // namespace renderer
} // namespace fallout

#endif // FALLOUT_HAVE_LIBPLACEBO

#endif // FALLOUT_RENDERER_PLACEBO_CONTEXT_H
```

---

### Phase 3: Implementation

#### 3.1 New Source: `src/scr/renderer/placebo_context.cc`

```cpp
#include "placebo_context.h"

#if FALLOUT_HAVE_LIBPLACEBO

#include <SDL.h>
#include <SDL_vulkan.h>
#include "logger.h"

namespace fallout {
namespace renderer {

// libplacebo log callback
static void placebo_log_cb(void* ctx, pl_log_level level, const char* msg) {
    const char* prefix = "";
    switch (level) {
        case PL_LOG_FATAL: prefix = "[FATAL]"; break;
        case PL_LOG_ERR:   prefix = "[ERROR]"; break;
        case PL_LOG_WARN:  prefix = "[WARN]";  break;
        case PL_LOG_INFO:  prefix = "[INFO]";  break;
        case PL_LOG_DEBUG: prefix = "[DEBUG]"; break;
        case PL_LOG_TRACE: prefix = "[TRACE]"; break;
        default: break;
    }
    rendererLog("%s libplacebo: %s", prefix, msg);
}

PlaceboContext::PlaceboContext(SDL_Window* window, int screenWidth, int screenHeight)
    : mWindow(window)
    , mScreenWidth(screenWidth)
    , mScreenHeight(screenHeight)
{
    // Initialize default render params
    mRenderParams = pl_render_default_params;
    mDebandParams = pl_deband_default_params;
    mSigmoidParams = pl_sigmoid_default_params;
    mColorAdj = pl_color_adjustment_neutral;
    mColorMapParams = pl_color_map_default_params;
    mDitherParams = pl_dither_default_params;
}

PlaceboContext::~PlaceboContext() {
    Shutdown();
}

bool PlaceboContext::Init() {
    // Create libplacebo log context
    mLog = pl_log_create(PL_API_VER, pl_log_params(
        .log_cb = placebo_log_cb,
        .log_level = PL_LOG_INFO,
    ));
    
    if (!mLog) {
        rendererLog("Failed to create libplacebo log context");
        return false;
    }
    
    if (!CreateVulkanInstance()) {
        rendererLog("Failed to create Vulkan instance");
        return false;
    }
    
    if (!CreateVulkanDevice()) {
        rendererLog("Failed to create Vulkan device");
        return false;
    }
    
    if (!CreateSwapchain()) {
        rendererLog("Failed to create swapchain");
        return false;
    }
    
    if (!CreateRenderer()) {
        rendererLog("Failed to create renderer");
        return false;
    }
    
    UpdateRenderParams();
    
    rendererLog("PlaceboContext initialized successfully");
    return true;
}

bool PlaceboContext::CreateVulkanInstance() {
    // Get required extensions from SDL
    unsigned int count = 0;
    if (!SDL_Vulkan_GetInstanceExtensions(mWindow, &count, nullptr)) {
        rendererLog("Failed to get Vulkan instance extension count");
        return false;
    }
    
    std::vector<const char*> extensions(count);
    if (!SDL_Vulkan_GetInstanceExtensions(mWindow, &count, extensions.data())) {
        rendererLog("Failed to get Vulkan instance extensions");
        return false;
    }
    
    mVkInst = pl_vk_inst_create(mLog, pl_vk_inst_params(
        .extensions = extensions.data(),
        .num_extensions = (int)extensions.size(),
        .debug = false,  // Enable for debugging
    ));
    
    return mVkInst != nullptr;
}

bool PlaceboContext::CreateVulkanDevice() {
    // Create Vulkan surface via SDL
    VkSurfaceKHR surface;
    if (!SDL_Vulkan_CreateSurface(mWindow, mVkInst->instance, &surface)) {
        rendererLog("Failed to create Vulkan surface: %s", SDL_GetError());
        return false;
    }
    
    mVulkan = pl_vulkan_create(mLog, pl_vulkan_params(
        .instance = mVkInst->instance,
        .get_proc_addr = mVkInst->get_proc_addr,
        .surface = surface,
        .allow_software = false,
    ));
    
    return mVulkan != nullptr;
}

bool PlaceboContext::CreateSwapchain() {
    mSwapchain = pl_vulkan_create_swapchain(mVulkan, pl_vulkan_swapchain_params(
        .surface = mVulkan->surface,
        .present_mode = VK_PRESENT_MODE_FIFO_KHR,  // Vsync
    ));
    
    if (!mSwapchain) {
        return false;
    }
    
    // Resize swapchain to window size
    int width = 0, height = 0;
    if (!pl_swapchain_resize(mSwapchain, &width, &height)) {
        rendererLog("Warning: Could not query swapchain size");
    }
    
    // Set HDR colorspace hint if available
    if (mConfig.colorMode == PlaceboColorMode::HDR10) {
        pl_swapchain_colorspace_hint(mSwapchain, &(struct pl_color_space) {
            .primaries = PL_COLOR_PRIM_BT_2020,
            .transfer = PL_COLOR_TRC_PQ,
        });
    }
    
    return true;
}

bool PlaceboContext::CreateRenderer() {
    mRenderer = pl_renderer_create(mLog, mVulkan->gpu);
    return mRenderer != nullptr;
}

void PlaceboContext::Shutdown() {
    if (mUploadTex) {
        pl_tex_destroy(mVulkan->gpu, &mUploadTex);
    }
    
    if (mRenderer) {
        pl_renderer_destroy(&mRenderer);
    }
    
    if (mSwapchain) {
        pl_swapchain_destroy(&mSwapchain);
    }
    
    if (mVulkan) {
        pl_vulkan_destroy(&mVulkan);
    }
    
    if (mVkInst) {
        pl_vk_inst_destroy(&mVkInst);
    }
    
    if (mLog) {
        pl_log_destroy(&mLog);
    }
}

void PlaceboContext::SetConfig(const PlaceboConfig& config) {
    mConfig = config;
    mConfigDirty = true;
}

void PlaceboContext::UpdateRenderParams() {
    // Set upscaler
    mRenderParams.upscaler = GetFilterConfig(mConfig.upscaler);
    mRenderParams.downscaler = &pl_filter_lanczos;  // Good default for downscaling
    
    // Anti-ringing
    mRenderParams.antiringing_strength = mConfig.antiringing;
    
    // Debanding
    if (mConfig.debanding) {
        mDebandParams.threshold = mConfig.debandThreshold;
        mDebandParams.grain = mConfig.debandGrain;
        mRenderParams.deband_params = &mDebandParams;
    } else {
        mRenderParams.deband_params = nullptr;
    }
    
    // Sigmoidization for better upscaling
    mRenderParams.sigmoid_params = &mSigmoidParams;
    
    // Color adjustment
    ApplyColorAdjustments(&mColorAdj);
    mRenderParams.color_adjustment = &mColorAdj;
    
    // Color mapping (HDR)
    mRenderParams.color_map_params = &mColorMapParams;
    
    // Dithering
    mRenderParams.dither_params = &mDitherParams;
    
    mConfigDirty = false;
}

const pl_filter_config* PlaceboContext::GetFilterConfig(PlaceboUpscaler upscaler) {
    switch (upscaler) {
        case PlaceboUpscaler::BILINEAR:        return &pl_filter_bilinear;
        case PlaceboUpscaler::BICUBIC:         return &pl_filter_bicubic;
        case PlaceboUpscaler::LANCZOS:         return &pl_filter_lanczos;
        case PlaceboUpscaler::EWA_LANCZOS:     return &pl_filter_ewa_lanczos;
        case PlaceboUpscaler::EWA_LANCZOSSHARP:return &pl_filter_ewa_lanczossharp;
        case PlaceboUpscaler::SPLINE36:        return &pl_filter_spline36;
        case PlaceboUpscaler::SPLINE64:        return &pl_filter_spline64;
        case PlaceboUpscaler::MITCHELL:        return &pl_filter_mitchell;
        case PlaceboUpscaler::CATMULL_ROM:     return &pl_filter_catmull_rom;
        case PlaceboUpscaler::OVERSAMPLE:      return &pl_filter_oversample;
        default:                               return &pl_filter_ewa_lanczos;
    }
}

void PlaceboContext::ApplyColorAdjustments(pl_color_adjustment* adj) {
    *adj = pl_color_adjustment_neutral;
    adj->saturation = mConfig.saturation;
    adj->contrast = mConfig.contrast;
    adj->brightness = mConfig.brightness;
    adj->gamma = mConfig.gamma;
}

void PlaceboContext::BeginFrame() {
    // libplacebo handles frame pacing internally
}

void PlaceboContext::EndFrame() {
    // Handled by swapchain
}

void PlaceboContext::RenderFrame(void* srcTexture, int srcWidth, int srcHeight) {
    if (mConfigDirty) {
        UpdateRenderParams();
    }
    
    // Start frame
    struct pl_swapchain_frame frame;
    if (!pl_swapchain_start_frame(mSwapchain, &frame)) {
        return;  // Window minimized or not ready
    }
    
    pl_tex src = (pl_tex)srcTexture;
    
    // Create source frame
    struct pl_frame srcFrame = {0};
    srcFrame.num_planes = 1;
    srcFrame.planes[0].texture = src;
    srcFrame.planes[0].components = 4;
    srcFrame.planes[0].component_mapping[0] = PL_CHANNEL_R;
    srcFrame.planes[0].component_mapping[1] = PL_CHANNEL_G;
    srcFrame.planes[0].component_mapping[2] = PL_CHANNEL_B;
    srcFrame.planes[0].component_mapping[3] = PL_CHANNEL_A;
    srcFrame.repr = pl_color_repr_rgb;
    srcFrame.color = pl_color_space_srgb;
    
    // Create target frame from swapchain
    struct pl_frame targetFrame;
    pl_frame_from_swapchain(&targetFrame, &frame);
    
    // Render!
    if (!pl_render_image(mRenderer, &srcFrame, &targetFrame, &mRenderParams)) {
        rendererLog("pl_render_image failed");
    }
    
    // Submit and present
    if (!pl_swapchain_submit_frame(mSwapchain)) {
        rendererLog("pl_swapchain_submit_frame failed");
    }
    
    pl_swapchain_swap_buffers(mSwapchain);
}

void PlaceboContext::Present(void* textureHandle, int srcWidth, int srcHeight,
                              int windowWidth, int windowHeight) {
    RenderFrame(textureHandle, srcWidth, srcHeight);
}

void* PlaceboContext::CreateTexture(const TextureDesc& desc, const void* initialData) {
    pl_fmt fmt = nullptr;
    
    switch (desc.format) {
        case TextureFormat::RGBA8:
            fmt = pl_find_fmt(mVulkan->gpu, PL_FMT_UNORM, 4, 8, 8, PL_FMT_CAP_SAMPLEABLE);
            break;
        case TextureFormat::RGBA16F:
            fmt = pl_find_fmt(mVulkan->gpu, PL_FMT_FLOAT, 4, 16, 16, PL_FMT_CAP_SAMPLEABLE);
            break;
        case TextureFormat::RGBA32F:
            fmt = pl_find_fmt(mVulkan->gpu, PL_FMT_FLOAT, 4, 32, 32, PL_FMT_CAP_SAMPLEABLE);
            break;
        default:
            return nullptr;
    }
    
    if (!fmt) {
        rendererLog("Failed to find compatible format");
        return nullptr;
    }
    
    pl_tex tex = pl_tex_create(mVulkan->gpu, pl_tex_params(
        .w = desc.width,
        .h = desc.height,
        .format = fmt,
        .sampleable = true,
        .host_writable = true,
        .storable = desc.isStorage,
        .initial_data = initialData,
    ));
    
    return (void*)tex;
}

void PlaceboContext::DestroyTexture(void* textureHandle) {
    pl_tex tex = (pl_tex)textureHandle;
    pl_tex_destroy(mVulkan->gpu, &tex);
}

void PlaceboContext::UpdateTexture(void* textureHandle, const void* data, int width, int height) {
    pl_tex tex = (pl_tex)textureHandle;
    
    pl_tex_upload(mVulkan->gpu, pl_tex_transfer_params(
        .tex = tex,
        .ptr = data,
    ));
}

void PlaceboContext::ReadbackTexture(void* textureHandle, void* data, int size) {
    pl_tex tex = (pl_tex)textureHandle;
    
    pl_tex_download(mVulkan->gpu, pl_tex_transfer_params(
        .tex = tex,
        .ptr = data,
    ));
}

bool PlaceboContext::IsHDRActive() const {
    if (!mSwapchain) return false;
    
    struct pl_swapchain_frame frame;
    // Query current swapchain state
    // Note: This is a simplified check
    return mConfig.colorMode == PlaceboColorMode::HDR10;
}

std::vector<std::string> PlaceboContext::GetAvailableUpscalers() {
    std::vector<std::string> names;
    for (int i = 0; i < pl_num_filter_configs; i++) {
        if (pl_filter_configs[i]) {
            if (pl_filter_configs[i]->allowed & PL_FILTER_UPSCALING) {
                names.push_back(pl_filter_configs[i]->name);
            }
        }
    }
    return names;
}

// Stubs for compute shader interface (not used with libplacebo's high-level API)
bool PlaceboContext::CreateComputeShader(const std::string& source, void** outShader) {
    return false;  // Use pl_renderer instead
}

void PlaceboContext::Dispatch(void* shader, int x, int y, int z) {
    // Not used
}

void PlaceboContext::BindTexture(int slot, void* textureHandle) {
    // Not used
}

void PlaceboContext::BindUnorderedAccessView(int slot, void* textureHandle, TextureFormat format) {
    // Not used
}

void PlaceboContext::SetConstants(int slot, const void* data, int size) {
    // Not used
}

bool PlaceboContext::Reconfigure(int width, int height) {
    if (!mSwapchain) return false;
    return pl_swapchain_resize(mSwapchain, &width, &height);
}

} // namespace renderer
} // namespace fallout

#endif // FALLOUT_HAVE_LIBPLACEBO
```

---

### Phase 4: Update RenderPipeline

#### 4.1 Modify `render_pipeline.h`

Add libplacebo support option:

```cpp
// In render_pipeline.h, add to includes:
#if FALLOUT_HAVE_LIBPLACEBO
#include "placebo_context.h"
#endif

// Add to RenderMode enum:
enum class RenderMode {
    SIMPLE = 0,
    ANIME4K = 1,
#if FALLOUT_HAVE_LIBPLACEBO
    LIBPLACEBO = 2,  // New mode
#endif
};

// Add to RenderPipeline class private section:
#if FALLOUT_HAVE_LIBPLACEBO
    PlaceboConfig mPlaceboConfig;
    bool mUsingPlacebo = false;
#endif
```

#### 4.2 Modify `render_pipeline.cc`

Update initialization:

```cpp
bool RenderPipeline::Init(int inputWidth, int inputHeight, 
                          int outputWidth, int outputHeight, 
                          SDL_Window* window) {
    // ... existing code ...
    
#if FALLOUT_HAVE_LIBPLACEBO
    if (mConfiguredMode == RenderMode::LIBPLACEBO) {
        auto placeboCtx = std::make_unique<PlaceboContext>(window, outputWidth, outputHeight);
        if (placeboCtx->Init()) {
            mContext = std::move(placeboCtx);
            mUsingPlacebo = true;
            mInitialized = true;
            return true;
        }
        rendererLog("Failed to init libplacebo, falling back to OpenGL");
    }
#endif
    
    // ... existing OpenGL/Vulkan fallback ...
}

void RenderPipeline::Dispatch() {
#if FALLOUT_HAVE_LIBPLACEBO
    if (mUsingPlacebo) {
        auto* placebo = static_cast<PlaceboContext*>(mContext.get());
        // Upload current frame to GPU
        placebo->UpdateTexture(mBuffers.GetInputTexture(), 
                               mBuffers.GetCurrentInputData(),
                               mInputDimensions.width, 
                               mInputDimensions.height);
        // Render with upscaling and effects
        placebo->RenderFrame(mBuffers.GetInputTexture(),
                            mInputDimensions.width,
                            mInputDimensions.height);
        return;
    }
#endif
    
    // ... existing dispatch code ...
}
```

---

### Phase 5: Configuration Integration

#### 5.1 Update `renderer_config.h`

```cpp
// Add libplacebo configuration options
struct RendererConfig {
    // ... existing options ...
    
    // libplacebo options
    bool useLibplacebo = true;
    std::string upscaler = "ewa_lanczos";
    float antiringing = 0.5f;
    bool debanding = true;
    float debandThreshold = 3.0f;
    float debandGrain = 4.0f;
    float saturation = 1.2f;
    float contrast = 1.1f;
    float brightness = 0.0f;
    std::string colorMode = "sdr_enhance";  // passthrough, sdr_enhance, hdr10
};
```

#### 5.2 Update `renderer_config.ini`

```ini
[libplacebo]
; Enable libplacebo renderer (requires Vulkan)
enabled = true

; Upscaler algorithm:
; bilinear, bicubic, lanczos, ewa_lanczos, ewa_lanczossharp,
; spline36, spline64, mitchell, catmull_rom, oversample
upscaler = ewa_lanczos

; Anti-ringing strength (0.0 - 1.0)
antiringing = 0.5

; Debanding
debanding = true
deband_threshold = 3.0
deband_grain = 4.0

; Color adjustments
saturation = 1.2
contrast = 1.1
brightness = 0.0
gamma = 1.0

; Color mode: passthrough, sdr_enhance, hdr10
color_mode = sdr_enhance

; HDR settings (only used when color_mode = hdr10)
peak_nits = 1000
paper_white_nits = 203
```

---

## Upscaler Comparison Guide

### Recommended Upscalers for Fallout 2

| Upscaler | Quality | Speed | Best For |
|----------|---------|-------|----------|
| `ewa_lanczos` | ★★★★★ | ★★★☆☆ | General use (recommended) |
| `ewa_lanczossharp` | ★★★★★ | ★★★☆☆ | Extra sharpness |
| `spline36` | ★★★★☆ | ★★★★☆ | Smooth, no ringing |
| `mitchell` | ★★★★☆ | ★★★★☆ | Balanced, soft |
| `lanczos` | ★★★★☆ | ★★★★★ | Fast, sharp |
| `bicubic` | ★★★☆☆ | ★★★★★ | Fast, smooth |
| `oversample` | ★★★★★ | ★★★★★ | Pixel art (best for Fallout!) |

### Special Recommendation: Oversample Filter

For retro pixel art games like Fallout 2, the `oversample` filter is highly recommended. It preserves pixel aspect ratios and prevents blur while still providing smooth scaling:

```cpp
mConfig.upscaler = PlaceboUpscaler::OVERSAMPLE;
```

---

## Custom mpv Shader Hooks

libplacebo supports loading custom mpv/GLSL shaders (like Anime4K):

```cpp
// Load custom shader hook
struct pl_hook* hook = nullptr;
if (pl_mpv_user_shader_parse(mVulkan->gpu, shaderSource.c_str(), 
                              shaderSource.length(), &hook)) {
    // Add to render params
    mRenderParams.hooks = &hook;
    mRenderParams.num_hooks = 1;
}
```

This allows you to continue using Anime4K shaders if desired, while benefiting from libplacebo's infrastructure.

---

## HDR Output

### Automatic HDR Detection

libplacebo automatically detects HDR capability and configures the swapchain:

```cpp
// Hint the desired colorspace
pl_swapchain_colorspace_hint(mSwapchain, &(struct pl_color_space) {
    .primaries = PL_COLOR_PRIM_BT_2020,
    .transfer = PL_COLOR_TRC_PQ,
});

// The actual output format is in the swapchain frame
struct pl_swapchain_frame frame;
pl_swapchain_start_frame(mSwapchain, &frame);
// frame.color_repr and frame.color_space contain the actual output format
```

### Tone Mapping

For SDR displays viewing HDR content (or vice versa):

```cpp
mColorMapParams = pl_color_map_default_params;
mColorMapParams.tone_mapping_function = &pl_tone_map_bt2390;  // ITU-R BT.2390
// Or for filmic look:
mColorMapParams.tone_mapping_function = &pl_tone_map_reinhard;
```

---

## Migration Checklist

- [ ] Install libplacebo (vcpkg recommended for MSVC)
- [ ] Update CMakeLists.txt with libplacebo detection
- [ ] Create `placebo_context.h` and `placebo_context.cc`
- [ ] Add `LIBPLACEBO` render mode
- [ ] Update `RenderPipeline` to use PlaceboContext
- [ ] Add configuration options to `renderer_config.ini`
- [ ] Test with various upscalers
- [ ] Test HDR output (if available)
- [ ] Remove old custom shader files (optional)
- [ ] Update documentation

---

## Files to Remove After Migration (Optional)

Once libplacebo is fully integrated and tested, these files can be simplified or removed:

- `data/shaders/` - Custom shader files (keep if using mpv hooks)
- `mpv_shader_parser.cc/h` - Built into libplacebo
- `generic_shader_pass.cc/h` - Replaced by pl_renderer
- `scaler_pass.cc/h` - Handled by libplacebo filters

---

## Troubleshooting

### "Vulkan not found"
Ensure the Vulkan SDK is installed and `VULKAN_SDK` environment variable is set.

### "libplacebo DLL not found"
Copy `libplacebo-351.dll` to the executable directory.

### "Swapchain creation failed"
Check that SDL_Vulkan is properly initialized and the window supports Vulkan.

### Black screen with libplacebo
Enable debug logging:
```cpp
mLog = pl_log_create(PL_API_VER, pl_log_params(
    .log_level = PL_LOG_DEBUG,
));
```

### Performance issues
- Use `spline36` or `lanczos` instead of `ewa_lanczos`
- Disable debanding
- Reduce antiringing strength

---

## References

- [libplacebo Documentation](https://libplacebo.org/)
- [libplacebo GitHub](https://github.com/haasn/libplacebo)
- [mpv Shader Documentation](https://mpv.io/manual/master/#options-glsl-shaders)
- [HDR on Windows](https://docs.microsoft.com/en-us/windows/win32/direct3darticles/high-dynamic-range)

---

## Files Created/Modified in This Migration

### New Files Created

| File | Description |
|------|-------------|
| [src/scr/renderer/placebo_context.h](../src/scr/renderer/placebo_context.h) | PlaceboContext class header with configuration types |
| [src/scr/renderer/placebo_context.cc](../src/scr/renderer/placebo_context.cc) | Full implementation of libplacebo integration |
| [tools/setup_libplacebo.ps1](../tools/setup_libplacebo.ps1) | PowerShell script to copy mingw package to project |
| [docs/LIBPLACEBO_MIGRATION_GUIDE.md](LIBPLACEBO_MIGRATION_GUIDE.md) | This comprehensive migration guide |

### Modified Files

| File | Changes |
|------|---------|
| [CMakeLists.txt](../CMakeLists.txt) | Added libplacebo detection and linking |
| [renderer_config.ini](../renderer_config.ini) | Added `[libplacebo]` configuration section |
| [src/scr/renderer/render_pipeline.h](../src/scr/renderer/render_pipeline.h) | Added `LIBPLACEBO` render mode and config |

### Installed Dependencies

| Directory | Contents |
|-----------|----------|
| `third_party/libplacebo/include/` | libplacebo headers |
| `third_party/libplacebo/lib/` | Static and import libraries |
| `third_party/libplacebo/bin/` | libplacebo-351.dll |

---

## Quick Start

After the files have been set up, to enable libplacebo:

1. **Verify libplacebo is installed**:
   ```powershell
   ls third_party/libplacebo/include/libplacebo/config.h
   ```

2. **Configure CMake**:
   ```powershell
   cmake -B build -DFALLOUT_USE_LIBPLACEBO=ON
   ```

3. **Build**:
   ```powershell
   cmake --build build --config Release
   ```

4. **Edit renderer_config.ini**:
   ```ini
   [General]
   Mode=2
   Backend=libplacebo
   
   [libplacebo]
   Upscaler=oversample
   ColorMode=sdr_enhance
   ```

5. **Run the game** and enjoy high-quality upscaling!

