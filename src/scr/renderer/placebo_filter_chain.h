/**
 * @file placebo_filter_chain.h
 * @brief Custom shader chain management for libplacebo.
 *
 * This file provides a filter chain abstraction for loading and applying
 * mpv-style custom shaders (.glsl with HOOK syntax) through libplacebo.
 * This enables running Anime4K and other shader pipelines through libplacebo.
 */

#ifndef FALLOUT_RENDERER_PLACEBO_FILTER_CHAIN_H
#define FALLOUT_RENDERER_PLACEBO_FILTER_CHAIN_H

#include "render_types.h"
#include <vector>
#include <string>
#include <memory>

#if FALLOUT_HAVE_LIBPLACEBO

#include <libplacebo/shaders/custom.h>
#include <libplacebo/gpu.h>
#include <libplacebo/log.h>

namespace fallout {
namespace renderer {

//-----------------------------------------------------------------------------
// Filter Stage Types
//-----------------------------------------------------------------------------

/**
 * @enum FilterStageType
 * @brief Identifies the type of filter stage.
 */
enum class FilterStageType {
    BUILTIN_UPSCALER,       ///< libplacebo built-in upscaler
    BUILTIN_DEBANDING,      ///< libplacebo debanding
    BUILTIN_COLOR_ADJUST,   ///< libplacebo color adjustments
    BUILTIN_DITHER,         ///< libplacebo dithering
    CUSTOM_SHADER,          ///< Custom mpv-style GLSL shader
    ANIME4K_PRESET          ///< Pre-configured Anime4K chain
};

/**
 * @struct FilterStageInfo
 * @brief Information about an active filter stage for logging.
 */
struct FilterStageInfo {
    FilterStageType type;
    std::string name;           ///< Human-readable name
    std::string description;    ///< Detailed description
    bool enabled = true;
    int order = 0;              ///< Execution order
    
    // Shader-specific info
    std::string shaderPath;     ///< Path to shader file (if custom)
    std::string hookPoint;      ///< HOOK point (MAIN, LUMA, etc.)
    int scaleFactor = 1;        ///< Scale factor if applicable
};

/**
 * @struct CustomShaderConfig
 * @brief Configuration for a single custom shader.
 */
struct CustomShaderConfig {
    std::string path;           ///< Path to .glsl shader file
    bool enabled = true;
    std::string description;    ///< User-friendly description
};

/**
 * @struct Anime4KPreset
 * @brief Pre-configured Anime4K shader chains.
 */
enum class Anime4KPreset {
    NONE,                   ///< No Anime4K processing
    MODE_A,                 ///< Anime4K Mode A (Fast)
    MODE_B,                 ///< Anime4K Mode B (Balanced)
    MODE_C,                 ///< Anime4K Mode C (Quality)
    MODE_A_HQ,              ///< Anime4K Mode A+A (High Quality)
    MODE_B_HQ,              ///< Anime4K Mode B+B (High Quality)
    MODE_C_HQ,              ///< Anime4K Mode C+A (Ultra Quality)
    CUSTOM                  ///< Custom shader list
};

//-----------------------------------------------------------------------------
// PlaceboFilterChain Class
//-----------------------------------------------------------------------------

/**
 * @class PlaceboFilterChain
 * @brief Manages custom shader chains for libplacebo.
 *
 * Responsibilities (Single Responsibility):
 * - Loading and parsing custom shaders (.glsl files)
 * - Managing shader lifecycle (creation/destruction)
 * - Providing shader hooks to pl_renderer
 * - Logging active filter stages
 *
 * Does NOT:
 * - Perform rendering (that's PlaceboContext)
 * - Manage configuration (that's RendererConfig)
 */
class PlaceboFilterChain {
public:
    PlaceboFilterChain() = default;
    ~PlaceboFilterChain();

    /**
     * @brief Initializes the filter chain with a GPU context.
     * @param log libplacebo log context.
     * @param gpu libplacebo GPU handle.
     * @return true if initialization succeeded.
     */
    bool Init(pl_log log, pl_gpu gpu);

    /**
     * @brief Shuts down and releases all shader resources.
     */
    void Shutdown();

    //-------------------------------------------------------------------------
    // Shader Loading
    //-------------------------------------------------------------------------

