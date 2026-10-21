// The display, driven by the GPU. See include/aios/userland/gpu_present.hpp
// for why this exists and why it never reads a frame back.
//
// The Vulkan ABI is spelled out here rather than included: this file must
// compile on a machine with no Vulkan headers, exactly as the compute backend
// does. The loader is bound at runtime through the host boundary, so a machine
// with no driver simply has no GPU display and the compositor keeps painting.
#include "aios/userland/gpu_present.hpp"
#include "aios/hal/host.hpp"
#include "../compute/shaders/aios_scene_spv.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <cstdio>
#include <vector>

namespace aios::userland {
namespace vkp {

using VkFlags = uint32_t;
using VkBool32 = uint32_t;
using VkDeviceSize = uint64_t;
using VkResult = int32_t;

#define VKP_HANDLE(name) struct name##_T; using name = name##_T*
VKP_HANDLE(VkInstance);
VKP_HANDLE(VkPhysicalDevice);
VKP_HANDLE(VkDevice);
VKP_HANDLE(VkQueue);
VKP_HANDLE(VkSurfaceKHR);
VKP_HANDLE(VkSwapchainKHR);
VKP_HANDLE(VkImage);
VKP_HANDLE(VkBuffer);
VKP_HANDLE(VkImageView);
VKP_HANDLE(VkDeviceMemory);
VKP_HANDLE(VkShaderModule);
VKP_HANDLE(VkPipelineLayout);
VKP_HANDLE(VkPipeline);
VKP_HANDLE(VkPipelineCache);
VKP_HANDLE(VkDescriptorSetLayout);
VKP_HANDLE(VkDescriptorPool);
VKP_HANDLE(VkDescriptorSet);
VKP_HANDLE(VkCommandPool);
VKP_HANDLE(VkCommandBuffer);
VKP_HANDLE(VkFence);
VKP_HANDLE(VkSemaphore);
VKP_HANDLE(VkQueryPool);
VKP_HANDLE(VkBuffer);
#undef VKP_HANDLE

constexpr VkResult OK = 0;
constexpr VkResult SUBOPTIMAL = 1000001003;
constexpr VkResult OUT_OF_DATE = -1000001004;

constexpr uint32_t ST_APPLICATION_INFO = 0;
constexpr uint32_t ST_INSTANCE_CREATE_INFO = 1;
constexpr uint32_t ST_DEVICE_QUEUE_CREATE_INFO = 2;
constexpr uint32_t ST_DEVICE_CREATE_INFO = 3;
constexpr uint32_t ST_SUBMIT_INFO = 4;
constexpr uint32_t ST_MEMORY_ALLOCATE_INFO = 5;
constexpr uint32_t ST_FENCE_CREATE_INFO = 8;
constexpr uint32_t ST_SEMAPHORE_CREATE_INFO = 9;
constexpr uint32_t ST_QUERY_POOL_CREATE_INFO = 11;
constexpr uint32_t ST_BUFFER_CREATE_INFO = 12;
constexpr uint32_t ST_IMAGE_CREATE_INFO = 14;
constexpr uint32_t ST_IMAGE_VIEW_CREATE_INFO = 15;
constexpr uint32_t ST_SHADER_MODULE_CREATE_INFO = 16;
constexpr uint32_t ST_PIPELINE_SHADER_STAGE_CREATE_INFO = 18;
constexpr uint32_t ST_COMPUTE_PIPELINE_CREATE_INFO = 29;
constexpr uint32_t ST_PIPELINE_LAYOUT_CREATE_INFO = 30;
constexpr uint32_t ST_DESCRIPTOR_SET_LAYOUT_CREATE_INFO = 32;
constexpr uint32_t ST_DESCRIPTOR_POOL_CREATE_INFO = 33;
constexpr uint32_t ST_DESCRIPTOR_SET_ALLOCATE_INFO = 34;
constexpr uint32_t ST_WRITE_DESCRIPTOR_SET = 35;
constexpr uint32_t ST_COMMAND_POOL_CREATE_INFO = 39;
constexpr uint32_t ST_COMMAND_BUFFER_ALLOCATE_INFO = 40;
constexpr uint32_t ST_COMMAND_BUFFER_BEGIN_INFO = 42;
constexpr uint32_t ST_IMAGE_MEMORY_BARRIER = 45;
constexpr uint32_t ST_SWAPCHAIN_CREATE_INFO_KHR = 1000001000;
constexpr uint32_t ST_PRESENT_INFO_KHR = 1000001001;

constexpr uint32_t QUEUE_COMPUTE_BIT = 0x2;
constexpr uint32_t IMAGE_USAGE_TRANSFER_SRC = 0x00000001;
constexpr uint32_t IMAGE_USAGE_TRANSFER_DST = 0x00000002;
// 0x8. This was 0x20 — which is DEPTH_STENCIL_ATTACHMENT for an image, and the
// STORAGE bit for a BUFFER, so the value had been taken from the wrong table.
// Every image the shader was meant to write was therefore created WITHOUT
// storage usage, and this driver answers a storage write to such an image by
// dropping it: no error, no warning, a black screen. It also made the swapchain
// support probe below test the wrong bit, which is why this surface looked like
// one that refuses a shader write.
constexpr uint32_t IMAGE_USAGE_STORAGE = 0x00000008;
constexpr uint32_t IMAGE_USAGE_COLOR_ATTACHMENT = 0x00000010;
constexpr uint32_t DESCRIPTOR_TYPE_STORAGE_IMAGE = 3;
constexpr uint32_t DESCRIPTOR_TYPE_STORAGE_BUFFER = 7;
constexpr uint32_t BUFFER_USAGE_STORAGE = 0x00000020;
constexpr uint32_t BUFFER_USAGE_TRANSFER_DST = 0x00000002;
constexpr uint32_t SHADER_STAGE_COMPUTE = 0x00000020;
constexpr uint32_t PIPELINE_BIND_POINT_COMPUTE = 1;
constexpr uint32_t MEMORY_HOST_VISIBLE = 0x2;
constexpr uint32_t MEMORY_HOST_COHERENT = 0x4;
constexpr uint32_t LAYOUT_UNDEFINED = 0;
constexpr uint32_t LAYOUT_GENERAL = 1;
constexpr uint32_t LAYOUT_TRANSFER_SRC = 6;
constexpr uint32_t LAYOUT_TRANSFER_DST = 7;
constexpr uint32_t LAYOUT_PRESENT_SRC = 1000001002;
constexpr uint32_t PIPELINE_STAGE_TOP = 0x00000001;
constexpr uint32_t PIPELINE_STAGE_COMPUTE = 0x00000800;
constexpr uint32_t PIPELINE_STAGE_BOTTOM = 0x00002000;
constexpr uint32_t ACCESS_SHADER_READ  = 0x00000020;
constexpr uint32_t ACCESS_SHADER_WRITE = 0x00000100;
constexpr uint32_t ST_MEMORY_BARRIER = 46;
constexpr uint32_t ACCESS_TRANSFER_READ = 0x00000800;
constexpr uint32_t ACCESS_TRANSFER_WRITE = 0x00001000;
constexpr uint32_t PIPELINE_STAGE_TRANSFER = 0x00001000;
constexpr uint32_t MEMORY_DEVICE_LOCAL = 0x1;
constexpr uint32_t ASPECT_COLOUR = 0x1;
constexpr uint32_t FORMAT_B8G8R8A8_UNORM = 44;
constexpr uint32_t FORMAT_R8G8B8A8_UNORM = 37;
constexpr uint32_t IMAGE_TILING_LINEAR = 1;
constexpr uint32_t IMAGE_TYPE_2D = 1;
constexpr uint32_t IMAGE_VIEW_TYPE_2D = 1;
constexpr uint32_t SHARING_EXCLUSIVE = 0;
constexpr uint32_t PRESENT_MODE_FIFO = 2;
constexpr uint32_t QUERY_TYPE_TIMESTAMP = 2;
constexpr uint32_t COMPOSITE_ALPHA_OPAQUE = 0x1;

struct VkExtent2D { uint32_t width, height; };
struct VkExtent3D { uint32_t width, height, depth; };
struct VkOffset3D { int32_t x, y, z; };
struct VkApplicationInfo {
  uint32_t sType; const void* pNext; const char* pApplicationName; uint32_t applicationVersion;
  const char* pEngineName; uint32_t engineVersion; uint32_t apiVersion;
};
struct VkInstanceCreateInfo {
  uint32_t sType; const void* pNext; VkFlags flags; const VkApplicationInfo* pApplicationInfo;
  uint32_t enabledLayerCount; const char* const* ppEnabledLayerNames;
  uint32_t enabledExtensionCount; const char* const* ppEnabledExtensionNames;
};
struct VkQueueFamilyProperties { VkFlags queueFlags; uint32_t queueCount, timestampValidBits; VkExtent3D granularity; };
struct VkPhysicalDeviceProperties {
  uint32_t apiVersion, driverVersion, vendorID, deviceID, deviceType;
  char deviceName[256]; uint8_t uuid[16];
  // The driver writes limits and sparse properties past here; this backend
  // reads only timestampPeriod, which sits at a known offset inside limits.
  uint8_t limits_and_sparse[2048];
};
struct VkMemoryType { VkFlags propertyFlags; uint32_t heapIndex; };
struct VkMemoryHeap { VkDeviceSize size; VkFlags flags; };
struct VkPhysicalDeviceMemoryProperties {
  uint32_t memoryTypeCount; VkMemoryType memoryTypes[32];
  uint32_t memoryHeapCount; VkMemoryHeap memoryHeaps[16];
};
struct VkDeviceQueueCreateInfo {
  uint32_t sType; const void* pNext; VkFlags flags; uint32_t queueFamilyIndex, queueCount; const float* pQueuePriorities;
};
struct VkDeviceCreateInfo {
  uint32_t sType; const void* pNext; VkFlags flags;
  uint32_t queueCreateInfoCount; const VkDeviceQueueCreateInfo* pQueueCreateInfos;
  uint32_t enabledLayerCount; const char* const* ppEnabledLayerNames;
  uint32_t enabledExtensionCount; const char* const* ppEnabledExtensionNames;
  const void* pEnabledFeatures;
};
struct VkSurfaceCapabilitiesKHR {
  uint32_t minImageCount, maxImageCount;
  VkExtent2D currentExtent, minImageExtent, maxImageExtent;
  uint32_t maxImageArrayLayers, supportedTransforms, currentTransform,
           supportedCompositeAlpha, supportedUsageFlags;
};
struct VkSurfaceFormatKHR { uint32_t format, colorSpace; };
struct VkSwapchainCreateInfoKHR {
  uint32_t sType; const void* pNext; VkFlags flags; VkSurfaceKHR surface;
  uint32_t minImageCount, imageFormat, imageColorSpace;
  VkExtent2D imageExtent; uint32_t imageArrayLayers; VkFlags imageUsage;
  uint32_t imageSharingMode, queueFamilyIndexCount; const uint32_t* pQueueFamilyIndices;
  uint32_t preTransform, compositeAlpha, presentMode; VkBool32 clipped; VkSwapchainKHR oldSwapchain;
};
struct VkComponentMapping { uint32_t r, g, b, a; };
struct VkImageSubresourceRange { VkFlags aspectMask; uint32_t baseMipLevel, levelCount, baseArrayLayer, layerCount; };
struct VkImageViewCreateInfo {
  uint32_t sType; const void* pNext; VkFlags flags; VkImage image; uint32_t viewType, format;
  VkComponentMapping components; VkImageSubresourceRange subresourceRange;
};
struct VkImageCreateInfo {
  uint32_t sType; const void* pNext; VkFlags flags; uint32_t imageType, format;
  VkExtent3D extent; uint32_t mipLevels, arrayLayers, samples, tiling; VkFlags usage;
  uint32_t sharingMode, queueFamilyIndexCount; const uint32_t* pQueueFamilyIndices; uint32_t initialLayout;
};
struct VkMemoryRequirements { VkDeviceSize size, alignment; uint32_t memoryTypeBits; };
struct VkMemoryAllocateInfo { uint32_t sType; const void* pNext; VkDeviceSize allocationSize; uint32_t memoryTypeIndex; };
struct VkSubresourceLayout { VkDeviceSize offset, size, rowPitch, arrayPitch, depthPitch; };
struct VkImageSubresource { VkFlags aspectMask; uint32_t mipLevel, arrayLayer; };
struct VkImageSubresourceLayers { VkFlags aspectMask; uint32_t mipLevel, baseArrayLayer, layerCount; };
struct VkBufferImageCopy {
  VkDeviceSize bufferOffset; uint32_t bufferRowLength, bufferImageHeight;
  VkImageSubresourceLayers imageSubresource; VkOffset3D imageOffset; VkExtent3D imageExtent;
};
struct VkImageCopy {
  VkImageSubresourceLayers srcSubresource; VkOffset3D srcOffset;
  VkImageSubresourceLayers dstSubresource; VkOffset3D dstOffset; VkExtent3D extent;
};
struct VkImageBlit {
  VkImageSubresourceLayers srcSubresource; VkOffset3D srcOffsets[2];
  VkImageSubresourceLayers dstSubresource; VkOffset3D dstOffsets[2];
};
struct VkShaderModuleCreateInfo { uint32_t sType; const void* pNext; VkFlags flags; size_t codeSize; const uint32_t* pCode; };
struct VkDescriptorSetLayoutBinding { uint32_t binding, descriptorType, descriptorCount; VkFlags stageFlags; const void* pImmutableSamplers; };
struct VkDescriptorSetLayoutCreateInfo { uint32_t sType; const void* pNext; VkFlags flags; uint32_t bindingCount; const VkDescriptorSetLayoutBinding* pBindings; };
struct VkPushConstantRange { VkFlags stageFlags; uint32_t offset, size; };
struct VkPipelineLayoutCreateInfo {
  uint32_t sType; const void* pNext; VkFlags flags; uint32_t setLayoutCount; const VkDescriptorSetLayout* pSetLayouts;
  uint32_t pushConstantRangeCount; const VkPushConstantRange* pPushConstantRanges;
};
struct VkPipelineShaderStageCreateInfo {
  uint32_t sType; const void* pNext; VkFlags flags; VkFlags stage; VkShaderModule module_; const char* pName; const void* pSpecializationInfo;
};
struct VkComputePipelineCreateInfo {
  uint32_t sType; const void* pNext; VkFlags flags; VkPipelineShaderStageCreateInfo stage;
  VkPipelineLayout layout; VkPipeline basePipelineHandle; int32_t basePipelineIndex;
};
struct VkDescriptorPoolSize { uint32_t type, descriptorCount; };
struct VkDescriptorPoolCreateInfo { uint32_t sType; const void* pNext; VkFlags flags; uint32_t maxSets, poolSizeCount; const VkDescriptorPoolSize* pPoolSizes; };
struct VkDescriptorSetAllocateInfo { uint32_t sType; const void* pNext; VkDescriptorPool descriptorPool; uint32_t descriptorSetCount; const VkDescriptorSetLayout* pSetLayouts; };
struct VkDescriptorImageInfo { const void* sampler; VkImageView imageView; uint32_t imageLayout; };
struct VkDescriptorBufferInfo { VkBuffer buffer; VkDeviceSize offset, range; };
struct VkBufferCreateInfo {
  uint32_t sType; const void* pNext; VkFlags flags; VkDeviceSize size; VkFlags usage;
  uint32_t sharingMode, queueFamilyIndexCount; const uint32_t* pQueueFamilyIndices;
};
struct VkWriteDescriptorSet {
  uint32_t sType; const void* pNext; VkDescriptorSet dstSet; uint32_t dstBinding, dstArrayElement, descriptorCount, descriptorType;
  const VkDescriptorImageInfo* pImageInfo; const VkDescriptorBufferInfo* pBufferInfo; const void* pTexelBufferView;
};
struct VkCommandPoolCreateInfo { uint32_t sType; const void* pNext; VkFlags flags; uint32_t queueFamilyIndex; };
struct VkCommandBufferAllocateInfo { uint32_t sType; const void* pNext; VkCommandPool commandPool; uint32_t level, commandBufferCount; };
struct VkCommandBufferBeginInfo { uint32_t sType; const void* pNext; VkFlags flags; const void* pInheritanceInfo; };
struct VkMemoryBarrier { uint32_t sType; const void* pNext; VkFlags srcAccessMask, dstAccessMask; };
struct VkImageMemoryBarrier {
  uint32_t sType; const void* pNext; VkFlags srcAccessMask, dstAccessMask; uint32_t oldLayout, newLayout;
  uint32_t srcQueueFamilyIndex, dstQueueFamilyIndex; VkImage image; VkImageSubresourceRange subresourceRange;
};
struct VkSubmitInfo {
  uint32_t sType; const void* pNext; uint32_t waitSemaphoreCount; const VkSemaphore* pWaitSemaphores;
  const VkFlags* pWaitDstStageMask; uint32_t commandBufferCount; const VkCommandBuffer* pCommandBuffers;
  uint32_t signalSemaphoreCount; const VkSemaphore* pSignalSemaphores;
};
struct VkPresentInfoKHR {
  uint32_t sType; const void* pNext; uint32_t waitSemaphoreCount; const VkSemaphore* pWaitSemaphores;
  uint32_t swapchainCount; const VkSwapchainKHR* pSwapchains; const uint32_t* pImageIndices; VkResult* pResults;
};
struct VkFenceCreateInfo { uint32_t sType; const void* pNext; VkFlags flags; };
struct VkSemaphoreCreateInfo { uint32_t sType; const void* pNext; VkFlags flags; };
struct VkQueryPoolCreateInfo { uint32_t sType; const void* pNext; VkFlags flags; uint32_t queryType, queryCount; VkFlags pipelineStatistics; };

struct Api {
  void* lib = nullptr;
  bool bound = false;
  std::string why;

