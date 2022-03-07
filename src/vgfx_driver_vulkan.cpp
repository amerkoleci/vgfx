// Copyright � Amer Koleci and Contributors.
// Licensed under the MIT License (MIT). See LICENSE in the repository root for more information.

#if defined(VGFX_VULKAN_DRIVER)

#include "vgfx_driver.h"
VGFX_DISABLE_WARNINGS()
#include "volk.h"
#define VMA_IMPLEMENTATION
#include "vk_mem_alloc.h"
#include <spirv_reflect.h>
VGFX_ENABLE_WARNINGS()
#include <vector>
#include <deque>
#include <mutex>

#if defined(VK_USE_PLATFORM_XLIB_KHR) || defined(VK_USE_PLATFORM_XCB_KHR)
#include <dlfcn.h>
#include <X11/Xlib.h>
#include <X11/Xlib-xcb.h>

#undef Success
#undef None
#undef Always
#undef Bool
#endif

namespace
{
    inline const char* ToString(VkResult result)
    {
        switch (result)
        {
#define STR(r)   \
	case VK_##r: \
		return #r
            STR(NOT_READY);
            STR(TIMEOUT);
            STR(EVENT_SET);
            STR(EVENT_RESET);
            STR(INCOMPLETE);
            STR(ERROR_OUT_OF_HOST_MEMORY);
            STR(ERROR_OUT_OF_DEVICE_MEMORY);
            STR(ERROR_INITIALIZATION_FAILED);
            STR(ERROR_DEVICE_LOST);
            STR(ERROR_MEMORY_MAP_FAILED);
            STR(ERROR_LAYER_NOT_PRESENT);
            STR(ERROR_EXTENSION_NOT_PRESENT);
            STR(ERROR_FEATURE_NOT_PRESENT);
            STR(ERROR_INCOMPATIBLE_DRIVER);
            STR(ERROR_TOO_MANY_OBJECTS);
            STR(ERROR_FORMAT_NOT_SUPPORTED);
            STR(ERROR_SURFACE_LOST_KHR);
            STR(ERROR_NATIVE_WINDOW_IN_USE_KHR);
            STR(SUBOPTIMAL_KHR);
            STR(ERROR_OUT_OF_DATE_KHR);
            STR(ERROR_INCOMPATIBLE_DISPLAY_KHR);
            STR(ERROR_VALIDATION_FAILED_EXT);
            STR(ERROR_INVALID_SHADER_NV);
#undef STR
            default:
                return "UNKNOWN_ERROR";
        }
    }

    VKAPI_ATTR VkBool32 VKAPI_CALL DebugUtilsMessengerCallback(
        VkDebugUtilsMessageSeverityFlagBitsEXT messageSeverity,
        VkDebugUtilsMessageTypeFlagsEXT messageType,
        const VkDebugUtilsMessengerCallbackDataEXT* pCallbackData,
        void* pUserData)
    {
        _VGFX_UNUSED(pUserData);

        const char* messageTypeStr = "General";

        if (messageType == VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT)
            messageTypeStr = "Validation";
        else if (messageType == VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT)
            messageTypeStr = "Performance";

        // Log debug messge
        if (messageSeverity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT)
        {
            vgfxLogWarn("Vulkan - %s: %s", messageTypeStr, pCallbackData->pMessage);
        }
        else if (messageSeverity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT)
        {
            vgfxLogError("Vulkan - %s: %s", messageTypeStr, pCallbackData->pMessage);
        }

        return VK_FALSE;
    }

    bool ValidateLayers(const std::vector<const char*>& required, const std::vector<VkLayerProperties>& available)
    {
        for (auto layer : required)
        {
            bool found = false;
            for (auto& available_layer : available)
            {
                if (strcmp(available_layer.layerName, layer) == 0)
                {
                    found = true;
                    break;
                }
            }

            if (!found)
            {
                vgfxLogWarn("Validation Layer '%s' not found", layer);
                return false;
            }
        }

        return true;
    }

    std::vector<const char*> GetOptimalValidationLayers(const std::vector<VkLayerProperties>& supported_instance_layers)
    {
        std::vector<std::vector<const char*>> validation_layer_priority_list =
        {
            // The preferred validation layer is "VK_LAYER_KHRONOS_validation"
            {"VK_LAYER_KHRONOS_validation"},

            // Otherwise we fallback to using the LunarG meta layer
            {"VK_LAYER_LUNARG_standard_validation"},

            // Otherwise we attempt to enable the individual layers that compose the LunarG meta layer since it doesn't exist
            {
                "VK_LAYER_GOOGLE_threading",
                "VK_LAYER_LUNARG_parameter_validation",
                "VK_LAYER_LUNARG_object_tracker",
                "VK_LAYER_LUNARG_core_validation",
                "VK_LAYER_GOOGLE_unique_objects",
            },

            // Otherwise as a last resort we fallback to attempting to enable the LunarG core layer
            {"VK_LAYER_LUNARG_core_validation"}
        };

        for (auto& validation_layers : validation_layer_priority_list)
        {
            if (ValidateLayers(validation_layers, supported_instance_layers))
            {
                return validation_layers;
            }

            vgfxLogWarn("Couldn't enable validation layers (see log for error) - falling back");
        }

        // Else return nothing
        return {};
    }

    struct PhysicalDeviceExtensions
    {
        bool swapchain;
        bool depth_clip_enable;
        bool memory_budget;
        bool performance_query;
        bool host_query_reset;
        bool deferred_host_operations;
        bool renderPass2;
        bool accelerationStructure;
        bool raytracingPipeline;
        bool rayQuery;
        bool fragment_shading_rate;
        bool NV_mesh_shader;
        bool EXT_conditional_rendering;
        bool win32_full_screen_exclusive;
        bool vertex_attribute_divisor;
        bool extended_dynamic_state;
        bool vertex_input_dynamic_state;
        bool extended_dynamic_state2;
        bool dynamic_rendering;
    };

