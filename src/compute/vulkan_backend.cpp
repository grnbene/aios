// The Vulkan backend, on the GPU.
//
// What was here before reported cap_.lanes = 1024, 8 GiB and 1 TFLOP/s as
// struct literals on every machine, and then ran single-threaded `for` loops
// over system memory under a comment that called them a "SPIR-V simulation".
// There was no loader, no instance, no device and no SPIR-V.
//
// This binds the Vulkan loader at runtime through the host boundary — nothing
// links against it, so a machine with no Vulkan driver simply has no Vulkan
// device. The kernels are the SPIR-V module compiled from
// src/compute/shaders/aios_compute.comp, dispatched on a real compute queue.
//
// The ABI declarations below are Vulkan's own. They are spelled out here
// rather than included because this file must compile on a machine that has
// no Vulkan headers at all; every offset and enumerant is from the
// specification, and the structures the driver writes into are given a
// generous opaque tail rather than a guessed field list.
#include "aios/compute/backends.hpp"
#include "aios/compute/gpu_telemetry.hpp"
#include <cstdio>
#include <cstring>
#include <map>
#include "aios/hal/host.hpp"
#include "shaders/aios_compute_spv.h"

#include <algorithm>
#include <cstring>

namespace aios::compute {
namespace {

using VkFlags = uint32_t;
using VkBool32 = uint32_t;
using VkDeviceSize = uint64_t;
using VkResult = int32_t;

#define AIOS_VK_HANDLE(name) struct name##_T; using name = name##_T*
AIOS_VK_HANDLE(VkInstance);
AIOS_VK_HANDLE(VkPhysicalDevice);
AIOS_VK_HANDLE(VkDevice);
AIOS_VK_HANDLE(VkQueue);
AIOS_VK_HANDLE(VkBuffer);
AIOS_VK_HANDLE(VkDeviceMemory);
AIOS_VK_HANDLE(VkShaderModule);
AIOS_VK_HANDLE(VkPipelineLayout);
AIOS_VK_HANDLE(VkPipeline);
AIOS_VK_HANDLE(VkPipelineCache);
AIOS_VK_HANDLE(VkDescriptorSetLayout);
AIOS_VK_HANDLE(VkDescriptorPool);
AIOS_VK_HANDLE(VkDescriptorSet);
AIOS_VK_HANDLE(VkCommandPool);
AIOS_VK_HANDLE(VkCommandBuffer);
AIOS_VK_HANDLE(VkFence);
#undef AIOS_VK_HANDLE

constexpr VkResult VK_SUCCESS_ = 0;

constexpr uint32_t ST_APPLICATION_INFO = 0;
constexpr uint32_t ST_INSTANCE_CREATE_INFO = 1;
constexpr uint32_t ST_DEVICE_QUEUE_CREATE_INFO = 2;
constexpr uint32_t ST_DEVICE_CREATE_INFO = 3;
constexpr uint32_t ST_SUBMIT_INFO = 4;
constexpr uint32_t ST_MEMORY_ALLOCATE_INFO = 5;
constexpr uint32_t ST_FENCE_CREATE_INFO = 8;
constexpr uint32_t ST_BUFFER_CREATE_INFO = 12;
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

constexpr uint32_t QUEUE_COMPUTE_BIT = 0x00000002;
constexpr uint32_t BUFFER_USAGE_STORAGE = 0x00000020;
constexpr uint32_t DESCRIPTOR_TYPE_STORAGE_BUFFER = 7;
constexpr uint32_t SHADER_STAGE_COMPUTE = 0x00000020;
constexpr uint32_t PIPELINE_BIND_POINT_COMPUTE = 1;
constexpr uint32_t MEMORY_PROPERTY_HOST_VISIBLE = 0x00000002;
constexpr uint32_t MEMORY_PROPERTY_HOST_COHERENT = 0x00000004;
constexpr uint32_t COMMAND_BUFFER_USAGE_ONE_TIME = 0x00000001;
constexpr uint32_t DESCRIPTOR_POOL_FREE_SET = 0x00000001;

struct VkApplicationInfo {
  uint32_t sType; const void* pNext;
  const char* pApplicationName; uint32_t applicationVersion;
  const char* pEngineName; uint32_t engineVersion; uint32_t apiVersion;
};
struct VkInstanceCreateInfo {
  uint32_t sType; const void* pNext; VkFlags flags;
  const VkApplicationInfo* pApplicationInfo;
  uint32_t enabledLayerCount; const char* const* ppEnabledLayerNames;
  uint32_t enabledExtensionCount; const char* const* ppEnabledExtensionNames;
};
struct VkQueueFamilyProperties {
  VkFlags queueFlags; uint32_t queueCount; uint32_t timestampValidBits;
  uint32_t w, h, d;   // minImageTransferGranularity
};
// The driver writes a large structure here. Only the head of it is named:
// everything past deviceName is limits and sparse properties this backend does
// not read, so it is left as space for the driver rather than guessed at.
struct VkPhysicalDeviceProperties {
  uint32_t apiVersion, driverVersion, vendorID, deviceID, deviceType;
  char deviceName[256];
  uint8_t pipelineCacheUUID[16];
  uint8_t limits_and_sparse[2048];
};
struct VkMemoryType { VkFlags propertyFlags; uint32_t heapIndex; };
struct VkMemoryHeap { VkDeviceSize size; VkFlags flags; };
struct VkPhysicalDeviceMemoryProperties {
  uint32_t memoryTypeCount; VkMemoryType memoryTypes[32];
  uint32_t memoryHeapCount; VkMemoryHeap memoryHeaps[16];
};
struct VkDeviceQueueCreateInfo {
  uint32_t sType; const void* pNext; VkFlags flags;
  uint32_t queueFamilyIndex, queueCount; const float* pQueuePriorities;
};
struct VkDeviceCreateInfo {
  uint32_t sType; const void* pNext; VkFlags flags;
  uint32_t queueCreateInfoCount; const VkDeviceQueueCreateInfo* pQueueCreateInfos;
  uint32_t enabledLayerCount; const char* const* ppEnabledLayerNames;
  uint32_t enabledExtensionCount; const char* const* ppEnabledExtensionNames;
  const void* pEnabledFeatures;
};
struct VkBufferCreateInfo {
  uint32_t sType; const void* pNext; VkFlags flags;
  VkDeviceSize size; VkFlags usage; uint32_t sharingMode;
  uint32_t queueFamilyIndexCount; const uint32_t* pQueueFamilyIndices;
};
struct VkMemoryRequirements { VkDeviceSize size, alignment; uint32_t memoryTypeBits; };
struct VkMemoryAllocateInfo {
  uint32_t sType; const void* pNext; VkDeviceSize allocationSize; uint32_t memoryTypeIndex;
};
struct VkShaderModuleCreateInfo {
  uint32_t sType; const void* pNext; VkFlags flags; size_t codeSize; const uint32_t* pCode;
};
struct VkDescriptorSetLayoutBinding {
  uint32_t binding, descriptorType, descriptorCount; VkFlags stageFlags; const void* pImmutableSamplers;
};
struct VkDescriptorSetLayoutCreateInfo {
  uint32_t sType; const void* pNext; VkFlags flags;
  uint32_t bindingCount; const VkDescriptorSetLayoutBinding* pBindings;
};
struct VkPushConstantRange { VkFlags stageFlags; uint32_t offset, size; };
struct VkPipelineLayoutCreateInfo {
  uint32_t sType; const void* pNext; VkFlags flags;
  uint32_t setLayoutCount; const VkDescriptorSetLayout* pSetLayouts;
  uint32_t pushConstantRangeCount; const VkPushConstantRange* pPushConstantRanges;
};
struct VkPipelineShaderStageCreateInfo {
  uint32_t sType; const void* pNext; VkFlags flags; VkFlags stage;
  VkShaderModule module_; const char* pName; const void* pSpecializationInfo;
};
struct VkComputePipelineCreateInfo {
  uint32_t sType; const void* pNext; VkFlags flags;
  VkPipelineShaderStageCreateInfo stage; VkPipelineLayout layout;
  VkPipeline basePipelineHandle; int32_t basePipelineIndex;
};
struct VkDescriptorPoolSize { uint32_t type, descriptorCount; };
struct VkDescriptorPoolCreateInfo {
  uint32_t sType; const void* pNext; VkFlags flags;
  uint32_t maxSets, poolSizeCount; const VkDescriptorPoolSize* pPoolSizes;
};
struct VkDescriptorSetAllocateInfo {
  uint32_t sType; const void* pNext; VkDescriptorPool descriptorPool;
  uint32_t descriptorSetCount; const VkDescriptorSetLayout* pSetLayouts;
};
struct VkDescriptorBufferInfo { VkBuffer buffer; VkDeviceSize offset, range; };
struct VkWriteDescriptorSet {
  uint32_t sType; const void* pNext; VkDescriptorSet dstSet;
  uint32_t dstBinding, dstArrayElement, descriptorCount, descriptorType;
  const void* pImageInfo; const VkDescriptorBufferInfo* pBufferInfo; const void* pTexelBufferView;
};
struct VkCommandPoolCreateInfo { uint32_t sType; const void* pNext; VkFlags flags; uint32_t queueFamilyIndex; };
struct VkCommandBufferAllocateInfo {
  uint32_t sType; const void* pNext; VkCommandPool commandPool; uint32_t level, commandBufferCount;
};
struct VkCommandBufferBeginInfo { uint32_t sType; const void* pNext; VkFlags flags; const void* pInheritanceInfo; };
struct VkSubmitInfo {
  uint32_t sType; const void* pNext;
  uint32_t waitSemaphoreCount; const void* pWaitSemaphores; const VkFlags* pWaitDstStageMask;
  uint32_t commandBufferCount; const VkCommandBuffer* pCommandBuffers;
  uint32_t signalSemaphoreCount; const void* pSignalSemaphores;
};
struct VkFenceCreateInfo { uint32_t sType; const void* pNext; VkFlags flags; };

struct VulkanApi {
  void* lib = nullptr;
  bool bound = false;
  std::string why;

