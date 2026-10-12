#define VK_NO_PROTOTYPES
#define VK_USE_PLATFORM_WIN32_KHR
#include "VulkanVideo.hpp"

#include <vulkan/vulkan.h>

#include "libretro_vulkan.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

namespace RetroVulkan {

namespace {

#define RANGA_VK_GLOBAL(X) X(vkCreateInstance) X(vkEnumerateInstanceExtensionProperties)
#define RANGA_VK_INSTANCE(X)                                                                                                         \
    X(vkDestroyInstance) X(vkEnumeratePhysicalDevices) X(vkGetPhysicalDeviceProperties) X(vkGetPhysicalDeviceQueueFamilyProperties) \
    X(vkGetPhysicalDeviceSurfaceSupportKHR) X(vkGetPhysicalDeviceSurfaceCapabilitiesKHR) X(vkGetPhysicalDeviceSurfaceFormatsKHR)   \
    X(vkCreateWin32SurfaceKHR) X(vkDestroySurfaceKHR) X(vkCreateDevice) X(vkGetDeviceProcAddr) X(vkGetPhysicalDeviceMemoryProperties)
#define RANGA_VK_DEVICE(X)                                                                                                       \
    X(vkDestroyDevice) X(vkGetDeviceQueue) X(vkCreateSwapchainKHR) X(vkDestroySwapchainKHR) X(vkGetSwapchainImagesKHR)          \
    X(vkAcquireNextImageKHR) X(vkQueuePresentKHR) X(vkCreateCommandPool) X(vkDestroyCommandPool) X(vkAllocateCommandBuffers)     \
    X(vkBeginCommandBuffer) X(vkEndCommandBuffer) X(vkResetCommandBuffer) X(vkCmdPipelineBarrier) X(vkCmdBlitImage)             \
    X(vkCmdClearColorImage) X(vkQueueSubmit) X(vkCreateFence) X(vkDestroyFence) X(vkWaitForFences) X(vkResetFences)             \
    X(vkCreateSemaphore) X(vkDestroySemaphore) X(vkDeviceWaitIdle) X(vkCreateBuffer) X(vkDestroyBuffer) X(vkGetBufferMemoryRequirements) \
    X(vkAllocateMemory) X(vkFreeMemory) X(vkBindBufferMemory) X(vkMapMemory) X(vkCmdCopyImageToBuffer) X(vkQueueWaitIdle)
#define RANGA_VK_DECLARE(name) PFN_##name name = nullptr;
RANGA_VK_GLOBAL(RANGA_VK_DECLARE)
RANGA_VK_INSTANCE(RANGA_VK_DECLARE)
RANGA_VK_DEVICE(RANGA_VK_DECLARE)
#undef RANGA_VK_DECLARE

constexpr uint32_t kFrames = 2;

struct Context {
    LogFunction log = nullptr;
    HMODULE loader = nullptr;
    PFN_vkGetInstanceProcAddr getInstanceProcAddr = nullptr;
    VkInstance instance = VK_NULL_HANDLE;
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    VkPhysicalDevice gpu = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    uint32_t queueFamily = 0;
    bool coreMadeDevice = false;
    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_B8G8R8A8_UNORM;
    VkExtent2D extent{};
    std::vector<VkImage> images;
    std::vector<VkSemaphore> renderDone;
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer commands[kFrames]{};
    VkFence fences[kFrames]{};
    VkSemaphore acquired[kFrames]{};
    uint32_t index = 0;
    std::mutex queueLock;
    // What the core handed over for this frame.
    VkImage coreImage = VK_NULL_HANDLE;
    VkImageLayout coreLayout = VK_IMAGE_LAYOUT_GENERAL;
    uint32_t coreFamily = VK_QUEUE_FAMILY_IGNORED;
    std::vector<VkSemaphore> coreWaits;
    std::vector<VkCommandBuffer> coreCommands;
    VkSemaphore coreSignal = VK_NULL_HANDLE;
    bool needsRecreate = false;
    retro_hw_render_interface_vulkan face{};
    // RANGA_RETRO_PROBE=1: one pixel of the presented image read back every two seconds, for testing without a screenshot.
    bool probe = false;
    ULONGLONG probeAt = 0;
    VkBuffer probeBuffer = VK_NULL_HANDLE;
    VkDeviceMemory probeMemory = VK_NULL_HANDLE;
    const uint8_t* probePixel = nullptr;
    bool loggedImage = false;
} c;

template <typename T> bool LoadInstance(T& function, const char* name) {
    function = reinterpret_cast<T>(c.getInstanceProcAddr(c.instance, name));
    if (function == nullptr) c.log("[retro] Vulkan sem %s\n", name);
    return function != nullptr;
}

template <typename T> bool LoadDevice(T& function, const char* name) {
    function = reinterpret_cast<T>(vkGetDeviceProcAddr(c.device, name));
    if (function == nullptr) c.log("[retro] Vulkan sem %s\n", name);
    return function != nullptr;
}

// ---- the interface the core calls
void RETRO_CALLCONV SetImage(void*, const retro_vulkan_image* image, uint32_t count, const VkSemaphore* semaphores, uint32_t family) {
    if (image != nullptr && !c.loggedImage) {
        c.loggedImage = true;
        c.log("[retro] Vulkan: primeira imagem do core, formato %d, layout %d, fila %u\n", static_cast<int>(image->create_info.format), static_cast<int>(image->image_layout), family);
    }
    c.coreImage = image != nullptr ? image->create_info.image : VK_NULL_HANDLE;
    c.coreLayout = image != nullptr ? image->image_layout : VK_IMAGE_LAYOUT_GENERAL;
    c.coreFamily = family;
    c.coreWaits.assign(semaphores, semaphores + count);
}
uint32_t RETRO_CALLCONV GetSyncIndex(void*) { return c.index; }
uint32_t RETRO_CALLCONV GetSyncIndexMask(void*) { return (1u << kFrames) - 1; }
void RETRO_CALLCONV SetCommandBuffers(void*, uint32_t count, const VkCommandBuffer* commands) { c.coreCommands.assign(commands, commands + count); }
void RETRO_CALLCONV WaitSyncIndex(void*) { vkWaitForFences(c.device, 1, &c.fences[c.index], VK_TRUE, UINT64_MAX); }
void RETRO_CALLCONV LockQueue(void*) { c.queueLock.lock(); }
void RETRO_CALLCONV UnlockQueue(void*) { c.queueLock.unlock(); }
void RETRO_CALLCONV SetSignalSemaphore(void*, VkSemaphore semaphore) { c.coreSignal = semaphore; }

// ---- negotiation wrappers: the core builds the create infos, the frontend adds what presenting needs
VkInstance RETRO_CALLCONV CreateInstanceWrapper(void*, const VkInstanceCreateInfo* info) {
    std::vector<const char*> extensions(info->ppEnabledExtensionNames, info->ppEnabledExtensionNames + info->enabledExtensionCount);
    for (const char* needed : {VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_WIN32_SURFACE_EXTENSION_NAME}) {
        if (std::none_of(extensions.begin(), extensions.end(), [&](const char* e) { return std::strcmp(e, needed) == 0; })) extensions.push_back(needed);
    }
    VkInstanceCreateInfo copy = *info;
    copy.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
    copy.ppEnabledExtensionNames = extensions.data();
    VkInstance instance = VK_NULL_HANDLE;
    return vkCreateInstance(&copy, nullptr, &instance) == VK_SUCCESS ? instance : VK_NULL_HANDLE;
}

VkDevice RETRO_CALLCONV CreateDeviceWrapper(VkPhysicalDevice gpu, void*, const VkDeviceCreateInfo* info) {
    std::vector<const char*> extensions(info->ppEnabledExtensionNames, info->ppEnabledExtensionNames + info->enabledExtensionCount);
    if (std::none_of(extensions.begin(), extensions.end(), [](const char* e) { return std::strcmp(e, VK_KHR_SWAPCHAIN_EXTENSION_NAME) == 0; }))
        extensions.push_back(VK_KHR_SWAPCHAIN_EXTENSION_NAME);
    VkDeviceCreateInfo copy = *info;
    copy.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
    copy.ppEnabledExtensionNames = extensions.data();
    VkDevice device = VK_NULL_HANDLE;
    return vkCreateDevice(gpu, &copy, nullptr, &device) == VK_SUCCESS ? device : VK_NULL_HANDLE;
}

bool CreateInstance(const retro_hw_render_context_negotiation_interface_vulkan* negotiation) {
    VkApplicationInfo defaultApp{VK_STRUCTURE_TYPE_APPLICATION_INFO, nullptr, "Ranga Retro", 1, "Ranga", 1, VK_API_VERSION_1_1};
    const VkApplicationInfo* app = negotiation != nullptr && negotiation->get_application_info != nullptr ? negotiation->get_application_info() : nullptr;
    if (app == nullptr) app = &defaultApp;
    if (negotiation != nullptr && negotiation->interface_version >= 2 && negotiation->create_instance != nullptr) {
        c.instance = negotiation->create_instance(c.getInstanceProcAddr, app, CreateInstanceWrapper, nullptr);
    } else {
        const char* extensions[] = {VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_WIN32_SURFACE_EXTENSION_NAME};
        VkInstanceCreateInfo info{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
        info.pApplicationInfo = app;
        info.enabledExtensionCount = 2;
        info.ppEnabledExtensionNames = extensions;
        c.instance = CreateInstanceWrapper(nullptr, &info);
    }
    return c.instance != VK_NULL_HANDLE;
}

bool PickGpu() {
    uint32_t count = 0;
    vkEnumeratePhysicalDevices(c.instance, &count, nullptr);
    std::vector<VkPhysicalDevice> gpus(count);
    vkEnumeratePhysicalDevices(c.instance, &count, gpus.data());
    for (VkPhysicalDevice gpu : gpus) {
        VkPhysicalDeviceProperties properties{};
        vkGetPhysicalDeviceProperties(gpu, &properties);
        if (c.gpu == VK_NULL_HANDLE || properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) c.gpu = gpu;
        if (properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) break;
    }
    return c.gpu != VK_NULL_HANDLE;
}

bool CreateDevice(const retro_hw_render_context_negotiation_interface_vulkan* negotiation) {
    retro_vulkan_context context{};
    if (negotiation != nullptr && negotiation->interface_version >= 2 && negotiation->create_device2 != nullptr) {
        c.coreMadeDevice = negotiation->create_device2(&context, c.instance, c.gpu, c.surface, c.getInstanceProcAddr, CreateDeviceWrapper, nullptr);
    } else if (negotiation != nullptr && negotiation->create_device != nullptr) {
        const char* extensions[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
        c.coreMadeDevice = negotiation->create_device(&context, c.instance, c.gpu, c.surface, c.getInstanceProcAddr, extensions, 1, nullptr, 0, nullptr);
    }
    if (c.coreMadeDevice) {
        c.gpu = context.gpu;
        c.device = context.device;
        c.queue = context.queue;
        c.queueFamily = context.queue_family_index;
        if (context.presentation_queue != context.queue) c.log("[retro] Vulkan: o core escolheu uma fila de apresentação separada; o Ranga usa a principal\n");
        return true;
    }
    // The frontend's own device: one queue that draws, computes and presents.
    uint32_t count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(c.gpu, &count, nullptr);
    std::vector<VkQueueFamilyProperties> families(count);
    vkGetPhysicalDeviceQueueFamilyProperties(c.gpu, &count, families.data());
    bool found = false;
    for (uint32_t i = 0; i < count && !found; ++i) {
        VkBool32 present = VK_FALSE;
        vkGetPhysicalDeviceSurfaceSupportKHR(c.gpu, i, c.surface, &present);
        const VkQueueFlags needed = VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT;
        if ((families[i].queueFlags & needed) == needed && present) {
            c.queueFamily = i;
            found = true;
        }
    }
    if (!found) return false;
    const float priority = 1.0f;
    VkDeviceQueueCreateInfo queueInfo{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, nullptr, 0, c.queueFamily, 1, &priority};
    const char* extensions[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    VkDeviceCreateInfo info{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    info.queueCreateInfoCount = 1;
    info.pQueueCreateInfos = &queueInfo;
    info.enabledExtensionCount = 1;
    info.ppEnabledExtensionNames = extensions;
    if (vkCreateDevice(c.gpu, &info, nullptr, &c.device) != VK_SUCCESS) return false;
    return true;
}

void DestroySwapchain() {
    for (VkSemaphore semaphore : c.renderDone) vkDestroySemaphore(c.device, semaphore, nullptr);
    c.renderDone.clear();
    c.images.clear();
    if (c.swapchain != VK_NULL_HANDLE) vkDestroySwapchainKHR(c.device, c.swapchain, nullptr);
    c.swapchain = VK_NULL_HANDLE;
}

bool CreateSwapchain() {
    VkSurfaceCapabilitiesKHR caps{};
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(c.gpu, c.surface, &caps);
    if (caps.currentExtent.width == 0 || caps.currentExtent.height == 0) return false;
    uint32_t count = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(c.gpu, c.surface, &count, nullptr);
    std::vector<VkSurfaceFormatKHR> formats(count);
    vkGetPhysicalDeviceSurfaceFormatsKHR(c.gpu, c.surface, &count, formats.data());
    VkSurfaceFormatKHR chosen = formats.empty() ? VkSurfaceFormatKHR{VK_FORMAT_B8G8R8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR} : formats[0];
    for (const auto& format : formats) {
        if (format.format == VK_FORMAT_B8G8R8A8_UNORM) chosen = format;
    }
    c.format = chosen.format;
    c.extent = caps.currentExtent;
    VkSwapchainCreateInfoKHR info{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    info.surface = c.surface;
    info.minImageCount = std::max(caps.minImageCount, 2u) + 1;
    if (caps.maxImageCount != 0) info.minImageCount = std::min(info.minImageCount, caps.maxImageCount);
    info.imageFormat = chosen.format;
    info.imageColorSpace = chosen.colorSpace;
    info.imageExtent = c.extent;
    info.imageArrayLayers = 1;
    info.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | (c.probe ? VK_IMAGE_USAGE_TRANSFER_SRC_BIT : 0);
    info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    info.preTransform = caps.currentTransform;
    info.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    info.presentMode = VK_PRESENT_MODE_FIFO_KHR;
    info.clipped = VK_TRUE;
    if (vkCreateSwapchainKHR(c.device, &info, nullptr, &c.swapchain) != VK_SUCCESS) return false;
    vkGetSwapchainImagesKHR(c.device, c.swapchain, &count, nullptr);
    c.images.resize(count);
    vkGetSwapchainImagesKHR(c.device, c.swapchain, &count, c.images.data());
    c.renderDone.resize(count);
    VkSemaphoreCreateInfo semaphore{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    for (auto& done : c.renderDone) vkCreateSemaphore(c.device, &semaphore, nullptr, &done);
    return true;
}

void Barrier(VkCommandBuffer cmd, VkImage image, VkImageLayout from, VkImageLayout to, VkAccessFlags srcAccess, VkAccessFlags dstAccess,
             VkPipelineStageFlags srcStage, VkPipelineStageFlags dstStage, uint32_t srcFamily = VK_QUEUE_FAMILY_IGNORED, uint32_t dstFamily = VK_QUEUE_FAMILY_IGNORED) {
    VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    barrier.srcAccessMask = srcAccess;
    barrier.dstAccessMask = dstAccess;
    barrier.oldLayout = from;
    barrier.newLayout = to;
    barrier.srcQueueFamilyIndex = srcFamily;
    barrier.dstQueueFamilyIndex = dstFamily;
    barrier.image = image;
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(cmd, srcStage, dstStage, 0, 0, nullptr, 0, nullptr, 1, &barrier);
}

}

bool Create(HWND window, const retro_hw_render_context_negotiation_interface_vulkan* negotiation, LogFunction log) {
    c.log = log;
    c.loader = LoadLibraryW(L"vulkan-1.dll");
    if (c.loader == nullptr) {
        log("[retro] Vulkan: vulkan-1.dll não encontrada\n");
        return false;
    }
    c.getInstanceProcAddr = reinterpret_cast<PFN_vkGetInstanceProcAddr>(reinterpret_cast<void*>(GetProcAddress(c.loader, "vkGetInstanceProcAddr")));
#define RANGA_VK_LOAD_GLOBAL(name) name = reinterpret_cast<PFN_##name>(c.getInstanceProcAddr(VK_NULL_HANDLE, #name));
    RANGA_VK_GLOBAL(RANGA_VK_LOAD_GLOBAL)
#undef RANGA_VK_LOAD_GLOBAL
    if (vkCreateInstance == nullptr || !CreateInstance(negotiation)) {
        log("[retro] Vulkan: a instância não foi criada\n");
        return false;
    }
#define RANGA_VK_LOAD_INSTANCE(name) ok &= LoadInstance(name, #name);
    bool ok = true;
    RANGA_VK_INSTANCE(RANGA_VK_LOAD_INSTANCE)
#undef RANGA_VK_LOAD_INSTANCE
    if (!ok) return false;
    VkWin32SurfaceCreateInfoKHR surfaceInfo{VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR, nullptr, 0, GetModuleHandleW(nullptr), window};
    if (vkCreateWin32SurfaceKHR(c.instance, &surfaceInfo, nullptr, &c.surface) != VK_SUCCESS || !PickGpu() || !CreateDevice(negotiation)) {
        log("[retro] Vulkan: superfície ou dispositivo não criados\n");
        return false;
    }
#define RANGA_VK_LOAD_DEVICE(name) ok &= LoadDevice(name, #name);
    RANGA_VK_DEVICE(RANGA_VK_LOAD_DEVICE)
#undef RANGA_VK_LOAD_DEVICE
    if (!ok) return false;
    if (c.queue == VK_NULL_HANDLE) vkGetDeviceQueue(c.device, c.queueFamily, 0, &c.queue);
    VkCommandPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, nullptr, VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, c.queueFamily};
    vkCreateCommandPool(c.device, &poolInfo, nullptr, &c.pool);
    VkCommandBufferAllocateInfo allocate{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, nullptr, c.pool, VK_COMMAND_BUFFER_LEVEL_PRIMARY, kFrames};
    vkAllocateCommandBuffers(c.device, &allocate, c.commands);
    VkFenceCreateInfo fenceInfo{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, nullptr, VK_FENCE_CREATE_SIGNALED_BIT};
    VkSemaphoreCreateInfo semaphoreInfo{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    for (uint32_t i = 0; i < kFrames; ++i) {
        vkCreateFence(c.device, &fenceInfo, nullptr, &c.fences[i]);
        vkCreateSemaphore(c.device, &semaphoreInfo, nullptr, &c.acquired[i]);
    }
    c.probe = std::getenv("RANGA_RETRO_PROBE") != nullptr;
    if (c.probe) {
        VkBufferCreateInfo bufferInfo{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, nullptr, 0, 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, VK_SHARING_MODE_EXCLUSIVE};
        vkCreateBuffer(c.device, &bufferInfo, nullptr, &c.probeBuffer);
        VkMemoryRequirements needs{};
        vkGetBufferMemoryRequirements(c.device, c.probeBuffer, &needs);
        VkPhysicalDeviceMemoryProperties memory{};
        vkGetPhysicalDeviceMemoryProperties(c.gpu, &memory);
        uint32_t type = 0;
        const VkMemoryPropertyFlags wanted = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        for (uint32_t i = 0; i < memory.memoryTypeCount; ++i) {
            if ((needs.memoryTypeBits & (1u << i)) && (memory.memoryTypes[i].propertyFlags & wanted) == wanted) { type = i; break; }
        }
        VkMemoryAllocateInfo allocateInfo{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, nullptr, needs.size, type};
        vkAllocateMemory(c.device, &allocateInfo, nullptr, &c.probeMemory);
        vkBindBufferMemory(c.device, c.probeBuffer, c.probeMemory, 0);
        void* mapped = nullptr;
        vkMapMemory(c.device, c.probeMemory, 0, 4, 0, &mapped);
        c.probePixel = static_cast<const uint8_t*>(mapped);
    }
    if (!CreateSwapchain()) c.needsRecreate = true;
    VkPhysicalDeviceProperties properties{};
    vkGetPhysicalDeviceProperties(c.gpu, &properties);
    log("[retro] Vulkan: %s, fila %u, dispositivo criado pelo %s\n", properties.deviceName, c.queueFamily, c.coreMadeDevice ? "core" : "Ranga");

    c.face.interface_type = RETRO_HW_RENDER_INTERFACE_VULKAN;
    c.face.interface_version = RETRO_HW_RENDER_INTERFACE_VULKAN_VERSION;
    c.face.handle = &c;
    c.face.instance = c.instance;
    c.face.gpu = c.gpu;
    c.face.device = c.device;
    c.face.get_device_proc_addr = vkGetDeviceProcAddr;
    c.face.get_instance_proc_addr = c.getInstanceProcAddr;
    c.face.queue = c.queue;
    c.face.queue_index = c.queueFamily;
    c.face.set_image = SetImage;
    c.face.get_sync_index = GetSyncIndex;
    c.face.get_sync_index_mask = GetSyncIndexMask;
    c.face.set_command_buffers = SetCommandBuffers;
    c.face.wait_sync_index = WaitSyncIndex;
    c.face.lock_queue = LockQueue;
    c.face.unlock_queue = UnlockQueue;
    c.face.set_signal_semaphore = SetSignalSemaphore;
    return true;
}

const retro_hw_render_interface_vulkan* Interface() { return c.device != VK_NULL_HANDLE ? &c.face : nullptr; }

void BeginFrame() {
    c.index = (c.index + 1) % kFrames;
    vkWaitForFences(c.device, 1, &c.fences[c.index], VK_TRUE, UINT64_MAX);
}

void Resize() { c.needsRecreate = true; }

void Present(HWND, unsigned frameWidth, unsigned frameHeight, float aspect) {
    if (c.needsRecreate) {
        vkDeviceWaitIdle(c.device);
        DestroySwapchain();
        if (!CreateSwapchain()) return;
        c.needsRecreate = false;
    }
    if (c.swapchain == VK_NULL_HANDLE) return;
    uint32_t imageIndex = 0;
    VkResult result = vkAcquireNextImageKHR(c.device, c.swapchain, UINT64_MAX, c.acquired[c.index], VK_NULL_HANDLE, &imageIndex);
    if (result == VK_ERROR_OUT_OF_DATE_KHR) {
        c.needsRecreate = true;
        return;
    }
    vkResetFences(c.device, 1, &c.fences[c.index]);
    VkCommandBuffer cmd = c.commands[c.index];
    vkResetCommandBuffer(cmd, 0);
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, nullptr, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    vkBeginCommandBuffer(cmd, &begin);
    VkImage target = c.images[imageIndex];
    Barrier(cmd, target, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    const VkClearColorValue black{{0, 0, 0, 1}};
    const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdClearColorImage(cmd, target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &range);
    if (c.coreImage != VK_NULL_HANDLE && frameWidth != 0 && frameHeight != 0) {
        const float windowWidth = static_cast<float>(c.extent.width), windowHeight = static_cast<float>(c.extent.height);
        if (aspect <= 0) aspect = static_cast<float>(frameWidth) / static_cast<float>(frameHeight);
        float w = windowWidth, h = windowWidth / aspect;
        if (h > windowHeight) {
            h = windowHeight;
            w = h * aspect;
        }
        const int x0 = static_cast<int>((windowWidth - w) / 2), y0 = static_cast<int>((windowHeight - h) / 2);
        // The image changes hands only when the core works on another queue family.
        const bool transfer = c.coreFamily != VK_QUEUE_FAMILY_IGNORED && c.coreFamily != c.queueFamily;
        const uint32_t from = transfer ? c.coreFamily : VK_QUEUE_FAMILY_IGNORED, to = transfer ? c.queueFamily : VK_QUEUE_FAMILY_IGNORED;
        const bool general = c.coreLayout == VK_IMAGE_LAYOUT_GENERAL;
        const VkImageLayout readLayout = general ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        Barrier(cmd, c.coreImage, c.coreLayout, readLayout, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT,
                VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, from, to);
        VkImageBlit blit{};
        blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        blit.srcOffsets[1] = {static_cast<int32_t>(frameWidth), static_cast<int32_t>(frameHeight), 1};
        blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        blit.dstOffsets[0] = {x0, y0, 0};
        blit.dstOffsets[1] = {std::min(x0 + static_cast<int>(w), static_cast<int>(c.extent.width)), std::min(y0 + static_cast<int>(h), static_cast<int>(c.extent.height)), 1};
        vkCmdBlitImage(cmd, c.coreImage, readLayout, target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR);
        Barrier(cmd, c.coreImage, readLayout, c.coreLayout, VK_ACCESS_TRANSFER_READ_BIT, 0, VK_PIPELINE_STAGE_TRANSFER_BIT,
                VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, to, from);
    }
    const bool probeNow = c.probe && c.probeBuffer != VK_NULL_HANDLE && GetTickCount64() - c.probeAt > 2000;
    if (probeNow) {
        Barrier(cmd, target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        VkBufferImageCopy copy{};
        copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        copy.imageOffset = {static_cast<int32_t>(c.extent.width / 2), static_cast<int32_t>(c.extent.height / 2), 0};
        copy.imageExtent = {1, 1, 1};
        vkCmdCopyImageToBuffer(cmd, target, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, c.probeBuffer, 1, &copy);
        Barrier(cmd, target, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_ACCESS_TRANSFER_READ_BIT, 0,
                VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
    } else {
        Barrier(cmd, target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_ACCESS_TRANSFER_WRITE_BIT, 0,
                VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
    }
    vkEndCommandBuffer(cmd);

    std::vector<VkSemaphore> waits{c.acquired[c.index]};
    std::vector<VkPipelineStageFlags> stages{VK_PIPELINE_STAGE_TRANSFER_BIT};
    for (VkSemaphore semaphore : c.coreWaits) {
        waits.push_back(semaphore);
        stages.push_back(VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
    }
    std::vector<VkCommandBuffer> commands = c.coreCommands;
    commands.push_back(cmd);
    std::vector<VkSemaphore> signals{c.renderDone[imageIndex]};
    if (c.coreSignal != VK_NULL_HANDLE) signals.push_back(c.coreSignal);
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.waitSemaphoreCount = static_cast<uint32_t>(waits.size());
    submit.pWaitSemaphores = waits.data();
    submit.pWaitDstStageMask = stages.data();
    submit.commandBufferCount = static_cast<uint32_t>(commands.size());
    submit.pCommandBuffers = commands.data();
    submit.signalSemaphoreCount = static_cast<uint32_t>(signals.size());
    submit.pSignalSemaphores = signals.data();
    {
        std::lock_guard guard(c.queueLock);
        vkQueueSubmit(c.queue, 1, &submit, c.fences[c.index]);
        VkPresentInfoKHR present{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
        present.waitSemaphoreCount = 1;
        present.pWaitSemaphores = &c.renderDone[imageIndex];
        present.swapchainCount = 1;
        present.pSwapchains = &c.swapchain;
        present.pImageIndices = &imageIndex;
        result = vkQueuePresentKHR(c.queue, &present);
    }
    if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR) c.needsRecreate = true;
    if (probeNow) {
        vkWaitForFences(c.device, 1, &c.fences[c.index], VK_TRUE, UINT64_MAX);
        c.probeAt = GetTickCount64();
        c.log("[retro] sonda Vulkan: quadro %ux%u, centro da janela %u,%u,%u\n", frameWidth, frameHeight, c.probePixel[2], c.probePixel[1], c.probePixel[0]);
    }
    // Semaphores and command buffers are used once; the image stays for a duplicated frame.
    c.coreWaits.clear();
    c.coreCommands.clear();
    c.coreSignal = VK_NULL_HANDLE;
}

void Destroy(const retro_hw_render_context_negotiation_interface_vulkan* negotiation) {
    if (c.device != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(c.device);
        DestroySwapchain();
        for (uint32_t i = 0; i < kFrames; ++i) {
            vkDestroyFence(c.device, c.fences[i], nullptr);
            vkDestroySemaphore(c.device, c.acquired[i], nullptr);
        }
        vkDestroyCommandPool(c.device, c.pool, nullptr);
        if (c.probeBuffer != VK_NULL_HANDLE) vkDestroyBuffer(c.device, c.probeBuffer, nullptr);
        if (c.probeMemory != VK_NULL_HANDLE) vkFreeMemory(c.device, c.probeMemory, nullptr);
        if (c.coreMadeDevice && negotiation != nullptr && negotiation->destroy_device != nullptr) negotiation->destroy_device();
        vkDestroyDevice(c.device, nullptr);
        c.device = VK_NULL_HANDLE;
    }
    if (c.surface != VK_NULL_HANDLE) vkDestroySurfaceKHR(c.instance, c.surface, nullptr);
    if (c.instance != VK_NULL_HANDLE) vkDestroyInstance(c.instance, nullptr);
    c.surface = VK_NULL_HANDLE;
    c.instance = VK_NULL_HANDLE;
}

}
