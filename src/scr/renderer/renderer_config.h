#ifndef FALLOUT_RENDERER_CONFIG_H
#define FALLOUT_RENDERER_CONFIG_H

#include <string>
#include <unordered_map>

namespace fallout {
namespace renderer {

class RendererConfig {
public:
    static RendererConfig& GetInstance();

    bool Load(const std::string& configPath);
    void Save();

    int GetInt(const std::string& section, const std::string& key, int defaultValue);
    float GetFloat(const std::string& section, const std::string& key, float defaultValue);
    bool GetBool(const std::string& section, const std::string& key, bool defaultValue);
    std::string GetString(const std::string& section, const std::string& key, const std::string& defaultValue);

    void SetInt(const std::string& section, const std::string& key, int value);
    void SetFloat(const std::string& section, const std::string& key, float value);
    void SetBool(const std::string& section, const std::string& key, bool value);
    void SetString(const std::string& section, const std::string& key, const std::string& value);

private:
    RendererConfig() = default;
    std::string mConfigPath;
    std::unordered_map<std::string, std::unordered_map<std::string, std::string>> mData;
};

} // namespace renderer
} // namespace fallout

#endif // FALLOUT_RENDERER_CONFIG_H
