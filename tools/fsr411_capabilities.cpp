// SPDX-License-Identifier: GPL-2.0-or-later
#include "fsr411_support.h"
#include "fsr411_vulkan_support.h"
#include <cstdio>
#include <filesystem>
#include <windows.h>

extern "C" void PrintFsr411Capabilities(VkPhysicalDevice device) {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    VkPhysicalDeviceIDProperties id{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
    VkPhysicalDeviceProperties2 props{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &id};
    vkGetPhysicalDeviceProperties2(device, &props);
    auto dx12 = Fsr411Support{false, "hardware", "AMD GPU required"};
    if (props.properties.vendorID == 0x1002) {
        uint32_t n{};
        vkEnumerateDeviceExtensionProperties(device, nullptr, &n, nullptr);
        std::vector<VkExtensionProperties> extensions(n);
        vkEnumerateDeviceExtensionProperties(device, nullptr, &n, extensions.data());
        bool memory = false, semaphore = false;
        for (const auto& e : extensions) {
            memory |= !std::strcmp(e.extensionName, "VK_KHR_external_memory_win32");
            semaphore |= !std::strcmp(e.extensionName, "VK_KHR_external_semaphore_win32");
        }
        VkSemaphoreTypeCreateInfo timeline{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
        timeline.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
        VkPhysicalDeviceExternalSemaphoreInfo info{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_SEMAPHORE_INFO,
            &timeline, VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_D3D12_FENCE_BIT};
        VkExternalSemaphoreProperties fences{VK_STRUCTURE_TYPE_EXTERNAL_SEMAPHORE_PROPERTIES};
        if (semaphore) vkGetPhysicalDeviceExternalSemaphoreProperties(device, &info, &fences);
        bool interop = memory && semaphore &&
            (fences.externalSemaphoreFeatures & VK_EXTERNAL_SEMAPHORE_FEATURE_IMPORTABLE_BIT);
        dx12 = !id.deviceLUIDValid || !interop
            ? Fsr411Support{false, "interop", "Vulkan/DX12 shared memory"}
            : ProbeFsr411Dx12(id.deviceLUID);
        if (GetProcAddress(GetModuleHandleW(nullptr), "D3D12SDKVersion")) {
            std::wstring path(32768, L'\0');
            path.resize(GetModuleFileNameW(nullptr, path.data(), DWORD(path.size())));
            std::error_code error;
            if (!std::filesystem::is_regular_file(std::filesystem::path(path).parent_path() / L"D3D12Core.dll", error))
                dx12 = {false, "files", "D3D12Core.dll"};
        }
    }
    const char* vk = Fsr411VulkanRequirement(device, vkGetPhysicalDeviceProperties,
        vkEnumerateDeviceExtensionProperties, vkGetPhysicalDeviceFeatures2);
    // All diagnostic strings are fixed identifiers or numeric loader errors.
    std::printf("{\"vendor_id\":%u,\"dx12\":{\"supported\":%s,\"reason\":\"%s\",\"detail\":\"%s\"},"
                "\"vk\":{\"supported\":%s,\"reason\":\"%s\",\"detail\":\"%s\"}}\n",
        props.properties.vendorID, dx12.supported ? "true" : "false", dx12.reason,
        dx12.detail.c_str(), vk ? "false" : "true",
        !vk ? "ready" : !std::strcmp(vk, "Vulkan extension query") ? "check" : "hardware", vk ? vk : "");
}
