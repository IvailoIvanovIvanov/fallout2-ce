/**
 * @file placebo_filter_chain.cc
 * @brief Custom shader chain implementation for libplacebo.
 */

#include "placebo_filter_chain.h"

#if FALLOUT_HAVE_LIBPLACEBO

#include "logger.h"

#include <fstream>
#include <sstream>
#include <algorithm>
#include <regex>

namespace fallout {
namespace renderer {

//-----------------------------------------------------------------------------
// Destructor
//-----------------------------------------------------------------------------

PlaceboFilterChain::~PlaceboFilterChain() {
    Shutdown();
}

//-----------------------------------------------------------------------------
// Initialization
//-----------------------------------------------------------------------------

bool PlaceboFilterChain::Init(pl_log log, pl_gpu gpu) {
    if (mInitialized) {
        return true;
    }
    
    if (!log || !gpu) {
        Logger::Log(LogLevel::Error, "[FilterChain] Invalid log or GPU context");
        return false;
    }
    
    mLog = log;
    mGpu = gpu;
    mInitialized = true;
    
    Logger::Log(LogLevel::Info, "[FilterChain] Initialized");
    return true;
}

void PlaceboFilterChain::Shutdown() {
    ClearShaders();
    ClearBuiltinFilters();
    
    mLog = nullptr;
    mGpu = nullptr;
    mInitialized = false;
}

//-----------------------------------------------------------------------------
// Shader Loading
//-----------------------------------------------------------------------------

std::string PlaceboFilterChain::ReadShaderFile(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) {
        Logger::Log(LogLevel::Error, "[FilterChain] Failed to open shader: %s", path.c_str());
        return "";
    }
    