  VkResult (*CreateInstance)(const VkInstanceCreateInfo*, const void*, VkInstance*) = nullptr;
  void (*DestroyInstance)(VkInstance, const void*) = nullptr;
  VkResult (*EnumeratePhysicalDevices)(VkInstance, uint32_t*, VkPhysicalDevice*) = nullptr;
  void (*GetPhysicalDeviceProperties)(VkPhysicalDevice, VkPhysicalDeviceProperties*) = nullptr;
  void (*GetPhysicalDeviceQueueFamilyProperties)(VkPhysicalDevice, uint32_t*, VkQueueFamilyProperties*) = nullptr;
  void (*GetPhysicalDeviceMemoryProperties)(VkPhysicalDevice, VkPhysicalDeviceMemoryProperties*) = nullptr;
  // Vulkan 1.1 entry points, optional: they carry the PCI address and the
  // memory budget when the loader and driver are new enough to have them.
  void (*GetPhysicalDeviceProperties2)(VkPhysicalDevice, void*) = nullptr;
  void (*GetPhysicalDeviceMemoryProperties2)(VkPhysicalDevice, void*) = nullptr;
  VkResult (*EnumerateDeviceExtensionProperties)(VkPhysicalDevice, const char*, uint32_t*, void*) = nullptr;
  VkResult (*CreateDevice)(VkPhysicalDevice, const VkDeviceCreateInfo*, const void*, VkDevice*) = nullptr;
  void (*DestroyDevice)(VkDevice, const void*) = nullptr;
  void (*GetDeviceQueue)(VkDevice, uint32_t, uint32_t, VkQueue*) = nullptr;
  VkResult (*CreateBuffer)(VkDevice, const VkBufferCreateInfo*, const void*, VkBuffer*) = nullptr;
  void (*DestroyBuffer)(VkDevice, VkBuffer, const void*) = nullptr;
  void (*GetBufferMemoryRequirements)(VkDevice, VkBuffer, VkMemoryRequirements*) = nullptr;
  VkResult (*AllocateMemory)(VkDevice, const VkMemoryAllocateInfo*, const void*, VkDeviceMemory*) = nullptr;
  void (*FreeMemory)(VkDevice, VkDeviceMemory, const void*) = nullptr;
  VkResult (*BindBufferMemory)(VkDevice, VkBuffer, VkDeviceMemory, VkDeviceSize) = nullptr;
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
  void (*FreeCommandBuffers)(VkDevice, VkCommandPool, uint32_t, const VkCommandBuffer*) = nullptr;
  VkResult (*BeginCommandBuffer)(VkCommandBuffer, const VkCommandBufferBeginInfo*) = nullptr;
  VkResult (*EndCommandBuffer)(VkCommandBuffer) = nullptr;
  void (*CmdBindPipeline)(VkCommandBuffer, uint32_t, VkPipeline) = nullptr;
  void (*CmdBindDescriptorSets)(VkCommandBuffer, uint32_t, VkPipelineLayout, uint32_t, uint32_t, const VkDescriptorSet*, uint32_t, const uint32_t*) = nullptr;
  void (*CmdPushConstants)(VkCommandBuffer, VkPipelineLayout, VkFlags, uint32_t, uint32_t, const void*) = nullptr;
  void (*CmdDispatch)(VkCommandBuffer, uint32_t, uint32_t, uint32_t) = nullptr;
  VkResult (*CreateFence)(VkDevice, const VkFenceCreateInfo*, const void*, VkFence*) = nullptr;
  void (*DestroyFence)(VkDevice, VkFence, const void*) = nullptr;
  VkResult (*WaitForFences)(VkDevice, uint32_t, const VkFence*, VkBool32, uint64_t) = nullptr;
  VkResult (*QueueSubmit)(VkQueue, uint32_t, const VkSubmitInfo*, VkFence) = nullptr;
  VkResult (*QueueWaitIdle)(VkQueue) = nullptr;

