#ifndef FALLOUT_RENDERER_VULKAN_CONTEXT_H
#define FALLOUT_RENDERER_VULKAN_CONTEXT_H

/**
 * @file vulkan_context.h
 * @brief Vulkan-based GPU context implementation with HDR support.
 *
 * This file provides a Vulkan implementation of the GpuContext interface,
 * enabling HDR output on Windows 10/11 displays. The implementation uses
 * compute shaders for upscaling (similar to the OpenGL backend) and supports
 * HDR10 color space output via VK_EXT_swapchain_colorspace.
 */

#include "gpu_context.h"
#include "render_types.h"

#include <vector>
#include <string>
#include <memory>
#include <unordered_map>

// Only include Vulkan if available
#if FALLOUT_HAVE_VULKAN

#ifdef _WIN32
#define VK_USE_PLATFORM_WIN32_KHR
#endif

#include <vulkan/vulkan.h>

struct SDL_Window;

namespace fallout {
namespace renderer {

/**
 * @enum HDRMode
 * @brief HDR output mode for the Vulkan swapchain.
 */
enum class HDRMode {
    SDR,        ///< Standard dynamic range (sRGB)
    HDR10,      ///< HDR10 (PQ transfer function, Rec.2020 primaries)
    scRGB       ///< Linear scRGB (extended sRGB with values > 1.0)
};

/**
 * @struct VulkanTexture
 * @brief Internal representation of a Vulkan texture resource.
 */
struct VulkanTexture {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView imageView = VK_NULL_HANDLE;
    VkSampler sampler = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
    uint32_t width = 0;
    uint32_t height = 0;
    bool isStorageImage = false;  ///< True if used as compute shader output
    VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
};

/**
 * @struct VulkanPipeline
 * @brief Internal representation of a Vulkan compute pipeline.
 */
struct VulkanPipeline {
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkDescriptorSetLayout descriptorSetLayout = VK_NULL_HANDLE;
    VkShaderModule shaderModule = VK_NULL_HANDLE;
};

/**
 * @class VulkanContext
 * @brief Vulkan implementation of GpuContext with HDR support.
 *
 * Provides:
 * - Vulkan instance and device management
 * - HDR-capable swapchain (HDR10 or scRGB)
 * - Compute shader pipeline for upscaling
 * - Texture management with proper HDR formats
 *
 * Thread-safety: Not thread-safe. All calls must be from the main thread.
 */
class VulkanContext : public GpuContext {
public:
    /**
     * @brief Constructs a VulkanContext.
     * @param window SDL window handle.
     * @param screenWidth Target screen width.
     * @param screenHeight Target screen height.
     * @param enableHDR Whether to enable HDR output.
     */
    VulkanContext(SDL_Window* window, int screenWidth, int screenHeight, bool enableHDR = true);
    ~VulkanContext() override;

    //-------------------------------------------------------------------------
    // GpuContext Interface Implementation
    //-------------------------------------------------------------------------

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

    //-------------------------------------------------------------------------
    // Vulkan-Specific Methods
    //-------------------------------------------------------------------------

    /**
     * @brief Gets the current HDR mode.
     * @return The active HDR mode.
     */
    HDRMode GetHDRMode() const { return mHDRMode; }

    /**
     * @brief Checks if HDR output is currently active.
     * @return true if HDR swapchain is active, false otherwise.
     */
    bool IsHDRActive() const { return mHDRMode != HDRMode::SDR; }

    /**
     * @brief Compiles GLSL source to SPIR-V.
     * @param glslSource GLSL compute shader source.
     * @param spirvOutput Output vector for SPIR-V bytecode.
     * @return true if compilation succeeded, false otherwise.
     */
    bool CompileGLSLToSPIRV(const std::string& glslSource, std::vector<uint32_t>& spirvOutput);

private:
    //-------------------------------------------------------------------------
    // Initialization Helpers
    //-------------------------------------------------------------------------
    bool CreateInstance();
    bool SelectPhysicalDevice();
    bool CreateLogicalDevice();
    bool CreateSurface();
    bool CreateSwapchain();
    bool CreateCommandPool();
    bool CreateDescriptorPool();
    bool CreateSyncObjects();
    bool CreatePresentPipeline();