    inline PhysicalDeviceExtensions QueryPhysicalDeviceExtensions(VkPhysicalDevice physicalDevice)
    {
        uint32_t count = 0;
        VkResult result = vkEnumerateDeviceExtensionProperties(physicalDevice, nullptr, &count, nullptr);
        if (result != VK_SUCCESS)
            return {};

        std::vector<VkExtensionProperties> vk_extensions(count);
        vkEnumerateDeviceExtensionProperties(physicalDevice, nullptr, &count, vk_extensions.data());

        PhysicalDeviceExtensions extensions{};

        for (uint32_t i = 0; i < count; ++i)
        {
            if (strcmp(vk_extensions[i].extensionName, VK_KHR_SWAPCHAIN_EXTENSION_NAME) == 0) {
                extensions.swapchain = true;
            }
            else if (strcmp(vk_extensions[i].extensionName, VK_EXT_DEPTH_CLIP_ENABLE_EXTENSION_NAME) == 0) {
                extensions.depth_clip_enable = true;
            }
            else if (strcmp(vk_extensions[i].extensionName, VK_EXT_MEMORY_BUDGET_EXTENSION_NAME) == 0) {
                extensions.memory_budget = true;
            }
            else if (strcmp(vk_extensions[i].extensionName, VK_KHR_PERFORMANCE_QUERY_EXTENSION_NAME) == 0) {
                extensions.performance_query = true;
            }
            else if (strcmp(vk_extensions[i].extensionName, VK_EXT_HOST_QUERY_RESET_EXTENSION_NAME) == 0) {
                extensions.host_query_reset = true;
            }
            else if (strcmp(vk_extensions[i].extensionName, VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME) == 0) {
                extensions.deferred_host_operations = true;
            }
            else if (strcmp(vk_extensions[i].extensionName, VK_KHR_CREATE_RENDERPASS_2_EXTENSION_NAME) == 0) {
                extensions.renderPass2 = true;
            }
            else if (strcmp(vk_extensions[i].extensionName, VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME) == 0) {
                extensions.accelerationStructure = true;
            }
            else if (strcmp(vk_extensions[i].extensionName, VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME) == 0) {
                extensions.raytracingPipeline = true;
            }
            else if (strcmp(vk_extensions[i].extensionName, VK_KHR_RAY_QUERY_EXTENSION_NAME) == 0) {
                extensions.rayQuery = true;
            }
            else if (strcmp(vk_extensions[i].extensionName, VK_KHR_FRAGMENT_SHADING_RATE_EXTENSION_NAME) == 0) {
                extensions.fragment_shading_rate = true;
            }
            else if (strcmp(vk_extensions[i].extensionName, VK_NV_MESH_SHADER_EXTENSION_NAME) == 0) {
                extensions.NV_mesh_shader = true;
            }
            else if (strcmp(vk_extensions[i].extensionName, VK_EXT_CONDITIONAL_RENDERING_EXTENSION_NAME) == 0) {
                extensions.EXT_conditional_rendering = true;
            }
            else if (strcmp(vk_extensions[i].extensionName, VK_EXT_VERTEX_ATTRIBUTE_DIVISOR_EXTENSION_NAME) == 0) {
                extensions.vertex_attribute_divisor = true;
            }
            else if (strcmp(vk_extensions[i].extensionName, VK_EXT_EXTENDED_DYNAMIC_STATE_EXTENSION_NAME) == 0) {
                extensions.extended_dynamic_state = true;
            }
            else if (strcmp(vk_extensions[i].extensionName, VK_EXT_VERTEX_INPUT_DYNAMIC_STATE_EXTENSION_NAME) == 0) {
                extensions.vertex_input_dynamic_state = true;
            }
            else if (strcmp(vk_extensions[i].extensionName, VK_EXT_EXTENDED_DYNAMIC_STATE_2_EXTENSION_NAME) == 0) {
                extensions.extended_dynamic_state2 = true;
            }
            else if (strcmp(vk_extensions[i].extensionName, VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME) == 0) {
                extensions.dynamic_rendering = true;
            }
#if defined(VK_USE_PLATFORM_WIN32_KHR)
            else if (strcmp(vk_extensions[i].extensionName, VK_EXT_FULL_SCREEN_EXCLUSIVE_EXTENSION_NAME) == 0) {
                extensions.win32_full_screen_exclusive = true;
            }
#endif
        }

        VkPhysicalDeviceProperties gpuProps;
        vkGetPhysicalDeviceProperties(physicalDevice, &gpuProps);

        // Core 1.2
        if (gpuProps.apiVersion >= VK_API_VERSION_1_2)
        {
            extensions.renderPass2 = true;
        }

        // Core 1.3
        if (gpuProps.apiVersion >= VK_API_VERSION_1_3)
        {
        }

        return extensions;
    }
}

/// Helper macro to test the result of Vulkan calls which can return an error.
#define VK_CHECK(x) \
	do \
	{ \
		VkResult err = x; \
		if (err) \
		{ \
			vgfxLogError("Detected Vulkan error: %s", ToString(err)); \
		} \
	} while (0)

#define VK_LOG_ERROR(result, message) vgfxLogError("Vulkan: %s, error: %s", message, ToString(result));

struct VGFXVulkanRenderer;

struct VGFXVulkanTexture
{
    VkImage handle = VK_NULL_HANDLE;
    VmaAllocation  allocation = nullptr;
};

struct VGFXVulkanSwapChain
{
    VGFXVulkanRenderer* renderer = nullptr;
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    VkSwapchainKHR handle = VK_NULL_HANDLE;
    uint32_t width = 0;
    uint32_t height = 0;
    bool vsync = false;
    std::vector<VGFXTexture> backbufferTextures;
};

struct VGFXVulkanRenderer
{
#if defined(VK_USE_PLATFORM_XCB_KHR)
    struct {
        void* handle;
        decltype(&::XGetXCBConnection) GetXCBConnection;
    } x11xcb;
#endif

    bool debugUtils;
    VkInstance instance;
    VkDebugUtilsMessengerEXT debugUtilsMessenger;

    VkPhysicalDeviceProperties2 properties2 = {};
    VkPhysicalDeviceVulkan11Properties properties_1_1 = {};
    VkPhysicalDeviceVulkan12Properties properties_1_2 = {};
    VkPhysicalDeviceSamplerFilterMinmaxProperties sampler_minmax_properties = {};
    VkPhysicalDeviceAccelerationStructurePropertiesKHR acceleration_structure_properties = {};
    VkPhysicalDeviceRayTracingPipelinePropertiesKHR raytracing_properties = {};
    VkPhysicalDeviceFragmentShadingRatePropertiesKHR fragment_shading_rate_properties = {};
    VkPhysicalDeviceMeshShaderPropertiesNV mesh_shader_properties = {};

    VkPhysicalDeviceFeatures2 features2 = {};
    VkPhysicalDeviceVulkan11Features features_1_1 = {};
    VkPhysicalDeviceVulkan12Features features_1_2 = {};
    VkPhysicalDeviceAccelerationStructureFeaturesKHR acceleration_structure_features = {};
    VkPhysicalDeviceRayTracingPipelineFeaturesKHR raytracing_features = {};
    VkPhysicalDeviceRayQueryFeaturesKHR raytracing_query_features = {};
    VkPhysicalDeviceFragmentShadingRateFeaturesKHR fragment_shading_rate_features = {};
    VkPhysicalDeviceMeshShaderFeaturesNV mesh_shader_features = {};
    VkPhysicalDeviceConditionalRenderingFeaturesEXT conditional_rendering_features = {};
    VkPhysicalDeviceDepthClipEnableFeaturesEXT depth_clip_enable_features = {};
    VkPhysicalDevicePerformanceQueryFeaturesKHR perf_counter_features = {};
    VkPhysicalDeviceHostQueryResetFeatures host_query_reset_features = {};
    VkPhysicalDeviceVertexAttributeDivisorFeaturesEXT vertexDivisorFeatures = {};
    VkPhysicalDeviceExtendedDynamicStateFeaturesEXT extendedDynamicStateFeatures = {};
    VkPhysicalDeviceExtendedDynamicState2FeaturesEXT extendedDynamicState2Features = {};
    VkPhysicalDeviceVertexInputDynamicStateFeaturesEXT vertexInputDynamicStateFeatures = {};
    VkPhysicalDeviceDynamicRenderingFeaturesKHR dynamicRenderingFeaturesKHR = {};
    bool dynamicRendering{ false }; // Beta
    VkDeviceSize minAllocationAlignment{ 0 };

    VkPhysicalDevice physicalDevice;
    uint32_t graphicsQueueFamily = VK_QUEUE_FAMILY_IGNORED;
    uint32_t computeQueueFamily = VK_QUEUE_FAMILY_IGNORED;
    uint32_t copyQueueFamily = VK_QUEUE_FAMILY_IGNORED;
    VkQueue graphicsQueue = VK_NULL_HANDLE;
    VkQueue computeQueue = VK_NULL_HANDLE;
    VkQueue copyQueue = VK_NULL_HANDLE;

    VkDevice device = VK_NULL_HANDLE;
    VmaAllocator allocator = VK_NULL_HANDLE;

