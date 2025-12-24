#include "screenshot_manager.h"
#include "logger.h"
#include <SDL.h>
#include <vector>
#include <filesystem>
#include <ctime>
#include <iomanip>
#include <sstream>

namespace fallout {
namespace renderer {

ScreenshotManager::ScreenshotManager() {}

void ScreenshotManager::Capture(GpuContext& context, const RenderSurface& surface, const std::string& stageName) {
    if (!surface.handle) return;

    std::vector<uint8_t> pixels(surface.width * surface.height * 4);
    context.ReadbackTexture(surface.handle, pixels.data(), static_cast<int>(pixels.size()));

    // Create directory if it doesn't exist
    std::filesystem::path dir("screenshots");
    if (!std::filesystem::exists(dir)) {
        std::filesystem::create_directory(dir);
    }

    // Generate filename with timestamp and stage name
    auto t = std::time(nullptr);
    auto tm = *std::localtime(&t);
    
    std::stringstream ss;
    ss << "screenshots/shot_" << std::put_time(&tm, "%Y%m%d_%H%M%S") << "_" << stageName << ".bmp";
    
    SaveSurface(pixels.data(), surface.width, surface.height, ss.str());
}

void ScreenshotManager::SaveSurface(const void* data, int width, int height, const std::string& filename) {
    // Create SDL Surface to save as BMP
    // Data is already in the correct order for SDL (Top-Down) because
    // OpenGL texture data was uploaded Top-Down and ReadPixels returns it as is.
    
    int stride = width * 4;

    // Mask for RGBA (Little Endian: ABGR in memory, so R mask is 0x000000FF)
    #if SDL_BYTEORDER == SDL_BIG_ENDIAN
        uint32_t rmask = 0xff000000;
        uint32_t gmask = 0x00ff0000;
        uint32_t bmask = 0x0000ff00;
        uint32_t amask = 0x000000ff;
    #else
        uint32_t rmask = 0x000000ff;
        uint32_t gmask = 0x0000ff00;
        uint32_t bmask = 0x00ff0000;
        uint32_t amask = 0xff000000;
    #endif

    // SDL_CreateRGBSurfaceFrom expects a non-const pointer, but doesn't modify it.
    // We cast away const here. The data lifetime is managed by the caller (Capture).
    SDL_Surface* surf = SDL_CreateRGBSurfaceFrom(
        const_cast<void*>(data), width, height, 32, stride,
        rmask, gmask, bmask, amask
    );

    if (surf) {
        if (SDL_SaveBMP(surf, filename.c_str()) != 0) {
            Logger::Log(LogLevel::Error, "ScreenshotManager: Failed to save %s: %s", filename.c_str(), SDL_GetError());
        } else {
            Logger::Log(LogLevel::Info, "ScreenshotManager: Saved %s", filename.c_str());
        }
        SDL_FreeSurface(surf);
    } else {
        Logger::Log(LogLevel::Error, "ScreenshotManager: Failed to create surface: %s", SDL_GetError());
    }
}

} // namespace renderer
} // namespace fallout
