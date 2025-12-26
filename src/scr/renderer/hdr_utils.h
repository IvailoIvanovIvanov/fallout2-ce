#ifndef FALLOUT_RENDERER_HDR_UTILS_H
#define FALLOUT_RENDERER_HDR_UTILS_H

/**
 * @file hdr_utils.h
 * @brief HDR detection utilities for Windows.
 *
 * Provides functions to detect if HDR is enabled on the system display
 * to help choose the appropriate rendering backend.
 */

#ifdef _WIN32

#include <windows.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

namespace fallout {
namespace renderer {

/**
 * @brief Check if HDR is enabled on the primary monitor.
 * @return true if HDR is enabled on the display, false otherwise.
 */
inline bool IsHDREnabled() {
    Microsoft::WRL::ComPtr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
        return false;
    }

    // Get the primary adapter
    Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
    if (FAILED(factory->EnumAdapters1(0, &adapter))) {
        return false;
    }

    // Get the primary output
    Microsoft::WRL::ComPtr<IDXGIOutput> output;
    if (FAILED(adapter->EnumOutputs(0, &output))) {
        return false;
    }

    // Query for IDXGIOutput6 to get HDR capabilities
    Microsoft::WRL::ComPtr<IDXGIOutput6> output6;
    if (FAILED(output.As(&output6))) {
        return false;
    }

    DXGI_OUTPUT_DESC1 desc;
    if (FAILED(output6->GetDesc1(&desc))) {
        return false;
    }

    // Check if the display has HDR support
    return desc.ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020 ||
           desc.ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709;
}

} // namespace renderer
} // namespace fallout

#else // Non-Windows

namespace fallout {
namespace renderer {

inline bool IsHDREnabled() {
    return false;
}

} // namespace renderer
} // namespace fallout

#endif // _WIN32

#endif // FALLOUT_RENDERER_HDR_UTILS_H