    uint32_t frameIndex = 0;
    uint64_t frameCount = 0;

    // Deletion queue objects
    std::mutex destroyMutex;
    std::deque<std::pair<std::pair<VkImage, VmaAllocation>, uint64_t>> destroyedImagesQueue;
    std::deque<std::pair<VkImageView, uint64_t>> destroyedImageViews;
    std::deque<std::pair<VkSampler, uint64_t>> destroyedSamplers;
    std::deque<std::pair<std::pair<VkBuffer, VmaAllocation>, uint64_t>> destroyedBuffers;
    std::deque<std::pair<VkShaderModule, uint64_t>> destroyedShaderModules;
    std::deque<std::pair<VkPipeline, uint64_t>> destroyedPipelines;
    std::deque<std::pair<VkDescriptorPool, uint64_t>> destroyedDescriptorPools;
    std::deque<std::pair<VkQueryPool, uint64_t>> destroyedQueryPools;
};

static VkSurfaceKHR vulkan_createSurface(VGFXVulkanRenderer* renderer, VGFXSurface surface)
{
    VkResult result = VK_SUCCESS;
    VkSurfaceKHR vk_surface = VK_NULL_HANDLE;

    VkInstance instance = renderer->instance;

#if defined(VK_USE_PLATFORM_ANDROID_KHR)
    VkAndroidSurfaceCreateInfoKHR createInfo = {};
    createInfo.sType = VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR;
    createInfo.window = surface->window;
    result = vkCreateAndroidSurfaceKHR(instance, &createInfo, NULL, &vk_surface);
#elif defined(VK_USE_PLATFORM_WIN32_KHR)
    VkWin32SurfaceCreateInfoKHR createInfo = {};
    createInfo.sType = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR;
    createInfo.hinstance = surface->hinstance;
    createInfo.hwnd = surface->window;
    result = vkCreateWin32SurfaceKHR(instance, &createInfo, nullptr, &vk_surface);
#elif defined(VK_USE_PLATFORM_METAL_EXT)
    VkMetalSurfaceCreateInfoEXT createInfo = {};
    createInfo.sType = VK_STRUCTURE_TYPE_METAL_SURFACE_CREATE_INFO_EXT;
    createInfo.pLayer = (const CAMetalLayer*)surface->pLayer;
    result = vkCreateMetalSurfaceEXT(instance, &createInfo, nullptr, &vk_surface);
#elif defined(VK_USE_PLATFORM_XCB_KHR)
    VkXcbSurfaceCreateInfoKHR createInfo = {};
    createInfo.sType = VK_STRUCTURE_TYPE_XCB_SURFACE_CREATE_INFO_KHR;
    createInfo.connection = renderer->GetXCBConnection(static_cast<Display*>(surface->display));
    createInfo.window = surface->window;
    result = vkCreateXcbSurfaceKHR(instance, &createInfo, nullptr, &vk_surface);
#elif defined(VK_USE_PLATFORM_XLIB_KHR)
    VkXlibSurfaceCreateInfoKHR createInfo = {};
    createInfo.sType = VK_STRUCTURE_TYPE_XLIB_SURFACE_CREATE_INFO_KHR;
    createInfo.dpy = (Display*)surface->display;
    createInfo.window = (Window)surface->window;
    result = vkCreateXlibSurfaceKHR(instance, &createInfo, nullptr, &vk_surface);
#elif defined(VK_USE_PLATFORM_WAYLAND_KHR)
    VkWaylandSurfaceCreateInfoKHR createInfo = {};
    createInfo.sType = VK_STRUCTURE_TYPE_WAYLAND_SURFACE_CREATE_INFO_KHR;
    createInfo.display = display;
    createInfo.surface = window;
    result = vkCreateWaylandSurfaceKHR(instance, &createInfo, nullptr, &vk_surface);
#elif defined(VK_USE_PLATFORM_DISPLAY_KHR)
#elif defined(VK_USE_PLATFORM_HEADLESS_EXT)
    VkHeadlessSurfaceCreateInfoEXT createInfo = {};
    createInfo.sType = VK_STRUCTURE_TYPE_HEADLESS_SURFACE_CREATE_INFO_EXT;
    result = vkCreateHeadlessSurfaceEXT(instance, &createInfo, nullptr, &vk_surface);
#endif

    if (result != VK_SUCCESS)
    {
        VK_LOG_ERROR(result, "Failed to create surface");
    }

    return vk_surface;
}

static void vulkan_ProcessDeletionQueue(VGFXVulkanRenderer* renderer)
{
    renderer->destroyMutex.lock();
    while (!renderer->destroyedImagesQueue.empty())
    {
        if (renderer->destroyedImagesQueue.front().second + VGFX_MAX_INFLIGHT_FRAMES < renderer->frameCount)
        {
            auto item = renderer->destroyedImagesQueue.front();
            renderer->destroyedImagesQueue.pop_front();
            vmaDestroyImage(renderer->allocator, item.first.first, item.first.second);
        }
        else
        {
            break;
        }
    }

    while (!renderer->destroyedImageViews.empty())
    {
        if (renderer->destroyedImageViews.front().second + VGFX_MAX_INFLIGHT_FRAMES < renderer->frameCount)
        {
            auto item = renderer->destroyedImageViews.front();
            renderer->destroyedImageViews.pop_front();
            vkDestroyImageView(renderer->device, item.first, nullptr);
        }
        else
        {
            break;
        }
    }

    while (!renderer->destroyedSamplers.empty())
    {
        if (renderer->destroyedSamplers.front().second + VGFX_MAX_INFLIGHT_FRAMES < renderer->frameCount)
        {
            auto item = renderer->destroyedSamplers.front();
            renderer->destroyedSamplers.pop_front();
            vkDestroySampler(renderer->device, item.first, nullptr);
        }
        else
        {
            break;
        }
    }

    while (!renderer->destroyedBuffers.empty())
    {
        if (renderer->destroyedBuffers.front().second + VGFX_MAX_INFLIGHT_FRAMES < renderer->frameCount)
        {
            auto item = renderer->destroyedBuffers.front();
            renderer->destroyedBuffers.pop_front();
            vmaDestroyBuffer(renderer->allocator, item.first.first, item.first.second);
        }
        else
        {
            break;
        }
    }

    while (!renderer->destroyedPipelines.empty())
    {
        if (renderer->destroyedPipelines.front().second + VGFX_MAX_INFLIGHT_FRAMES < renderer->frameCount)
        {
            auto item = renderer->destroyedPipelines.front();
            renderer->destroyedPipelines.pop_front();
            vkDestroyPipeline(renderer->device, item.first, nullptr);
        }
        else
        {
            break;
        }
    }

    while (!renderer->destroyedDescriptorPools.empty())
    {
        if (renderer->destroyedDescriptorPools.front().second + VGFX_MAX_INFLIGHT_FRAMES < renderer->frameCount)
        {
            auto item = renderer->destroyedDescriptorPools.front();
            renderer->destroyedDescriptorPools.pop_front();
            vkDestroyDescriptorPool(renderer->device, item.first, nullptr);
        }
        else
        {
            break;
        }
    }

    while (!renderer->destroyedQueryPools.empty())
    {
        if (renderer->destroyedQueryPools.front().second + VGFX_MAX_INFLIGHT_FRAMES < renderer->frameCount)
        {
            auto item = renderer->destroyedQueryPools.front();
            renderer->destroyedQueryPools.pop_front();
            vkDestroyQueryPool(renderer->device, item.first, nullptr);
        }
        else
        {
            break;
        }
    }
    renderer->destroyMutex.unlock();
}