  VkResult (*CreateInstance)(const VkInstanceCreateInfo*, const void*, VkInstance*) = nullptr;
  void (*DestroyInstance)(VkInstance, const void*) = nullptr;
  VkResult (*EnumeratePhysicalDevices)(VkInstance, uint32_t*, VkPhysicalDevice*) = nullptr;
  void (*GetPhysicalDeviceProperties)(VkPhysicalDevice, VkPhysicalDeviceProperties*) = nullptr;
  void (*GetPhysicalDeviceQueueFamilyProperties)(VkPhysicalDevice, uint32_t*, VkQueueFamilyProperties*) = nullptr;
  void (*GetPhysicalDeviceMemoryProperties)(VkPhysicalDevice, VkPhysicalDeviceMemoryProperties*) = nullptr;
  VkResult (*GetPhysicalDeviceSurfaceSupportKHR)(VkPhysicalDevice, uint32_t, VkSurfaceKHR, VkBool32*) = nullptr;
  VkResult (*GetPhysicalDeviceSurfaceCapabilitiesKHR)(VkPhysicalDevice, VkSurfaceKHR, VkSurfaceCapabilitiesKHR*) = nullptr;
  VkResult (*GetPhysicalDeviceSurfaceFormatsKHR)(VkPhysicalDevice, VkSurfaceKHR, uint32_t*, VkSurfaceFormatKHR*) = nullptr;
  VkResult (*CreateDevice)(VkPhysicalDevice, const VkDeviceCreateInfo*, const void*, VkDevice*) = nullptr;
  void (*DestroyDevice)(VkDevice, const void*) = nullptr;
  void (*GetDeviceQueue)(VkDevice, uint32_t, uint32_t, VkQueue*) = nullptr;
  VkResult (*CreateSwapchainKHR)(VkDevice, const VkSwapchainCreateInfoKHR*, const void*, VkSwapchainKHR*) = nullptr;
  void (*DestroySwapchainKHR)(VkDevice, VkSwapchainKHR, const void*) = nullptr;
  VkResult (*GetSwapchainImagesKHR)(VkDevice, VkSwapchainKHR, uint32_t*, VkImage*) = nullptr;
  VkResult (*AcquireNextImageKHR)(VkDevice, VkSwapchainKHR, uint64_t, VkSemaphore, VkFence, uint32_t*) = nullptr;
  VkResult (*QueuePresentKHR)(VkQueue, const VkPresentInfoKHR*) = nullptr;
  VkResult (*CreateImageView)(VkDevice, const VkImageViewCreateInfo*, const void*, VkImageView*) = nullptr;
  void (*DestroyImageView)(VkDevice, VkImageView, const void*) = nullptr;
  void (*CmdCopyImageToBuffer)(VkCommandBuffer, VkImage, uint32_t, VkBuffer, uint32_t, const void*) = nullptr;
  VkResult (*CreateBuffer)(VkDevice, const VkBufferCreateInfo*, const void*, VkBuffer*) = nullptr;
  void (*DestroyBuffer)(VkDevice, VkBuffer, const void*) = nullptr;
  void (*GetBufferMemoryRequirements)(VkDevice, VkBuffer, VkMemoryRequirements*) = nullptr;
  VkResult (*BindBufferMemory)(VkDevice, VkBuffer, VkDeviceMemory, VkDeviceSize) = nullptr;
  VkResult (*CreateImage)(VkDevice, const VkImageCreateInfo*, const void*, VkImage*) = nullptr;
  void (*DestroyImage)(VkDevice, VkImage, const void*) = nullptr;
  void (*GetImageMemoryRequirements)(VkDevice, VkImage, VkMemoryRequirements*) = nullptr;
  void (*GetImageSubresourceLayout)(VkDevice, VkImage, const VkImageSubresource*, VkSubresourceLayout*) = nullptr;
  VkResult (*BindImageMemory)(VkDevice, VkImage, VkDeviceMemory, VkDeviceSize) = nullptr;
  VkResult (*AllocateMemory)(VkDevice, const VkMemoryAllocateInfo*, const void*, VkDeviceMemory*) = nullptr;
  void (*FreeMemory)(VkDevice, VkDeviceMemory, const void*) = nullptr;
  VkResult (*MapMemory)(VkDevice, VkDeviceMemory, VkDeviceSize, VkDeviceSize, VkFlags, void**) = nullptr;
  void (*UnmapMemory)(VkDevice, VkDeviceMemory) = nullptr;
  VkResult (*CreateShaderModule)(VkDevice, const VkShaderModuleCreateInfo*, const void*, VkShaderModule*) = nullptr;
  void (*DestroyShaderModule)(VkDevice, VkShaderModule, const void*) = nullptr;
  VkResult (*CreateDescriptorSetLayout)(VkDevice, const VkDescriptorSetLayoutCreateInfo*, const void*, VkDescriptorSetLayout*) = nullptr;
  void (*DestroyDescriptorSetLayout)(VkDevice, VkDescriptorSetLayout, const void*) = nullptr;
  VkResult (*CreatePipelineLayout)(VkDevice, const VkPipelineLayoutCreateInfo*, const void*, VkPipelineLayout*) = nullptr;
  void (*DestroyPipelineLayout)(VkDevice, VkPipelineLayout, const void*) = nullptr;
  VkResult (*CreateComputePipelines)(VkDevice, VkPipelineCache, uint32_t, const VkComputePipelineCreateInfo*, const void*, VkPipeline*) = nullptr;
  void (*DestroyPipeline)(VkDevice, VkPipeline, const void*) = nullptr;
  VkResult (*CreateDescriptorPool)(VkDevice, const VkDescriptorPoolCreateInfo*, const void*, VkDescriptorPool*) = nullptr;
  void (*DestroyDescriptorPool)(VkDevice, VkDescriptorPool, const void*) = nullptr;
  VkResult (*AllocateDescriptorSets)(VkDevice, const VkDescriptorSetAllocateInfo*, VkDescriptorSet*) = nullptr;
  void (*UpdateDescriptorSets)(VkDevice, uint32_t, const VkWriteDescriptorSet*, uint32_t, const void*) = nullptr;
  VkResult (*CreateCommandPool)(VkDevice, const VkCommandPoolCreateInfo*, const void*, VkCommandPool*) = nullptr;
  void (*DestroyCommandPool)(VkDevice, VkCommandPool, const void*) = nullptr;
  VkResult (*AllocateCommandBuffers)(VkDevice, const VkCommandBufferAllocateInfo*, VkCommandBuffer*) = nullptr;
  VkResult (*BeginCommandBuffer)(VkCommandBuffer, const VkCommandBufferBeginInfo*) = nullptr;
  VkResult (*EndCommandBuffer)(VkCommandBuffer) = nullptr;
  VkResult (*ResetCommandBuffer)(VkCommandBuffer, VkFlags) = nullptr;
  void (*CmdClearColorImage)(VkCommandBuffer, VkImage, uint32_t, const float*, uint32_t, const VkImageSubresourceRange*) = nullptr;
  void (*CmdBindPipeline)(VkCommandBuffer, uint32_t, VkPipeline) = nullptr;
  void (*CmdBindDescriptorSets)(VkCommandBuffer, uint32_t, VkPipelineLayout, uint32_t, uint32_t, const VkDescriptorSet*, uint32_t, const uint32_t*) = nullptr;
  void (*CmdPushConstants)(VkCommandBuffer, VkPipelineLayout, VkFlags, uint32_t, uint32_t, const void*) = nullptr;
  void (*CmdDispatch)(VkCommandBuffer, uint32_t, uint32_t, uint32_t) = nullptr;
  void (*CmdPipelineBarrier)(VkCommandBuffer, VkFlags, VkFlags, VkFlags, uint32_t, const void*, uint32_t, const void*, uint32_t, const VkImageMemoryBarrier*) = nullptr;
  void (*CmdCopyImage)(VkCommandBuffer, VkImage, uint32_t, VkImage, uint32_t, uint32_t, const VkImageCopy*) = nullptr;
  void (*CmdBlitImage)(VkCommandBuffer, VkImage, uint32_t, VkImage, uint32_t, uint32_t, const VkImageBlit*, uint32_t) = nullptr;
  void (*CmdWriteTimestamp)(VkCommandBuffer, VkFlags, VkQueryPool, uint32_t) = nullptr;
  void (*CmdResetQueryPool)(VkCommandBuffer, VkQueryPool, uint32_t, uint32_t) = nullptr;
  VkResult (*CreateQueryPool)(VkDevice, const VkQueryPoolCreateInfo*, const void*, VkQueryPool*) = nullptr;
  void (*DestroyQueryPool)(VkDevice, VkQueryPool, const void*) = nullptr;
  VkResult (*GetQueryPoolResults)(VkDevice, VkQueryPool, uint32_t, uint32_t, size_t, void*, VkDeviceSize, VkFlags) = nullptr;
  VkResult (*CreateFence)(VkDevice, const VkFenceCreateInfo*, const void*, VkFence*) = nullptr;
  void (*DestroyFence)(VkDevice, VkFence, const void*) = nullptr;
  VkResult (*WaitForFences)(VkDevice, uint32_t, const VkFence*, VkBool32, uint64_t) = nullptr;
  VkResult (*ResetFences)(VkDevice, uint32_t, const VkFence*) = nullptr;
  VkResult (*CreateSemaphore)(VkDevice, const VkSemaphoreCreateInfo*, const void*, VkSemaphore*) = nullptr;
  void (*DestroySemaphore)(VkDevice, VkSemaphore, const void*) = nullptr;
  VkResult (*QueueSubmit)(VkQueue, uint32_t, const VkSubmitInfo*, VkFence) = nullptr;
  VkResult (*DeviceWaitIdle)(VkDevice) = nullptr;

