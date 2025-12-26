#ifndef FALLOUT_RENDERER_RENDER_TYPES_H
#define FALLOUT_RENDERER_RENDER_TYPES_H

/**
 * @file render_types.h
 * @brief Core rendering types and data structures.
 *
 * This file defines fundamental types used throughout the renderer subsystem,
 * including texture formats, texture descriptors, render surfaces, and
 * common parameter structures for shader operations.
 */

namespace fallout {
namespace renderer {

//-----------------------------------------------------------------------------
// Texture Formats
//-----------------------------------------------------------------------------

/**
 * @enum TextureFormat
 * @brief Supported texture pixel formats.
 */
enum class TextureFormat {
    RGBA8,    ///< 8-bit per channel RGBA (32-bit total, standard format)
    RGBA16F,  ///< 16-bit float per channel RGBA (64-bit, high precision)
    RGBA32F   ///< 32-bit float per channel RGBA (128-bit, maximum precision)
};

//-----------------------------------------------------------------------------
// Texture Descriptor
//-----------------------------------------------------------------------------

/**
 * @struct TextureDesc
 * @brief Describes texture properties for creation.
 */
struct TextureDesc {
    int width = 0;                          ///< Width in pixels
    int height = 0;                         ///< Height in pixels
    TextureFormat format = TextureFormat::RGBA8;  ///< Pixel format
};

//-----------------------------------------------------------------------------
// Render Surface
//-----------------------------------------------------------------------------

/**
 * @struct RenderSurface
 * @brief Represents a render target or texture with associated metadata.
 *
 * Used to pass texture handles along with dimension and format information
 * to shader passes and render operations.
 */
struct RenderSurface {
    void* handle = nullptr;                 ///< Opaque GPU texture handle
    int width = 0;                          ///< Surface width in pixels
    int height = 0;                         ///< Surface height in pixels
    TextureFormat format = TextureFormat::RGBA8;  ///< Pixel format
    
    /**
     * @brief Checks if the surface has a valid handle.
     */
    bool IsValid() const { return handle != nullptr && width > 0 && height > 0; }
};

//-----------------------------------------------------------------------------
// Shader Constants
//-----------------------------------------------------------------------------

/**
 * @struct ScalerConstants
 * @brief Constant buffer data for scaling shader operations.
 *
 * Contains dimensions and transformation parameters needed by
 * compute shaders that perform image scaling.
 */
struct ScalerConstants {
    int inputWidth = 0;     ///< Input texture width
    int inputHeight = 0;    ///< Input texture height
    int outputWidth = 0;    ///< Output texture width
    int outputHeight = 0;   ///< Output texture height
    float offsetX = 0.0f;   ///< Horizontal offset for centered scaling
    float offsetY = 0.0f;   ///< Vertical offset for centered scaling
    float scaleX = 1.0f;    ///< Horizontal scale factor
    float scaleY = 1.0f;    ///< Vertical scale factor
};

//-----------------------------------------------------------------------------
// Dimensions Helper
//-----------------------------------------------------------------------------

/**
 * @struct Dimensions
 * @brief Simple width/height pair for dimension passing.
 */
struct Dimensions {
    int width = 0;
    int height = 0;
    
    Dimensions() = default;
    Dimensions(int w, int h) : width(w), height(h) {}
    
    bool operator==(const Dimensions& other) const {
        return width == other.width && height == other.height;
    }
    
    bool operator!=(const Dimensions& other) const {
        return !(*this == other);
    }
    
    /**
     * @brief Calculates aspect ratio (width / height).
     */
    float AspectRatio() const {
        return (height > 0) ? static_cast<float>(width) / height : 1.0f;
    }
};

} // namespace renderer
} // namespace fallout

#endif // FALLOUT_RENDERER_RENDER_TYPES_H
