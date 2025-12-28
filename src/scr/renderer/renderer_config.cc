#include "renderer_config.h"
#include <fstream>
#include <sstream>
#include <algorithm>

namespace fallout {
namespace renderer {

RendererConfig& RendererConfig::GetInstance() {
    static RendererConfig instance;
    return instance;
}

static std::string Trim(const std::string& str) {
    size_t first = str.find_first_not_of(" \t\r\n");
    if (std::string::npos == first) return str;
    size_t last = str.find_last_not_of(" \t\r\n");
    return str.substr(first, (last - first + 1));
}

bool RendererConfig::Load(const std::string& configPath) {
    mConfigPath = configPath;
    std::ifstream file(configPath);
    if (!file.is_open()) return false;

    std::string line;
    std::string currentSection;

    while (std::getline(file, line)) {
        // Remove comments at end of line
        size_t commentPos = line.find(';');
        if (commentPos != std::string::npos) {
            line = line.substr(0, commentPos);
        }
        commentPos = line.find('#');
        if (commentPos != std::string::npos) {
            line = line.substr(0, commentPos);
        }

        line = Trim(line);
        if (line.empty()) continue;

        if (line[0] == '[' && line.back() == ']') {
            currentSection = line.substr(1, line.size() - 2);
        } else {
            size_t eqPos = line.find('=');
            if (eqPos != std::string::npos) {
                std::string key = Trim(line.substr(0, eqPos));
                std::string value = Trim(line.substr(eqPos + 1));
                mData[currentSection][key] = value;
            }
        }
    }
    return true;
}

void RendererConfig::Save() {
    if (mConfigPath.empty()) return;
    std::ofstream file(mConfigPath);
    if (!file.is_open()) return;

    for (const auto& sectionPair : mData) {
        file << "[" << sectionPair.first << "]\n";
        for (const auto& keyPair : sectionPair.second) {
            file << keyPair.first << " = " << keyPair.second << "\n";
        }
        file << "\n";
    }
}

int RendererConfig::GetInt(const std::string& section, const std::string& key, int defaultValue) {
    if (mData.count(section) && mData[section].count(key)) {
        try {
            return std::stoi(mData[section][key]);
        } catch (...) {}
    }
    return defaultValue;
}

float RendererConfig::GetFloat(const std::string& section, const std::string& key, float defaultValue) {
    if (mData.count(section) && mData[section].count(key)) {
        try {
            return std::stof(mData[section][key]);
        } catch (...) {}
    }
    return defaultValue;
}

bool RendererConfig::GetBool(const std::string& section, const std::string& key, bool defaultValue) {
    if (mData.count(section) && mData[section].count(key)) {
        std::string val = mData[section][key];
        std::transform(val.begin(), val.end(), val.begin(), ::tolower);
        return val == "true" || val == "1" || val == "yes" || val == "on";
    }
    return defaultValue;
}

std::string RendererConfig::GetString(const std::string& section, const std::string& key, const std::string& defaultValue) {
    if (mData.count(section) && mData[section].count(key)) {
        return mData[section][key];
    }
    return defaultValue;
}

void RendererConfig::SetInt(const std::string& section, const std::string& key, int value) {
    mData[section][key] = std::to_string(value);
}

void RendererConfig::SetFloat(const std::string& section, const std::string& key, float value) {
    mData[section][key] = std::to_string(value);
}

void RendererConfig::SetBool(const std::string& section, const std::string& key, bool value) {
    mData[section][key] = value ? "true" : "false";
}

void RendererConfig::SetString(const std::string& section, const std::string& key, const std::string& value) {
    mData[section][key] = value;
}

} // namespace renderer
} // namespace fallout