  template <typename Fn>
  bool bind(Fn& slot, const char* name) {
    slot = reinterpret_cast<Fn>(hal::library_symbol(lib, name));
    return slot != nullptr;
  }

  bool load() {
    if (bound || !why.empty()) return bound;
    for (const char* n : {"libvulkan.so.1", "libvulkan.so", "libMoltenVK.dylib", "libvulkan.1.dylib", "vulkan-1.dll"}) {
      lib = hal::load_library(n);
      if (lib) break;
    }
    if (!lib) { why = "no Vulkan loader on this machine"; return false; }
    bool ok = true;
    ok &= bind(CreateInstance, "vkCreateInstance");
    ok &= bind(DestroyInstance, "vkDestroyInstance");
    ok &= bind(EnumeratePhysicalDevices, "vkEnumeratePhysicalDevices");
    ok &= bind(GetPhysicalDeviceProperties, "vkGetPhysicalDeviceProperties");
    ok &= bind(GetPhysicalDeviceQueueFamilyProperties, "vkGetPhysicalDeviceQueueFamilyProperties");
    ok &= bind(GetPhysicalDeviceMemoryProperties, "vkGetPhysicalDeviceMemoryProperties");
    ok &= bind(GetPhysicalDeviceSurfaceSupportKHR, "vkGetPhysicalDeviceSurfaceSupportKHR");
    ok &= bind(GetPhysicalDeviceSurfaceCapabilitiesKHR, "vkGetPhysicalDeviceSurfaceCapabilitiesKHR");
    ok &= bind(GetPhysicalDeviceSurfaceFormatsKHR, "vkGetPhysicalDeviceSurfaceFormatsKHR");
    ok &= bind(CreateDevice, "vkCreateDevice");
    ok &= bind(DestroyDevice, "vkDestroyDevice");
    ok &= bind(GetDeviceQueue, "vkGetDeviceQueue");
    ok &= bind(CreateSwapchainKHR, "vkCreateSwapchainKHR");
    ok &= bind(DestroySwapchainKHR, "vkDestroySwapchainKHR");
    ok &= bind(GetSwapchainImagesKHR, "vkGetSwapchainImagesKHR");
    ok &= bind(AcquireNextImageKHR, "vkAcquireNextImageKHR");
    ok &= bind(QueuePresentKHR, "vkQueuePresentKHR");
    ok &= bind(CreateImageView, "vkCreateImageView");
    ok &= bind(DestroyImageView, "vkDestroyImageView");
    ok &= bind(CmdCopyImageToBuffer, "vkCmdCopyImageToBuffer");
    ok &= bind(CreateBuffer, "vkCreateBuffer");
    ok &= bind(DestroyBuffer, "vkDestroyBuffer");
    ok &= bind(GetBufferMemoryRequirements, "vkGetBufferMemoryRequirements");
    ok &= bind(BindBufferMemory, "vkBindBufferMemory");
    ok &= bind(CreateImage, "vkCreateImage");
    ok &= bind(DestroyImage, "vkDestroyImage");
    ok &= bind(GetImageMemoryRequirements, "vkGetImageMemoryRequirements");
    ok &= bind(GetImageSubresourceLayout, "vkGetImageSubresourceLayout");
    ok &= bind(BindImageMemory, "vkBindImageMemory");
    ok &= bind(AllocateMemory, "vkAllocateMemory");
    ok &= bind(FreeMemory, "vkFreeMemory");
    ok &= bind(MapMemory, "vkMapMemory");
    ok &= bind(UnmapMemory, "vkUnmapMemory");
    ok &= bind(CreateShaderModule, "vkCreateShaderModule");
    ok &= bind(DestroyShaderModule, "vkDestroyShaderModule");
    ok &= bind(CreateDescriptorSetLayout, "vkCreateDescriptorSetLayout");
    ok &= bind(DestroyDescriptorSetLayout, "vkDestroyDescriptorSetLayout");
    ok &= bind(CreatePipelineLayout, "vkCreatePipelineLayout");
    ok &= bind(DestroyPipelineLayout, "vkDestroyPipelineLayout");
    ok &= bind(CreateComputePipelines, "vkCreateComputePipelines");
    ok &= bind(DestroyPipeline, "vkDestroyPipeline");
    ok &= bind(CreateDescriptorPool, "vkCreateDescriptorPool");
    ok &= bind(DestroyDescriptorPool, "vkDestroyDescriptorPool");
    ok &= bind(AllocateDescriptorSets, "vkAllocateDescriptorSets");
    ok &= bind(UpdateDescriptorSets, "vkUpdateDescriptorSets");
    ok &= bind(CreateCommandPool, "vkCreateCommandPool");
    ok &= bind(DestroyCommandPool, "vkDestroyCommandPool");
    ok &= bind(AllocateCommandBuffers, "vkAllocateCommandBuffers");
    ok &= bind(BeginCommandBuffer, "vkBeginCommandBuffer");
    ok &= bind(EndCommandBuffer, "vkEndCommandBuffer");
    ok &= bind(ResetCommandBuffer, "vkResetCommandBuffer");
    ok &= bind(CmdClearColorImage, "vkCmdClearColorImage");
    ok &= bind(CmdBindPipeline, "vkCmdBindPipeline");
    ok &= bind(CmdBindDescriptorSets, "vkCmdBindDescriptorSets");
    ok &= bind(CmdPushConstants, "vkCmdPushConstants");
    ok &= bind(CmdDispatch, "vkCmdDispatch");
    ok &= bind(CmdPipelineBarrier, "vkCmdPipelineBarrier");
    ok &= bind(CmdCopyImage, "vkCmdCopyImage");
    ok &= bind(CmdBlitImage, "vkCmdBlitImage");
    bind(CmdWriteTimestamp, "vkCmdWriteTimestamp");
    bind(CmdResetQueryPool, "vkCmdResetQueryPool");
    bind(CreateQueryPool, "vkCreateQueryPool");
    bind(DestroyQueryPool, "vkDestroyQueryPool");
    bind(GetQueryPoolResults, "vkGetQueryPoolResults");
    ok &= bind(CreateFence, "vkCreateFence");
    ok &= bind(DestroyFence, "vkDestroyFence");
    ok &= bind(WaitForFences, "vkWaitForFences");
    ok &= bind(ResetFences, "vkResetFences");
    ok &= bind(CreateSemaphore, "vkCreateSemaphore");
    ok &= bind(DestroySemaphore, "vkDestroySemaphore");
    ok &= bind(QueueSubmit, "vkQueueSubmit");
    ok &= bind(DeviceWaitIdle, "vkDeviceWaitIdle");
    if (!ok) { why = "the Vulkan loader is here but does not export the core and swapchain calls this needs"; return false; }
    bound = true;
    return true;
  }
};

Api& api() { static Api a; return a; }

struct Push {
  uint32_t width, height, scene, have_overlay, bgra;
  float backdrop;
  float seconds;
  uint32_t pass;     // 0 = march the scene, 1 = composite the frame
  uint32_t scale;    // the background is marched at 1/scale in each axis
  uint32_t cache_w;  // ceil(width / scale)
  uint32_t cache_h;
  float tod;         // the hour on the shoreline; scene 0 alone reads it
};

}  // namespace vkp

using namespace vkp;

struct GpuPresenter::Vk {
  VkInstance instance = nullptr;
  VkSurfaceKHR surface = nullptr;
  VkPhysicalDevice physical = nullptr;
  VkDevice device = nullptr;
  VkQueue queue = nullptr;
  uint32_t family = 0;
  VkSwapchainKHR swapchain = nullptr;
  uint32_t format = FORMAT_B8G8R8A8_UNORM;
  std::vector<VkImage> images;
  std::vector<VkImageView> views;
  std::vector<VkDescriptorSet> sets;
  VkShaderModule shader = nullptr;
  VkDescriptorSetLayout set_layout = nullptr;
  VkPipelineLayout layout = nullptr;
  VkPipeline pipeline = nullptr;
  VkDescriptorPool pool = nullptr;
  VkCommandPool cmd_pool = nullptr;
  VkCommandBuffer cmd = nullptr;
  VkFence fence = nullptr;
  VkSemaphore acquired = nullptr, rendered = nullptr;
  VkPhysicalDeviceMemoryProperties memory{};

