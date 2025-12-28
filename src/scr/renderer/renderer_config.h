#ifndef FALLOUT_RENDERER_CONFIG_H
#define FALLOUT_RENDERER_CONFIG_H

/**
 * @file renderer_config.h
 * @brief INI-based configuration for the rendering pipeline.
 *
 * RendererConfig provides a simple key-value configuration system
 * using INI file format. It supports typed accessors (int, float,
 * bool, string) with default values for missing keys.
 */

#include <string>
#include <unordered_map>

namespace fallout {
namespace renderer {

/**
 * @class RendererConfig
 * @brief Singleton configuration manager for renderer settings.
 *
 * Loads settings from renderer_config.ini and provides typed accessors.
 * Settings are organized into sections (e.g., [General], [Anime4K]).
 *
 * Configuration File Format:
 * @code
 *   [General]
 *   Mode=1
 *   VerboseLogging=false
 *
 *   [Anime4K]
 *   EnableScale1=true
 *   Scale1Shader=Anime4K_Upscale_CNN_x2_L.glsl
 * @endcode
 */
class RendererConfig {
public:
    /**
     * @brief Returns the singleton instance.
     */
    static RendererConfig& GetInstance();

    //-------------------------------------------------------------------------
    // File Operations
    //-------------------------------------------------------------------------

    /**
     * @brief Loads configuration from an INI file.
     * @param configPath Path to the configuration file.
     * @return true if file was loaded successfully.
     */
    bool Load(const std::string& configPath);

    /**
     * @brief Saves current configuration to the loaded file.
     */
    void Save();

    //-------------------------------------------------------------------------
    // Getters
    //-------------------------------------------------------------------------

    /**
     * @brief Gets an integer value.
     * @param section INI section name (e.g., "General").
     * @param key Setting key name.
     * @param defaultValue Value to return if key is not found.
     */
    int GetInt(const std::string& section, const std::string& key, int defaultValue);

    /**
     * @brief Gets a float value.
     */
    float GetFloat(const std::string& section, const std::string& key, float defaultValue);

    /**
     * @brief Gets a boolean value.
     * @note Recognizes "true", "1", "yes" as true (case-insensitive).
     */
    bool GetBool(const std::string& section, const std::string& key, bool defaultValue);

    /**
     * @brief Gets a string value.
     */
    std::string GetString(const std::string& section, const std::string& key,
                          const std::string& defaultValue);

    //-------------------------------------------------------------------------
    // Setters
    //-------------------------------------------------------------------------

    void SetInt(const std::string& section, const std::string& key, int value);
    void SetFloat(const std::string& section, const std::string& key, float value);
    void SetBool(const std::string& section, const std::string& key, bool value);
    void SetString(const std::string& section, const std::string& key, const std::string& value);

private:
    RendererConfig() = default;

    /// Path to the loaded configuration file
    std::string mConfigPath;

    /// Nested map: section -> (key -> value)
    std::unordered_map<std::string, std::unordered_map<std::string, std::string>> mData;
};

} // namespace renderer
} // namespace fallout

#endif // FALLOUT_RENDERER_CONFIG_H