    //-------------------------------------------------------------------------
    // Resource Helpers
    //-------------------------------------------------------------------------
    VkFormat GetVulkanFormat(TextureFormat format) const;
    uint32_t FindMemoryType(uint32_t typeFilter, VkMemoryPropertyFlags properties) const;
    VkCommandBuffer BeginSingleTimeCommands();
    void EndSingleTimeCommands(VkCommandBuffer commandBuffer);
    void TransitionImageLayout(VkImage image, VkFormat format, 
                               VkImageLayout oldLayout, VkImageLayout newLayout);

    //-------------------------------------------------------------------------
    // Swapchain Helpers
    //-------------------------------------------------------------------------
    void CleanupSwapchain();
    void RecreateSwapchain();
    VkSurfaceFormatKHR ChooseSurfaceFormat(const std::vector<VkSurfaceFormatKHR>& formats);
    VkPresentModeKHR ChoosePresentMode(const std::vector<VkPresentModeKHR>& modes);
    VkExtent2D ChooseSwapExtent(const VkSurfaceCapabilitiesKHR& capabilities);

    //-------------------------------------------------------------------------
    // Member Variables
    //-------------------------------------------------------------------------
    SDL_Window* mWindow = nullptr;
    int mScreenWidth = 0;
    int mScreenHeight = 0;
    bool mEnableHDR = true;
    HDRMode mHDRMode = HDRMode::SDR;

    // Vulkan core objects
    VkInstance mInstance = VK_NULL_HANDLE;
    VkPhysicalDevice mPhysicalDevice = VK_NULL_HANDLE;
    VkDevice mDevice = VK_NULL_HANDLE;
    VkQueue mGraphicsQueue = VK_NULL_HANDLE;
    VkQueue mComputeQueue = VK_NULL_HANDLE;
    VkQueue mPresentQueue = VK_NULL_HANDLE;
    uint32_t mGraphicsQueueFamily = 0;
    uint32_t mComputeQueueFamily = 0;
    uint32_t mPresentQueueFamily = 0;

    // Surface and swapchain
    VkSurfaceKHR mSurface = VK_NULL_HANDLE;
    VkSwapchainKHR mSwapchain = VK_NULL_HANDLE;
    std::vector<VkImage> mSwapchainImages;
    std::vector<VkImageLayout> mSwapchainImageLayouts;
    std::vector<VkImageView> mSwapchainImageViews;
    VkFormat mSwapchainFormat = VK_FORMAT_UNDEFINED;
    VkExtent2D mSwapchainExtent = {0, 0};
    VkColorSpaceKHR mSwapchainColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;

    // Command pools and buffers
    VkCommandPool mCommandPool = VK_NULL_HANDLE;
    std::vector<VkCommandBuffer> mCommandBuffers;

    // Descriptor management
    VkDescriptorPool mDescriptorPool = VK_NULL_HANDLE;

    // Synchronization
    std::vector<VkSemaphore> mImageAvailableSemaphores;
    std::vector<VkSemaphore> mRenderFinishedSemaphores;
    std::vector<VkFence> mInFlightFences;
    uint32_t mCurrentFrame = 0;
    uint32_t mCurrentImageIndex = 0;
    static constexpr int MAX_FRAMES_IN_FLIGHT = 2;

    // Present pipeline (for blitting to swapchain)
    VulkanPipeline mPresentPipeline;

    // Uniform buffer for constants
    VkBuffer mUniformBuffer = VK_NULL_HANDLE;
    VkDeviceMemory mUniformBufferMemory = VK_NULL_HANDLE;
    void* mUniformBufferMapped = nullptr;

    // Staging buffer for texture uploads
    VkBuffer mStagingBuffer = VK_NULL_HANDLE;
    VkDeviceMemory mStagingBufferMemory = VK_NULL_HANDLE;
    void* mStagingBufferMapped = nullptr;
    VkDeviceSize mStagingBufferSize = 0;

    // Current bindings for dispatch
    struct {
        void* textures[16] = {};
        void* storageImages[8] = {};
        VkDescriptorSet currentDescriptorSet = VK_NULL_HANDLE;
    } mBindings;

    // Validation layers (debug builds only)
#ifdef _DEBUG
    VkDebugUtilsMessengerEXT mDebugMessenger = VK_NULL_HANDLE;
#endif
};

} // namespace renderer
} // namespace fallout

#else // !FALLOUT_HAVE_VULKAN

// Stub class when Vulkan is not available
namespace fallout {
namespace renderer {

class VulkanContext : public GpuContext {
public:
    VulkanContext(void*, int, int, bool = true) {}
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

#endif // FALLOUT_HAVE_VULKAN

#endif // FALLOUT_RENDERER_VULKAN_CONTEXT_H