  // The compositor's own layer, in memory the host can write and the shader
  // can read. A BUFFER, not an image: a linear image with storage usage is not
  // something every driver will make, and NVIDIA will not. Written only when
  // the UI has actually changed.
  VkBuffer overlay = nullptr;
  VkDeviceMemory overlay_memory = nullptr;
  void* overlay_mapped = nullptr;
  VkDeviceSize overlay_bytes = 0;
  uint64_t overlay_generation = ~0ull;
  // The composed scene's description. Fixed size — it is a header and a bounded
  // number of solids, not a per-pixel buffer, so it does not follow the display.
  // The last scene the shader marched, at display size, device-local. The
  // background is the expensive layer and the slowest-changing one, so it is
  // computed at its own rate and read back on the frames between. Nothing on
  // the host ever touches this: it is written and read by the same shader.
  VkBuffer cache = nullptr;
  VkDeviceMemory cache_memory = nullptr;
  VkDeviceSize cache_bytes = 0;
  VkBuffer scene_buf = nullptr;
  VkDeviceMemory scene_memory = nullptr;
  void* scene_mapped = nullptr;
  VkDeviceSize scene_bytes = 0;
  // The description itself, kept on this side too. The buffer is made when the
  // swapchain is, which is on the FIRST PRESENT and again after every resize —
  // so a description handed over before that, or held across a resize, has to
  // be re-applied to the new buffer or the scene silently goes empty.
  std::vector<float> scene_desc;

  // When the surface will not let a shader write its images (most will not),
  // the shader writes THIS instead and the frame is copied into the swapchain
  // image on the device. That copy never leaves the GPU, so the property that
  // matters — nothing crosses PCIe — is kept either way.
  bool direct_to_swapchain = false;
  VkImage offscreen = nullptr;
  VkImageView offscreen_view = nullptr;
  VkDeviceMemory offscreen_memory = nullptr;
  VkDescriptorSet offscreen_set = nullptr;