    std::stringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
}

void PlaceboFilterChain::ParseShaderInfo(const std::string& source, FilterStageInfo& info) {
    // Parse HOOK points
    std::regex hookRegex(R"(//!\s*HOOK\s+(\w+))");
    std::smatch match;
    if (std::regex_search(source, match, hookRegex)) {
        info.hookPoint = match[1].str();
    }
    
    // Parse DESC
    std::regex descRegex(R"(//!\s*DESC\s+(.+))");
    if (std::regex_search(source, match, descRegex)) {
        info.description = match[1].str();
    }
    
    // Parse WIDTH/HEIGHT for scale detection
    std::regex widthRegex(R"(//!\s*WIDTH\s+.+\s+(\d+)\s+\*)");
    if (std::regex_search(source, match, widthRegex)) {
        info.scaleFactor = std::stoi(match[1].str());
    }
}

bool PlaceboFilterChain::LoadShader(const std::string& path, const std::string& description) {
    if (!mInitialized) {
        Logger::Log(LogLevel::Error, "[FilterChain] Not initialized");
        return false;
    }
    
    std::string source = ReadShaderFile(path);
    if (source.empty()) {
        return false;
    }
    
    // Keep source alive (libplacebo references it)
    mShaderSources.push_back(std::move(source));
    const std::string& srcRef = mShaderSources.back();
    
    // Parse the shader
    const struct pl_hook* hook = pl_mpv_user_shader_parse(mGpu, srcRef.c_str(), srcRef.length());
    if (!hook) {
        Logger::Log(LogLevel::Error, "[FilterChain] Failed to parse shader: %s", path.c_str());
        mShaderSources.pop_back();
        return false;
    }
    
    mHooks.push_back(hook);
    
    // Create stage info
    FilterStageInfo info;
    info.type = FilterStageType::CUSTOM_SHADER;
    info.shaderPath = path;
    info.enabled = true;
    info.order = static_cast<int>(mActiveStages.size());
    
    // Extract filename for name
    size_t lastSlash = path.find_last_of("/\\");
    info.name = (lastSlash != std::string::npos) ? path.substr(lastSlash + 1) : path;
    
    // Parse shader for additional info
    ParseShaderInfo(srcRef, info);
    
    if (!description.empty()) {
        info.description = description;
    }
    
    mActiveStages.push_back(info);
    
    Logger::Log(LogLevel::Info, "[FilterChain] Loaded shader: %s (Hook: %s)", 
                info.name.c_str(), info.hookPoint.c_str());
    
    return true;
}

int PlaceboFilterChain::LoadShaders(const std::vector<std::string>& paths) {
    int loaded = 0;
    for (const auto& path : paths) {
        if (LoadShader(path)) {
            loaded++;
        }
    }
    return loaded;
}

std::vector<std::string> PlaceboFilterChain::GetAnime4KShaderList(Anime4KPreset preset) {
    std::vector<std::string> shaders;
    
    switch (preset) {
        case Anime4KPreset::MODE_A:
            // Fast mode - basic restore + upscale
            shaders = {
                "Anime4K_Clamp_Highlights.glsl",
                "Anime4K_Restore_CNN_M.glsl",
                "Anime4K_Upscale_CNN_x2_M.glsl"
            };
            break;
            
        case Anime4KPreset::MODE_B:
            // Balanced - denoise + restore + upscale
            shaders = {
                "Anime4K_Clamp_Highlights.glsl",
                "Anime4K_Restore_CNN_M.glsl",
                "Anime4K_Upscale_CNN_x2_M.glsl",
                "Anime4K_AutoDownscalePre_x2.glsl",
                "Anime4K_AutoDownscalePre_x4.glsl"
            };
            break;
            
        case Anime4KPreset::MODE_C:
            // Quality - full pipeline
            shaders = {
                "Anime4K_Clamp_Highlights.glsl",
                "Anime4K_Restore_CNN_VL.glsl",
                "Anime4K_Upscale_CNN_x2_VL.glsl",
                "Anime4K_AutoDownscalePre_x2.glsl",
                "Anime4K_AutoDownscalePre_x4.glsl",
                "Anime4K_Upscale_CNN_x2_M.glsl"
            };
            break;
            
        case Anime4KPreset::MODE_A_HQ:
            // High Quality A+A
            shaders = {
                "Anime4K_Clamp_Highlights.glsl",
                "Anime4K_Restore_CNN_M.glsl",
                "Anime4K_Upscale_CNN_x2_M.glsl",
                "Anime4K_Restore_CNN_S.glsl",
                "Anime4K_AutoDownscalePre_x2.glsl",
                "Anime4K_AutoDownscalePre_x4.glsl",
                "Anime4K_Upscale_CNN_x2_S.glsl"
            };
            break;
            
        case Anime4KPreset::MODE_B_HQ:
            // High Quality B+B
            shaders = {
                "Anime4K_Clamp_Highlights.glsl",
                "Anime4K_Restore_CNN_M.glsl",
                "Anime4K_Upscale_CNN_x2_M.glsl",
                "Anime4K_AutoDownscalePre_x2.glsl",
                "Anime4K_AutoDownscalePre_x4.glsl",
                "Anime4K_Restore_CNN_S.glsl",
                "Anime4K_Upscale_CNN_x2_S.glsl"
            };
            break;
            
        case Anime4KPreset::MODE_C_HQ:
            // Ultra Quality C+A
            shaders = {
                "Anime4K_Clamp_Highlights.glsl",
                "Anime4K_Restore_CNN_VL.glsl",
                "Anime4K_Upscale_CNN_x2_VL.glsl",
                "Anime4K_Restore_CNN_M.glsl",
                "Anime4K_AutoDownscalePre_x2.glsl",
                "Anime4K_AutoDownscalePre_x4.glsl",
                "Anime4K_Upscale_CNN_x2_M.glsl"
            };
            break;
            
        case Anime4KPreset::NONE:
        case Anime4KPreset::CUSTOM:
        default:
            break;
    }
    
    return shaders;
}

bool PlaceboFilterChain::LoadAnime4KPreset(Anime4KPreset preset, const std::string& shaderDir) {
    if (preset == Anime4KPreset::NONE || preset == Anime4KPreset::CUSTOM) {
        return true; // Nothing to load
    }
    
    auto shaderNames = GetAnime4KShaderList(preset);
    if (shaderNames.empty()) {
        return false;
    }
    
    Logger::Log(LogLevel::Info, "[FilterChain] Loading Anime4K preset with %zu shaders", 
                shaderNames.size());
    
    int loaded = 0;
    for (const auto& name : shaderNames) {
        std::string path = shaderDir + "/" + name;
        if (LoadShader(path, "Anime4K: " + name)) {
            loaded++;
        }
    }
    
    if (loaded > 0) {
        // Mark the first stage as part of Anime4K preset
        for (auto& stage : mActiveStages) {
            if (stage.type == FilterStageType::CUSTOM_SHADER) {
                stage.type = FilterStageType::ANIME4K_PRESET;
            }
        }
    }
    
    Logger::Log(LogLevel::Info, "[FilterChain] Loaded %d/%zu Anime4K shaders", 
                loaded, shaderNames.size());
    
    return loaded > 0;
}

void PlaceboFilterChain::ClearShaders() {
    // Destroy hooks
    for (auto hook : mHooks) {
        if (hook) {
            pl_mpv_user_shader_destroy(&hook);
        }
    }
    mHooks.clear();
    mShaderSources.clear();
    
    // Remove custom shader stages from active stages
    mActiveStages.erase(
        std::remove_if(mActiveStages.begin(), mActiveStages.end(),
            [](const FilterStageInfo& info) {
                return info.type == FilterStageType::CUSTOM_SHADER ||
                       info.type == FilterStageType::ANIME4K_PRESET;
            }),
        mActiveStages.end()
    );
}

//-----------------------------------------------------------------------------
// Shader Access
//-----------------------------------------------------------------------------

const struct pl_hook** PlaceboFilterChain::GetHooks() const {
    if (mHooks.empty()) {
        return nullptr;
    }
    
    // Return pointer to internal array (caller should not modify)
    return const_cast<const struct pl_hook**>(mHooks.data());
}

//-----------------------------------------------------------------------------
// Filter Stage Logging
//-----------------------------------------------------------------------------

void PlaceboFilterChain::RegisterBuiltinFilter(FilterStageType type,
                                                const std::string& name,
                                                const std::string& description) {
    FilterStageInfo info;
    info.type = type;
    info.name = name;
    info.description = description;
    info.enabled = true;
    info.order = static_cast<int>(mActiveStages.size());
    
    mActiveStages.push_back(info);
}

void PlaceboFilterChain::ClearBuiltinFilters() {
    mActiveStages.erase(
        std::remove_if(mActiveStages.begin(), mActiveStages.end(),
            [](const FilterStageInfo& info) {
                return info.type != FilterStageType::CUSTOM_SHADER &&
                       info.type != FilterStageType::ANIME4K_PRESET;
            }),
        mActiveStages.end()
    );
}

void PlaceboFilterChain::LogActiveFilters() const {
    Logger::Log(LogLevel::Info, "=== Active Filter Pipeline ===");
    
    if (mActiveStages.empty()) {
        Logger::Log(LogLevel::Info, "  (No active filters)");
        return;
    }
    
    // Sort by order
    std::vector<FilterStageInfo> sorted = mActiveStages;
    std::sort(sorted.begin(), sorted.end(), 
              [](const FilterStageInfo& a, const FilterStageInfo& b) {
                  return a.order < b.order;
              });
    
    int stageNum = 1;
    for (const auto& stage : sorted) {
        if (!stage.enabled) continue;
        
        const char* typeStr = "";
        switch (stage.type) {
            case FilterStageType::BUILTIN_UPSCALER:     typeStr = "Upscaler"; break;
            case FilterStageType::BUILTIN_DEBANDING:    typeStr = "Deband"; break;
            case FilterStageType::BUILTIN_COLOR_ADJUST: typeStr = "Color"; break;
            case FilterStageType::BUILTIN_DITHER:       typeStr = "Dither"; break;
            case FilterStageType::CUSTOM_SHADER:        typeStr = "Shader"; break;
            case FilterStageType::ANIME4K_PRESET:       typeStr = "Anime4K"; break;
        }
        
        if (stage.description.empty()) {
            Logger::Log(LogLevel::Info, "  [%d] %s: %s", stageNum++, typeStr, stage.name.c_str());
        } else {
            Logger::Log(LogLevel::Info, "  [%d] %s: %s (%s)", 
                        stageNum++, typeStr, stage.name.c_str(), stage.description.c_str());
        }
        
        // Extra info for shaders
        if ((stage.type == FilterStageType::CUSTOM_SHADER || 
             stage.type == FilterStageType::ANIME4K_PRESET) && 
            !stage.hookPoint.empty()) {
            Logger::Log(LogLevel::Info, "       Hook: %s, Scale: %dx", 
                        stage.hookPoint.c_str(), stage.scaleFactor);
        }
    }
    
    Logger::Log(LogLevel::Info, "==============================");
}

} // namespace renderer
} // namespace fallout

#endif // FALLOUT_HAVE_LIBPLACEBO