    /**
     * @brief Loads a custom shader from file.
     * @param path Path to .glsl shader file.
     * @param description Optional description for logging.
     * @return true if shader was loaded successfully.
     */
    bool LoadShader(const std::string& path, const std::string& description = "");

    /**
     * @brief Loads multiple shaders from a list of paths.
     * @param paths Vector of shader file paths.
     * @return Number of shaders loaded successfully.
     */
    int LoadShaders(const std::vector<std::string>& paths);

    /**
     * @brief Loads an Anime4K preset.
     * @param preset The preset to load.
     * @param shaderDir Base directory for shader files.
     * @return true if preset loaded successfully.
     */
    bool LoadAnime4KPreset(Anime4KPreset preset, const std::string& shaderDir = "data/shaders");

    /**
     * @brief Clears all loaded shaders.
     */
    void ClearShaders();

    //-------------------------------------------------------------------------
    // Shader Access
    //-------------------------------------------------------------------------

    /**
     * @brief Gets the array of loaded shader hooks.
     * @return Pointer to array of pl_hook pointers (null-terminated).
     */
    const struct pl_hook** GetHooks() const;

    /**
     * @brief Gets the number of loaded shaders.
     * @return Number of shaders.
     */
    size_t GetShaderCount() const { return mHooks.size(); }

    /**
     * @brief Checks if any custom shaders are active.
     * @return true if shaders are loaded.
     */
    bool HasActiveShaders() const { return !mHooks.empty(); }

    //-------------------------------------------------------------------------
    // Filter Stage Logging
    //-------------------------------------------------------------------------

    /**
     * @brief Gets information about all active filter stages.
     * @return Vector of filter stage info structs.
     */
    std::vector<FilterStageInfo> GetActiveStages() const { return mActiveStages; }

    /**
     * @brief Logs all active filter stages to the renderer log.
     */
    void LogActiveFilters() const;

    /**
     * @brief Adds a built-in filter stage for logging.
     * @param type Type of built-in filter.
     * @param name Name of the filter.
     * @param description Description of settings.
     */
    void RegisterBuiltinFilter(FilterStageType type, 
                               const std::string& name,
                               const std::string& description);

    /**
     * @brief Clears registered built-in filters.
     */
    void ClearBuiltinFilters();

private:
    //-------------------------------------------------------------------------
    // Helpers
    //-------------------------------------------------------------------------
    
    std::string ReadShaderFile(const std::string& path);
    std::vector<std::string> GetAnime4KShaderList(Anime4KPreset preset);
    void ParseShaderInfo(const std::string& source, FilterStageInfo& info);

    //-------------------------------------------------------------------------
    // Member Variables
    //-------------------------------------------------------------------------
    
    pl_log mLog = nullptr;
    pl_gpu mGpu = nullptr;
    
    std::vector<const struct pl_hook*> mHooks;
    std::vector<std::string> mShaderSources;  // Keep sources alive
    std::vector<FilterStageInfo> mActiveStages;
    
    bool mInitialized = false;
};

} // namespace renderer
} // namespace fallout

#else // !FALLOUT_HAVE_LIBPLACEBO

// Stub when libplacebo not available
namespace fallout {
namespace renderer {

enum class FilterStageType { CUSTOM_SHADER };
enum class Anime4KPreset { NONE };
struct FilterStageInfo { std::string name; bool enabled = false; };
struct CustomShaderConfig { std::string path; bool enabled = false; };

class PlaceboFilterChain {
public:
    bool Init(void*, void*) { return false; }
    void Shutdown() {}
    bool LoadShader(const std::string&, const std::string& = "") { return false; }
    int LoadShaders(const std::vector<std::string>&) { return 0; }
    bool LoadAnime4KPreset(Anime4KPreset, const std::string& = "") { return false; }
    void ClearShaders() {}
    const void** GetHooks() const { return nullptr; }
    size_t GetShaderCount() const { return 0; }
    bool HasActiveShaders() const { return false; }
    std::vector<FilterStageInfo> GetActiveStages() const { return {}; }
    void LogActiveFilters() const {}
    void RegisterBuiltinFilter(FilterStageType, const std::string&, const std::string&) {}
    void ClearBuiltinFilters() {}
};

} // namespace renderer
} // namespace fallout

#endif // FALLOUT_HAVE_LIBPLACEBO

#endif // FALLOUT_RENDERER_PLACEBO_FILTER_CHAIN_H