  template <typename Fn>
  bool bind(Fn& slot, const char* name) {
    slot = reinterpret_cast<Fn>(hal::library_symbol(lib, name));
    return slot != nullptr;
  }

  bool load() {
    if (bound || !why.empty()) return bound;
    for (const char* n : {"libvulkan.so.1", "libvulkan.so", "libMoltenVK.dylib",
                          "libvulkan.1.dylib", "vulkan-1.dll"}) {
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
    bind(GetPhysicalDeviceProperties2, "vkGetPhysicalDeviceProperties2");
    bind(GetPhysicalDeviceMemoryProperties2, "vkGetPhysicalDeviceMemoryProperties2");
    bind(EnumerateDeviceExtensionProperties, "vkEnumerateDeviceExtensionProperties");
    ok &= bind(CreateDevice, "vkCreateDevice");
    ok &= bind(DestroyDevice, "vkDestroyDevice");
    ok &= bind(GetDeviceQueue, "vkGetDeviceQueue");
    ok &= bind(CreateBuffer, "vkCreateBuffer");
    ok &= bind(DestroyBuffer, "vkDestroyBuffer");
    ok &= bind(GetBufferMemoryRequirements, "vkGetBufferMemoryRequirements");
    ok &= bind(AllocateMemory, "vkAllocateMemory");
    ok &= bind(FreeMemory, "vkFreeMemory");
    ok &= bind(BindBufferMemory, "vkBindBufferMemory");
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
    ok &= bind(FreeCommandBuffers, "vkFreeCommandBuffers");
    ok &= bind(BeginCommandBuffer, "vkBeginCommandBuffer");
    ok &= bind(EndCommandBuffer, "vkEndCommandBuffer");
    ok &= bind(CmdBindPipeline, "vkCmdBindPipeline");
    ok &= bind(CmdBindDescriptorSets, "vkCmdBindDescriptorSets");
    ok &= bind(CmdPushConstants, "vkCmdPushConstants");
    ok &= bind(CmdDispatch, "vkCmdDispatch");
    ok &= bind(CreateFence, "vkCreateFence");
    ok &= bind(DestroyFence, "vkDestroyFence");
    ok &= bind(WaitForFences, "vkWaitForFences");
    ok &= bind(QueueSubmit, "vkQueueSubmit");
    ok &= bind(QueueWaitIdle, "vkQueueWaitIdle");
    if (!ok) { why = "the Vulkan loader is here but does not export the 1.0 core this expects"; return false; }
    bound = true;
    return true;
  }
};

VulkanApi& vk() { static VulkanApi api; return api; }

uint64_t fnv1a_hash(const std::string& str) {
  uint64_t hash = 14695981039346656037ULL;
  for (char c : str) { hash ^= static_cast<uint8_t>(c); hash *= 1099511628211ULL; }
  return hash;
}

// The push constants the shader declares, in its layout.
struct Params {
  uint32_t op, n, m, k, nn;
  float scalar;
};

constexpr uint32_t OP_FILL = 0, OP_ADD = 1, OP_MUL = 2, OP_SCALE = 3,
                   OP_RELU = 4, OP_COPY = 5, OP_MATMUL = 6;

}  // namespace

struct VulkanComputeBackend::Gpu {
  VkInstance instance = nullptr;
  VkPhysicalDevice physical = nullptr;
  VkDevice device = nullptr;
  VkQueue queue = nullptr;
  uint32_t queue_family = 0;
  VkShaderModule shader = nullptr;
  VkDescriptorSetLayout set_layout = nullptr;
  VkPipelineLayout layout = nullptr;
  VkPipeline pipeline = nullptr;
  VkCommandPool command_pool = nullptr;
  VkPhysicalDeviceMemoryProperties memory{};
  hal::Mutex synced_mu;
  std::map<MemoryID, uint64_t> synced;   // Device memory -> the generation its buffer holds
  std::string pci;            // "0000:01:00.0" from VK_EXT_pci_bus_info, empty when the driver does not say
  bool memory_budget = false; // VK_EXT_memory_budget enabled on this device
  uint32_t local_heap = 0;    // the device-local heap the capacity was read from
};

namespace {
constexpr uint32_t ST_PHYSICAL_DEVICE_PROPERTIES_2 = 1000059001;
constexpr uint32_t ST_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2 = 1000059006;
constexpr uint32_t ST_PHYSICAL_DEVICE_PCI_BUS_INFO_PROPERTIES_EXT = 1000212000;
constexpr uint32_t ST_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT = 1000237000;
struct VkPhysicalDeviceProperties2 { uint32_t sType; void* pNext; VkPhysicalDeviceProperties properties; };
struct VkPhysicalDevicePCIBusInfoPropertiesEXT { uint32_t sType; void* pNext; uint32_t pciDomain, pciBus, pciDevice, pciFunction; };
struct VkPhysicalDeviceMemoryProperties2 { uint32_t sType; void* pNext; VkPhysicalDeviceMemoryProperties memoryProperties; };
struct VkPhysicalDeviceMemoryBudgetPropertiesEXT { uint32_t sType; void* pNext; VkDeviceSize heapBudget[16]; VkDeviceSize heapUsage[16]; };
struct VkExtensionProperties { char extensionName[256]; uint32_t specVersion; };
}  // namespace

namespace {

// A storage buffer the host can write and read, which is what a compute
// dispatch over operands living in system memory needs.
struct HostBuffer {
  VulkanComputeBackend::Gpu* gpu = nullptr;
  VkBuffer buffer = nullptr;
  VkDeviceMemory memory = nullptr;
  VkDeviceSize bytes = 0;

  ~HostBuffer() {
    if (!gpu) return;
    if (buffer) vk().DestroyBuffer(gpu->device, buffer, nullptr);
    if (memory) vk().FreeMemory(gpu->device, memory, nullptr);
  }