static void vulkan_destroyDevice(VGFXDevice device)
{
    VGFXVulkanRenderer* renderer = (VGFXVulkanRenderer*)device->driverData;
    VK_CHECK(vkDeviceWaitIdle(renderer->device));

    renderer->frameCount = UINT64_MAX;
    vulkan_ProcessDeletionQueue(renderer);
    renderer->frameCount = 0;

    if (renderer->allocator != VK_NULL_HANDLE)
    {
        //VmaStats stats;
        //vmaCalculateStats(renderer->allocator, &stats);

        //if (stats.total.usedBytes > 0)
        //{
        //    vgfxLogError("Total device memory leaked: {} bytes.", stats.total.usedBytes);
        //}

        vmaDestroyAllocator(renderer->allocator);
        renderer->allocator = VK_NULL_HANDLE;
    }

    if (renderer->device != VK_NULL_HANDLE)
    {
        vkDestroyDevice(renderer->device, nullptr);
        renderer->device = VK_NULL_HANDLE;
    }

    if (renderer->debugUtilsMessenger != VK_NULL_HANDLE)
    {
        vkDestroyDebugUtilsMessengerEXT(renderer->instance, renderer->debugUtilsMessenger, nullptr);
        renderer->debugUtilsMessenger = VK_NULL_HANDLE;
    }

    if (renderer->instance != VK_NULL_HANDLE)
    {
        vkDestroyInstance(renderer->instance, nullptr);
        renderer->instance = VK_NULL_HANDLE;
    }

#if defined(VK_USE_PLATFORM_XCB_KHR)
    if (renderer->x11xcb.handle)
    {
        dlclose(renderer->x11xcb.handle);
        renderer->x11xcb.handle = NULL;
    }
#endif

    delete renderer;
    VGFX_FREE(device);
}

static void vulkan_frame(VGFXRenderer* driverData)
{
    VGFXVulkanRenderer* renderer = (VGFXVulkanRenderer*)driverData;

    renderer->frameCount++;
    renderer->frameIndex = renderer->frameCount % VGFX_MAX_INFLIGHT_FRAMES;

    // Begin new frame
    {
        // Safe delete deferred destroys
        vulkan_ProcessDeletionQueue(renderer);

        // Prepare the command buffers to be used for the next frame
    }
}

static void vulkan_waitIdle(VGFXRenderer* driverData)
{
    VGFXVulkanRenderer* renderer = (VGFXVulkanRenderer*)driverData;
    VK_CHECK(vkDeviceWaitIdle(renderer->device));
}

static bool vulkan_queryFeature(VGFXRenderer* driverData, VGFXFeature feature)
{
    VGFXVulkanRenderer* renderer = (VGFXVulkanRenderer*)driverData;
    switch (feature)
    {
        case VGFXFeature_Compute:
            return true;

        case VGFXFeature_TextureCompressionBC:
            return renderer->features2.features.textureCompressionBC;

        case VGFXFeature_TextureCompressionETC2:
            return renderer->features2.features.textureCompressionETC2;

        case VGFXFeature_TextureCompressionASTC:
            return renderer->features2.features.textureCompressionASTC_LDR;

        default:
            return false;
    }
}

/* Texture */
static void vulkan_destroyTexture(VGFXRenderer* driverData, VGFXTexture texture)
{
    VGFXVulkanRenderer* renderer = (VGFXVulkanRenderer*)driverData;
    VGFXVulkanTexture* vulkanTexture = (VGFXVulkanTexture*)texture;

    if (vulkanTexture->allocation)
    {
        renderer->destroyMutex.lock();
        renderer->destroyedImagesQueue.push_back(std::make_pair(std::make_pair(vulkanTexture->handle, vulkanTexture->allocation), renderer->frameCount));
        renderer->destroyMutex.unlock();
    }

    delete vulkanTexture;
}

/* SwapChain */

static void vulkan_updateSwapChain(VGFXVulkanRenderer* renderer, VGFXVulkanSwapChain* swapChain)
{
}

static VGFXSwapChain vulkan_createSwapChain(VGFXRenderer* driverData, VGFXSurface surface, const VGFXSwapChainInfo* info)
{
    VGFXVulkanRenderer* renderer = (VGFXVulkanRenderer*)driverData;
    VkSurfaceKHR vk_surface = vulkan_createSurface(renderer, surface);
    if (vk_surface == VK_NULL_HANDLE)
    {
        return nullptr;
    }

    VkBool32 supported = VK_FALSE;
    VK_CHECK(
        vkGetPhysicalDeviceSurfaceSupportKHR(renderer->physicalDevice, renderer->graphicsQueueFamily, vk_surface, &supported)
    );
    if (!supported)
    {
        return false;
    }

    VGFXVulkanSwapChain* swapChain = new VGFXVulkanSwapChain();
    swapChain->renderer = renderer;
    swapChain->surface = vk_surface;
    swapChain->vsync = info->presentMode == VGFXPresentMode_Fifo;
    vulkan_updateSwapChain(renderer, swapChain);

    return (VGFXSwapChain)swapChain;
}

static void vulkan_destroySwapChain(VGFXRenderer* driverData, VGFXSwapChain swapChain)
{
    _VGFX_UNUSED(driverData);

    VGFXVulkanSwapChain* vulkanSwapChain = (VGFXVulkanSwapChain*)swapChain;
    //vulkan_destroyTexture(nullptr, d3dSwapChain->backbufferTexture);

    delete vulkanSwapChain;
}

static void vulkan_getSwapChainSize(VGFXRenderer* driverData, VGFXSwapChain swapChain, VGFXSize2D* pSize)
{
    _VGFX_UNUSED(driverData);

    VGFXVulkanSwapChain* vulkanSwapChain = (VGFXVulkanSwapChain*)swapChain;
    pSize->width = vulkanSwapChain->width;
    pSize->height = vulkanSwapChain->height;
}


static VGFXTexture vulkan_acquireNextTexture(VGFXRenderer* driverData, VGFXSwapChain swapChain)
{
    VGFXVulkanRenderer* renderer = (VGFXVulkanRenderer*)driverData;
    VGFXVulkanSwapChain* vulkanSwapChain = (VGFXVulkanSwapChain*)swapChain;
    //vulkanSwapChain->renderer->swapChains.push_back(d3dSwapChain);
    //return vulkanSwapChain->backbufferTexture;
    return nullptr;
}

static void vulkan_beginRenderPass(VGFXRenderer* driverData, const VGFXRenderPassInfo* info)
{
    VGFXVulkanRenderer* renderer = (VGFXVulkanRenderer*)driverData;
}

static void vulkan_endRenderPass(VGFXRenderer* driverData)
{
    VGFXVulkanRenderer* renderer = (VGFXVulkanRenderer*)driverData;
}

static bool vulkan_isSupported(void)
{
    static bool available_initialized = false;
    static bool available = false;

    if (available_initialized) {
        return available;
    }

    available_initialized = true;

    VkResult result = volkInitialize();
    if (result != VK_SUCCESS)
    {
        return false;
    }

    available = true;
    return true;
}

