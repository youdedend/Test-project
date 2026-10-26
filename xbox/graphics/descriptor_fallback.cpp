// SPDX-License-Identifier: GPL-2.0-or-later
#include "descriptor_fallback.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <stdexcept>
#include <unordered_map>
#include <vector>

extern "C" uint64_t bb_uwp_app_memory_usage(void);
extern "C" uint64_t bb_uwp_app_memory_limit(void);

namespace BbUwp {
namespace {
PFN_vkGetInstanceProcAddr g_real_get_instance_proc_addr{};
PFN_vkGetPhysicalDeviceMemoryProperties g_real_get_mem_props{};
PFN_vkGetPhysicalDeviceMemoryProperties2 g_real_get_mem_props2{};
PFN_vkGetPhysicalDeviceProperties2 g_real_get_props2{};

uint64_t QueryXboxAppMemoryLimit() {
    const uint64_t limit = bb_uwp_app_memory_limit();
    return limit >= (512ULL << 20) ? limit : (5ULL << 30);
}

void PatchUwpMemoryProperties(VkPhysicalDeviceMemoryProperties* props) {
    if (!props || props->memoryHeapCount == 0) {
        return;
    }
    const VkDeviceSize app_limit = static_cast<VkDeviceSize>(QueryXboxAppMemoryLimit());
    uint32_t primary_heap = UINT32_MAX;
    VkDeviceSize max_local = 0;
    for (uint32_t i = 0; i < props->memoryHeapCount; ++i) {
        if ((props->memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) != 0 &&
            props->memoryHeaps[i].size >= max_local) {
            max_local = props->memoryHeaps[i].size;
            primary_heap = i;
        }
    }
    if (primary_heap == UINT32_MAX) {
        primary_heap = 0;
    }
    /* On Xbox Series S (SraKmd_arden, UMA == TRUE), Mesa Dozen skips dedicated_video_memory
     * and exposes a single VK_MEMORY_HEAP_DEVICE_LOCAL_BIT heap sized to shared_system_memory
     * (1,280 MiB), causing VMA to reject all buffer/image allocations past 1,280 MiB even when
     * ~2 GiB of Xbox unified Game Mode RAM is free. Align the UMA heap ceiling to Xbox's real
     * AppMemoryUsageLimit (5,120 MiB in Game Mode). */
    if (props->memoryHeaps[primary_heap].size < app_limit) {
        const VkDeviceSize old_size = props->memoryHeaps[primary_heap].size;
        props->memoryHeaps[primary_heap].size = app_limit;
        static std::atomic<bool> logged{false};
        if (!logged.exchange(true)) {
            std::printf("[uwp-vk] Upgraded UMA VkMemoryHeap[%u] size from %llu MiB -> %llu MiB "
                        "(Xbox AppMemoryUsageLimit)\n",
                        primary_heap,
                        static_cast<unsigned long long>(old_size >> 20),
                        static_cast<unsigned long long>(app_limit >> 20));
            std::fflush(stdout);
        }
    }
}

VKAPI_ATTR void VKAPI_CALL UwpGetPhysicalDeviceMemoryProperties(
    VkPhysicalDevice physicalDevice, VkPhysicalDeviceMemoryProperties* pMemoryProperties) {
    g_real_get_mem_props(physicalDevice, pMemoryProperties);
    PatchUwpMemoryProperties(pMemoryProperties);
}

VKAPI_ATTR void VKAPI_CALL UwpGetPhysicalDeviceMemoryProperties2(
    VkPhysicalDevice physicalDevice, VkPhysicalDeviceMemoryProperties2* pMemoryProperties) {
    g_real_get_mem_props2(physicalDevice, pMemoryProperties);
    if (!pMemoryProperties) {
        return;
    }
    PatchUwpMemoryProperties(&pMemoryProperties->memoryProperties);
    const VkDeviceSize app_limit = static_cast<VkDeviceSize>(QueryXboxAppMemoryLimit());
    const VkDeviceSize safe_budget =
        app_limit > (128ULL << 20) ? (app_limit - (128ULL << 20)) : app_limit;
    const VkDeviceSize app_usage = std::min<VkDeviceSize>(
        static_cast<VkDeviceSize>(bb_uwp_app_memory_usage()), safe_budget);

    for (auto* ext = reinterpret_cast<VkBaseOutStructure*>(pMemoryProperties->pNext);
         ext != nullptr; ext = ext->pNext) {
        if (ext->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT) {
            auto* budget = reinterpret_cast<VkPhysicalDeviceMemoryBudgetPropertiesEXT*>(ext);
            for (uint32_t i = 0; i < pMemoryProperties->memoryProperties.memoryHeapCount; ++i) {
                if ((pMemoryProperties->memoryProperties.memoryHeaps[i].flags &
                     VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) != 0 &&
                    (pMemoryProperties->memoryProperties.memoryHeapCount == 1 ||
                     budget->heapBudget[i] < safe_budget)) {
                    budget->heapBudget[i] = safe_budget;
                    budget->heapUsage[i] = app_usage;
                }
            }
        }
    }
}

VKAPI_ATTR void VKAPI_CALL UwpGetPhysicalDeviceProperties2(
    VkPhysicalDevice physicalDevice, VkPhysicalDeviceProperties2* pProperties) {
    g_real_get_props2(physicalDevice, pProperties);
    if (!pProperties) {
        return;
    }
    constexpr VkDeviceSize kMinMaxAlloc = 2ULL << 30;
    for (auto* ext = reinterpret_cast<VkBaseOutStructure*>(pProperties->pNext); ext != nullptr;
         ext = ext->pNext) {
        if (ext->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_PROPERTIES) {
            auto* p = reinterpret_cast<VkPhysicalDeviceVulkan11Properties*>(ext);
            p->maxMemoryAllocationSize = std::max(p->maxMemoryAllocationSize, kMinMaxAlloc);
        } else if (ext->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_3_PROPERTIES) {
            auto* p = reinterpret_cast<VkPhysicalDeviceMaintenance3Properties*>(ext);
            p->maxMemoryAllocationSize = std::max(p->maxMemoryAllocationSize, kMinMaxAlloc);
        } else if (ext->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_4_PROPERTIES) {
            auto* p = reinterpret_cast<VkPhysicalDeviceMaintenance4Properties*>(ext);
            p->maxBufferSize = std::max(p->maxBufferSize, kMinMaxAlloc);
        }
    }
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL UwpGetInstanceProcAddr(VkInstance instance,
                                                                const char* pName) {
    if (!pName || !g_real_get_instance_proc_addr) {
        return nullptr;
    }
    if (std::strcmp(pName, "vkGetInstanceProcAddr") == 0) {
        return reinterpret_cast<PFN_vkVoidFunction>(&UwpGetInstanceProcAddr);
    }
    PFN_vkVoidFunction fn = g_real_get_instance_proc_addr(instance, pName);
    if (!fn) {
        return nullptr;
    }
    if (std::strcmp(pName, "vkGetPhysicalDeviceMemoryProperties") == 0) {
        g_real_get_mem_props = reinterpret_cast<PFN_vkGetPhysicalDeviceMemoryProperties>(fn);
        return reinterpret_cast<PFN_vkVoidFunction>(&UwpGetPhysicalDeviceMemoryProperties);
    }
    if (std::strcmp(pName, "vkGetPhysicalDeviceMemoryProperties2") == 0 ||
        std::strcmp(pName, "vkGetPhysicalDeviceMemoryProperties2KHR") == 0) {
        g_real_get_mem_props2 = reinterpret_cast<PFN_vkGetPhysicalDeviceMemoryProperties2>(fn);
        return reinterpret_cast<PFN_vkVoidFunction>(&UwpGetPhysicalDeviceMemoryProperties2);
    }
    if (std::strcmp(pName, "vkGetPhysicalDeviceProperties2") == 0 ||
        std::strcmp(pName, "vkGetPhysicalDeviceProperties2KHR") == 0) {
        g_real_get_props2 = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties2>(fn);
        return reinterpret_cast<PFN_vkVoidFunction>(&UwpGetPhysicalDeviceProperties2);
    }
    return fn;
}
struct Layout {
    std::vector<VkDescriptorSetLayoutBinding> bindings;
};
struct PipelineLayout {
    std::vector<VkDescriptorSetLayout> sets;
};
struct Commands {
    VkCommandPool owner{};
    std::vector<VkDescriptorPool> pools;
    size_t pool = 0;
    std::unordered_map<VkPipelineLayout, std::unordered_map<uint32_t, VkDescriptorSet>> pushed;
};

std::mutex mutex;
VkDevice active_device{};
std::unordered_map<VkDescriptorSetLayout, Layout> layouts;
std::unordered_map<VkPipelineLayout, PipelineLayout> pipelines;
std::unordered_map<VkCommandBuffer, Commands> commands;

PFN_vkCreateDescriptorSetLayout create_layout;
PFN_vkDestroyDescriptorSetLayout destroy_layout;
PFN_vkCreatePipelineLayout create_pipeline;
PFN_vkDestroyPipelineLayout destroy_pipeline;
PFN_vkAllocateCommandBuffers allocate_commands;
PFN_vkFreeCommandBuffers free_commands;
PFN_vkDestroyCommandPool destroy_command_pool;
PFN_vkBeginCommandBuffer begin_commands;
PFN_vkDestroyDevice destroy_device;

void Require(VkResult r, const char* operation) {
    if (r != VK_SUCCESS) {
        std::fprintf(stderr, "%s: Vulkan %d\n", operation, r);
        throw std::runtime_error(operation);
    }
}

VKAPI_ATTR VkResult VKAPI_CALL CreateLayout(VkDevice d,
                                            const VkDescriptorSetLayoutCreateInfo* info,
                                            const VkAllocationCallbacks* alloc,
                                            VkDescriptorSetLayout* result) {
    auto core = *info;
    core.flags &= ~VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR;
    VkResult r = create_layout(d, &core, alloc, result);
    if (r == VK_SUCCESS) {
        std::scoped_lock lock(mutex);
        layouts[*result].bindings.assign(info->pBindings, info->pBindings + info->bindingCount);
    }
    return r;
}

VKAPI_ATTR void VKAPI_CALL DestroyLayout(VkDevice d, VkDescriptorSetLayout layout,
                                         const VkAllocationCallbacks* alloc) {
    {
        std::scoped_lock lock(mutex);
        layouts.erase(layout);
    }
    destroy_layout(d, layout, alloc);
}

VKAPI_ATTR VkResult VKAPI_CALL CreatePipeline(VkDevice d,
                                              const VkPipelineLayoutCreateInfo* info,
                                              const VkAllocationCallbacks* alloc,
                                              VkPipelineLayout* result) {
    auto r = create_pipeline(d, info, alloc, result);
    if (r == VK_SUCCESS) {
        std::scoped_lock lock(mutex);
        pipelines[*result].sets.assign(info->pSetLayouts, info->pSetLayouts + info->setLayoutCount);
    }
    return r;
}

VKAPI_ATTR void VKAPI_CALL DestroyPipeline(VkDevice d, VkPipelineLayout layout,
                                           const VkAllocationCallbacks* alloc) {
    {
        std::scoped_lock lock(mutex);
        pipelines.erase(layout);
    }
    destroy_pipeline(d, layout, alloc);
}

void Release(Commands& c) {
    for (auto pool : c.pools) {
        VULKAN_HPP_DEFAULT_DISPATCHER.vkDestroyDescriptorPool(active_device, pool, nullptr);
    }
    c.pools.clear();
    c.pushed.clear();
}

VKAPI_ATTR VkResult VKAPI_CALL AllocateCommands(VkDevice d,
                                                const VkCommandBufferAllocateInfo* info,
                                                VkCommandBuffer* result) {
    auto r = allocate_commands(d, info, result);
    if (r == VK_SUCCESS) {
        std::scoped_lock lock(mutex);
        for (uint32_t i = 0; i < info->commandBufferCount; ++i) {
            commands[result[i]].owner = info->commandPool;
        }
    }
    return r;
}

VKAPI_ATTR void VKAPI_CALL FreeCommands(VkDevice d, VkCommandPool pool, uint32_t count,
                                        const VkCommandBuffer* buffers) {
    {
        std::scoped_lock lock(mutex);
        for (uint32_t i = 0; i < count; ++i) {
            auto it = commands.find(buffers[i]);
            if (it != commands.end()) {
                Release(it->second);
                commands.erase(it);
            }
        }
    }
    free_commands(d, pool, count, buffers);
}

VKAPI_ATTR void VKAPI_CALL DestroyCommandPool(VkDevice d, VkCommandPool pool,
                                              const VkAllocationCallbacks* alloc) {
    {
        std::scoped_lock lock(mutex);
        for (auto it = commands.begin(); it != commands.end();) {
            if (it->second.owner == pool) {
                Release(it->second);
                it = commands.erase(it);
            } else {
                ++it;
            }
        }
    }
    destroy_command_pool(d, pool, alloc);
}

VKAPI_ATTR VkResult VKAPI_CALL BeginCommands(VkCommandBuffer buffer,
                                             const VkCommandBufferBeginInfo* info) {
    auto r = begin_commands(buffer, info);
    if (r == VK_SUCCESS) {
        std::scoped_lock lock(mutex);
        auto& c = commands.at(buffer);
        for (auto pool : c.pools) {
            Require(VULKAN_HPP_DEFAULT_DISPATCHER.vkResetDescriptorPool(active_device, pool, 0),
                    "Reset fallback descriptor pool");
        }
        c.pool = 0;
        c.pushed.clear();
    }
    return r;
}

VkDescriptorSet Allocate(Commands& c, VkDescriptorSetLayout layout) {
    for (;;) {
        if (c.pool == c.pools.size()) {
            constexpr VkDescriptorType types[] = {
                VK_DESCRIPTOR_TYPE_SAMPLER,
                VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
                VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER,
                VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER,
                VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
            };
            std::array<VkDescriptorPoolSize, std::size(types)> sizes{};
            for (size_t i = 0; i < sizes.size(); ++i) {
                const uint32_t count =
                    (types[i] == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER) ? 32768u : 16384u;
                sizes[i] = {types[i], count};
            }
            VkDescriptorPoolCreateInfo info{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
            info.maxSets = 256;
            info.poolSizeCount = uint32_t(sizes.size());
            info.pPoolSizes = sizes.data();
            VkDescriptorPool pool{};
            Require(VULKAN_HPP_DEFAULT_DISPATCHER.vkCreateDescriptorPool(active_device, &info,
                                                                         nullptr, &pool),
                    "Create fallback descriptor pool");
            c.pools.push_back(pool);
        }
        VkDescriptorSetAllocateInfo info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        info.descriptorPool = c.pools[c.pool];
        info.descriptorSetCount = 1;
        info.pSetLayouts = &layout;
        VkDescriptorSet set{};
        auto r = VULKAN_HPP_DEFAULT_DISPATCHER.vkAllocateDescriptorSets(active_device, &info, &set);
        if (r == VK_SUCCESS) return set;
        if (r != VK_ERROR_OUT_OF_POOL_MEMORY && r != VK_ERROR_FRAGMENTED_POOL) {
            Require(r, "Allocate fallback descriptor set");
        }
        ++c.pool;
    }
}

VKAPI_ATTR void VKAPI_CALL Push(VkCommandBuffer cmd, VkPipelineBindPoint point,
                                VkPipelineLayout pipeline, uint32_t index, uint32_t count,
                                const VkWriteDescriptorSet* writes) {
    std::scoped_lock lock(mutex);
    auto& c = commands.at(cmd);
    auto layout = pipelines.at(pipeline).sets.at(index);
    auto set = Allocate(c, layout);
    auto& previous = c.pushed[pipeline][index];
    std::vector<VkCopyDescriptorSet> copies;
    if (previous) {
        for (auto const& b : layouts.at(layout).bindings) {
            if (b.descriptorCount) {
                VkCopyDescriptorSet copy{VK_STRUCTURE_TYPE_COPY_DESCRIPTOR_SET};
                copy.srcSet = previous;
                copy.dstSet = set;
                copy.srcBinding = copy.dstBinding = b.binding;
                copy.descriptorCount = b.descriptorCount;
                copies.push_back(copy);
            }
        }
    }
    if (!copies.empty()) {
        VULKAN_HPP_DEFAULT_DISPATCHER.vkUpdateDescriptorSets(
            active_device, 0, nullptr, uint32_t(copies.size()), copies.data());
    }
    std::vector<VkWriteDescriptorSet> updates(writes, writes + count);
    for (auto& write : updates) {
        write.dstSet = set;
    }
    VULKAN_HPP_DEFAULT_DISPATCHER.vkUpdateDescriptorSets(active_device, count, updates.data(), 0,
                                                         nullptr);
    VULKAN_HPP_DEFAULT_DISPATCHER.vkCmdBindDescriptorSets(cmd, point, pipeline, index, 1, &set, 0,
                                                          nullptr);
    previous = set;
}

VKAPI_ATTR void VKAPI_CALL DestroyDevice(VkDevice d, const VkAllocationCallbacks* alloc) {
    {
        std::scoped_lock lock(mutex);
        for (auto& [cmd, c] : commands) {
            (void)cmd;
            Release(c);
        }
        commands.clear();
        layouts.clear();
        pipelines.clear();
    }
    destroy_device(d, alloc);
}
} // namespace

PFN_vkGetInstanceProcAddr WrapUwpGetInstanceProcAddr(PFN_vkGetInstanceProcAddr real_get_proc) {
    g_real_get_instance_proc_addr = real_get_proc;
    return &UwpGetInstanceProcAddr;
}

void InstallDescriptorFallback(vk::Device device) {
    active_device = device;
    auto& d = VULKAN_HPP_DEFAULT_DISPATCHER;
    create_layout = d.vkCreateDescriptorSetLayout;
    destroy_layout = d.vkDestroyDescriptorSetLayout;
    create_pipeline = d.vkCreatePipelineLayout;
    destroy_pipeline = d.vkDestroyPipelineLayout;
    allocate_commands = d.vkAllocateCommandBuffers;
    free_commands = d.vkFreeCommandBuffers;
    destroy_command_pool = d.vkDestroyCommandPool;
    begin_commands = d.vkBeginCommandBuffer;
    destroy_device = d.vkDestroyDevice;

    d.vkCreateDescriptorSetLayout = CreateLayout;
    d.vkDestroyDescriptorSetLayout = DestroyLayout;
    d.vkCreatePipelineLayout = CreatePipeline;
    d.vkDestroyPipelineLayout = DestroyPipeline;
    d.vkAllocateCommandBuffers = AllocateCommands;
    d.vkFreeCommandBuffers = FreeCommands;
    d.vkDestroyCommandPool = DestroyCommandPool;
    d.vkBeginCommandBuffer = BeginCommands;
    d.vkDestroyDevice = DestroyDevice;
    d.vkCmdPushDescriptorSetKHR = Push;
    std::puts("UWP host push bindings: ordinary descriptor sets, fence-safe command-buffer pools");
}

} // namespace BbUwp