  Result<void> create(VulkanComputeBackend::Gpu* g, VkDeviceSize size) {
    gpu = g;
    bytes = size ? size : 4;
    VkBufferCreateInfo bi{};
    bi.sType = ST_BUFFER_CREATE_INFO;
    bi.size = bytes;
    bi.usage = BUFFER_USAGE_STORAGE;
    if (vk().CreateBuffer(g->device, &bi, nullptr, &buffer) != VK_SUCCESS_) {
      return Result<void>(Status::Exhausted, "vkCreateBuffer refused");
    }
    VkMemoryRequirements req{};
    vk().GetBufferMemoryRequirements(g->device, buffer, &req);

    uint32_t want = MEMORY_PROPERTY_HOST_VISIBLE | MEMORY_PROPERTY_HOST_COHERENT;
    uint32_t chosen = UINT32_MAX;
    for (uint32_t i = 0; i < g->memory.memoryTypeCount; ++i) {
      if (!(req.memoryTypeBits & (1u << i))) continue;
      if ((g->memory.memoryTypes[i].propertyFlags & want) == want) { chosen = i; break; }
    }
    if (chosen == UINT32_MAX) {
      return Result<void>(Status::Unsupported, "this device has no host-visible coherent memory to stage through");
    }
    VkMemoryAllocateInfo ai{};
    ai.sType = ST_MEMORY_ALLOCATE_INFO;
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = chosen;
    if (vk().AllocateMemory(g->device, &ai, nullptr, &memory) != VK_SUCCESS_) {
      return Result<void>(Status::Exhausted, "vkAllocateMemory refused");
    }
    if (vk().BindBufferMemory(g->device, buffer, memory, 0) != VK_SUCCESS_) {
      return Result<void>(Status::IoError, "vkBindBufferMemory refused");
    }
    return Ok();
  }

  Result<void> write(const void* src, size_t n) {
    if (!n) return Ok();
    void* p = nullptr;
    if (vk().MapMemory(gpu->device, memory, 0, bytes, 0, &p) != VK_SUCCESS_ || !p) {
      return Result<void>(Status::IoError, "vkMapMemory refused");
    }
    std::memcpy(p, src, n);
    vk().UnmapMemory(gpu->device, memory);
    return Ok();
  }

  Result<void> read(void* dst, size_t n) const {
    if (!n) return Ok();
    void* p = nullptr;
    if (vk().MapMemory(gpu->device, memory, 0, bytes, 0, &p) != VK_SUCCESS_ || !p) {
      return Result<void>(Status::IoError, "vkMapMemory refused");
    }
    std::memcpy(dst, p, n);
    vk().UnmapMemory(gpu->device, memory);
    return Ok();
  }
};

// MemoryType::Device on a Vulkan device: a storage buffer the shaders bind
// directly, plus the host mirror the fabric reads and writes. The buffer is
// host-visible (that is how this backend reaches it), so on a discrete card it
// may sit in system memory the GPU reads over the bus rather than in VRAM; what
// it saves is the per-operation buffer and copy. It is rewritten from the
// mirror only when the mirror has moved past it (by generation).
char kVulkanOwner;   // its address tags a Mapping whose device_ctx is a HostBuffer of this backend

class VulkanDeviceMemoryProvider final : public MemoryProvider {
 public:
  VulkanDeviceMemoryProvider(hal::Host& host, std::shared_ptr<VulkanComputeBackend::Gpu> gpu, uint64_t capacity)
      : host_(host), gpu_(std::move(gpu)), capacity_(capacity) {}
  MemoryType type() const override { return MemoryType::Device; }
  uint64_t capacity() const override { return capacity_; }

  Result<hal::Mapping> allocate(uint64_t size, const MemoryID&) override {
    auto m = host_.map(size, hal::MappingKind::Private);
    if (!m.ok()) return m;
    auto buf = std::make_unique<HostBuffer>();
    auto r = buf->create(gpu_.get(), size);
    if (!r.ok()) {
      host_.unmap(*m);
      return Result<hal::Mapping>(r.status(), "device memory allocation failed: " + r.error().message);
    }
    m->device_ctx = buf.release();
    m->device_owner = &kVulkanOwner;
    return m;
  }

  void release(hal::Mapping& m) override {
    if (m.device_ctx && m.device_owner == &kVulkanOwner) {
      delete static_cast<HostBuffer*>(m.device_ctx);
      m.device_ctx = nullptr;
      m.device_owner = nullptr;
    }
    host_.unmap(m);
  }

