/**
 * @file vulkan_context.cc
 * @brief Vulkan-based GPU context implementation with HDR support.
 *
 * This implementation provides:
 * - Vulkan instance/device setup with validation layers (debug)
 * - HDR swapchain using VK_EXT_swapchain_colorspace
 * - Compute shader pipelines for upscaling
 * - Texture and buffer management
 */

#include "vulkan_context.h"
#include "logger.h"

#if FALLOUT_HAVE_VULKAN

#include <SDL.h>
#include <SDL_vulkan.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <set>
#include <fstream>

#if FALLOUT_HAVE_SHADERC
#include <shaderc/shaderc.hpp>
#endif

namespace fallout {
namespace renderer {

//-----------------------------------------------------------------------------
// Validation Layer Callback (Debug Builds)
//-----------------------------------------------------------------------------
#ifdef _DEBUG
static VKAPI_ATTR VkBool32 VKAPI_CALL DebugCallback(
    VkDebugUtilsMessageSeverityFlagBitsEXT messageSeverity,
    VkDebugUtilsMessageTypeFlagsEXT messageType,
    const VkDebugUtilsMessengerCallbackDataEXT* pCallbackData,
    void* pUserData) {
    
    if (messageSeverity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) {
        Logger::Log(LogLevel::Warning, "Vulkan: %s", pCallbackData->pMessage);
    }
    return VK_FALSE;
}
#endif

//-----------------------------------------------------------------------------
// Constructor/Destructor
//-----------------------------------------------------------------------------

VulkanContext::VulkanContext(SDL_Window* window, int screenWidth, int screenHeight, bool enableHDR)
    : mWindow(window)
    , mScreenWidth(screenWidth)
    , mScreenHeight(screenHeight)
    , mEnableHDR(enableHDR) {
}

VulkanContext::~VulkanContext() {
    Shutdown();
}

//-----------------------------------------------------------------------------
// GpuContext Interface Implementation
//-----------------------------------------------------------------------------

bool VulkanContext::Init() {
    Logger::Log(LogLevel::Info, "VulkanContext: Initializing Vulkan context");

    if (!CreateInstance()) {
        Logger::Log(LogLevel::Error, "VulkanContext: Failed to create Vulkan instance");
        return false;
    }

    if (!CreateSurface()) {
        Logger::Log(LogLevel::Error, "VulkanContext: Failed to create Vulkan surface");
        return false;
    }

    if (!SelectPhysicalDevice()) {
        Logger::Log(LogLevel::Error, "VulkanContext: Failed to select physical device");
        return false;
    }

    if (!CreateLogicalDevice()) {
        Logger::Log(LogLevel::Error, "VulkanContext: Failed to create logical device");
        return false;
    }

    if (!CreateSwapchain()) {
        Logger::Log(LogLevel::Error, "VulkanContext: Failed to create swapchain");
        return false;
    }

    if (!CreateCommandPool()) {
        Logger::Log(LogLevel::Error, "VulkanContext: Failed to create command pool");
        return false;
    }

    if (!CreateDescriptorPool()) {
        Logger::Log(LogLevel::Error, "VulkanContext: Failed to create descriptor pool");
        return false;
    }

    if (!CreateSyncObjects()) {
        Logger::Log(LogLevel::Error, "VulkanContext: Failed to create sync objects");
        return false;
    }

    Logger::Log(LogLevel::Info, "VulkanContext: Initialization complete, HDR mode: %s",
                mHDRMode == HDRMode::HDR10 ? "HDR10" : 
                (mHDRMode == HDRMode::scRGB ? "scRGB" : "SDR"));
    return true;
}

void VulkanContext::Shutdown() {
    if (mDevice == VK_NULL_HANDLE) return;

    vkDeviceWaitIdle(mDevice);

    // Cleanup sync objects
    for (size_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
        if (mRenderFinishedSemaphores.size() > i && mRenderFinishedSemaphores[i] != VK_NULL_HANDLE) {
            vkDestroySemaphore(mDevice, mRenderFinishedSemaphores[i], nullptr);
        }
        if (mImageAvailableSemaphores.size() > i && mImageAvailableSemaphores[i] != VK_NULL_HANDLE) {
            vkDestroySemaphore(mDevice, mImageAvailableSemaphores[i], nullptr);
        }
        if (mInFlightFences.size() > i && mInFlightFences[i] != VK_NULL_HANDLE) {
            vkDestroyFence(mDevice, mInFlightFences[i], nullptr);
        }
    }

    // Cleanup uniform buffer
    if (mUniformBuffer != VK_NULL_HANDLE) {
        vkDestroyBuffer(mDevice, mUniformBuffer, nullptr);
    }
    if (mUniformBufferMemory != VK_NULL_HANDLE) {
        vkFreeMemory(mDevice, mUniformBufferMemory, nullptr);
    }

    // Cleanup staging buffer
    if (mStagingBuffer != VK_NULL_HANDLE) {
        vkDestroyBuffer(mDevice, mStagingBuffer, nullptr);
    }
    if (mStagingBufferMemory != VK_NULL_HANDLE) {
        vkFreeMemory(mDevice, mStagingBufferMemory, nullptr);
    }

    // Cleanup descriptor pool
    if (mDescriptorPool != VK_NULL_HANDLE) {
        vkDestroyDescriptorPool(mDevice, mDescriptorPool, nullptr);
    }

    // Cleanup command pool
    if (mCommandPool != VK_NULL_HANDLE) {
        vkDestroyCommandPool(mDevice, mCommandPool, nullptr);
    }

    // Cleanup swapchain
    CleanupSwapchain();

    // Cleanup device
    vkDestroyDevice(mDevice, nullptr);
    mDevice = VK_NULL_HANDLE;

    // Cleanup surface
    if (mSurface != VK_NULL_HANDLE) {
        vkDestroySurfaceKHR(mInstance, mSurface, nullptr);
        mSurface = VK_NULL_HANDLE;
    }

#ifdef _DEBUG
    // Cleanup debug messenger
    if (mDebugMessenger != VK_NULL_HANDLE) {
        auto func = (PFN_vkDestroyDebugUtilsMessengerEXT)vkGetInstanceProcAddr(
            mInstance, "vkDestroyDebugUtilsMessengerEXT");
        if (func != nullptr) {
            func(mInstance, mDebugMessenger, nullptr);
        }
    }
#endif

    // Cleanup instance
    if (mInstance != VK_NULL_HANDLE) {
        vkDestroyInstance(mInstance, nullptr);
        mInstance = VK_NULL_HANDLE;
    }

    Logger::Log(LogLevel::Info, "VulkanContext: Shutdown complete");
}

//-----------------------------------------------------------------------------
// Initialization Helpers
//-----------------------------------------------------------------------------

bool VulkanContext::CreateInstance() {
    VkApplicationInfo appInfo{};
    appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    appInfo.pApplicationName = "Fallout 2 CE";
    appInfo.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
    appInfo.pEngineName = "Fallout2CE Renderer";
    appInfo.engineVersion = VK_MAKE_VERSION(1, 0, 0);
    appInfo.apiVersion = VK_API_VERSION_1_2;

    // Get required extensions from SDL
    unsigned int sdlExtensionCount = 0;
    if (!SDL_Vulkan_GetInstanceExtensions(mWindow, &sdlExtensionCount, nullptr)) {
        Logger::Log(LogLevel::Error, "VulkanContext: Failed to get SDL Vulkan extension count");
        return false;
    }

    std::vector<const char*> extensions(sdlExtensionCount);
    if (!SDL_Vulkan_GetInstanceExtensions(mWindow, &sdlExtensionCount, extensions.data())) {
        Logger::Log(LogLevel::Error, "VulkanContext: Failed to get SDL Vulkan extensions");
        return false;
    }

    // Add swapchain colorspace extension for HDR
    extensions.push_back(VK_EXT_SWAPCHAIN_COLOR_SPACE_EXTENSION_NAME);

#ifdef _DEBUG
    extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
#endif

    VkInstanceCreateInfo createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    createInfo.pApplicationInfo = &appInfo;
    createInfo.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
    createInfo.ppEnabledExtensionNames = extensions.data();

#ifdef _DEBUG
    const char* validationLayers[] = { "VK_LAYER_KHRONOS_validation" };
    createInfo.enabledLayerCount = 1;
    createInfo.ppEnabledLayerNames = validationLayers;
#else
    createInfo.enabledLayerCount = 0;
#endif

    VkResult result = vkCreateInstance(&createInfo, nullptr, &mInstance);
    if (result != VK_SUCCESS) {
        Logger::Log(LogLevel::Error, "VulkanContext: vkCreateInstance failed: %d", result);
        return false;
    }

#ifdef _DEBUG
    // Setup debug messenger
    VkDebugUtilsMessengerCreateInfoEXT debugCreateInfo{};
    debugCreateInfo.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
    debugCreateInfo.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT |
                                      VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                                      VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
    debugCreateInfo.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                                  VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                                  VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
    debugCreateInfo.pfnUserCallback = DebugCallback;

    auto func = (PFN_vkCreateDebugUtilsMessengerEXT)vkGetInstanceProcAddr(
        mInstance, "vkCreateDebugUtilsMessengerEXT");
    if (func != nullptr) {
        func(mInstance, &debugCreateInfo, nullptr, &mDebugMessenger);
    }
#endif

    Logger::Log(LogLevel::Info, "VulkanContext: Instance created successfully");
    return true;
}

bool VulkanContext::CreateSurface() {
    if (!SDL_Vulkan_CreateSurface(mWindow, mInstance, &mSurface)) {
        Logger::Log(LogLevel::Error, "VulkanContext: SDL_Vulkan_CreateSurface failed: %s", SDL_GetError());
        return false;
    }
    Logger::Log(LogLevel::Info, "VulkanContext: Surface created");
    return true;
}

bool VulkanContext::SelectPhysicalDevice() {
    uint32_t deviceCount = 0;
    vkEnumeratePhysicalDevices(mInstance, &deviceCount, nullptr);
    
    if (deviceCount == 0) {
        Logger::Log(LogLevel::Error, "VulkanContext: No Vulkan-capable devices found");
        return false;
    }

    std::vector<VkPhysicalDevice> devices(deviceCount);
    vkEnumeratePhysicalDevices(mInstance, &deviceCount, devices.data());

    // Find a suitable device with graphics, compute, and present support
    for (const auto& device : devices) {
        VkPhysicalDeviceProperties deviceProperties;
        vkGetPhysicalDeviceProperties(device, &deviceProperties);

        // Get queue families
        uint32_t queueFamilyCount = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(device, &queueFamilyCount, nullptr);
        std::vector<VkQueueFamilyProperties> queueFamilies(queueFamilyCount);
        vkGetPhysicalDeviceQueueFamilyProperties(device, &queueFamilyCount, queueFamilies.data());

        bool foundGraphics = false;
        bool foundCompute = false;
        bool foundPresent = false;

        for (uint32_t i = 0; i < queueFamilyCount; i++) {
            if (queueFamilies[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
                mGraphicsQueueFamily = i;
                foundGraphics = true;
            }
            if (queueFamilies[i].queueFlags & VK_QUEUE_COMPUTE_BIT) {
                mComputeQueueFamily = i;
                foundCompute = true;
            }
            VkBool32 presentSupport = false;
            vkGetPhysicalDeviceSurfaceSupportKHR(device, i, mSurface, &presentSupport);
            if (presentSupport) {
                mPresentQueueFamily = i;
                foundPresent = true;
            }
        }

        if (foundGraphics && foundCompute && foundPresent) {
            mPhysicalDevice = device;
            Logger::Log(LogLevel::Info, "VulkanContext: Selected device: %s", deviceProperties.deviceName);
            return true;
        }
    }

    Logger::Log(LogLevel::Error, "VulkanContext: No suitable device found");
    return false;
}

bool VulkanContext::CreateLogicalDevice() {
    std::set<uint32_t> uniqueQueueFamilies = {
        mGraphicsQueueFamily, mComputeQueueFamily, mPresentQueueFamily
    };

    std::vector<VkDeviceQueueCreateInfo> queueCreateInfos;
    float queuePriority = 1.0f;

    for (uint32_t queueFamily : uniqueQueueFamilies) {
        VkDeviceQueueCreateInfo queueCreateInfo{};
        queueCreateInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        queueCreateInfo.queueFamilyIndex = queueFamily;
        queueCreateInfo.queueCount = 1;
        queueCreateInfo.pQueuePriorities = &queuePriority;
        queueCreateInfos.push_back(queueCreateInfo);
    }

    VkPhysicalDeviceFeatures deviceFeatures{};
    // We don't need any special features for basic compute

    std::vector<const char*> deviceExtensions = {
        VK_KHR_SWAPCHAIN_EXTENSION_NAME
    };

    VkDeviceCreateInfo createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    createInfo.queueCreateInfoCount = static_cast<uint32_t>(queueCreateInfos.size());
    createInfo.pQueueCreateInfos = queueCreateInfos.data();
    createInfo.pEnabledFeatures = &deviceFeatures;
    createInfo.enabledExtensionCount = static_cast<uint32_t>(deviceExtensions.size());
    createInfo.ppEnabledExtensionNames = deviceExtensions.data();

    VkResult result = vkCreateDevice(mPhysicalDevice, &createInfo, nullptr, &mDevice);
    if (result != VK_SUCCESS) {
        Logger::Log(LogLevel::Error, "VulkanContext: vkCreateDevice failed: %d", result);
        return false;
    }

    vkGetDeviceQueue(mDevice, mGraphicsQueueFamily, 0, &mGraphicsQueue);
    vkGetDeviceQueue(mDevice, mComputeQueueFamily, 0, &mComputeQueue);
    vkGetDeviceQueue(mDevice, mPresentQueueFamily, 0, &mPresentQueue);

    Logger::Log(LogLevel::Info, "VulkanContext: Logical device created");
    return true;
}

VkSurfaceFormatKHR VulkanContext::ChooseSurfaceFormat(const std::vector<VkSurfaceFormatKHR>& formats) {
    // If HDR is enabled, try to find an HDR-capable format
    // NOTE: HDR10 with ST2084 color space requires PQ encoding of content.
    //       Since we're rendering SDR content (8-bit palette), we skip HDR10 for now
    //       and prefer higher-bit-depth SDR formats instead.
    if (mEnableHDR) {
        // DISABLED: HDR10 requires PQ encoding which we don't have yet
        // Sending sRGB content to HDR10 ST2084 makes everything appear very dark
        /*
        for (const auto& format : formats) {
            if (format.format == VK_FORMAT_A2B10G10R10_UNORM_PACK32 &&
                format.colorSpace == VK_COLOR_SPACE_HDR10_ST2084_EXT) {
                mHDRMode = HDRMode::HDR10;
                Logger::Log(LogLevel::Info, "VulkanContext: Using HDR10 color space");
                return format;
            }
        }
        */

        // DISABLED: scRGB also needs proper linear light handling
        /*
        for (const auto& format : formats) {
            if (format.format == VK_FORMAT_R16G16B16A16_SFLOAT &&
                format.colorSpace == VK_COLOR_SPACE_EXTENDED_SRGB_LINEAR_EXT) {
                mHDRMode = HDRMode::scRGB;
                Logger::Log(LogLevel::Info, "VulkanContext: Using scRGB (extended sRGB linear) color space");
                return format;
            }
        }
        */

        // Try 10-bit sRGB (higher bit depth but standard sRGB gamma curve)
        for (const auto& format : formats) {
            if (format.format == VK_FORMAT_A2B10G10R10_UNORM_PACK32 &&
                format.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
                Logger::Log(LogLevel::Info, "VulkanContext: Using 10-bit sRGB (SDR)");
                mHDRMode = HDRMode::SDR;
                return format;
            }
        }
    }

    // Fall back to standard sRGB
    for (const auto& format : formats) {
        if (format.format == VK_FORMAT_B8G8R8A8_SRGB &&
            format.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
            mHDRMode = HDRMode::SDR;
            return format;
        }
    }

    // Just use the first available format
    mHDRMode = HDRMode::SDR;
    return formats[0];
}

VkPresentModeKHR VulkanContext::ChoosePresentMode(const std::vector<VkPresentModeKHR>& modes) {
    // Prefer mailbox (triple buffering) for low latency
    for (const auto& mode : modes) {
        if (mode == VK_PRESENT_MODE_MAILBOX_KHR) {
            return mode;
        }
    }
    // Fallback to FIFO (vsync)
    return VK_PRESENT_MODE_FIFO_KHR;
}

VkExtent2D VulkanContext::ChooseSwapExtent(const VkSurfaceCapabilitiesKHR& capabilities) {
    if (capabilities.currentExtent.width != UINT32_MAX) {
        return capabilities.currentExtent;
    }
    
    VkExtent2D actualExtent = {
        static_cast<uint32_t>(mScreenWidth),
        static_cast<uint32_t>(mScreenHeight)
    };

    actualExtent.width = std::max(capabilities.minImageExtent.width,
                                  std::min(capabilities.maxImageExtent.width, actualExtent.width));
    actualExtent.height = std::max(capabilities.minImageExtent.height,
                                   std::min(capabilities.maxImageExtent.height, actualExtent.height));

    return actualExtent;
}

bool VulkanContext::CreateSwapchain() {
    // Get surface capabilities
    VkSurfaceCapabilitiesKHR capabilities;
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(mPhysicalDevice, mSurface, &capabilities);

    // Get surface formats
    uint32_t formatCount;
    vkGetPhysicalDeviceSurfaceFormatsKHR(mPhysicalDevice, mSurface, &formatCount, nullptr);
    std::vector<VkSurfaceFormatKHR> formats(formatCount);
    vkGetPhysicalDeviceSurfaceFormatsKHR(mPhysicalDevice, mSurface, &formatCount, formats.data());

    // Get present modes
    uint32_t presentModeCount;
    vkGetPhysicalDeviceSurfacePresentModesKHR(mPhysicalDevice, mSurface, &presentModeCount, nullptr);
    std::vector<VkPresentModeKHR> presentModes(presentModeCount);
    vkGetPhysicalDeviceSurfacePresentModesKHR(mPhysicalDevice, mSurface, &presentModeCount, presentModes.data());

    // Log available formats for debugging
    Logger::Log(LogLevel::Info, "VulkanContext: Available surface formats:");
    for (const auto& format : formats) {
        Logger::Log(LogLevel::Info, "  Format: %d, ColorSpace: %d", format.format, format.colorSpace);
    }

    VkSurfaceFormatKHR surfaceFormat = ChooseSurfaceFormat(formats);
    VkPresentModeKHR presentMode = ChoosePresentMode(presentModes);
    VkExtent2D extent = ChooseSwapExtent(capabilities);

    uint32_t imageCount = capabilities.minImageCount + 1;
    if (capabilities.maxImageCount > 0 && imageCount > capabilities.maxImageCount) {
        imageCount = capabilities.maxImageCount;
    }

    VkSwapchainCreateInfoKHR createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    createInfo.surface = mSurface;
    createInfo.minImageCount = imageCount;
    createInfo.imageFormat = surfaceFormat.format;
    createInfo.imageColorSpace = surfaceFormat.colorSpace;
    createInfo.imageExtent = extent;
    createInfo.imageArrayLayers = 1;
    createInfo.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;

    uint32_t queueFamilyIndices[] = { mGraphicsQueueFamily, mPresentQueueFamily };
    if (mGraphicsQueueFamily != mPresentQueueFamily) {
        createInfo.imageSharingMode = VK_SHARING_MODE_CONCURRENT;
        createInfo.queueFamilyIndexCount = 2;
        createInfo.pQueueFamilyIndices = queueFamilyIndices;
    } else {
        createInfo.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    }

    createInfo.preTransform = capabilities.currentTransform;
    createInfo.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    createInfo.presentMode = presentMode;
    createInfo.clipped = VK_TRUE;
    createInfo.oldSwapchain = VK_NULL_HANDLE;

    VkResult result = vkCreateSwapchainKHR(mDevice, &createInfo, nullptr, &mSwapchain);
    if (result != VK_SUCCESS) {
        Logger::Log(LogLevel::Error, "VulkanContext: vkCreateSwapchainKHR failed: %d", result);
        return false;
    }

    mSwapchainFormat = surfaceFormat.format;
    mSwapchainColorSpace = surfaceFormat.colorSpace;
    mSwapchainExtent = extent;

    // Get swapchain images
    vkGetSwapchainImagesKHR(mDevice, mSwapchain, &imageCount, nullptr);
    mSwapchainImages.resize(imageCount);
    vkGetSwapchainImagesKHR(mDevice, mSwapchain, &imageCount, mSwapchainImages.data());

    // Track swapchain image layouts (first use is UNDEFINED)
    mSwapchainImageLayouts.assign(imageCount, VK_IMAGE_LAYOUT_UNDEFINED);

    // Create image views
    mSwapchainImageViews.resize(imageCount);
    for (size_t i = 0; i < imageCount; i++) {
        VkImageViewCreateInfo viewInfo{};
        viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image = mSwapchainImages[i];
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = mSwapchainFormat;
        viewInfo.components.r = VK_COMPONENT_SWIZZLE_IDENTITY;
        viewInfo.components.g = VK_COMPONENT_SWIZZLE_IDENTITY;
        viewInfo.components.b = VK_COMPONENT_SWIZZLE_IDENTITY;
        viewInfo.components.a = VK_COMPONENT_SWIZZLE_IDENTITY;
        viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        viewInfo.subresourceRange.baseMipLevel = 0;
        viewInfo.subresourceRange.levelCount = 1;
        viewInfo.subresourceRange.baseArrayLayer = 0;
        viewInfo.subresourceRange.layerCount = 1;

        result = vkCreateImageView(mDevice, &viewInfo, nullptr, &mSwapchainImageViews[i]);
        if (result != VK_SUCCESS) {
            Logger::Log(LogLevel::Error, "VulkanContext: Failed to create image view: %d", result);
            return false;
        }
    }

    Logger::Log(LogLevel::Info, "VulkanContext: Swapchain created (%dx%d, format: %d, colorSpace: %d)",
                extent.width, extent.height, mSwapchainFormat, mSwapchainColorSpace);
    return true;
}

void VulkanContext::CleanupSwapchain() {
    for (auto imageView : mSwapchainImageViews) {
        vkDestroyImageView(mDevice, imageView, nullptr);
    }
    mSwapchainImageViews.clear();

    if (mSwapchain != VK_NULL_HANDLE) {
        vkDestroySwapchainKHR(mDevice, mSwapchain, nullptr);
        mSwapchain = VK_NULL_HANDLE;
    }
}

bool VulkanContext::CreateCommandPool() {
    VkCommandPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    poolInfo.queueFamilyIndex = mGraphicsQueueFamily;

    VkResult result = vkCreateCommandPool(mDevice, &poolInfo, nullptr, &mCommandPool);
    if (result != VK_SUCCESS) {
        Logger::Log(LogLevel::Error, "VulkanContext: Failed to create command pool: %d", result);
        return false;
    }

    // Allocate command buffers
    mCommandBuffers.resize(MAX_FRAMES_IN_FLIGHT);
    VkCommandBufferAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocInfo.commandPool = mCommandPool;
    allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocInfo.commandBufferCount = static_cast<uint32_t>(mCommandBuffers.size());

    result = vkAllocateCommandBuffers(mDevice, &allocInfo, mCommandBuffers.data());
    if (result != VK_SUCCESS) {
        Logger::Log(LogLevel::Error, "VulkanContext: Failed to allocate command buffers: %d", result);
        return false;
    }

    return true;
}

bool VulkanContext::CreateDescriptorPool() {
    std::array<VkDescriptorPoolSize, 3> poolSizes{};
    poolSizes[0].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    poolSizes[0].descriptorCount = 100;
    poolSizes[1].type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    poolSizes[1].descriptorCount = 100;
    poolSizes[2].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    poolSizes[2].descriptorCount = 100;

    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.poolSizeCount = static_cast<uint32_t>(poolSizes.size());
    poolInfo.pPoolSizes = poolSizes.data();
    poolInfo.maxSets = 100;
    poolInfo.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;

    VkResult result = vkCreateDescriptorPool(mDevice, &poolInfo, nullptr, &mDescriptorPool);
    if (result != VK_SUCCESS) {
        Logger::Log(LogLevel::Error, "VulkanContext: Failed to create descriptor pool: %d", result);
        return false;
    }

    return true;
}

bool VulkanContext::CreateSyncObjects() {
    mImageAvailableSemaphores.resize(MAX_FRAMES_IN_FLIGHT);
    mRenderFinishedSemaphores.resize(MAX_FRAMES_IN_FLIGHT);
    mInFlightFences.resize(MAX_FRAMES_IN_FLIGHT);

    VkSemaphoreCreateInfo semaphoreInfo{};
    semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;

    VkFenceCreateInfo fenceInfo{};
    fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;

    for (size_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
        if (vkCreateSemaphore(mDevice, &semaphoreInfo, nullptr, &mImageAvailableSemaphores[i]) != VK_SUCCESS ||
            vkCreateSemaphore(mDevice, &semaphoreInfo, nullptr, &mRenderFinishedSemaphores[i]) != VK_SUCCESS ||
            vkCreateFence(mDevice, &fenceInfo, nullptr, &mInFlightFences[i]) != VK_SUCCESS) {
            Logger::Log(LogLevel::Error, "VulkanContext: Failed to create sync objects");
            return false;
        }
    }

    return true;
}

//-----------------------------------------------------------------------------
// Texture Management
//-----------------------------------------------------------------------------

VkFormat VulkanContext::GetVulkanFormat(TextureFormat format) const {
    switch (format) {
        case TextureFormat::RGBA8:
            return VK_FORMAT_R8G8B8A8_UNORM;
        case TextureFormat::RGBA16F:
            return VK_FORMAT_R16G16B16A16_SFLOAT;
        case TextureFormat::R8_UNORM:
            return VK_FORMAT_R8_UNORM;
        default:
            return VK_FORMAT_R8G8B8A8_UNORM;
    }
}

uint32_t VulkanContext::FindMemoryType(uint32_t typeFilter, VkMemoryPropertyFlags properties) const {
    VkPhysicalDeviceMemoryProperties memProperties;
    vkGetPhysicalDeviceMemoryProperties(mPhysicalDevice, &memProperties);

    for (uint32_t i = 0; i < memProperties.memoryTypeCount; i++) {
        if ((typeFilter & (1 << i)) && 
            (memProperties.memoryTypes[i].propertyFlags & properties) == properties) {
            return i;
        }
    }

    Logger::Log(LogLevel::Error, "VulkanContext: Failed to find suitable memory type");
    return 0;
}

void* VulkanContext::CreateTexture(const TextureDesc& desc, const void* initialData) {
    VulkanTexture* texture = new VulkanTexture();
    texture->width = desc.width;
    texture->height = desc.height;
    texture->format = GetVulkanFormat(desc.format);
    texture->isStorageImage = desc.isStorage;
    texture->layout = VK_IMAGE_LAYOUT_UNDEFINED;

    // Create image
    VkImageCreateInfo imageInfo{};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.extent.width = desc.width;
    imageInfo.extent.height = desc.height;
    imageInfo.extent.depth = 1;
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.format = texture->format;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    // Always include TRANSFER_SRC for blitting to swapchain
    imageInfo.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    if (desc.isStorage) {
        imageInfo.usage |= VK_IMAGE_USAGE_STORAGE_BIT;
    }
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    if (vkCreateImage(mDevice, &imageInfo, nullptr, &texture->image) != VK_SUCCESS) {
        Logger::Log(LogLevel::Error, "VulkanContext: Failed to create image");
        delete texture;
        return nullptr;
    }

    // Allocate memory
    VkMemoryRequirements memRequirements;
    vkGetImageMemoryRequirements(mDevice, texture->image, &memRequirements);

    VkMemoryAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize = memRequirements.size;
    allocInfo.memoryTypeIndex = FindMemoryType(memRequirements.memoryTypeBits, 
                                                VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

    if (vkAllocateMemory(mDevice, &allocInfo, nullptr, &texture->memory) != VK_SUCCESS) {
        Logger::Log(LogLevel::Error, "VulkanContext: Failed to allocate image memory");
        vkDestroyImage(mDevice, texture->image, nullptr);
        delete texture;
        return nullptr;
    }

    vkBindImageMemory(mDevice, texture->image, texture->memory, 0);

    // Create image view
    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = texture->image;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = texture->format;
    viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    viewInfo.subresourceRange.baseMipLevel = 0;
    viewInfo.subresourceRange.levelCount = 1;
    viewInfo.subresourceRange.baseArrayLayer = 0;
    viewInfo.subresourceRange.layerCount = 1;

    if (vkCreateImageView(mDevice, &viewInfo, nullptr, &texture->imageView) != VK_SUCCESS) {
        Logger::Log(LogLevel::Error, "VulkanContext: Failed to create image view");
        vkFreeMemory(mDevice, texture->memory, nullptr);
        vkDestroyImage(mDevice, texture->image, nullptr);
        delete texture;
        return nullptr;
    }

    // Create sampler
    VkSamplerCreateInfo samplerInfo{};
    samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    samplerInfo.magFilter = VK_FILTER_NEAREST;
    samplerInfo.minFilter = VK_FILTER_NEAREST;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.anisotropyEnable = VK_FALSE;
    samplerInfo.borderColor = VK_BORDER_COLOR_INT_OPAQUE_BLACK;
    samplerInfo.unnormalizedCoordinates = VK_FALSE;
    samplerInfo.compareEnable = VK_FALSE;
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;

    if (vkCreateSampler(mDevice, &samplerInfo, nullptr, &texture->sampler) != VK_SUCCESS) {
        Logger::Log(LogLevel::Error, "VulkanContext: Failed to create sampler");
        vkDestroyImageView(mDevice, texture->imageView, nullptr);
        vkFreeMemory(mDevice, texture->memory, nullptr);
        vkDestroyImage(mDevice, texture->image, nullptr);
        delete texture;
        return nullptr;
    }

    // Upload initial data if provided
    if (initialData != nullptr) {
        UpdateTexture(texture, initialData, desc.width, desc.height);
    }

    return texture;
}

void VulkanContext::DestroyTexture(void* textureHandle) {
    if (textureHandle == nullptr) return;

    VulkanTexture* texture = static_cast<VulkanTexture*>(textureHandle);
    
    if (texture->sampler != VK_NULL_HANDLE) {
        vkDestroySampler(mDevice, texture->sampler, nullptr);
    }
    if (texture->imageView != VK_NULL_HANDLE) {
        vkDestroyImageView(mDevice, texture->imageView, nullptr);
    }
    if (texture->image != VK_NULL_HANDLE) {
        vkDestroyImage(mDevice, texture->image, nullptr);
    }
    if (texture->memory != VK_NULL_HANDLE) {
        vkFreeMemory(mDevice, texture->memory, nullptr);
    }

    delete texture;
}

VkCommandBuffer VulkanContext::BeginSingleTimeCommands() {
    VkCommandBufferAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocInfo.commandPool = mCommandPool;
    allocInfo.commandBufferCount = 1;

    VkCommandBuffer commandBuffer;
    vkAllocateCommandBuffers(mDevice, &allocInfo, &commandBuffer);

    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;

    vkBeginCommandBuffer(commandBuffer, &beginInfo);
    return commandBuffer;
}

void VulkanContext::EndSingleTimeCommands(VkCommandBuffer commandBuffer) {
    vkEndCommandBuffer(commandBuffer);

    VkSubmitInfo submitInfo{};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &commandBuffer;

    vkQueueSubmit(mGraphicsQueue, 1, &submitInfo, VK_NULL_HANDLE);
    vkQueueWaitIdle(mGraphicsQueue);

    vkFreeCommandBuffers(mDevice, mCommandPool, 1, &commandBuffer);
}

void VulkanContext::TransitionImageLayout(VkImage image, VkFormat format,
                                          VkImageLayout oldLayout, VkImageLayout newLayout) {
    VkCommandBuffer commandBuffer = BeginSingleTimeCommands();

    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.oldLayout = oldLayout;
    barrier.newLayout = newLayout;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.baseMipLevel = 0;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.baseArrayLayer = 0;
    barrier.subresourceRange.layerCount = 1;

    VkPipelineStageFlags sourceStage;
    VkPipelineStageFlags destinationStage;

    if (oldLayout == VK_IMAGE_LAYOUT_UNDEFINED && newLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL) {
        barrier.srcAccessMask = 0;
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        sourceStage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
        destinationStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    } else if (oldLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL &&
               newLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL) {
        barrier.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        sourceStage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
        destinationStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    } else if (oldLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL && 
               newLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        sourceStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
        destinationStage = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
    } else if (oldLayout == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL &&
               newLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        sourceStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
        destinationStage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    } else if (oldLayout == VK_IMAGE_LAYOUT_UNDEFINED && 
               newLayout == VK_IMAGE_LAYOUT_GENERAL) {
        barrier.srcAccessMask = 0;
        barrier.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        sourceStage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
        destinationStage = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
    } else {
        Logger::Log(LogLevel::Warning, "VulkanContext: Unsupported layout transition");
        sourceStage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
        destinationStage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    }

    vkCmdPipelineBarrier(commandBuffer, sourceStage, destinationStage, 0,
                         0, nullptr, 0, nullptr, 1, &barrier);

    EndSingleTimeCommands(commandBuffer);
}

void VulkanContext::UpdateTexture(void* textureHandle, const void* data, int width, int height) {
    if (textureHandle == nullptr || data == nullptr) return;

    VulkanTexture* texture = static_cast<VulkanTexture*>(textureHandle);
    VkDeviceSize imageSize = width * height * 4; // Assuming RGBA8

    // Create staging buffer if needed
    if (mStagingBuffer == VK_NULL_HANDLE || mStagingBufferSize < imageSize) {
        if (mStagingBuffer != VK_NULL_HANDLE) {
            vkDestroyBuffer(mDevice, mStagingBuffer, nullptr);
            vkFreeMemory(mDevice, mStagingBufferMemory, nullptr);
        }

        VkBufferCreateInfo bufferInfo{};
        bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bufferInfo.size = imageSize;
        bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

        vkCreateBuffer(mDevice, &bufferInfo, nullptr, &mStagingBuffer);

        VkMemoryRequirements memRequirements;
        vkGetBufferMemoryRequirements(mDevice, mStagingBuffer, &memRequirements);

        VkMemoryAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocInfo.allocationSize = memRequirements.size;
        allocInfo.memoryTypeIndex = FindMemoryType(memRequirements.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

        vkAllocateMemory(mDevice, &allocInfo, nullptr, &mStagingBufferMemory);
        vkBindBufferMemory(mDevice, mStagingBuffer, mStagingBufferMemory, 0);
        vkMapMemory(mDevice, mStagingBufferMemory, 0, imageSize, 0, &mStagingBufferMapped);
        mStagingBufferSize = imageSize;
    }

    // Copy data to staging buffer
    memcpy(mStagingBufferMapped, data, imageSize);
    Logger::Log(LogLevel::Info, "[VK] UpdateTexture: handle=%p, data=%p, dims=%dx%d, imageSize=%llu", textureHandle, data, width, height, (unsigned long long)imageSize);
    // Dump first 4 pixels for debugging
    const uint32_t* px = static_cast<const uint32_t*>(data);
    Logger::Log(LogLevel::Info, "[VK] UpdateTexture: first4=0x%08X 0x%08X 0x%08X 0x%08X", px[0], px[1], px[2], px[3]);
    int nonBlackSample = 0;
    const int sampleCount = std::min(1024, width * height);
    for (int i = 0; i < sampleCount; i++) {
        if ((px[i] & 0x00FFFFFF) != 0) {
            nonBlackSample++;
        }
    }
    Logger::Log(LogLevel::Info, "[VK] UpdateTexture: nonBlackSample=%d/%d (RGB != 0)", nonBlackSample, sampleCount);

    // Transition image layout and copy
    const VkImageLayout oldLayout = texture->layout;
    TransitionImageLayout(texture->image, texture->format,
                          oldLayout, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    texture->layout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;

    VkCommandBuffer commandBuffer = BeginSingleTimeCommands();

    VkBufferImageCopy region{};
    region.bufferOffset = 0;
    region.bufferRowLength = 0;
    region.bufferImageHeight = 0;
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.mipLevel = 0;
    region.imageSubresource.baseArrayLayer = 0;
    region.imageSubresource.layerCount = 1;
    region.imageOffset = {0, 0, 0};
    region.imageExtent = {static_cast<uint32_t>(width), static_cast<uint32_t>(height), 1};

    vkCmdCopyBufferToImage(commandBuffer, mStagingBuffer, texture->image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

    EndSingleTimeCommands(commandBuffer);

    TransitionImageLayout(texture->image, texture->format,
                          VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    texture->layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
}

void VulkanContext::ReadbackTexture(void* textureHandle, void* data, int size) {
    // TODO: Implement readback for screenshots
    Logger::Log(LogLevel::Warning, "VulkanContext: ReadbackTexture not yet implemented");
}

//-----------------------------------------------------------------------------
// Compute Shader Operations
//-----------------------------------------------------------------------------

bool VulkanContext::CreateComputeShader(const std::string& source, void** outShader) {
    *outShader = nullptr;
    
    // Compile GLSL to SPIR-V
    std::vector<uint32_t> spirvCode;
    if (!CompileGLSLToSPIRV(source, spirvCode)) {
        return false;
    }
    
    // Create shader module
    VkShaderModuleCreateInfo moduleInfo{};
    moduleInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    moduleInfo.codeSize = spirvCode.size() * sizeof(uint32_t);
    moduleInfo.pCode = spirvCode.data();
    
    VulkanPipeline* pipeline = new VulkanPipeline();
    
    VkResult result = vkCreateShaderModule(mDevice, &moduleInfo, nullptr, &pipeline->shaderModule);
    if (result != VK_SUCCESS) {
        Logger::Log(LogLevel::Error, "VulkanContext: Failed to create shader module: %d", result);
        delete pipeline;
        return false;
    }
    
    // Create descriptor set layout
    // Binding 0: Combined image sampler (input texture)
    // Binding 1: Storage image (output texture)
    // Binding 2: Uniform buffer (constants)
    std::array<VkDescriptorSetLayoutBinding, 3> bindings{};
    
    bindings[0].binding = 0;
    bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[0].descriptorCount = 1;
    bindings[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    
    bindings[1].binding = 1;
    bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    bindings[1].descriptorCount = 1;
    bindings[1].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    
    bindings[2].binding = 0;  // UBO binding in set 0
    bindings[2].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    bindings[2].descriptorCount = 1;
    bindings[2].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    
    VkDescriptorSetLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = static_cast<uint32_t>(bindings.size());
    layoutInfo.pBindings = bindings.data();
    
    result = vkCreateDescriptorSetLayout(mDevice, &layoutInfo, nullptr, &pipeline->descriptorSetLayout);
    if (result != VK_SUCCESS) {
        Logger::Log(LogLevel::Error, "VulkanContext: Failed to create descriptor set layout: %d", result);
        vkDestroyShaderModule(mDevice, pipeline->shaderModule, nullptr);
        delete pipeline;
        return false;
    }
    
    // Create pipeline layout
    VkPipelineLayoutCreateInfo pipelineLayoutInfo{};
    pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pipelineLayoutInfo.setLayoutCount = 1;
    pipelineLayoutInfo.pSetLayouts = &pipeline->descriptorSetLayout;
    
    result = vkCreatePipelineLayout(mDevice, &pipelineLayoutInfo, nullptr, &pipeline->layout);
    if (result != VK_SUCCESS) {
        Logger::Log(LogLevel::Error, "VulkanContext: Failed to create pipeline layout: %d", result);
        vkDestroyDescriptorSetLayout(mDevice, pipeline->descriptorSetLayout, nullptr);
        vkDestroyShaderModule(mDevice, pipeline->shaderModule, nullptr);
        delete pipeline;
        return false;
    }
    
    // Create compute pipeline
    VkComputePipelineCreateInfo pipelineInfo{};
    pipelineInfo.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    pipelineInfo.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    pipelineInfo.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    pipelineInfo.stage.module = pipeline->shaderModule;
    pipelineInfo.stage.pName = "main";
    pipelineInfo.layout = pipeline->layout;
    
    result = vkCreateComputePipelines(mDevice, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &pipeline->pipeline);
    if (result != VK_SUCCESS) {
        Logger::Log(LogLevel::Error, "VulkanContext: Failed to create compute pipeline: %d", result);
        vkDestroyPipelineLayout(mDevice, pipeline->layout, nullptr);
        vkDestroyDescriptorSetLayout(mDevice, pipeline->descriptorSetLayout, nullptr);
        vkDestroyShaderModule(mDevice, pipeline->shaderModule, nullptr);
        delete pipeline;
        return false;
    }
    
    Logger::Log(LogLevel::Info, "VulkanContext: Compute pipeline created successfully");
    *outShader = pipeline;
    return true;
}

void VulkanContext::Dispatch(void* shader, int x, int y, int z) {
    if (shader == nullptr) return;
    
    VulkanPipeline* pipeline = static_cast<VulkanPipeline*>(shader);
    VkCommandBuffer cmd = mCommandBuffers[mCurrentFrame];
    
    // Allocate descriptor set for this dispatch
    VkDescriptorSetAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocInfo.descriptorPool = mDescriptorPool;
    allocInfo.descriptorSetCount = 1;
    allocInfo.pSetLayouts = &pipeline->descriptorSetLayout;
    
    VkDescriptorSet descriptorSet;
    VkResult result = vkAllocateDescriptorSets(mDevice, &allocInfo, &descriptorSet);
    if (result != VK_SUCCESS) {
        Logger::Log(LogLevel::Error, "VulkanContext: Failed to allocate descriptor set: %d", result);
        return;
    }
    
    // Update descriptor set with current bindings
    std::vector<VkWriteDescriptorSet> descriptorWrites;
    std::vector<VkDescriptorImageInfo> imageInfos;
    VkDescriptorBufferInfo bufferInfo{};
    
    // Input texture (binding 0)
    if (mBindings.textures[0] != nullptr) {
        VulkanTexture* inputTex = static_cast<VulkanTexture*>(mBindings.textures[0]);
        VkDescriptorImageInfo imageInfo{};
        imageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        imageInfo.imageView = inputTex->imageView;
        imageInfo.sampler = inputTex->sampler;
        imageInfos.push_back(imageInfo);
        
        VkWriteDescriptorSet write{};
        write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet = descriptorSet;
        write.dstBinding = 0;
        write.dstArrayElement = 0;
        write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        write.descriptorCount = 1;
        write.pImageInfo = &imageInfos.back();
        descriptorWrites.push_back(write);
    }
    
    // Output storage image (binding 1)
    if (mBindings.storageImages[0] != nullptr) {
        VulkanTexture* outputTex = static_cast<VulkanTexture*>(mBindings.storageImages[0]);
        VkDescriptorImageInfo imageInfo{};
        imageInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        imageInfo.imageView = outputTex->imageView;
        imageInfo.sampler = VK_NULL_HANDLE;
        imageInfos.push_back(imageInfo);
        
        VkWriteDescriptorSet write{};
        write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet = descriptorSet;
        write.dstBinding = 1;
        write.dstArrayElement = 0;
        write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        write.descriptorCount = 1;
        write.pImageInfo = &imageInfos.back();
        descriptorWrites.push_back(write);
    }
    
    // Uniform buffer (binding 2)
    if (mUniformBuffer != VK_NULL_HANDLE) {
        bufferInfo.buffer = mUniformBuffer;
        bufferInfo.offset = 0;
        bufferInfo.range = VK_WHOLE_SIZE;
        
        VkWriteDescriptorSet write{};
        write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet = descriptorSet;
        write.dstBinding = 2;
        write.dstArrayElement = 0;
        write.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        write.descriptorCount = 1;
        write.pBufferInfo = &bufferInfo;
        descriptorWrites.push_back(write);
    }
    
    if (!descriptorWrites.empty()) {
        vkUpdateDescriptorSets(mDevice, static_cast<uint32_t>(descriptorWrites.size()), 
                               descriptorWrites.data(), 0, nullptr);
    }
    
    // Bind pipeline and dispatch
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline->pipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline->layout, 
                            0, 1, &descriptorSet, 0, nullptr);
    vkCmdDispatch(cmd, x, y, z);
    
    // Add memory barrier for compute shader writes
    VkMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT;
    
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
                         0, 1, &barrier, 0, nullptr, 0, nullptr);
}

void VulkanContext::BindTexture(int slot, void* textureHandle) {
    if (slot < 16) {
        mBindings.textures[slot] = textureHandle;
    }
}

void VulkanContext::BindUnorderedAccessView(int slot, void* textureHandle, TextureFormat format) {
    if (slot < 8) {
        mBindings.storageImages[slot] = textureHandle;
    }
}

void VulkanContext::SetConstants(int slot, const void* data, int size) {
    if (data == nullptr || size <= 0) return;
    
    // Create or resize uniform buffer if needed
    if (mUniformBuffer == VK_NULL_HANDLE) {
        VkBufferCreateInfo bufferInfo{};
        bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bufferInfo.size = 256;  // Fixed size for constants
        bufferInfo.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
        bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        
        if (vkCreateBuffer(mDevice, &bufferInfo, nullptr, &mUniformBuffer) != VK_SUCCESS) {
            Logger::Log(LogLevel::Error, "VulkanContext: Failed to create uniform buffer");
            return;
        }
        
        VkMemoryRequirements memRequirements;
        vkGetBufferMemoryRequirements(mDevice, mUniformBuffer, &memRequirements);
        
        VkMemoryAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocInfo.allocationSize = memRequirements.size;
        allocInfo.memoryTypeIndex = FindMemoryType(memRequirements.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        
        if (vkAllocateMemory(mDevice, &allocInfo, nullptr, &mUniformBufferMemory) != VK_SUCCESS) {
            Logger::Log(LogLevel::Error, "VulkanContext: Failed to allocate uniform buffer memory");
            return;
        }
        
        vkBindBufferMemory(mDevice, mUniformBuffer, mUniformBufferMemory, 0);
        vkMapMemory(mDevice, mUniformBufferMemory, 0, 256, 0, &mUniformBufferMapped);
    }
    
    // Copy data to uniform buffer
    memcpy(mUniformBufferMapped, data, std::min(size, 256));
}

//-----------------------------------------------------------------------------
// Frame Management
//-----------------------------------------------------------------------------

void VulkanContext::BeginFrame() {
    vkWaitForFences(mDevice, 1, &mInFlightFences[mCurrentFrame], VK_TRUE, UINT64_MAX);

    VkResult result = vkAcquireNextImageKHR(mDevice, mSwapchain, UINT64_MAX,
                                            mImageAvailableSemaphores[mCurrentFrame],
                                            VK_NULL_HANDLE, &mCurrentImageIndex);

    if (result == VK_ERROR_OUT_OF_DATE_KHR) {
        RecreateSwapchain();
        return;
    }

    vkResetFences(mDevice, 1, &mInFlightFences[mCurrentFrame]);
    vkResetCommandBuffer(mCommandBuffers[mCurrentFrame], 0);

    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    vkBeginCommandBuffer(mCommandBuffers[mCurrentFrame], &beginInfo);
}

void VulkanContext::EndFrame() {
    vkEndCommandBuffer(mCommandBuffers[mCurrentFrame]);

    VkSubmitInfo submitInfo{};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;

    VkSemaphore waitSemaphores[] = { mImageAvailableSemaphores[mCurrentFrame] };
    VkPipelineStageFlags waitStages[] = { VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT };
    submitInfo.waitSemaphoreCount = 1;
    submitInfo.pWaitSemaphores = waitSemaphores;
    submitInfo.pWaitDstStageMask = waitStages;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &mCommandBuffers[mCurrentFrame];

    VkSemaphore signalSemaphores[] = { mRenderFinishedSemaphores[mCurrentFrame] };
    submitInfo.signalSemaphoreCount = 1;
    submitInfo.pSignalSemaphores = signalSemaphores;

    vkQueueSubmit(mGraphicsQueue, 1, &submitInfo, mInFlightFences[mCurrentFrame]);

    VkPresentInfoKHR presentInfo{};
    presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    presentInfo.waitSemaphoreCount = 1;
    presentInfo.pWaitSemaphores = signalSemaphores;

    VkSwapchainKHR swapChains[] = { mSwapchain };
    presentInfo.swapchainCount = 1;
    presentInfo.pSwapchains = swapChains;
    presentInfo.pImageIndices = &mCurrentImageIndex;

    VkResult result = vkQueuePresentKHR(mPresentQueue, &presentInfo);
    if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR) {
        RecreateSwapchain();
    }

    mCurrentFrame = (mCurrentFrame + 1) % MAX_FRAMES_IN_FLIGHT;
}

void VulkanContext::Present(void* textureHandle, int srcWidth, int srcHeight,
                            int windowWidth, int windowHeight) {
    if (textureHandle == nullptr) {
        Logger::Log(LogLevel::Error, "[VK] Present called with null textureHandle");
        return;
    }
    VulkanTexture* srcTexture = static_cast<VulkanTexture*>(textureHandle);
    VkCommandBuffer cmd = mCommandBuffers[mCurrentFrame];
    
    // IMPORTANT: Use actual swapchain extent, not requested window size!
    // SDL window size may exceed Vulkan surface capabilities.
    windowWidth = static_cast<int>(mSwapchainExtent.width);
    windowHeight = static_cast<int>(mSwapchainExtent.height);
    VkImage swapchainImage = mSwapchainImages[mCurrentImageIndex];
    VkImageLayout swapchainOldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (mCurrentImageIndex < mSwapchainImageLayouts.size()) {
        swapchainOldLayout = mSwapchainImageLayouts[mCurrentImageIndex];
    }
    Logger::Log(LogLevel::Info, "[VK] Present: srcTexture=%p, srcImage=%p, swapchainImage=%p, srcDims=%dx%d, winDims=%dx%d", srcTexture, (void*)srcTexture->image, (void*)swapchainImage, srcWidth, srcHeight, windowWidth, windowHeight);
    
    // Transition source texture to transfer source layout
    // We track the current layout per texture to avoid incorrect transitions.
    VkImageMemoryBarrier srcBarrier{};
    srcBarrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    srcBarrier.oldLayout = srcTexture->layout;
    srcBarrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    srcBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    srcBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    srcBarrier.image = srcTexture->image;
    srcBarrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    srcBarrier.subresourceRange.baseMipLevel = 0;
    srcBarrier.subresourceRange.levelCount = 1;
    srcBarrier.subresourceRange.baseArrayLayer = 0;
    srcBarrier.subresourceRange.layerCount = 1;
    // Conservative access mask: texture might have last been written by transfer
    // but typical steady-state is SHADER_READ_ONLY.
    srcBarrier.srcAccessMask = (srcBarrier.oldLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL)
                                   ? VK_ACCESS_TRANSFER_WRITE_BIT
                                   : VK_ACCESS_SHADER_READ_BIT;
    srcBarrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    
    // Transition swapchain image to transfer destination layout
    VkImageMemoryBarrier dstBarrier{};
    dstBarrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    dstBarrier.oldLayout = swapchainOldLayout;
    dstBarrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    dstBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    dstBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    dstBarrier.image = swapchainImage;
    dstBarrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    dstBarrier.subresourceRange.baseMipLevel = 0;
    dstBarrier.subresourceRange.levelCount = 1;
    dstBarrier.subresourceRange.baseArrayLayer = 0;
    dstBarrier.subresourceRange.layerCount = 1;
    dstBarrier.srcAccessMask = 0;
    dstBarrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    
    // Use ALL_COMMANDS for source stage to be safe, as UpdateTexture might have used TRANSFER or COMPUTE
    // But since we waited for idle, TOP_OF_PIPE is technically ok, but let's be explicit about dependencies.
    // The previous operation on srcTexture was a transition to SHADER_READ_ONLY.
    // That transition happened in UpdateTexture using TRANSFER stage (copy) -> TRANSFER stage (barrier).
    // So we should wait on TRANSFER stage.
    
    VkImageMemoryBarrier barriers[] = { srcBarrier, dstBarrier };
    Logger::Log(LogLevel::Info, "[VK] Present: PipelineBarrier srcImage=%p (SHADER_READ->TRANSFER_SRC), dstImage=%p (UNDEFINED->TRANSFER_DST)", (void*)srcTexture->image, (void*)swapchainImage);
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 
                         VK_PIPELINE_STAGE_TRANSFER_BIT,
                         0, 0, nullptr, 0, nullptr, 2, barriers);

    // Clear entire swapchain image so letterbox/pillarbox areas are deterministic.
    VkClearColorValue clearColor = { { 0.0f, 0.0f, 0.0f, 1.0f } };
    VkImageSubresourceRange clearRange{};
    clearRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    clearRange.baseMipLevel = 0;
    clearRange.levelCount = 1;
    clearRange.baseArrayLayer = 0;
    clearRange.layerCount = 1;
    vkCmdClearColorImage(cmd, swapchainImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clearColor, 1, &clearRange);

    // Calculate aspect-correct scaling
    float srcAspect = static_cast<float>(srcWidth) / srcHeight;
    float dstAspect = static_cast<float>(windowWidth) / windowHeight;
    
    int32_t dstX = 0, dstY = 0;
    int32_t dstW = windowWidth, dstH = windowHeight;
    
    if (srcAspect > dstAspect) {
        // Letterbox (black bars on top/bottom)
        dstH = static_cast<int32_t>(windowWidth / srcAspect);
        dstY = (windowHeight - dstH) / 2;
    } else {
        // Pillarbox (black bars on left/right)
        dstW = static_cast<int32_t>(windowHeight * srcAspect);
        dstX = (windowWidth - dstW) / 2;
    }
    
    Logger::Log(LogLevel::Info, "[VK] Present: BlitRegion src=(0,0)-(%d,%d) dst=(%d,%d)-(%d,%d) swapchain=%dx%d",
                srcWidth, srcHeight, dstX, dstY, dstX + dstW, dstY + dstH, windowWidth, windowHeight);
    
    // Blit (with scaling) from source texture to swapchain
    VkImageBlit blitRegion{};
    blitRegion.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    blitRegion.srcSubresource.mipLevel = 0;
    blitRegion.srcSubresource.baseArrayLayer = 0;
    blitRegion.srcSubresource.layerCount = 1;
    blitRegion.srcOffsets[0] = { 0, 0, 0 };
    blitRegion.srcOffsets[1] = { srcWidth, srcHeight, 1 };
    
    blitRegion.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    blitRegion.dstSubresource.mipLevel = 0;
    blitRegion.dstSubresource.baseArrayLayer = 0;
    blitRegion.dstSubresource.layerCount = 1;
    blitRegion.dstOffsets[0] = { dstX, dstY, 0 };
    blitRegion.dstOffsets[1] = { dstX + dstW, dstY + dstH, 1 };
    
    vkCmdBlitImage(cmd, srcTexture->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                   swapchainImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                   1, &blitRegion, VK_FILTER_LINEAR);
    
    // Transition swapchain image to present layout
    VkImageMemoryBarrier presentBarrier{};
    presentBarrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    presentBarrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    presentBarrier.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    presentBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    presentBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    presentBarrier.image = swapchainImage;
    presentBarrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    presentBarrier.subresourceRange.baseMipLevel = 0;
    presentBarrier.subresourceRange.levelCount = 1;
    presentBarrier.subresourceRange.baseArrayLayer = 0;
    presentBarrier.subresourceRange.layerCount = 1;
    presentBarrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    presentBarrier.dstAccessMask = 0;
    
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &presentBarrier);

    // Transition source back to shader read layout for next frame
    VkImageMemoryBarrier srcBackBarrier{};
    srcBackBarrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    srcBackBarrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    srcBackBarrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    srcBackBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    srcBackBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    srcBackBarrier.image = srcTexture->image;
    srcBackBarrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    srcBackBarrier.subresourceRange.baseMipLevel = 0;
    srcBackBarrier.subresourceRange.levelCount = 1;
    srcBackBarrier.subresourceRange.baseArrayLayer = 0;
    srcBackBarrier.subresourceRange.layerCount = 1;
    srcBackBarrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    srcBackBarrier.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT;
    
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &srcBackBarrier);

    // Update tracked layouts
    srcTexture->layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    if (mCurrentImageIndex < mSwapchainImageLayouts.size()) {
        mSwapchainImageLayouts[mCurrentImageIndex] = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    }
}

void VulkanContext::RecreateSwapchain() {
    vkDeviceWaitIdle(mDevice);
    CleanupSwapchain();
    CreateSwapchain();
}

bool VulkanContext::Reconfigure(int width, int height) {
    mScreenWidth = width;
    mScreenHeight = height;
    
    // Ensure we wait for device idle before destroying swapchain
    vkDeviceWaitIdle(mDevice);
    
    RecreateSwapchain();
    return true;
}

bool VulkanContext::CompileGLSLToSPIRV(const std::string& glslSource, std::vector<uint32_t>& spirvOutput) {
#if FALLOUT_HAVE_SHADERC
    shaderc::Compiler compiler;
    shaderc::CompileOptions options;
    
    // Set optimization level
    options.SetOptimizationLevel(shaderc_optimization_level_performance);
    
    // Target Vulkan 1.2
    options.SetTargetEnvironment(shaderc_target_env_vulkan, shaderc_env_version_vulkan_1_2);
    options.SetTargetSpirv(shaderc_spirv_version_1_5);
    
    // Compile as compute shader
    shaderc::SpvCompilationResult result = compiler.CompileGlslToSpv(
        glslSource, shaderc_compute_shader, "shader.comp", options);
    
    if (result.GetCompilationStatus() != shaderc_compilation_status_success) {
        Logger::Log(LogLevel::Error, "VulkanContext: Shader compilation failed: %s", 
                    result.GetErrorMessage().c_str());
        return false;
    }
    
    // Copy SPIR-V output
    spirvOutput.assign(result.cbegin(), result.cend());
    
    Logger::Log(LogLevel::Info, "VulkanContext: Shader compiled successfully (%zu words)", 
                spirvOutput.size());
    return true;
#else
    Logger::Log(LogLevel::Error, "VulkanContext: Shaderc not available - cannot compile shaders");
    return false;
#endif
}

} // namespace renderer
} // namespace fallout

#endif // FALLOUT_HAVE_VULKAN