static VGFXDevice vulkan_createDevice(VGFXSurface surface, const VGFXDeviceInfo* info)
{
    VGFXVulkanRenderer* renderer = new VGFXVulkanRenderer();

#if defined(VK_USE_PLATFORM_XCB_KHR)
#if defined(__CYGWIN__)
    renderer->x11xcb.handle = dlopen("libX11-xcb-1.so", RTLD_LAZY | RTLD_LOCAL);
#elif defined(__OpenBSD__) || defined(__NetBSD__)
    renderer->x11xcb.handle = dlopen("libX11-xcb.so", RTLD_LAZY | RTLD_LOCAL);
#else
    renderer->x11xcb.handle = dlopen("libX11-xcb.so.1", RTLD_LAZY | RTLD_LOCAL);
#endif

    if (renderer->x11xcb.handle)
    {
        renderer->x11xcb.GetXCBConnection = (PFN_XGetXCBConnection)dlsym(renderer->x11xcb.handle, "XGetXCBConnection");
    }
#endif

    // Enumerate available layers and extensions:
    {
        uint32_t instanceLayerCount;
        VK_CHECK(vkEnumerateInstanceLayerProperties(&instanceLayerCount, nullptr));
        std::vector<VkLayerProperties> availableInstanceLayers(instanceLayerCount);
        VK_CHECK(vkEnumerateInstanceLayerProperties(&instanceLayerCount, availableInstanceLayers.data()));

        uint32_t extensionCount = 0;
        VK_CHECK(vkEnumerateInstanceExtensionProperties(nullptr, &extensionCount, nullptr));
        std::vector<VkExtensionProperties> availableInstanceExtensions(extensionCount);
        VK_CHECK(vkEnumerateInstanceExtensionProperties(nullptr, &extensionCount, availableInstanceExtensions.data()));

        std::vector<const char*> instanceLayers;
        std::vector<const char*> instanceExtensions;

        for (auto& availableExtension : availableInstanceExtensions)
        {
            if (strcmp(availableExtension.extensionName, VK_EXT_DEBUG_UTILS_EXTENSION_NAME) == 0)
            {
                renderer->debugUtils = true;
                instanceExtensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
            }
            else if (strcmp(availableExtension.extensionName, VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME) == 0)
            {
                instanceExtensions.push_back(VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME);
            }
            else if (strcmp(availableExtension.extensionName, VK_EXT_SWAPCHAIN_COLOR_SPACE_EXTENSION_NAME) == 0)
            {
                instanceExtensions.push_back(VK_EXT_SWAPCHAIN_COLOR_SPACE_EXTENSION_NAME);
            }
        }

        instanceExtensions.push_back(VK_KHR_SURFACE_EXTENSION_NAME);

        // Enable surface extensions depending on os
#if defined(VK_USE_PLATFORM_ANDROID_KHR)
        instanceExtensions.push_back(VK_KHR_ANDROID_SURFACE_EXTENSION_NAME);
#elif defined(VK_USE_PLATFORM_WIN32_KHR)
        instanceExtensions.push_back(VK_KHR_WIN32_SURFACE_EXTENSION_NAME);
#elif defined(VK_USE_PLATFORM_METAL_EXT)
        instanceExtensions.push_back(VK_EXT_METAL_SURFACE_EXTENSION_NAME);
#elif defined(VK_USE_PLATFORM_XCB_KHR)
        instanceExtensions.push_back(VK_KHR_XCB_SURFACE_EXTENSION_NAME);
#elif defined(VK_USE_PLATFORM_XLIB_KHR)
        instanceExtensions.push_back(VK_KHR_XLIB_SURFACE_EXTENSION_NAME);
#elif defined(VK_USE_PLATFORM_WAYLAND_KHR)
        instanceExtensions.push_back(VK_KHR_WAYLAND_SURFACE_EXTENSION_NAME);
#elif defined(VK_USE_PLATFORM_DISPLAY_KHR)
        instanceExtensions.push_back(VK_KHR_DISPLAY_EXTENSION_NAME);
#elif defined(VK_USE_PLATFORM_HEADLESS_EXT)
        instanceExtensions.push_back(VK_EXT_HEADLESS_SURFACE_EXTENSION_NAME);
#else
#   pragma error Platform not supported
#endif

        if (info->validationMode != VGFXValidationMode_Disabled)
        {
            // Determine the optimal validation layers to enable that are necessary for useful debugging
            std::vector<const char*> optimalValidationLyers = GetOptimalValidationLayers(availableInstanceLayers);
            instanceLayers.insert(instanceLayers.end(), optimalValidationLyers.begin(), optimalValidationLyers.end());
        }

#if defined(_DEBUG)
        bool validationFeatures = false;
        if (info->validationMode == VGFXValidationMode_GPU)
        {
            uint32_t layerInstanceExtensionCount;
            VK_CHECK(vkEnumerateInstanceExtensionProperties("VK_LAYER_KHRONOS_validation", &layerInstanceExtensionCount, nullptr));
            std::vector<VkExtensionProperties> availableLayerInstanceExtensions(layerInstanceExtensionCount);
            VK_CHECK(vkEnumerateInstanceExtensionProperties("VK_LAYER_KHRONOS_validation", &layerInstanceExtensionCount, availableLayerInstanceExtensions.data()));

            for (auto& availableExtension : availableLayerInstanceExtensions)
            {
                if (strcmp(availableExtension.extensionName, VK_EXT_VALIDATION_FEATURES_EXTENSION_NAME) == 0)
                {
                    validationFeatures = true;
                    instanceExtensions.push_back(VK_EXT_VALIDATION_FEATURES_EXTENSION_NAME);
                }
            }
        }
#endif 

        VkApplicationInfo appInfo = {};
        appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
        appInfo.pEngineName = "vgfx";
        appInfo.engineVersion = VK_MAKE_VERSION(VGFX_VERSION_MAJOR, VGFX_VERSION_MINOR, VGFX_VERSION_PATCH);
        appInfo.apiVersion = VK_API_VERSION_1_2;

        VkInstanceCreateInfo createInfo;
        createInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
        createInfo.pNext = NULL;
        createInfo.flags = 0;
        createInfo.pApplicationInfo = &appInfo;
        createInfo.enabledLayerCount = static_cast<uint32_t>(instanceLayers.size());
        createInfo.ppEnabledLayerNames = instanceLayers.data();
        createInfo.enabledExtensionCount = static_cast<uint32_t>(instanceExtensions.size());
        createInfo.ppEnabledExtensionNames = instanceExtensions.data();

        VkDebugUtilsMessengerCreateInfoEXT debugUtilsCreateInfo{};

        if (info->validationMode != VGFXValidationMode_Disabled && renderer->debugUtils)
        {
            debugUtilsCreateInfo.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
            debugUtilsCreateInfo.pNext = nullptr;
            debugUtilsCreateInfo.flags = 0;

            debugUtilsCreateInfo.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
            if (info->validationMode == VGFXValidationMode_Verbose)
            {
                debugUtilsCreateInfo.messageSeverity |= VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT;
            }

            debugUtilsCreateInfo.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
            debugUtilsCreateInfo.pfnUserCallback = DebugUtilsMessengerCallback;
            createInfo.pNext = &debugUtilsCreateInfo;
        }

#if defined(_DEBUG)
        VkValidationFeaturesEXT validationFeaturesInfo = { VK_STRUCTURE_TYPE_VALIDATION_FEATURES_EXT };
        if (validationFeatures)
        {
            static const VkValidationFeatureEnableEXT enable_features[2] = {
                VK_VALIDATION_FEATURE_ENABLE_GPU_ASSISTED_EXT,
                VK_VALIDATION_FEATURE_ENABLE_GPU_ASSISTED_RESERVE_BINDING_SLOT_EXT,
            };
            validationFeaturesInfo.enabledValidationFeatureCount = 2;
            validationFeaturesInfo.pEnabledValidationFeatures = enable_features;
            validationFeaturesInfo.pNext = createInfo.pNext;
            createInfo.pNext = &validationFeaturesInfo;
        }
#endif

        VkResult result = vkCreateInstance(&createInfo, nullptr, &renderer->instance);
        if (result != VK_SUCCESS)
        {
            VK_LOG_ERROR(result, "Failed to create Vulkan instance.");
            delete renderer;
            return nullptr;
        }

        volkLoadInstanceOnly(renderer->instance);

        if (info->validationMode != VGFXValidationMode_Disabled && renderer->debugUtils)
        {
            result = vkCreateDebugUtilsMessengerEXT(renderer->instance, &debugUtilsCreateInfo, nullptr, &renderer->debugUtilsMessenger);
            if (result != VK_SUCCESS)
            {
                VK_LOG_ERROR(result, "Could not create debug utils messenger");
            }
        }

#ifdef _DEBUG
        vgfxLogInfo("Created VkInstance with version: %d.%d.%d",
            VK_VERSION_MAJOR(appInfo.apiVersion),
            VK_VERSION_MINOR(appInfo.apiVersion),
            VK_VERSION_PATCH(appInfo.apiVersion)
        );

        if (createInfo.enabledLayerCount)
        {
            vgfxLogInfo("Enabled %d Validation Layers:", createInfo.enabledLayerCount);

            for (uint32_t i = 0; i < createInfo.enabledLayerCount; ++i)
            {
                vgfxLogInfo("	\t%s", createInfo.ppEnabledLayerNames[i]);
            }
        }

        vgfxLogInfo("Enabled %d Instance Extensions:", createInfo.enabledExtensionCount);
        for (uint32_t i = 0; i < createInfo.enabledExtensionCount; ++i)
        {
            vgfxLogInfo("	\t%s", createInfo.ppEnabledExtensionNames[i]);
        }
#endif
    }

    // Create surface
    VkSurfaceKHR vk_surface = vulkan_createSurface(renderer, surface);
    if (!vk_surface)
    {
        delete renderer;
        return nullptr;
    }

    // Enumerate physical device and create logical device.
    {
        uint32_t deviceCount = 0;
        VK_CHECK(vkEnumeratePhysicalDevices(renderer->instance, &deviceCount, nullptr));

        if (deviceCount == 0)
        {
            vgfxLogError("Vulkan: Failed to find GPUs with Vulkan support");
            delete renderer;
            return nullptr;
        }

        std::vector<VkPhysicalDevice> physicalDevices(deviceCount);
        VK_CHECK(vkEnumeratePhysicalDevices(renderer->instance, &deviceCount, physicalDevices.data()));

        std::vector<const char*> enabledExtensions;
        for (const VkPhysicalDevice& candidatePhysicalDevice : physicalDevices)
        {
            bool suitable = true;

            // We require minimum 1.1
            VkPhysicalDeviceProperties gpuProps;
            vkGetPhysicalDeviceProperties(candidatePhysicalDevice, &gpuProps);
            if (gpuProps.apiVersion < VK_API_VERSION_1_1)
            {
                continue;
            }

            PhysicalDeviceExtensions physicalDeviceExt = QueryPhysicalDeviceExtensions(candidatePhysicalDevice);
            suitable = physicalDeviceExt.swapchain && physicalDeviceExt.depth_clip_enable;

            if (!suitable)
            {
                continue;
            }

            renderer->features2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
            renderer->features_1_1.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES;
            renderer->features_1_2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
            renderer->features2.pNext = &renderer->features_1_1;
            renderer->features_1_1.pNext = &renderer->features_1_2;
            void** features_chain = &renderer->features_1_2.pNext;
            renderer->acceleration_structure_features = {};
            renderer->raytracing_features = {};
            renderer->raytracing_query_features = {};
            renderer->fragment_shading_rate_features = {};
            renderer->mesh_shader_features = {};

            renderer->properties2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
            renderer->properties_1_1.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_PROPERTIES;
            renderer->properties_1_2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_PROPERTIES;
            renderer->properties2.pNext = &renderer->properties_1_1;
            renderer->properties_1_1.pNext = &renderer->properties_1_2;
            void** properties_chain = &renderer->properties_1_2.pNext;
            renderer->sampler_minmax_properties = {};
            renderer->acceleration_structure_properties = {};
            renderer->raytracing_properties = {};
            renderer->fragment_shading_rate_properties = {};
            renderer->mesh_shader_properties = {};

            renderer->sampler_minmax_properties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SAMPLER_FILTER_MINMAX_PROPERTIES;
            *properties_chain = &renderer->sampler_minmax_properties;
            properties_chain = &renderer->sampler_minmax_properties.pNext;

            enabledExtensions.clear();
            enabledExtensions.push_back(VK_KHR_SWAPCHAIN_EXTENSION_NAME);

            // For performance queries, we also use host query reset since queryPool resets cannot live in the same command buffer as beginQuery
            if (physicalDeviceExt.performance_query &&
                physicalDeviceExt.host_query_reset)
            {
                renderer->perf_counter_features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PERFORMANCE_QUERY_FEATURES_KHR;
                enabledExtensions.push_back(VK_KHR_PERFORMANCE_QUERY_EXTENSION_NAME);
                *features_chain = &renderer->perf_counter_features;
                features_chain = &renderer->perf_counter_features.pNext;

                renderer->host_query_reset_features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_HOST_QUERY_RESET_FEATURES;
                enabledExtensions.push_back(VK_EXT_HOST_QUERY_RESET_EXTENSION_NAME);
                *features_chain = &renderer->host_query_reset_features;
                features_chain = &renderer->host_query_reset_features.pNext;
            }

            if (physicalDeviceExt.memory_budget)
            {
                enabledExtensions.push_back(VK_EXT_MEMORY_BUDGET_EXTENSION_NAME);
            }

            if (physicalDeviceExt.depth_clip_enable)
            {
                renderer->depth_clip_enable_features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_CLIP_ENABLE_FEATURES_EXT;
                enabledExtensions.push_back(VK_EXT_DEPTH_CLIP_ENABLE_EXTENSION_NAME);
                *features_chain = &renderer->depth_clip_enable_features;
                features_chain = &renderer->depth_clip_enable_features.pNext;
            }

            // Core 1.2.
            {
                // Required by VK_KHR_spirv_1_4
                //enabledExtensions.push_back(VK_KHR_SHADER_FLOAT_CONTROLS_EXTENSION_NAME);

                // Required for VK_KHR_ray_tracing_pipeline
                //enabledExtensions.push_back(VK_KHR_SPIRV_1_4_EXTENSION_NAME);

                //enabledExtensions.push_back(VK_EXT_SHADER_VIEWPORT_INDEX_LAYER_EXTENSION_NAME);

                // Required by VK_KHR_acceleration_structure
                //enabledExtensions.push_back(VK_KHR_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME);

                // Required by VK_KHR_acceleration_structure
                //enabledExtensions.push_back(VK_EXT_DESCRIPTOR_INDEXING_EXTENSION_NAME);
            }

            if (physicalDeviceExt.accelerationStructure)
            {
                VGFX_ASSERT(physicalDeviceExt.deferred_host_operations);

                // Required by VK_KHR_acceleration_structure
                enabledExtensions.push_back(VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME);

                enabledExtensions.push_back(VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME);
                renderer->acceleration_structure_features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR;
                *features_chain = &renderer->acceleration_structure_features;
                features_chain = &renderer->acceleration_structure_features.pNext;
                renderer->acceleration_structure_properties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_PROPERTIES_KHR;
                *properties_chain = &renderer->acceleration_structure_properties;
                properties_chain = &renderer->acceleration_structure_properties.pNext;

                if (physicalDeviceExt.raytracingPipeline)
                {
                    enabledExtensions.push_back(VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME);
                    renderer->raytracing_features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_FEATURES_KHR;
                    *features_chain = &renderer->raytracing_features;
                    features_chain = &renderer->raytracing_features.pNext;
                    renderer->raytracing_properties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_PROPERTIES_KHR;
                    *properties_chain = &renderer->raytracing_properties;
                    properties_chain = &renderer->raytracing_properties.pNext;
                }

                if (physicalDeviceExt.rayQuery)
                {
                    enabledExtensions.push_back(VK_KHR_RAY_QUERY_EXTENSION_NAME);
                    renderer->raytracing_query_features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR;
                    *features_chain = &renderer->raytracing_query_features;
                    features_chain = &renderer->raytracing_query_features.pNext;
                }
            }

            if (physicalDeviceExt.fragment_shading_rate)
            {
                enabledExtensions.push_back(VK_KHR_CREATE_RENDERPASS_2_EXTENSION_NAME);
                enabledExtensions.push_back(VK_KHR_FRAGMENT_SHADING_RATE_EXTENSION_NAME);
                renderer->fragment_shading_rate_features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADING_RATE_FEATURES_KHR;
                *features_chain = &renderer->fragment_shading_rate_features;
                features_chain = &renderer->fragment_shading_rate_features.pNext;
                renderer->fragment_shading_rate_properties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADING_RATE_PROPERTIES_KHR;
                *properties_chain = &renderer->fragment_shading_rate_properties;
                properties_chain = &renderer->fragment_shading_rate_properties.pNext;
            }

            if (physicalDeviceExt.NV_mesh_shader)
            {
                enabledExtensions.push_back(VK_NV_MESH_SHADER_EXTENSION_NAME);
                renderer->mesh_shader_features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MESH_SHADER_FEATURES_NV;
                *features_chain = &renderer->mesh_shader_features;
                features_chain = &renderer->mesh_shader_features.pNext;
                renderer->mesh_shader_properties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MESH_SHADER_PROPERTIES_NV;
                *properties_chain = &renderer->mesh_shader_properties;
                properties_chain = &renderer->mesh_shader_properties.pNext;
            }

            if (physicalDeviceExt.EXT_conditional_rendering)
            {
                enabledExtensions.push_back(VK_EXT_CONDITIONAL_RENDERING_EXTENSION_NAME);
                renderer->conditional_rendering_features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CONDITIONAL_RENDERING_FEATURES_EXT;
                *features_chain = &renderer->conditional_rendering_features;
                features_chain = &renderer->conditional_rendering_features.pNext;
            }

            if (physicalDeviceExt.vertex_attribute_divisor)
            {
                enabledExtensions.push_back(VK_EXT_VERTEX_ATTRIBUTE_DIVISOR_EXTENSION_NAME);
                renderer->vertexDivisorFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_ATTRIBUTE_DIVISOR_FEATURES_EXT;
                *features_chain = &renderer->vertexDivisorFeatures;
                features_chain = &renderer->vertexDivisorFeatures.pNext;
            }

            if (physicalDeviceExt.extended_dynamic_state)
            {
                enabledExtensions.push_back(VK_EXT_EXTENDED_DYNAMIC_STATE_EXTENSION_NAME);
                renderer->extendedDynamicStateFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_FEATURES_EXT;
                *features_chain = &renderer->extendedDynamicStateFeatures;
                features_chain = &renderer->extendedDynamicStateFeatures.pNext;
            }

            if (physicalDeviceExt.vertex_input_dynamic_state)
            {
                enabledExtensions.push_back(VK_EXT_VERTEX_INPUT_DYNAMIC_STATE_EXTENSION_NAME);
                renderer->vertexInputDynamicStateFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_INPUT_DYNAMIC_STATE_FEATURES_EXT;
                *features_chain = &renderer->vertexInputDynamicStateFeatures;
                features_chain = &renderer->vertexInputDynamicStateFeatures.pNext;
            }

            if (physicalDeviceExt.extended_dynamic_state2)
            {
                enabledExtensions.push_back(VK_EXT_EXTENDED_DYNAMIC_STATE_2_EXTENSION_NAME);
                renderer->extendedDynamicState2Features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_2_FEATURES_EXT;
                *features_chain = &renderer->extendedDynamicState2Features;
                features_chain = &renderer->extendedDynamicState2Features.pNext;
            }

            // TODO: Enable
            //if (physicalDeviceExt.dynamic_rendering)
            //{
            //    dynamicRenderingFeaturesKHR.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES_KHR;
            //    enabledExtensions.push_back(VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME);
            //    *features_chain = &dynamicRenderingFeaturesKHR;
            //    features_chain = &dynamicRenderingFeaturesKHR.pNext;
            //}

            vkGetPhysicalDeviceProperties2(candidatePhysicalDevice, &renderer->properties2);

            bool discrete = renderer->properties2.properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU;
            if (discrete || renderer->physicalDevice == VK_NULL_HANDLE)
            {
                renderer->physicalDevice = candidatePhysicalDevice;
                if (discrete)
                {
                    break; // if this is discrete GPU, look no further (prioritize discrete GPU)
                }
            }
        }

        if (renderer->physicalDevice == VK_NULL_HANDLE)
        {
            vgfxLogError("Vulkan: Failed to find a suitable GPU");
            delete renderer;
            return nullptr;
        }

        vkGetPhysicalDeviceFeatures2(renderer->physicalDevice, &renderer->features2);

        // Find queue families:
        uint32_t queueFamilyCount = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(renderer->physicalDevice, &queueFamilyCount, nullptr);
        std::vector<VkQueueFamilyProperties> queueFamilies(queueFamilyCount);
        vkGetPhysicalDeviceQueueFamilyProperties(renderer->physicalDevice, &queueFamilyCount, queueFamilies.data());

        std::vector<uint32_t> queueOffsets(queueFamilyCount);
        std::vector<std::vector<float>> queuePriorities(queueFamilyCount);

        uint32_t graphicsQueueIndex = 0;
        uint32_t computeQueueIndex = 0;
        uint32_t copyQueueIndex = 0;

        const auto FindVacantQueue = [&](uint32_t& family, uint32_t& index,
            VkQueueFlags required, VkQueueFlags ignore_flags,
            float priority) -> bool
        {
            for (uint32_t familyIndex = 0; familyIndex < queueFamilyCount; familyIndex++)
            {
                if ((queueFamilies[familyIndex].queueFlags & ignore_flags) != 0)
                    continue;

                // A graphics queue candidate must support present for us to select it.
                if ((required & VK_QUEUE_GRAPHICS_BIT) != 0)
                {
                    VkBool32 supported = VK_FALSE;
                    if (vkGetPhysicalDeviceSurfaceSupportKHR(renderer->physicalDevice, familyIndex, vk_surface, &supported) != VK_SUCCESS || !supported)
                        continue;
                }

                if (queueFamilies[familyIndex].queueCount &&
                    (queueFamilies[familyIndex].queueFlags & required) == required)
                {
                    family = familyIndex;
                    queueFamilies[familyIndex].queueCount--;
                    index = queueOffsets[familyIndex]++;
                    queuePriorities[familyIndex].push_back(priority);
                    return true;
                }
            }

            return false;
        };

        if (!FindVacantQueue(renderer->graphicsQueueFamily, graphicsQueueIndex,
            VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT, 0, 0.5f))
        {
            vgfxLogError("Vulkan: Could not find suitable graphics queue.");
            delete renderer;
            return nullptr;
        }

        // Prefer another graphics queue since we can do async graphics that way.
        // The compute queue is to be treated as high priority since we also do async graphics on it.
        if (!FindVacantQueue(renderer->computeQueueFamily, computeQueueIndex, VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT, 0, 1.0f) &&
            !FindVacantQueue(renderer->computeQueueFamily, computeQueueIndex, VK_QUEUE_COMPUTE_BIT, 0, 1.0f))
        {
            // Fallback to the graphics queue if we must.
            renderer->computeQueueFamily = renderer->graphicsQueueFamily;
            computeQueueIndex = graphicsQueueIndex;
        }

        // For transfer, try to find a queue which only supports transfer, e.g. DMA queue.
        // If not, fallback to a dedicated compute queue.
        // Finally, fallback to same queue as compute.
        if (!FindVacantQueue(renderer->copyQueueFamily, copyQueueIndex, VK_QUEUE_TRANSFER_BIT, VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT, 0.5f) &&
            !FindVacantQueue(renderer->copyQueueFamily, copyQueueIndex, VK_QUEUE_COMPUTE_BIT, VK_QUEUE_GRAPHICS_BIT, 0.5f))
        {
            renderer->copyQueueFamily = renderer->computeQueueFamily;
            copyQueueIndex = computeQueueIndex;
        }

        VkDeviceCreateInfo createInfo{ VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };

        std::vector<VkDeviceQueueCreateInfo> queueInfos;
        for (uint32_t familyIndex = 0; familyIndex < queueFamilyCount; familyIndex++)
        {
            if (queueOffsets[familyIndex] == 0)
                continue;

            VkDeviceQueueCreateInfo info = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
            info.queueFamilyIndex = familyIndex;
            info.queueCount = queueOffsets[familyIndex];
            info.pQueuePriorities = queuePriorities[familyIndex].data();
            queueInfos.push_back(info);
        }

        createInfo.pNext = &renderer->features2;
        createInfo.queueCreateInfoCount = static_cast<uint32_t>(queueInfos.size());
        createInfo.pQueueCreateInfos = queueInfos.data();
        createInfo.enabledLayerCount = 0;
        createInfo.ppEnabledLayerNames = nullptr;
        createInfo.pEnabledFeatures = nullptr;
        createInfo.enabledExtensionCount = static_cast<uint32_t>(enabledExtensions.size());
        createInfo.ppEnabledExtensionNames = enabledExtensions.data();

        VkResult result = vkCreateDevice(renderer->physicalDevice, &createInfo, nullptr, &renderer->device);
        if (result != VK_SUCCESS)
        {
            VK_LOG_ERROR(result, "Cannot create device");
            delete renderer;
            return nullptr;
        }

        volkLoadDevice(renderer->device);

        vkGetDeviceQueue(renderer->device, renderer->graphicsQueueFamily, graphicsQueueIndex, &renderer->graphicsQueue);
        vkGetDeviceQueue(renderer->device, renderer->computeQueueFamily, computeQueueIndex, &renderer->computeQueue);
        vkGetDeviceQueue(renderer->device, renderer->copyQueueFamily, copyQueueIndex, &renderer->copyQueue);

#ifdef _DEBUG
        vgfxLogInfo("Enabled %d Device Extensions:", createInfo.enabledExtensionCount);
        for (uint32_t i = 0; i < createInfo.enabledExtensionCount; ++i)
        {
            vgfxLogInfo("	\t%s", createInfo.ppEnabledExtensionNames[i]);
        }
#endif
    }

    // Create memory allocator
    {
        VmaVulkanFunctions vma_vulkan_func{};
        vma_vulkan_func.vkGetInstanceProcAddr = vkGetInstanceProcAddr;
        vma_vulkan_func.vkGetDeviceProcAddr = vkGetDeviceProcAddr;
        vma_vulkan_func.vkGetPhysicalDeviceProperties = vkGetPhysicalDeviceProperties;
        vma_vulkan_func.vkGetPhysicalDeviceMemoryProperties = vkGetPhysicalDeviceMemoryProperties;
        vma_vulkan_func.vkAllocateMemory = vkAllocateMemory;
        vma_vulkan_func.vkFreeMemory = vkFreeMemory;
        vma_vulkan_func.vkMapMemory = vkMapMemory;
        vma_vulkan_func.vkUnmapMemory = vkUnmapMemory;
        vma_vulkan_func.vkFlushMappedMemoryRanges = vkFlushMappedMemoryRanges;
        vma_vulkan_func.vkInvalidateMappedMemoryRanges = vkInvalidateMappedMemoryRanges;
        vma_vulkan_func.vkBindBufferMemory = vkBindBufferMemory;
        vma_vulkan_func.vkBindImageMemory = vkBindImageMemory;
        vma_vulkan_func.vkGetBufferMemoryRequirements = vkGetBufferMemoryRequirements;
        vma_vulkan_func.vkGetImageMemoryRequirements = vkGetImageMemoryRequirements;
        vma_vulkan_func.vkCreateBuffer = vkCreateBuffer;
        vma_vulkan_func.vkDestroyBuffer = vkDestroyBuffer;
        vma_vulkan_func.vkCreateImage = vkCreateImage;
        vma_vulkan_func.vkDestroyImage = vkDestroyImage;
        vma_vulkan_func.vkCmdCopyBuffer = vkCmdCopyBuffer;

        VmaAllocatorCreateInfo allocatorInfo{};
        allocatorInfo.physicalDevice = renderer->physicalDevice;
        allocatorInfo.device = renderer->device;
        allocatorInfo.instance = renderer->instance;

        // Core in 1.1
        allocatorInfo.flags = VMA_ALLOCATOR_CREATE_KHR_DEDICATED_ALLOCATION_BIT | VMA_ALLOCATOR_CREATE_KHR_BIND_MEMORY2_BIT;
        vma_vulkan_func.vkGetBufferMemoryRequirements2KHR = vkGetBufferMemoryRequirements2;
        vma_vulkan_func.vkGetImageMemoryRequirements2KHR = vkGetImageMemoryRequirements2;

        if (renderer->features_1_2.bufferDeviceAddress == VK_TRUE)
        {
            allocatorInfo.flags |= VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT;
            vma_vulkan_func.vkBindBufferMemory2KHR = vkBindBufferMemory2;
            vma_vulkan_func.vkBindImageMemory2KHR = vkBindImageMemory2;
        }

        allocatorInfo.pVulkanFunctions = &vma_vulkan_func;

        VkResult result = vmaCreateAllocator(&allocatorInfo, &renderer->allocator);

        if (result != VK_SUCCESS)
        {
            VK_LOG_ERROR(result, "Cannot create allocator");
        }
    }

    // Destroy surface
    if (vk_surface != VK_NULL_HANDLE)
    {
        vkDestroySurfaceKHR(renderer->instance, vk_surface, nullptr);
        vk_surface = VK_NULL_HANDLE;
    }

    vgfxLogInfo("vgfx driver: Vulkan");

    VGFXDevice_T* device = (VGFXDevice_T*)VGFX_MALLOC(sizeof(VGFXDevice_T));
    ASSIGN_DRIVER(vulkan);

    device->driverData = (VGFXRenderer*)renderer;
    return device;
}

VGFXDriver vulkan_driver = {
    VGFXAPI_Vulkan,
    vulkan_isSupported,
    vulkan_createDevice
};

#endif /* VGFX_VULKAN_DRIVER */