 private:
  hal::Host& host_;
  std::shared_ptr<VulkanComputeBackend::Gpu> gpu_;
  uint64_t capacity_;
};

}  // namespace

VulkanComputeBackend::VulkanComputeBackend(const DeviceID& id, int ordinal) : id_(id) {
  auto& api = vk();
  if (!api.load()) { unavailable_ = api.why; return; }

  auto gpu = std::make_unique<Gpu>();

  VkApplicationInfo app{};
  app.sType = ST_APPLICATION_INFO;
  app.pApplicationName = "aios";
  app.pEngineName = "aios";
  // 1.1 makes vkGetPhysicalDevice*Properties2 legal (PCI address, memory
  // budget). A 1.0-only loader refuses it, and everything else here is core
  // 1.0, so that refusal falls back rather than failing the device.
  app.apiVersion = (1u << 22) | (1u << 12);
  VkInstanceCreateInfo ici{};
  ici.sType = ST_INSTANCE_CREATE_INFO;
  ici.pApplicationInfo = &app;
  bool instance_11 = api.CreateInstance(&ici, nullptr, &gpu->instance) == VK_SUCCESS_;
  if (!instance_11) {
    app.apiVersion = (1u << 22) | (0u << 12);
    if (api.CreateInstance(&ici, nullptr, &gpu->instance) != VK_SUCCESS_) {
      unavailable_ = "vkCreateInstance refused; there is a loader but no working driver behind it";
      return;
    }
  }

  uint32_t count = 0;
  api.EnumeratePhysicalDevices(gpu->instance, &count, nullptr);
  if (count == 0) {
    api.DestroyInstance(gpu->instance, nullptr);
    unavailable_ = "the Vulkan loader reports no physical devices";
    return;
  }
  std::vector<VkPhysicalDevice> devices(count);
  api.EnumeratePhysicalDevices(gpu->instance, &count, devices.data());

  struct Candidate {
    VkPhysicalDevice pd;
    uint32_t queue_family;
    uint32_t queue_count;
    VkPhysicalDeviceProperties props;
  };
  std::vector<Candidate> candidates;

  for (VkPhysicalDevice pd : devices) {
    uint32_t families = 0;
    api.GetPhysicalDeviceQueueFamilyProperties(pd, &families, nullptr);
    if (!families) continue;
    std::vector<VkQueueFamilyProperties> qs(families);
    api.GetPhysicalDeviceQueueFamilyProperties(pd, &families, qs.data());
    for (uint32_t i = 0; i < families; ++i) {
      if (qs[i].queueFlags & QUEUE_COMPUTE_BIT) {
        VkPhysicalDeviceProperties p{};
        api.GetPhysicalDeviceProperties(pd, &p);
        candidates.push_back({pd, i, qs[i].queueCount, p});
        break;
      }
    }
  }

  if (candidates.empty()) {
    api.DestroyInstance(gpu->instance, nullptr);
    unavailable_ = "no Vulkan device on this machine has a compute queue";
    return;
  }

  if (ordinal < 0 || ordinal >= (int)candidates.size()) {
    api.DestroyInstance(gpu->instance, nullptr);
    unavailable_ = "ordinal " + std::to_string(ordinal) + " out of range (" + std::to_string(candidates.size()) + " device(s))";
    return;
  }

  const auto& cand = candidates[ordinal];
  gpu->physical = cand.pd;
  gpu->queue_family = cand.queue_family;
  const auto& props = cand.props;
  cap_.lanes = cand.queue_count ? cand.queue_count : 1;

  api.GetPhysicalDeviceMemoryProperties(gpu->physical, &gpu->memory);
  uint64_t device_local = 0;
  for (uint32_t i = 0; i < gpu->memory.memoryHeapCount; ++i) {
    if (gpu->memory.memoryHeaps[i].flags & 0x00000001 /* VK_MEMORY_HEAP_DEVICE_LOCAL_BIT */) {
      if (device_local == 0) gpu->local_heap = i;
      device_local += gpu->memory.memoryHeaps[i].size;
    }
  }
  if (device_local == 0) {
    for (uint32_t i = 0; i < gpu->memory.memoryHeapCount; ++i) {
      if (gpu->memory.memoryHeaps[i].size > device_local) { device_local = gpu->memory.memoryHeaps[i].size; gpu->local_heap = i; }
    }
  }
  cap_.memory = device_local;

  // Which optional device extensions this driver offers. The PCI address is
  // what lets the same card be recognised across CUDA, OpenCL and Vulkan; the
  // budget is the driver's own live usage figure when there is no NVML.
  bool has_pci_ext = false, has_budget_ext = false;
  if (api.EnumerateDeviceExtensionProperties) {
    uint32_t n = 0;
    if (api.EnumerateDeviceExtensionProperties(gpu->physical, nullptr, &n, nullptr) == VK_SUCCESS_ && n) {
      std::vector<VkExtensionProperties> exts(n);
      if (api.EnumerateDeviceExtensionProperties(gpu->physical, nullptr, &n, exts.data()) == VK_SUCCESS_) {
        for (uint32_t i = 0; i < n; ++i) {
          if (!std::strcmp(exts[i].extensionName, "VK_EXT_pci_bus_info")) has_pci_ext = true;
          if (!std::strcmp(exts[i].extensionName, "VK_EXT_memory_budget")) has_budget_ext = true;
        }
      }
    }
  }
  if (instance_11 && has_pci_ext && api.GetPhysicalDeviceProperties2) {
    VkPhysicalDevicePCIBusInfoPropertiesEXT pci{};
    pci.sType = ST_PHYSICAL_DEVICE_PCI_BUS_INFO_PROPERTIES_EXT;
    VkPhysicalDeviceProperties2 p2{};
    p2.sType = ST_PHYSICAL_DEVICE_PROPERTIES_2;
    p2.pNext = &pci;
    api.GetPhysicalDeviceProperties2(gpu->physical, &p2);
    char buf[32];
    std::snprintf(buf, sizeof buf, "%04x:%02x:%02x.%x", pci.pciDomain & 0xffff, pci.pciBus & 0xff, pci.pciDevice & 0x1f, pci.pciFunction & 0x7);
    gpu->pci = buf;
  }
  const char* device_exts[2];
  uint32_t device_ext_count = 0;
  if (instance_11 && has_budget_ext && api.GetPhysicalDeviceMemoryProperties2) { device_exts[device_ext_count++] = "VK_EXT_memory_budget"; gpu->memory_budget = true; }
  if (has_pci_ext) device_exts[device_ext_count++] = "VK_EXT_pci_bus_info";
  // Vulkan reports no clock and no core count, so no FLOP rate is claimed:
  // the scheduler weighs this device by what it can be told, and a number
  // written here would only be a guess with authority.
  cap_.flops_per_second = 0;
  cap_.native_memory = (props.deviceType == 4 /* VK_PHYSICAL_DEVICE_TYPE_CPU */) ? MemoryType::System : MemoryType::Device;
  cap_.reads_host_memory = true;   // stages from the operands' host windows itself

  float priority = 1.0f;
  VkDeviceQueueCreateInfo qci{};
  qci.sType = ST_DEVICE_QUEUE_CREATE_INFO;
  qci.queueFamilyIndex = gpu->queue_family;
  qci.queueCount = 1;
  qci.pQueuePriorities = &priority;
  VkDeviceCreateInfo dci{};
  dci.sType = ST_DEVICE_CREATE_INFO;
  dci.queueCreateInfoCount = 1;
  dci.pQueueCreateInfos = &qci;
  dci.enabledExtensionCount = device_ext_count;
  dci.ppEnabledExtensionNames = device_ext_count ? device_exts : nullptr;
  if (api.CreateDevice(gpu->physical, &dci, nullptr, &gpu->device) != VK_SUCCESS_) {
    api.DestroyInstance(gpu->instance, nullptr);
    unavailable_ = "vkCreateDevice refused";
    return;
  }
  api.GetDeviceQueue(gpu->device, gpu->queue_family, 0, &gpu->queue);

  VkShaderModuleCreateInfo smci{};
  smci.sType = ST_SHADER_MODULE_CREATE_INFO;
  smci.codeSize = kAiosComputeSpvWords * sizeof(uint32_t);
  smci.pCode = kAiosComputeSpv;
  if (api.CreateShaderModule(gpu->device, &smci, nullptr, &gpu->shader) != VK_SUCCESS_) {
    api.DestroyDevice(gpu->device, nullptr);
    api.DestroyInstance(gpu->instance, nullptr);
    unavailable_ = "the driver rejected the SPIR-V module";
    return;
  }

  VkDescriptorSetLayoutBinding bindings[3]{};
  for (uint32_t i = 0; i < 3; ++i) {
    bindings[i].binding = i;
    bindings[i].descriptorType = DESCRIPTOR_TYPE_STORAGE_BUFFER;
    bindings[i].descriptorCount = 1;
    bindings[i].stageFlags = SHADER_STAGE_COMPUTE;
  }
  VkDescriptorSetLayoutCreateInfo dslci{};
  dslci.sType = ST_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
  dslci.bindingCount = 3;
  dslci.pBindings = bindings;
  if (api.CreateDescriptorSetLayout(gpu->device, &dslci, nullptr, &gpu->set_layout) != VK_SUCCESS_) {
    unavailable_ = "vkCreateDescriptorSetLayout refused";
    return;
  }

  VkPushConstantRange range{};
  range.stageFlags = SHADER_STAGE_COMPUTE;
  range.offset = 0;
  range.size = sizeof(Params);
  VkPipelineLayoutCreateInfo plci{};
  plci.sType = ST_PIPELINE_LAYOUT_CREATE_INFO;
  plci.setLayoutCount = 1;
  plci.pSetLayouts = &gpu->set_layout;
  plci.pushConstantRangeCount = 1;
  plci.pPushConstantRanges = &range;
  if (api.CreatePipelineLayout(gpu->device, &plci, nullptr, &gpu->layout) != VK_SUCCESS_) {
    unavailable_ = "vkCreatePipelineLayout refused";
    return;
  }

  VkComputePipelineCreateInfo cpci{};
  cpci.sType = ST_COMPUTE_PIPELINE_CREATE_INFO;
  cpci.stage.sType = ST_PIPELINE_SHADER_STAGE_CREATE_INFO;
  cpci.stage.stage = SHADER_STAGE_COMPUTE;
  cpci.stage.module_ = gpu->shader;
  cpci.stage.pName = "main";
  cpci.layout = gpu->layout;
  cpci.basePipelineIndex = -1;
  if (api.CreateComputePipelines(gpu->device, nullptr, 1, &cpci, nullptr, &gpu->pipeline) != VK_SUCCESS_) {
    unavailable_ = "vkCreateComputePipelines refused the shader";
    return;
  }

  VkCommandPoolCreateInfo cpi{};
  cpi.sType = ST_COMMAND_POOL_CREATE_INFO;
  cpi.queueFamilyIndex = gpu->queue_family;
  if (api.CreateCommandPool(gpu->device, &cpi, nullptr, &gpu->command_pool) != VK_SUCCESS_) {
    unavailable_ = "vkCreateCommandPool refused";
    return;
  }

  // Prefixed so it can be named at the prompt: `dev=vulkan:` picks this one
  // even when the same card is also present through CUDA and OpenCL.
  name_ = "vulkan:" + std::string(props.deviceName) + " api" +
          std::to_string((props.apiVersion >> 22) & 0x7F) + "." +
          std::to_string((props.apiVersion >> 12) & 0x3FF) + " [" + std::to_string(ordinal) + "]";
  gpu_ = std::move(gpu);
}

VulkanComputeBackend::~VulkanComputeBackend() {
  if (!gpu_) return;
  auto& api = vk();
  if (gpu_->command_pool) api.DestroyCommandPool(gpu_->device, gpu_->command_pool, nullptr);
  if (gpu_->pipeline) api.DestroyPipeline(gpu_->device, gpu_->pipeline, nullptr);
  if (gpu_->layout) api.DestroyPipelineLayout(gpu_->device, gpu_->layout, nullptr);
  if (gpu_->set_layout) api.DestroyDescriptorSetLayout(gpu_->device, gpu_->set_layout, nullptr);
  if (gpu_->shader) api.DestroyShaderModule(gpu_->device, gpu_->shader, nullptr);
  if (gpu_->device) api.DestroyDevice(gpu_->device, nullptr);
  if (gpu_->instance) api.DestroyInstance(gpu_->instance, nullptr);
}

bool VulkanComputeBackend::supports(const Operation& op) const {
  if (!gpu_) return false;
  if (op.code == OpCode::Custom) {
    hal::LockGuard g(mu_);
    return kernels_.count(op.custom_kernel) > 0;
  }
  switch (op.code) {
    case OpCode::Fill: case OpCode::Add: case OpCode::Mul: case OpCode::Scale:
    case OpCode::Relu: case OpCode::Copy: case OpCode::MatMul:
      return true;
    default:
      // The shader has no kernel for these. Running them on the CPU and
      // reporting a GPU device is what this backend used to do.
      return false;
  }
}

Result<void> VulkanComputeBackend::register_kernel(const std::string& name, CustomKernel k) {
  hal::LockGuard g(mu_);
  kernels_[fnv1a_hash(name)] = std::move(k);
  return Ok();
}

uint32_t VulkanComputeBackend::busy_lanes() const { return busy_lanes_.load(); }

Result<DeviceMemoryStats> VulkanComputeBackend::memory_stats() const {
  if (!gpu_) return Result<DeviceMemoryStats>(Status::Unsupported, "no Vulkan device: " + unavailable_);
  // First choice: the vendor's management library by PCI address, which sees
  // every process on the card. Second: the driver's own budget/usage for the
  // device-local heap. Last: the capacity, labelled as such (empty source).
  if (!gpu_->pci.empty()) {
    auto nv = nvidia_memory_by_pci(gpu_->pci);
    if (nv.ok()) return nv;
  }
  auto& api = vk();
  if (gpu_->memory_budget && api.GetPhysicalDeviceMemoryProperties2) {
    VkPhysicalDeviceMemoryBudgetPropertiesEXT budget{};
    budget.sType = ST_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT;
    VkPhysicalDeviceMemoryProperties2 m2{};
    m2.sType = ST_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2;
    m2.pNext = &budget;
    api.GetPhysicalDeviceMemoryProperties2(gpu_->physical, &m2);
    DeviceMemoryStats s;
    s.total_bytes = cap_.memory;
    s.used_bytes = budget.heapUsage[gpu_->local_heap];
    uint64_t b = budget.heapBudget[gpu_->local_heap];
    s.free_bytes = b > s.used_bytes ? b - s.used_bytes : 0;
    s.source = "vulkan-budget";
    return s;
  }
  return Device::memory_stats();
}

Result<void> VulkanComputeBackend::execute(const Operation& op, MemoryFabric& fabric, uint32_t lanes) {
  if (!gpu_) return Result<void>(Status::Unsupported, "no Vulkan device: " + unavailable_);
  auto& api = vk();

  lanes = std::max<uint32_t>(1, std::min(lanes, cap_.lanes));
  busy_lanes_ += lanes;
  struct Guard { std::atomic<uint32_t>& b; uint32_t n; ~Guard() { b -= n; } } guard{busy_lanes_, lanes};

  std::vector<MemoryFabric::Window> in, out;
  for (const auto& t : op.inputs) {
    auto w = fabric.window(t.memory);
    if (!w.ok()) return w.error();
    if (t.offset + t.bytes() > w->size) return Result<void>(Status::Invalid, "input tensor exceeds memory");
    in.push_back(*w);
  }
  for (const auto& t : op.outputs) {
    auto w = fabric.window(t.memory);
    if (!w.ok()) return w.error();
    if (t.offset + t.bytes() > w->size) return Result<void>(Status::Invalid, "output tensor exceeds memory");
    out.push_back(*w);
  }

  if (op.code == OpCode::Custom) {
    CustomKernel k;
    {
      hal::LockGuard lg(mu_);
      auto it = kernels_.find(op.custom_kernel);
      if (it == kernels_.end()) return Result<void>(Status::NotFound, "custom kernel not registered on this device");
      k = it->second;
    }
    KernelArgs args{op, in, out, lanes};
    auto r = k(args);
    if (r.ok()) for (const auto& t : op.outputs) fabric.touch(t.memory);
    return r;
  }
  if (out.empty()) return Result<void>(Status::Invalid, "this operation writes nothing");

  Params params{};
  params.scalar = (float)op.scalar;
  uint64_t work = 0;
  uint64_t a_elems = 0, b_elems = 0, o_elems = op.outputs[0].elements();

  switch (op.code) {
    case OpCode::Fill:   params.op = OP_FILL;  work = o_elems; break;
    case OpCode::Copy:   params.op = OP_COPY;  break;
    case OpCode::Relu:   params.op = OP_RELU;  break;
    case OpCode::Scale:  params.op = OP_SCALE; break;
    case OpCode::Add:    params.op = OP_ADD;   break;
    case OpCode::Mul:    params.op = OP_MUL;   break;
    case OpCode::MatMul: params.op = OP_MATMUL; break;
    default: return Result<void>(Status::Unsupported,
        std::string("no Vulkan kernel for ") + opcode_name(op.code));
  }

  if (op.code == OpCode::MatMul) {
    if (in.size() < 2 || op.inputs[0].rank() < 2 || op.inputs[1].rank() < 2) {
      return Result<void>(Status::Invalid, "matmul needs two matrices");
    }
    params.m = (uint32_t)op.inputs[0].shape[op.inputs[0].rank() - 2];
    params.k = (uint32_t)op.inputs[0].shape[op.inputs[0].rank() - 1];
    params.nn = (uint32_t)op.inputs[1].shape[op.inputs[1].rank() - 1];
    if ((uint32_t)op.inputs[1].shape[op.inputs[1].rank() - 2] != params.k) {
      return Result<void>(Status::Invalid, "matmul inner dimensions disagree");
    }
    a_elems = (uint64_t)params.m * params.k;
    b_elems = (uint64_t)params.k * params.nn;
    o_elems = (uint64_t)params.m * params.nn;
    work = o_elems;
  } else if (op.code != OpCode::Fill) {
    if (in.empty()) return Result<void>(Status::Invalid, "this operation needs an input");
    a_elems = op.inputs[0].elements();
    b_elems = in.size() > 1 ? op.inputs[1].elements() : 0;
    work = std::min(o_elems, a_elems);
    if (b_elems) work = std::min<uint64_t>(work, b_elems);
    o_elems = work;
    params.n = (uint32_t)work;
  }
  if (op.code == OpCode::Fill) params.n = (uint32_t)work;
  if (work == 0) return Ok();

  // Operands: Vulkan Device memory of this device, bound in place when the
  // tensor starts the object (a descriptor offset would have to meet the
  // device's alignment), brought up to date first if the mirror is newer;
  // anything else through a temporary buffer for this call.
  auto own_buffer = [&](const MemoryID& id, uint64_t offset) -> HostBuffer* {
    const MemoryObject* o = fabric.lookup(id);
    if (!o || offset != 0 || o->type != MemoryType::Device || o->mapping.device_owner != &kVulkanOwner) return nullptr;
    auto* hb = static_cast<HostBuffer*>(o->mapping.device_ctx);
    return hb && hb->gpu == gpu_.get() ? hb : nullptr;
  };
  auto sync = [&](const MemoryID& id, HostBuffer* hb) -> Result<void> {
    const MemoryObject* o = fabric.lookup(id);
    hal::LockGuard g(gpu_->synced_mu);
    auto it = gpu_->synced.find(id);
    if (it != gpu_->synced.end() && it->second == o->generation) return Ok();
    AIOS_TRY(hb->write(o->base(), o->size));
    gpu_->synced[id] = o->generation;
    return Ok();
  };
  HostBuffer ta, tb, to;   // temporaries, for operands that are not this device's memory
  auto bind = [&](const Tensor* t, HostBuffer& temp, uint64_t elems, const uint8_t* src) -> Result<HostBuffer*> {
    if (t) if (HostBuffer* hb = own_buffer(t->memory, t->offset)) { AIOS_TRY(sync(t->memory, hb)); return hb; }
    AIOS_TRY(temp.create(gpu_.get(), std::max<uint64_t>(4, elems * sizeof(float))));
    if (src && elems) AIOS_TRY(temp.write(src, elems * sizeof(float)));
    return &temp;
  };
  auto pa = bind(a_elems ? &op.inputs[0] : nullptr, ta, a_elems, a_elems ? in[0].base + op.inputs[0].offset : nullptr);
  if (!pa.ok()) return pa.error();
  auto pb = bind(b_elems ? &op.inputs[1] : nullptr, tb, b_elems, b_elems ? in[1].base + op.inputs[1].offset : nullptr);
  if (!pb.ok()) return pb.error();
  auto po = bind(&op.outputs[0], to, o_elems, nullptr);
  if (!po.ok()) return po.error();
  HostBuffer& ba = **pa;
  HostBuffer& bb = **pb;
  HostBuffer& bo = **po;
  const bool out_in_place = &bo != &to;

  VkDescriptorPoolSize size{DESCRIPTOR_TYPE_STORAGE_BUFFER, 3};
  VkDescriptorPoolCreateInfo dpci{};
  dpci.sType = ST_DESCRIPTOR_POOL_CREATE_INFO;
  dpci.flags = DESCRIPTOR_POOL_FREE_SET;
  dpci.maxSets = 1;
  dpci.poolSizeCount = 1;
  dpci.pPoolSizes = &size;
  VkDescriptorPool pool = nullptr;
  if (api.CreateDescriptorPool(gpu_->device, &dpci, nullptr, &pool) != VK_SUCCESS_) {
    return Result<void>(Status::Exhausted, "vkCreateDescriptorPool refused");
  }
  struct PoolGuard { VkDevice d; VkDescriptorPool p; ~PoolGuard() { if (p) vk().DestroyDescriptorPool(d, p, nullptr); } }
      pool_guard{gpu_->device, pool};

  VkDescriptorSetAllocateInfo dsai{};
  dsai.sType = ST_DESCRIPTOR_SET_ALLOCATE_INFO;
  dsai.descriptorPool = pool;
  dsai.descriptorSetCount = 1;
  dsai.pSetLayouts = &gpu_->set_layout;
  VkDescriptorSet set = nullptr;
  if (api.AllocateDescriptorSets(gpu_->device, &dsai, &set) != VK_SUCCESS_) {
    return Result<void>(Status::Exhausted, "vkAllocateDescriptorSets refused");
  }

  VkDescriptorBufferInfo infos[3] = {
    {ba.buffer, 0, ba.bytes}, {bb.buffer, 0, bb.bytes}, {bo.buffer, 0, bo.bytes},
  };
  VkWriteDescriptorSet writes[3]{};
  for (uint32_t i = 0; i < 3; ++i) {
    writes[i].sType = ST_WRITE_DESCRIPTOR_SET;
    writes[i].dstSet = set;
    writes[i].dstBinding = i;
    writes[i].descriptorCount = 1;
    writes[i].descriptorType = DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[i].pBufferInfo = &infos[i];
  }
  api.UpdateDescriptorSets(gpu_->device, 3, writes, 0, nullptr);

  VkCommandBufferAllocateInfo cbai{};
  cbai.sType = ST_COMMAND_BUFFER_ALLOCATE_INFO;
  cbai.commandPool = gpu_->command_pool;
  cbai.level = 0;
  cbai.commandBufferCount = 1;
  VkCommandBuffer cmd = nullptr;
  if (api.AllocateCommandBuffers(gpu_->device, &cbai, &cmd) != VK_SUCCESS_) {
    return Result<void>(Status::Exhausted, "vkAllocateCommandBuffers refused");
  }
  struct CmdGuard { VkDevice d; VkCommandPool p; VkCommandBuffer c;
                    ~CmdGuard() { if (c) vk().FreeCommandBuffers(d, p, 1, &c); } }
      cmd_guard{gpu_->device, gpu_->command_pool, cmd};

  VkCommandBufferBeginInfo cbbi{};
  cbbi.sType = ST_COMMAND_BUFFER_BEGIN_INFO;
  cbbi.flags = COMMAND_BUFFER_USAGE_ONE_TIME;
  api.BeginCommandBuffer(cmd, &cbbi);
  api.CmdBindPipeline(cmd, PIPELINE_BIND_POINT_COMPUTE, gpu_->pipeline);
  api.CmdBindDescriptorSets(cmd, PIPELINE_BIND_POINT_COMPUTE, gpu_->layout, 0, 1, &set, 0, nullptr);
  api.CmdPushConstants(cmd, gpu_->layout, SHADER_STAGE_COMPUTE, 0, sizeof(Params), &params);
  api.CmdDispatch(cmd, (uint32_t)((work + 63) / 64), 1, 1);
  api.EndCommandBuffer(cmd);

  VkFenceCreateInfo fci{};
  fci.sType = ST_FENCE_CREATE_INFO;
  VkFence fence = nullptr;
  api.CreateFence(gpu_->device, &fci, nullptr, &fence);
  struct FenceGuard { VkDevice d; VkFence f; ~FenceGuard() { if (f) vk().DestroyFence(d, f, nullptr); } }
      fence_guard{gpu_->device, fence};

  VkSubmitInfo si{};
  si.sType = ST_SUBMIT_INFO;
  si.commandBufferCount = 1;
  si.pCommandBuffers = &cmd;
  if (api.QueueSubmit(gpu_->queue, 1, &si, fence) != VK_SUCCESS_) {
    return Result<void>(Status::IoError, "vkQueueSubmit refused");
  }
  if (api.WaitForFences(gpu_->device, 1, &fence, 1, 30000000000ull) != VK_SUCCESS_) {
    return Result<void>(Status::Busy, "the dispatch did not finish within 30 seconds");
  }

  AIOS_TRY(bo.read(out[0].base + op.outputs[0].offset, o_elems * sizeof(float)));
  for (const auto& t : op.outputs) fabric.touch(t.memory);
  if (out_in_place) {
    // The result was written in the output's own buffer: it stays current past
    // the generation the touch just moved to.
    hal::LockGuard g(gpu_->synced_mu);
    if (const MemoryObject* o = fabric.lookup(op.outputs[0].memory)) gpu_->synced[op.outputs[0].memory] = o->generation;
  }
  return Ok();
}

AcceleratorReport register_vulkan_devices(DeviceFabric& devices, MemoryFabric& memory, hal::Host& host, IdGenerator& ids) {
  AcceleratorReport rep;
  if (const std::string e = host.env("AIOS_NO_VULKAN"); !e.empty() && e != "0") {
    rep.note = "Vulkan disabled (AIOS_NO_VULKAN)";
    return rep;
  }
  auto& api = vk();
  if (!api.load()) { rep.note = api.why; return rep; }

  VkApplicationInfo app{};
  app.sType = ST_APPLICATION_INFO;
  app.pApplicationName = "aios";
  app.apiVersion = (1u << 22) | (0u << 12);
  VkInstanceCreateInfo ici{};
  ici.sType = ST_INSTANCE_CREATE_INFO;
  ici.pApplicationInfo = &app;
  VkInstance inst = nullptr;
  if (api.CreateInstance(&ici, nullptr, &inst) != VK_SUCCESS_) {
    rep.note = "Vulkan loader present but no working driver";
    return rep;
  }

  uint32_t count = 0;
  api.EnumeratePhysicalDevices(inst, &count, nullptr);
  api.DestroyInstance(inst, nullptr);

  if (count == 0) {
    rep.note = "the Vulkan loader reports no physical devices";
    return rep;
  }

  bool provided_memory = memory.has_provider(MemoryType::Device);
  for (uint32_t i = 0; i < count; ++i) {
    auto dev = std::make_unique<VulkanComputeBackend>(ids.next_as<DeviceID>(), (int)i);
    if (!dev->on_gpu()) {
      rep.note += (rep.note.empty() ? "" : "; ") + ("device " + std::to_string(i) + ": " + dev->unavailable());
      continue;
    }
    if (!provided_memory && !memory.has_provider(MemoryType::Device) && dev->gpu() && dev->gpu()->device) {
      memory.register_provider(std::make_unique<VulkanDeviceMemoryProvider>(host, dev->gpu(), dev->capacity().memory));
      provided_memory = true;
    }
    rep.devices.push_back(dev->name());
    devices.add(std::move(dev));
  }
  return rep;
}

}  // namespace aios::compute