  uint32_t width = 0, height = 0;
  uint32_t last_index = 0;      // the image the last frame was presented from
  bool have_frame = false;
  bool needs_rebuild = false;
};

GpuPresenter::GpuPresenter() : vk_(std::make_unique<Vk>()) {}

GpuPresenter::~GpuPresenter() {
  if (!vk_ || !vk_->device) return;
  auto& a = api();
  a.DeviceWaitIdle(vk_->device);
  if (vk_->overlay_mapped) a.UnmapMemory(vk_->device, vk_->overlay_memory);
  if (vk_->overlay) a.DestroyBuffer(vk_->device, vk_->overlay, nullptr);
  if (vk_->overlay_memory) a.FreeMemory(vk_->device, vk_->overlay_memory, nullptr);
  if (vk_->scene_mapped) a.UnmapMemory(vk_->device, vk_->scene_memory);
  if (vk_->cache) a.DestroyBuffer(vk_->device, vk_->cache, nullptr);
  if (vk_->cache_memory) a.FreeMemory(vk_->device, vk_->cache_memory, nullptr);
  if (vk_->scene_buf) a.DestroyBuffer(vk_->device, vk_->scene_buf, nullptr);
  if (vk_->scene_memory) a.FreeMemory(vk_->device, vk_->scene_memory, nullptr);
  if (vk_->offscreen_view) a.DestroyImageView(vk_->device, vk_->offscreen_view, nullptr);
  if (vk_->offscreen) a.DestroyImage(vk_->device, vk_->offscreen, nullptr);
  if (vk_->offscreen_memory) a.FreeMemory(vk_->device, vk_->offscreen_memory, nullptr);
  for (auto v : vk_->views) a.DestroyImageView(vk_->device, v, nullptr);
  if (vk_->swapchain) a.DestroySwapchainKHR(vk_->device, vk_->swapchain, nullptr);
  if (vk_->acquired) a.DestroySemaphore(vk_->device, vk_->acquired, nullptr);
  if (vk_->rendered) a.DestroySemaphore(vk_->device, vk_->rendered, nullptr);
  if (vk_->fence) a.DestroyFence(vk_->device, vk_->fence, nullptr);
  if (vk_->cmd_pool) a.DestroyCommandPool(vk_->device, vk_->cmd_pool, nullptr);
  if (vk_->pool) a.DestroyDescriptorPool(vk_->device, vk_->pool, nullptr);
  if (vk_->pipeline) a.DestroyPipeline(vk_->device, vk_->pipeline, nullptr);
  if (vk_->layout) a.DestroyPipelineLayout(vk_->device, vk_->layout, nullptr);
  if (vk_->set_layout) a.DestroyDescriptorSetLayout(vk_->device, vk_->set_layout, nullptr);
  if (vk_->shader) a.DestroyShaderModule(vk_->device, vk_->shader, nullptr);
  a.DestroyDevice(vk_->device, nullptr);
}

Result<void*> GpuPresenter::make_instance(const char* const* extensions, uint32_t count, std::string& why) {
  auto& a = api();
  if (!a.load()) { why = a.why; return Result<void*>(Status::Unsupported, why); }
  VkApplicationInfo app{};
  app.sType = ST_APPLICATION_INFO;
  app.pApplicationName = "aios";
  app.pEngineName = "aios";
  app.apiVersion = (1u << 22);
  VkInstanceCreateInfo ici{};
  ici.sType = ST_INSTANCE_CREATE_INFO;
  ici.pApplicationInfo = &app;
  ici.enabledExtensionCount = count;
  ici.ppEnabledExtensionNames = extensions;
  VkInstance inst = nullptr;
  if (a.CreateInstance(&ici, nullptr, &inst) != OK) {
    why = "vkCreateInstance refused the window system's extensions";
    return Result<void*>(Status::Unsupported, why);
  }
  return (void*)inst;
}

void GpuPresenter::resize(uint32_t width, uint32_t height) {
  if (!vk_) return;
  vk_->width = width;
  vk_->height = height;
  vk_->needs_rebuild = true;
}

namespace {

uint32_t pick_memory(const vkp::VkPhysicalDeviceMemoryProperties& props, uint32_t bits, uint32_t want) {
  for (uint32_t i = 0; i < props.memoryTypeCount; ++i) {
    if (!(bits & (1u << i))) continue;
    if ((props.memoryTypes[i].propertyFlags & want) == want) return i;
  }
  return UINT32_MAX;
}

}  // namespace

Result<void> GpuPresenter::attach(void* instance, void* surface, uint32_t width, uint32_t height) {
  auto& a = api();
  if (!a.load()) { why_ = a.why; return Result<void>(Status::Unsupported, why_); }
  vk_->instance = (VkInstance)instance;
  vk_->surface = (VkSurfaceKHR)surface;
  vk_->width = width;
  vk_->height = height;

  uint32_t count = 0;
  a.EnumeratePhysicalDevices(vk_->instance, &count, nullptr);
  if (!count) { why_ = "the Vulkan loader reports no physical devices"; return Result<void>(Status::NotFound, why_); }
  std::vector<VkPhysicalDevice> devices(count);
  a.EnumeratePhysicalDevices(vk_->instance, &count, devices.data());

  VkPhysicalDeviceProperties props{};
  bool found = false;
  for (VkPhysicalDevice pd : devices) {
    uint32_t families = 0;
    a.GetPhysicalDeviceQueueFamilyProperties(pd, &families, nullptr);
    std::vector<VkQueueFamilyProperties> qs(families);
    a.GetPhysicalDeviceQueueFamilyProperties(pd, &families, qs.data());
    for (uint32_t i = 0; i < families; ++i) {
      if (!(qs[i].queueFlags & QUEUE_COMPUTE_BIT)) continue;
      VkBool32 can_present = 0;
      a.GetPhysicalDeviceSurfaceSupportKHR(pd, i, vk_->surface, &can_present);
      if (!can_present) continue;
      vk_->physical = pd;
      vk_->family = i;
      a.GetPhysicalDeviceProperties(pd, &props);
      found = true;
      break;
    }
    if (found) break;
  }
  if (!found) {
    why_ = "no Vulkan device here can both compute and present to this window";
    return Result<void>(Status::Unsupported, why_);
  }
  device_name_ = props.deviceName;
  a.GetPhysicalDeviceMemoryProperties(vk_->physical, &vk_->memory);

  float priority = 1.0f;
  VkDeviceQueueCreateInfo qci{};
  qci.sType = ST_DEVICE_QUEUE_CREATE_INFO;
  qci.queueFamilyIndex = vk_->family;
  qci.queueCount = 1;
  qci.pQueuePriorities = &priority;
  const char* device_ext[] = {"VK_KHR_swapchain"};
  VkDeviceCreateInfo dci{};
  dci.sType = ST_DEVICE_CREATE_INFO;
  dci.queueCreateInfoCount = 1;
  dci.pQueueCreateInfos = &qci;
  dci.enabledExtensionCount = 1;
  dci.ppEnabledExtensionNames = device_ext;
  if (a.CreateDevice(vk_->physical, &dci, nullptr, &vk_->device) != OK) {
    why_ = "vkCreateDevice refused, so this machine cannot present through Vulkan";
    return Result<void>(Status::Unsupported, why_);
  }
  a.GetDeviceQueue(vk_->device, vk_->family, 0, &vk_->queue);

  VkShaderModuleCreateInfo smci{};
  smci.sType = ST_SHADER_MODULE_CREATE_INFO;
  smci.codeSize = compute::kAiosSceneSpvWords * sizeof(uint32_t);
  smci.pCode = compute::kAiosSceneSpv;
  if (a.CreateShaderModule(vk_->device, &smci, nullptr, &vk_->shader) != OK) {
    why_ = "the driver rejected the scene shader";
    return Result<void>(Status::Unsupported, why_);
  }

  // 0 is the image being presented; 1 is the compositor's layer, a buffer.
  VkDescriptorSetLayoutBinding bindings[4]{};
  bindings[0].binding = 0;
  bindings[0].descriptorType = DESCRIPTOR_TYPE_STORAGE_IMAGE;
  bindings[0].descriptorCount = 1;
  bindings[0].stageFlags = SHADER_STAGE_COMPUTE;
  bindings[1].binding = 1;
  bindings[1].descriptorType = DESCRIPTOR_TYPE_STORAGE_BUFFER;
  bindings[1].descriptorCount = 1;
  bindings[1].stageFlags = SHADER_STAGE_COMPUTE;
  bindings[2].binding = 2;
  bindings[2].descriptorType = DESCRIPTOR_TYPE_STORAGE_BUFFER;
  bindings[2].descriptorCount = 1;
  bindings[2].stageFlags = SHADER_STAGE_COMPUTE;
  bindings[3].binding = 3;
  bindings[3].descriptorType = DESCRIPTOR_TYPE_STORAGE_BUFFER;
  bindings[3].descriptorCount = 1;
  bindings[3].stageFlags = SHADER_STAGE_COMPUTE;
  VkDescriptorSetLayoutCreateInfo dslci{};
  dslci.sType = ST_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
  dslci.bindingCount = 4;
  dslci.pBindings = bindings;
  if (a.CreateDescriptorSetLayout(vk_->device, &dslci, nullptr, &vk_->set_layout) != OK) {
    why_ = "vkCreateDescriptorSetLayout refused";
    return Result<void>(Status::Unsupported, why_);
  }

  VkPushConstantRange range{SHADER_STAGE_COMPUTE, 0, sizeof(Push)};
  VkPipelineLayoutCreateInfo plci{};
  plci.sType = ST_PIPELINE_LAYOUT_CREATE_INFO;
  plci.setLayoutCount = 1;
  plci.pSetLayouts = &vk_->set_layout;
  plci.pushConstantRangeCount = 1;
  plci.pPushConstantRanges = &range;
  if (a.CreatePipelineLayout(vk_->device, &plci, nullptr, &vk_->layout) != OK) {
    why_ = "vkCreatePipelineLayout refused";
    return Result<void>(Status::Unsupported, why_);
  }

  VkComputePipelineCreateInfo cpci{};
  cpci.sType = ST_COMPUTE_PIPELINE_CREATE_INFO;
  cpci.stage.sType = ST_PIPELINE_SHADER_STAGE_CREATE_INFO;
  cpci.stage.stage = SHADER_STAGE_COMPUTE;
  cpci.stage.module_ = vk_->shader;
  cpci.stage.pName = "main";
  cpci.layout = vk_->layout;
  cpci.basePipelineIndex = -1;
  if (a.CreateComputePipelines(vk_->device, nullptr, 1, &cpci, nullptr, &vk_->pipeline) != OK) {
    why_ = "vkCreateComputePipelines refused the scene shader";
    return Result<void>(Status::Unsupported, why_);
  }

  VkCommandPoolCreateInfo cpi{};
  cpi.sType = ST_COMMAND_POOL_CREATE_INFO;
  cpi.flags = 0x2;   // RESET_COMMAND_BUFFER
  cpi.queueFamilyIndex = vk_->family;
  if (a.CreateCommandPool(vk_->device, &cpi, nullptr, &vk_->cmd_pool) != OK) {
    why_ = "vkCreateCommandPool refused";
    return Result<void>(Status::Unsupported, why_);
  }
  VkCommandBufferAllocateInfo cbai{};
  cbai.sType = ST_COMMAND_BUFFER_ALLOCATE_INFO;
  cbai.commandPool = vk_->cmd_pool;
  cbai.commandBufferCount = 1;
  a.AllocateCommandBuffers(vk_->device, &cbai, &vk_->cmd);

  VkFenceCreateInfo fci{};
  fci.sType = ST_FENCE_CREATE_INFO;
  fci.flags = 0x1;   // signalled, so the first frame does not wait forever
  a.CreateFence(vk_->device, &fci, nullptr, &vk_->fence);
  VkSemaphoreCreateInfo sci{};
  sci.sType = ST_SEMAPHORE_CREATE_INFO;
  a.CreateSemaphore(vk_->device, &sci, nullptr, &vk_->acquired);
  a.CreateSemaphore(vk_->device, &sci, nullptr, &vk_->rendered);

  vk_->needs_rebuild = true;
  ready_ = true;
  return Ok();
}

namespace {

// Everything that depends on the display's size: the swapchain, its views, the
// descriptor sets that name them, and the overlay image the UI is written into.
Result<void> rebuild(GpuPresenter::Vk& v, std::string& why) {
  auto& a = api();
  a.DeviceWaitIdle(v.device);

  for (auto view : v.views) a.DestroyImageView(v.device, view, nullptr);
  v.views.clear();
  v.images.clear();
  if (v.pool) { a.DestroyDescriptorPool(v.device, v.pool, nullptr); v.pool = nullptr; }
  v.sets.clear();

  VkSurfaceCapabilitiesKHR caps{};
  if (a.GetPhysicalDeviceSurfaceCapabilitiesKHR(v.physical, v.surface, &caps) != OK) {
    why = "the surface would not report its capabilities";
    return Result<void>(Status::IoError, why);
  }
  // Can the shader write the swapchain's images itself? Most surfaces say no.
  // When they do, the frame is rendered into an image of our own and copied
  // into the swapchain ON THE DEVICE — which is not the copy this design set
  // out to avoid. That one was the copy back to the host across PCIe, and
  // neither path does that.
  v.direct_to_swapchain = (caps.supportedUsageFlags & IMAGE_USAGE_STORAGE) != 0;
  if (!(caps.supportedUsageFlags & IMAGE_USAGE_TRANSFER_DST) && !v.direct_to_swapchain) {
    why = "this surface accepts neither a shader write nor a transfer, so nothing can reach it";
    return Result<void>(Status::Unsupported, why);
  }
  if (caps.currentExtent.width != 0xFFFFFFFFu) {
    v.width = caps.currentExtent.width;
    v.height = caps.currentExtent.height;
  }
  if (!v.width || !v.height) { why = "the surface has no size"; return Result<void>(Status::Invalid, why); }

  uint32_t nformats = 0;
  a.GetPhysicalDeviceSurfaceFormatsKHR(v.physical, v.surface, &nformats, nullptr);
  std::vector<VkSurfaceFormatKHR> formats(nformats ? nformats : 1);
  if (nformats) a.GetPhysicalDeviceSurfaceFormatsKHR(v.physical, v.surface, &nformats, formats.data());
  v.format = FORMAT_B8G8R8A8_UNORM;
  uint32_t colour_space = 0;
  // R8G8B8A8 first: it is the order the scene shader declares, so on a surface
  // that offers it the direct-to-swapchain write needs no pre-swap and is not
  // relying on a format mismatch the specification calls undefined.
  for (const auto& f : formats) {
    if (f.format == FORMAT_R8G8B8A8_UNORM || f.format == FORMAT_B8G8R8A8_UNORM) {
      v.format = f.format;
      colour_space = f.colorSpace;
      break;
    }
  }

  uint32_t want = std::max(2u, caps.minImageCount);
  if (caps.maxImageCount && want > caps.maxImageCount) want = caps.maxImageCount;

  VkSwapchainCreateInfoKHR sc{};
  sc.sType = ST_SWAPCHAIN_CREATE_INFO_KHR;
  sc.surface = v.surface;
  sc.minImageCount = want;
  sc.imageFormat = v.format;
  sc.imageColorSpace = colour_space;
  sc.imageExtent = {v.width, v.height};
  sc.imageArrayLayers = 1;
  sc.imageUsage = IMAGE_USAGE_COLOR_ATTACHMENT | IMAGE_USAGE_TRANSFER_DST |
                  (v.direct_to_swapchain ? IMAGE_USAGE_STORAGE : 0u);
  sc.imageSharingMode = SHARING_EXCLUSIVE;
  sc.preTransform = caps.currentTransform;
  sc.compositeAlpha = COMPOSITE_ALPHA_OPAQUE;
  sc.presentMode = PRESENT_MODE_FIFO;   // the display's own pace; no tearing
  sc.clipped = 1;
  sc.oldSwapchain = v.swapchain;
  VkSwapchainKHR fresh = nullptr;
  if (a.CreateSwapchainKHR(v.device, &sc, nullptr, &fresh) != OK) {
    why = "vkCreateSwapchainKHR refused";
    return Result<void>(Status::IoError, why);
  }
  if (v.swapchain) a.DestroySwapchainKHR(v.device, v.swapchain, nullptr);
  v.swapchain = fresh;

  uint32_t nimages = 0;
  a.GetSwapchainImagesKHR(v.device, v.swapchain, &nimages, nullptr);
  v.images.resize(nimages);
  a.GetSwapchainImagesKHR(v.device, v.swapchain, &nimages, v.images.data());

  if (v.direct_to_swapchain) {
    for (auto img : v.images) {
      VkImageViewCreateInfo ivci{};
      ivci.sType = ST_IMAGE_VIEW_CREATE_INFO;
      ivci.image = img;
      ivci.viewType = IMAGE_VIEW_TYPE_2D;
      ivci.format = v.format;
      ivci.subresourceRange = {ASPECT_COLOUR, 0, 1, 0, 1};
      VkImageView view = nullptr;
      if (a.CreateImageView(v.device, &ivci, nullptr, &view) != OK) {
        why = "vkCreateImageView refused a swapchain image";
        return Result<void>(Status::IoError, why);
      }
      v.views.push_back(view);
    }
  } else {
    // One image of our own, device-local, that the shader can write and the
    // copy can read.
    if (v.offscreen_view) { a.DestroyImageView(v.device, v.offscreen_view, nullptr); v.offscreen_view = nullptr; }
    if (v.offscreen) { a.DestroyImage(v.device, v.offscreen, nullptr); v.offscreen = nullptr; }
    if (v.offscreen_memory) { a.FreeMemory(v.device, v.offscreen_memory, nullptr); v.offscreen_memory = nullptr; }

    // R8G8B8A8, whatever the surface's own order is: the shader declares its
    // target `rgba8`, and a storage image whose view format does not match the
    // shader's qualifier is undefined — this driver answers it by writing
    // nothing at all, which is a black screen and no error anywhere. The image
    // is ours, so it can simply be the format the shader says. Where the
    // swapchain is B8G8R8A8 the copy below moves bytes, not channels, so the
    // shader pre-swaps (the `bgra` push constant) and the copy lands correct.
    VkImageCreateInfo oci{};
    oci.sType = ST_IMAGE_CREATE_INFO;
    oci.imageType = IMAGE_TYPE_2D;
    oci.format = FORMAT_R8G8B8A8_UNORM;
    oci.extent = {v.width, v.height, 1};
    oci.mipLevels = 1;
    oci.arrayLayers = 1;
    oci.samples = 1;
    oci.tiling = 0;   // OPTIMAL: nothing on the host reads this
    oci.usage = IMAGE_USAGE_STORAGE | IMAGE_USAGE_TRANSFER_SRC;
    oci.sharingMode = SHARING_EXCLUSIVE;
    oci.initialLayout = LAYOUT_UNDEFINED;
    if (a.CreateImage(v.device, &oci, nullptr, &v.offscreen) != OK) {
      why = "the driver would not make a storage image to render into";
      return Result<void>(Status::Unsupported, why);
    }
    VkMemoryRequirements oreq{};
    a.GetImageMemoryRequirements(v.device, v.offscreen, &oreq);
    uint32_t otype = pick_memory(v.memory, oreq.memoryTypeBits, MEMORY_DEVICE_LOCAL);
    if (otype == UINT32_MAX) otype = pick_memory(v.memory, oreq.memoryTypeBits, 0);
    if (otype == UINT32_MAX) { why = "no memory type can hold the render target"; return Result<void>(Status::Exhausted, why); }
    VkMemoryAllocateInfo omai{};
    omai.sType = ST_MEMORY_ALLOCATE_INFO;
    omai.allocationSize = oreq.size;
    omai.memoryTypeIndex = otype;
    if (a.AllocateMemory(v.device, &omai, nullptr, &v.offscreen_memory) != OK ||
        a.BindImageMemory(v.device, v.offscreen, v.offscreen_memory, 0) != OK) {
      why = "could not back the render target with memory";
      return Result<void>(Status::Exhausted, why);
    }
    VkImageViewCreateInfo ovi{};
    ovi.sType = ST_IMAGE_VIEW_CREATE_INFO;
    ovi.image = v.offscreen;
    ovi.viewType = IMAGE_VIEW_TYPE_2D;
    ovi.format = FORMAT_R8G8B8A8_UNORM;
    ovi.subresourceRange = {ASPECT_COLOUR, 0, 1, 0, 1};
    if (a.CreateImageView(v.device, &ovi, nullptr, &v.offscreen_view) != OK) {
      why = "vkCreateImageView refused the render target";
      return Result<void>(Status::IoError, why);
    }
  }

  // The overlay: a host-visible storage buffer, written by the CPU and read by
  // the shader. Rebuilt with the display because it is the display's size.
  if (v.overlay_mapped) { a.UnmapMemory(v.device, v.overlay_memory); v.overlay_mapped = nullptr; }
  if (v.overlay) { a.DestroyBuffer(v.device, v.overlay, nullptr); v.overlay = nullptr; }
  if (v.overlay_memory) { a.FreeMemory(v.device, v.overlay_memory, nullptr); v.overlay_memory = nullptr; }

  v.overlay_bytes = (VkDeviceSize)v.width * v.height * 4;
  VkBufferCreateInfo bci{};
  bci.sType = ST_BUFFER_CREATE_INFO;
  bci.size = v.overlay_bytes;
  bci.usage = BUFFER_USAGE_STORAGE;
  bci.sharingMode = SHARING_EXCLUSIVE;
  if (a.CreateBuffer(v.device, &bci, nullptr, &v.overlay) != OK) {
    why = "the driver would not make a host-writable overlay buffer";
    return Result<void>(Status::Unsupported, why);
  }
  VkMemoryRequirements req{};
  a.GetBufferMemoryRequirements(v.device, v.overlay, &req);
  uint32_t type = pick_memory(v.memory, req.memoryTypeBits, MEMORY_HOST_VISIBLE | MEMORY_HOST_COHERENT);
  if (type == UINT32_MAX) {
    why = "this device has no host-visible coherent memory for the overlay";
    return Result<void>(Status::Unsupported, why);
  }
  VkMemoryAllocateInfo mai{};
  mai.sType = ST_MEMORY_ALLOCATE_INFO;
  mai.allocationSize = req.size;
  mai.memoryTypeIndex = type;
  if (a.AllocateMemory(v.device, &mai, nullptr, &v.overlay_memory) != OK ||
      a.BindBufferMemory(v.device, v.overlay, v.overlay_memory, 0) != OK) {
    why = "could not back the overlay buffer with memory";
    return Result<void>(Status::Exhausted, why);
  }
  if (a.MapMemory(v.device, v.overlay_memory, 0, req.size, 0, &v.overlay_mapped) != OK) {
    why = "could not map the overlay buffer";
    return Result<void>(Status::IoError, why);
  }
  std::memset(v.overlay_mapped, 0, (size_t)v.overlay_bytes);
  v.overlay_generation = ~0ull;

  // The scene cache: the marched background, which is smaller than the display
  // whenever scale is above 1. DEVICE-LOCAL, never mapped. Sized for the full
  // display so any scale fits, and rebuilt here with everything else that
  // follows the display. Its contents are one frame's scratch — the march pass
  // fills all of it every frame and the composite pass reads it back — so there
  // is nothing in it worth preserving across a rebuild.
  if (v.cache) { a.DestroyBuffer(v.device, v.cache, nullptr); v.cache = nullptr; }
  if (v.cache_memory) { a.FreeMemory(v.device, v.cache_memory, nullptr); v.cache_memory = nullptr; }

  v.cache_bytes = (VkDeviceSize)v.width * v.height * 4;
  VkBufferCreateInfo cbci{};
  cbci.sType = ST_BUFFER_CREATE_INFO;
  cbci.size = v.cache_bytes;
  cbci.usage = BUFFER_USAGE_STORAGE;
  cbci.sharingMode = SHARING_EXCLUSIVE;
  if (a.CreateBuffer(v.device, &cbci, nullptr, &v.cache) != OK) {
    why = "the driver would not make a buffer for the scene cache";
    return Result<void>(Status::Unsupported, why);
  }
  VkMemoryRequirements creq{};
  a.GetBufferMemoryRequirements(v.device, v.cache, &creq);
  uint32_t ctype = pick_memory(v.memory, creq.memoryTypeBits, MEMORY_DEVICE_LOCAL);
  if (ctype == UINT32_MAX) {
    why = "this device has no device-local memory for the scene cache";
    return Result<void>(Status::Unsupported, why);
  }
  VkMemoryAllocateInfo cmai{};
  cmai.sType = ST_MEMORY_ALLOCATE_INFO;
  cmai.allocationSize = creq.size;
  cmai.memoryTypeIndex = ctype;
  if (a.AllocateMemory(v.device, &cmai, nullptr, &v.cache_memory) != OK ||
      a.BindBufferMemory(v.device, v.cache, v.cache_memory, 0) != OK) {
    why = "could not back the scene cache with memory";
    return Result<void>(Status::Exhausted, why);
  }

  // The composed scene's description. Made once at the size the shader's own
  // bound implies, so a description can be swapped in without touching the
  // pipeline: 8 header vec4s plus 3 per solid, 64 solids.
  if (v.scene_mapped) { a.UnmapMemory(v.device, v.scene_memory); v.scene_mapped = nullptr; }
  if (v.scene_buf) { a.DestroyBuffer(v.device, v.scene_buf, nullptr); v.scene_buf = nullptr; }
  if (v.scene_memory) { a.FreeMemory(v.device, v.scene_memory, nullptr); v.scene_memory = nullptr; }

  v.scene_bytes = (VkDeviceSize)(8 + 64 * 3) * 4 * sizeof(float);
  VkBufferCreateInfo sbci{};
  sbci.sType = ST_BUFFER_CREATE_INFO;
  sbci.size = v.scene_bytes;
  sbci.usage = BUFFER_USAGE_STORAGE;
  sbci.sharingMode = SHARING_EXCLUSIVE;
  if (a.CreateBuffer(v.device, &sbci, nullptr, &v.scene_buf) != OK) {
    why = "the driver would not make a buffer for a composed scene";
    return Result<void>(Status::Unsupported, why);
  }
  VkMemoryRequirements sreq{};
  a.GetBufferMemoryRequirements(v.device, v.scene_buf, &sreq);
  uint32_t stype = pick_memory(v.memory, sreq.memoryTypeBits, MEMORY_HOST_VISIBLE | MEMORY_HOST_COHERENT);
  if (stype == UINT32_MAX) {
    why = "this device has no host-visible coherent memory for a composed scene";
    return Result<void>(Status::Unsupported, why);
  }
  VkMemoryAllocateInfo smai{};
  smai.sType = ST_MEMORY_ALLOCATE_INFO;
  smai.allocationSize = sreq.size;
  smai.memoryTypeIndex = stype;
  if (a.AllocateMemory(v.device, &smai, nullptr, &v.scene_memory) != OK ||
      a.BindBufferMemory(v.device, v.scene_buf, v.scene_memory, 0) != OK) {
    why = "could not back the composed scene buffer with memory";
    return Result<void>(Status::Exhausted, why);
  }
  if (a.MapMemory(v.device, v.scene_memory, 0, sreq.size, 0, &v.scene_mapped) != OK) {
    why = "could not map the composed scene buffer";
    return Result<void>(Status::IoError, why);
  }
  // Zeroed means no solids, which draws sky and ground and nothing else —
  // never uninitialised memory read as geometry.
  std::memset(v.scene_mapped, 0, (size_t)v.scene_bytes);
  if (!v.scene_desc.empty()) {
    std::memcpy(v.scene_mapped, v.scene_desc.data(), v.scene_desc.size() * sizeof(float));
  }

  // One descriptor set per swapchain image: the target changes every frame,
  // the overlay does not.
  const size_t set_count = v.direct_to_swapchain ? v.images.size() : 1;
  VkDescriptorPoolSize sizes[2] = {
    {DESCRIPTOR_TYPE_STORAGE_IMAGE, (uint32_t)set_count},
    {DESCRIPTOR_TYPE_STORAGE_BUFFER, (uint32_t)(set_count * 3)},
  };
  VkDescriptorPoolCreateInfo dpci{};
  dpci.sType = ST_DESCRIPTOR_POOL_CREATE_INFO;
  dpci.maxSets = (uint32_t)set_count;
  dpci.poolSizeCount = 2;
  dpci.pPoolSizes = sizes;
  if (a.CreateDescriptorPool(v.device, &dpci, nullptr, &v.pool) != OK) {
    why = "vkCreateDescriptorPool refused";
    return Result<void>(Status::Exhausted, why);
  }
  for (size_t i = 0; i < set_count; ++i) {
    VkDescriptorSetAllocateInfo dsai{};
    dsai.sType = ST_DESCRIPTOR_SET_ALLOCATE_INFO;
    dsai.descriptorPool = v.pool;
    dsai.descriptorSetCount = 1;
    dsai.pSetLayouts = &v.set_layout;
    VkDescriptorSet set = nullptr;
    if (a.AllocateDescriptorSets(v.device, &dsai, &set) != OK) {
      why = "vkAllocateDescriptorSets refused";
      return Result<void>(Status::Exhausted, why);
    }
    VkDescriptorImageInfo target{nullptr, v.direct_to_swapchain ? v.views[i] : v.offscreen_view, LAYOUT_GENERAL};
    VkDescriptorBufferInfo over{v.overlay, 0, v.overlay_bytes};
    VkDescriptorBufferInfo scene_info{v.scene_buf, 0, v.scene_bytes};
    VkDescriptorBufferInfo cache_info{v.cache, 0, v.cache_bytes};
    VkWriteDescriptorSet writes[4]{};
    writes[0].sType = ST_WRITE_DESCRIPTOR_SET;
    writes[0].dstSet = set;
    writes[0].dstBinding = 0;
    writes[0].descriptorCount = 1;
    writes[0].descriptorType = DESCRIPTOR_TYPE_STORAGE_IMAGE;
    writes[0].pImageInfo = &target;
    writes[1].sType = ST_WRITE_DESCRIPTOR_SET;
    writes[1].dstSet = set;
    writes[1].dstBinding = 1;
    writes[1].descriptorCount = 1;
    writes[1].descriptorType = DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[1].pBufferInfo = &over;
    writes[2].sType = ST_WRITE_DESCRIPTOR_SET;
    writes[2].dstSet = set;
    writes[2].dstBinding = 2;
    writes[2].descriptorCount = 1;
    writes[2].descriptorType = DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[2].pBufferInfo = &scene_info;
    writes[3].sType = ST_WRITE_DESCRIPTOR_SET;
    writes[3].dstSet = set;
    writes[3].dstBinding = 3;
    writes[3].descriptorCount = 1;
    writes[3].descriptorType = DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[3].pBufferInfo = &cache_info;
    a.UpdateDescriptorSets(v.device, 4, writes, 0, nullptr);
    v.sets.push_back(set);
  }
  if (!v.direct_to_swapchain) v.offscreen_set = v.sets[0];

  v.needs_rebuild = false;
  return Ok();
}

}  // namespace

Result<void> GpuPresenter::set_scene_description(const std::vector<float>& floats) {
  if (!ready_) return Result<void>(Status::Unsupported, why_.empty() ? "no GPU display" : why_);
  auto& v = *vk_;
  // The header is 32 floats and each solid is 12; anything shorter is not a
  // scene, and saying so beats drawing whatever the buffer had in it.
  if (floats.size() < 32) return Result<void>(Status::Invalid, "a scene description needs its header");

  // The shader's own bound on the solid loop. Clamped rather than refused: a
  // description with more solids than fit draws the ones that do, which is a
  // better answer than a black screen — but then the count the shader reads
  // must agree with what actually landed, or it indexes past its own data.
  const size_t room = 32 + 64 * 12;
  const size_t n = floats.size() > room ? room : floats.size();
  v.scene_desc.assign(floats.begin(), floats.begin() + (long)n);
  v.scene_desc[28] = (float)((n - 32) / 12);

  // The buffer exists only once the swapchain does. Accepted either way: it is
  // applied on the next rebuild, which is what makes a description survive a
  // resize and one handed over before the first frame arrive at all.
  if (v.scene_mapped) {
    std::memset(v.scene_mapped, 0, (size_t)v.scene_bytes);
    std::memcpy(v.scene_mapped, v.scene_desc.data(), v.scene_desc.size() * sizeof(float));
  }
  return Ok();
}

Result<void> GpuPresenter::present(int scene, double seconds, const uint32_t* overlay,
                                   uint64_t overlay_generation, int quality_scale) {
  if (!ready_) return Result<void>(Status::Unsupported, why_.empty() ? "no GPU display" : why_);
  auto& a = api();
  Vk& v = *vk_;
  auto t0 = std::chrono::steady_clock::now();

  if (v.needs_rebuild) {
    auto r = rebuild(v, why_);
    if (!r.ok()) { ready_ = false; return r; }
  }

  a.WaitForFences(v.device, 1, &v.fence, 1, 2000000000ull);

  // The overlay is uploaded only when the compositor says its layer changed.
  // A still interface over a moving background costs nothing but the dispatch,
  // which is the whole reason this is worth doing at 1080p.
  if (overlay && overlay_generation != v.overlay_generation && v.overlay_mapped) {
    // One contiguous copy: the buffer is laid out exactly as the compositor's
    // framebuffer is, so there is no row pitch to step around.
    std::memcpy(v.overlay_mapped, overlay, (size_t)v.overlay_bytes);
    v.overlay_generation = overlay_generation;
    cost_.overlay_uploads++;
  }

  uint32_t index = 0;
  VkResult acquired = a.AcquireNextImageKHR(v.device, v.swapchain, 2000000000ull, v.acquired, nullptr, &index);
  if (acquired == OUT_OF_DATE) { v.needs_rebuild = true; return Ok(); }
  if (acquired != OK && acquired != SUBOPTIMAL) {
    return Result<void>(Status::IoError, "vkAcquireNextImageKHR failed");
  }
  a.ResetFences(v.device, 1, &v.fence);

  a.ResetCommandBuffer(v.cmd, 0);
  VkCommandBufferBeginInfo cbbi{};
  cbbi.sType = ST_COMMAND_BUFFER_BEGIN_INFO;
  cbbi.flags = 0x1;
  a.BeginCommandBuffer(v.cmd, &cbbi);

  // Whatever the shader is going to write has to be in GENERAL first, and an
  // acquired swapchain image comes back in an undefined layout every time.
  VkImage target = v.direct_to_swapchain ? v.images[index] : v.offscreen;
  VkImageMemoryBarrier to_general{};
  to_general.sType = ST_IMAGE_MEMORY_BARRIER;
  to_general.dstAccessMask = ACCESS_SHADER_WRITE;
  to_general.oldLayout = LAYOUT_UNDEFINED;
  to_general.newLayout = LAYOUT_GENERAL;
  to_general.srcQueueFamilyIndex = ~0u;
  to_general.dstQueueFamilyIndex = ~0u;
  to_general.image = target;
  to_general.subresourceRange = {ASPECT_COLOUR, 0, 1, 0, 1};
  a.CmdPipelineBarrier(v.cmd, PIPELINE_STAGE_TOP, PIPELINE_STAGE_COMPUTE, 0, 0, nullptr, 0, nullptr, 1, &to_general);

  // The overlay needs no barrier of its own any more. It is a buffer in
  // host-coherent memory, and queue submission is itself the domain operation
  // that makes a host write to such memory visible to the device — which is
  // also why the memcpy above can happen right before this submit.

  a.CmdBindPipeline(v.cmd, PIPELINE_BIND_POINT_COMPUTE, v.pipeline);
  const VkDescriptorSet& set = v.direct_to_swapchain ? v.sets[index] : v.sets[0];
  a.CmdBindDescriptorSets(v.cmd, PIPELINE_BIND_POINT_COMPUTE, v.layout, 0, 1, &set, 0, nullptr);
  // Whether the shader has to pre-swap red and blue depends on WHICH path this
  // is, not just on the surface's format:
  //
  //  - Writing the swapchain image directly, the driver honours that image's own
  //    format when it converts the shader's value, so the shader writes plain
  //    RGB and a B8G8R8A8 surface still comes out right. Pre-swapping here is
  //    what turned the tide brown and the overlay's teal to yellow.
  //  - Through our own render target, the image is R8G8B8A8 (matching what the
  //    shader declares) and vkCmdCopyImage then moves BYTES, not channels — so
  //    into a B8G8R8A8 swapchain the shader must pre-swap for the copy to land
  //    the right way round.
  const uint32_t swap_rb =
      (!v.direct_to_swapchain && v.format == FORMAT_B8G8R8A8_UNORM) ? 1u : 0u;
  // How finely the background is marched this frame. `scale` 1 is the full
  // picture; 2 marches a quarter of the samples and 3 a ninth, and the composite
  // pass interpolates back up. The caller asks; it is clamped here, because a
  // scale the cache was not sized for would read off the end of it.
  const int clamped = std::clamp(scene, 0, 4);
  const uint32_t scale = (uint32_t)std::clamp(quality_scale, 1, 4);
  const uint32_t cache_w = (v.width + scale - 1) / scale;
  const uint32_t cache_h = (v.height + scale - 1) / scale;

  // A buffer written by one dispatch and read by the next needs the dependency
  // said out loud — a fence is host-side sync and does not make a device write
  // visible to a later device read.
  VkMemoryBarrier cache_dep{};
  cache_dep.sType = ST_MEMORY_BARRIER;
  cache_dep.srcAccessMask = ACCESS_SHADER_WRITE;
  cache_dep.dstAccessMask = ACCESS_SHADER_READ | ACCESS_SHADER_WRITE;
  a.CmdPipelineBarrier(v.cmd, PIPELINE_STAGE_COMPUTE, PIPELINE_STAGE_COMPUTE, 0,
                       1, &cache_dep, 0, nullptr, 0, nullptr);

  // Pass 0: march the background, all of it, at 1/scale in each axis. The
  // dispatch is that much smaller, which is where the saving actually is.
  //
  // Thinning it by BRANCHING inside a full-screen dispatch — "march this pixel,
  // read that one back" — looks equivalent and saves nothing at all: a GPU runs
  // a subgroup in lockstep, so a workgroup holding both kinds executes the march
  // anyway and throws part of it away. Measured, it moved the power draw by
  // nothing whatsoever. Only a smaller dispatch is cheaper.
  Push march{v.width, v.height, (uint32_t)clamped, overlay ? 1u : 0u,
             swap_rb, backdrop_, (float)seconds, 0u, scale, cache_w, cache_h,
             tod_};
  a.CmdPushConstants(v.cmd, v.layout, SHADER_STAGE_COMPUTE, 0, sizeof march, &march);
  a.CmdDispatch(v.cmd, (cache_w + 7) / 8, (cache_h + 7) / 8, 1);

  a.CmdPipelineBarrier(v.cmd, PIPELINE_STAGE_COMPUTE, PIPELINE_STAGE_COMPUTE, 0,
                       1, &cache_dep, 0, nullptr, 0, nullptr);

  // Pass 1: the frame that is presented — a cache read and the overlay over it.
  Push composite = march;
  composite.pass = 1u;
  a.CmdPushConstants(v.cmd, v.layout, SHADER_STAGE_COMPUTE, 0, sizeof composite, &composite);
  a.CmdDispatch(v.cmd, (v.width + 7) / 8, (v.height + 7) / 8, 1);

  // Every frame marches the whole background, so there is no stale picture to
  // invalidate and nothing to carry between frames: the cache is this frame's
  // scratch, not a record of an older one.
  cost_.scene_marches++;
  cost_.scene_pixels_marched += (uint64_t)cache_w * cache_h;


  if (v.direct_to_swapchain) {
    VkImageMemoryBarrier to_present = to_general;
    to_present.oldLayout = LAYOUT_GENERAL;
    to_present.newLayout = LAYOUT_PRESENT_SRC;
    a.CmdPipelineBarrier(v.cmd, PIPELINE_STAGE_COMPUTE, PIPELINE_STAGE_BOTTOM, 0, 0, nullptr, 0, nullptr, 1, &to_present);
  } else {
    // The frame is in our own image; move it into the swapchain's. Both are on
    // the device, so this is a blit inside the GPU's own memory and the frame
    // still never crosses the bus.
    VkImageMemoryBarrier src{};
    src.sType = ST_IMAGE_MEMORY_BARRIER;
    src.srcAccessMask = ACCESS_SHADER_WRITE;
    src.dstAccessMask = ACCESS_TRANSFER_READ;
    src.oldLayout = LAYOUT_GENERAL;
    src.newLayout = LAYOUT_TRANSFER_SRC;
    src.srcQueueFamilyIndex = ~0u;
    src.dstQueueFamilyIndex = ~0u;
    src.image = v.offscreen;
    src.subresourceRange = {ASPECT_COLOUR, 0, 1, 0, 1};

    VkImageMemoryBarrier dst = src;
    dst.srcAccessMask = 0;
    dst.dstAccessMask = ACCESS_TRANSFER_WRITE;
    dst.oldLayout = LAYOUT_UNDEFINED;
    dst.newLayout = LAYOUT_TRANSFER_DST;
    dst.image = v.images[index];

    VkImageMemoryBarrier both[2] = {src, dst};
    a.CmdPipelineBarrier(v.cmd, PIPELINE_STAGE_COMPUTE, PIPELINE_STAGE_TRANSFER, 0, 0, nullptr, 0, nullptr, 2, both);

    VkImageCopy region{};
    region.srcSubresource = {ASPECT_COLOUR, 0, 0, 1};
    region.dstSubresource = {ASPECT_COLOUR, 0, 0, 1};
    region.extent = {v.width, v.height, 1};
    a.CmdCopyImage(v.cmd, v.offscreen, LAYOUT_TRANSFER_SRC, v.images[index], LAYOUT_TRANSFER_DST, 1, &region);

    VkImageMemoryBarrier to_present{};
    to_present.sType = ST_IMAGE_MEMORY_BARRIER;
    to_present.srcAccessMask = ACCESS_TRANSFER_WRITE;
    to_present.oldLayout = LAYOUT_TRANSFER_DST;
    to_present.newLayout = LAYOUT_PRESENT_SRC;
    to_present.srcQueueFamilyIndex = ~0u;
    to_present.dstQueueFamilyIndex = ~0u;
    to_present.image = v.images[index];
    to_present.subresourceRange = {ASPECT_COLOUR, 0, 1, 0, 1};
    a.CmdPipelineBarrier(v.cmd, PIPELINE_STAGE_TRANSFER, PIPELINE_STAGE_BOTTOM, 0, 0, nullptr, 0, nullptr, 1, &to_present);
  }
  a.EndCommandBuffer(v.cmd);

  VkFlags wait_stage = PIPELINE_STAGE_COMPUTE;
  VkSubmitInfo si{};
  si.sType = ST_SUBMIT_INFO;
  si.waitSemaphoreCount = 1;
  si.pWaitSemaphores = &v.acquired;
  si.pWaitDstStageMask = &wait_stage;
  si.commandBufferCount = 1;
  si.pCommandBuffers = &v.cmd;
  si.signalSemaphoreCount = 1;
  si.pSignalSemaphores = &v.rendered;
  if (a.QueueSubmit(v.queue, 1, &si, v.fence) != OK) {
    return Result<void>(Status::IoError, "vkQueueSubmit refused the frame");
  }

  VkPresentInfoKHR pi{};
  pi.sType = ST_PRESENT_INFO_KHR;
  pi.waitSemaphoreCount = 1;
  pi.pWaitSemaphores = &v.rendered;
  pi.swapchainCount = 1;
  pi.pSwapchains = &v.swapchain;
  pi.pImageIndices = &index;
  VkResult presented = a.QueuePresentKHR(v.queue, &pi);
  if (presented == OUT_OF_DATE || presented == SUBOPTIMAL) v.needs_rebuild = true;

  v.last_index = index;
  v.have_frame = true;
  cost_.frames++;
  cost_.wall_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  return Ok();
}

// A one-off read-back, for looking at what the device actually produced.
//
// The frame path deliberately never copies back — that is the whole design, and
// at 1080p it would cost about a millisecond a frame. This is not that: it runs
// once, when a person asks to see the frame, and it is the only way to inspect
// this path honestly on a machine whose window system will not hand a
// Vulkan-presented window's pixels to a screen grab. Verifying a GPU display by
// screenshotting the X root window proves nothing about the GPU.
bool GpuPresenter::direct_to_swapchain() const {
  return vk_ && vk_->direct_to_swapchain;
}

Result<void> GpuPresenter::capture(std::vector<uint32_t>& out) {
  if (!ready_ || !vk_ || !vk_->device) return Result<void>(Status::Unsupported, why_);
  auto& v = *vk_;
  auto& a = api();
  if (!v.have_frame) return Result<void>(Status::NotFound, "no frame has been presented yet");

  const VkDeviceSize bytes = (VkDeviceSize)v.width * v.height * 4;
  VkBuffer host = nullptr;
  VkDeviceMemory mem = nullptr;
  void* mapped = nullptr;
  auto release = [&]() {
    if (mapped) a.UnmapMemory(v.device, mem);
    if (host) a.DestroyBuffer(v.device, host, nullptr);
    if (mem) a.FreeMemory(v.device, mem, nullptr);
  };

  VkBufferCreateInfo bci{};
  bci.sType = ST_BUFFER_CREATE_INFO;
  bci.size = bytes;
  bci.usage = BUFFER_USAGE_TRANSFER_DST;
  bci.sharingMode = SHARING_EXCLUSIVE;
  if (a.CreateBuffer(v.device, &bci, nullptr, &host) != OK) {
    return Result<void>(Status::Exhausted, "vkCreateBuffer refused the capture buffer");
  }
  VkMemoryRequirements req{};
  a.GetBufferMemoryRequirements(v.device, host, &req);
  uint32_t type = pick_memory(v.memory, req.memoryTypeBits, MEMORY_HOST_VISIBLE | MEMORY_HOST_COHERENT);
  if (type == UINT32_MAX) { release(); return Result<void>(Status::Unsupported, "no host-visible memory to capture into"); }
  VkMemoryAllocateInfo mai{};
  mai.sType = ST_MEMORY_ALLOCATE_INFO;
  mai.allocationSize = req.size;
  mai.memoryTypeIndex = type;
  if (a.AllocateMemory(v.device, &mai, nullptr, &mem) != OK ||
      a.BindBufferMemory(v.device, host, mem, 0) != OK) {
    release();
    return Result<void>(Status::Exhausted, "could not back the capture buffer");
  }

  if (a.MapMemory(v.device, mem, 0, req.size, 0, &mapped) != OK) {
    release();
    return Result<void>(Status::IoError, "could not map the capture buffer");
  }
  // A sentinel, so "the device produced black" and "nothing was copied" are not
  // the same answer. Black is a legitimate frame; unwritten is a bug.
  std::memset(mapped, 0x7F, (size_t)bytes);

  // The source is whichever image the last frame was actually written into.
  VkImage source = v.direct_to_swapchain ? v.images[v.last_index] : v.offscreen;
  const uint32_t from_layout = v.direct_to_swapchain ? LAYOUT_PRESENT_SRC : LAYOUT_TRANSFER_SRC;

  a.WaitForFences(v.device, 1, &v.fence, 1, 2000000000ull);
  a.ResetFences(v.device, 1, &v.fence);
  a.ResetCommandBuffer(v.cmd, 0);
  VkCommandBufferBeginInfo cbi{};
  cbi.sType = ST_COMMAND_BUFFER_BEGIN_INFO;
  a.BeginCommandBuffer(v.cmd, &cbi);

  VkImageMemoryBarrier to_src{};
  to_src.sType = ST_IMAGE_MEMORY_BARRIER;
  to_src.srcAccessMask = 0;
  to_src.dstAccessMask = ACCESS_TRANSFER_READ;
  to_src.oldLayout = from_layout;
  to_src.newLayout = LAYOUT_TRANSFER_SRC;
  to_src.srcQueueFamilyIndex = ~0u;
  to_src.dstQueueFamilyIndex = ~0u;
  to_src.image = source;
  to_src.subresourceRange = {ASPECT_COLOUR, 0, 1, 0, 1};
  a.CmdPipelineBarrier(v.cmd, PIPELINE_STAGE_TOP, PIPELINE_STAGE_TRANSFER, 0, 0, nullptr, 0, nullptr, 1, &to_src);

  VkBufferImageCopy region{};
  region.imageSubresource = {ASPECT_COLOUR, 0, 0, 1};
  region.imageExtent = {v.width, v.height, 1};
  a.CmdCopyImageToBuffer(v.cmd, source, LAYOUT_TRANSFER_SRC, host, 1, &region);

  // Put it back the way it was found, or the next present has a surprise.
  VkImageMemoryBarrier back = to_src;
  back.srcAccessMask = ACCESS_TRANSFER_READ;
  back.dstAccessMask = 0;
  back.oldLayout = LAYOUT_TRANSFER_SRC;
  back.newLayout = from_layout;
  a.CmdPipelineBarrier(v.cmd, PIPELINE_STAGE_TRANSFER, PIPELINE_STAGE_BOTTOM, 0, 0, nullptr, 0, nullptr, 1, &back);
  a.EndCommandBuffer(v.cmd);

  VkSubmitInfo si{};
  si.sType = ST_SUBMIT_INFO;
  si.commandBufferCount = 1;
  si.pCommandBuffers = &v.cmd;
  if (a.QueueSubmit(v.queue, 1, &si, v.fence) != OK) {
    release();
    return Result<void>(Status::IoError, "vkQueueSubmit refused the capture");
  }
  a.WaitForFences(v.device, 1, &v.fence, 1, 2000000000ull);

  {
    const uint8_t* p8 = static_cast<const uint8_t*>(mapped);
    size_t untouched = 0;
    for (size_t i = 0; i < (size_t)bytes; ++i) if (p8[i] == 0x7F) untouched++;
    if (untouched == (size_t)bytes) {
      release();
      return Result<void>(Status::IoError, "the copy wrote nothing into the capture buffer");
    }
  }
  out.assign((size_t)v.width * v.height, 0);
  const uint8_t* src = static_cast<const uint8_t*>(mapped);
  for (size_t i = 0; i < out.size(); ++i) {
    // The image holds the surface's own channel order; hand back the ARGB8888
    // word the rest of this machine's painters use, whichever that was.
    const uint8_t c0 = src[i * 4 + 0], c1 = src[i * 4 + 1], c2 = src[i * 4 + 2];
    const uint8_t r = v.format == FORMAT_B8G8R8A8_UNORM ? c2 : c0;
    const uint8_t b = v.format == FORMAT_B8G8R8A8_UNORM ? c0 : c2;
    out[i] = 0xFF000000u | ((uint32_t)r << 16) | ((uint32_t)c1 << 8) | b;
  }
  release();
  return Ok();
}

}  // namespace aios::userland
