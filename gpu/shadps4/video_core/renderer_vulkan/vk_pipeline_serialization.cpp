// SPDX-FileCopyrightText: Copyright 2025-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <array>
#include <atomic>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>
#include <boost/container/small_vector.hpp>

#include "common/elf_info.h"
#include "common/io_file.h"
#include "common/path_util.h"
#include "common/scope_exit.h"
#include "common/serdes.h"
#include "core/emulator_settings.h"
#include "shader_recompiler/frontend/fetch_shader.h"
#include "shader_recompiler/info.h"
#include "video_core/cache_storage.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_pipeline_cache.h"
#include "video_core/renderer_vulkan/vk_pipeline_serialization.h"
#include "video_core/renderer_vulkan/vk_shader_util.h"

#ifdef BB_UWP
#include <windows.h>
#include <malloc.h>
#endif

namespace Serialization {
/* You should increment versions below once corresponding serialization scheme is changed. */
// bbport: above every version a release has used (1.9.2: 11, 8, 6), never back to an old one: the
// GPU rollback to 1.8.2's code took them back to 7, 7, 5, and a cache another build wrote into
// the same folder then passed as this build's.
#ifdef BB_UWP
static constexpr u32 ShaderBinaryVersion = 13u;
static constexpr u32 ShaderMetaVersion = 9u;
static constexpr u32 PipelineKeyVersion = 8u;
#else
static constexpr u32 ShaderBinaryVersion = 12u;
static constexpr u32 ShaderMetaVersion = 9u;
static constexpr u32 PipelineKeyVersion = 7u;
#endif

// bbport: shader binaries carry no version of their own, so their names do: a build with another
// version writes its binaries beside these instead of over them (where a shader's metadata
// passed its version check and then loaded the other build's binary).
static std::string ShaderBinaryName(u64 pgm_hash, size_t perm_idx) {
    return fmt::format("{:#018x}_{}_v{}", pgm_hash, perm_idx, ShaderBinaryVersion);
}
} // namespace Serialization

namespace Vulkan {

namespace {

std::atomic<u32> g_pipeline_binary_hits{0};
std::atomic<u32> g_pipeline_binary_saves{0};

class PipelineBinaryCache {
public:
    static PipelineBinaryCache& Get() {
        static PipelineBinaryCache cache;
        return cache;
    }

    template <typename CreateInfo>
    vk::ResultValue<vk::UniquePipeline> CreatePipeline(const Vulkan::Instance& instance,
                                                       vk::PipelineCache pipeline_cache,
                                                       const CreateInfo& base_info,
                                                       bool is_compute) {
        const vk::Device device = instance.GetDevice();
        const auto& global_key = instance.GetPipelineBinaryGlobalKey();
        if (!global_key.keySize) {
            return CreatePlain(device, pipeline_cache, base_info, is_compute);
        }

        vk::PipelineBinaryKeyKHR pipeline_key{};
        const vk::PipelineCreateInfoKHR lookup_info{
            .pNext = const_cast<CreateInfo*>(&base_info),
        };
        if (device.getPipelineKeyKHR(&lookup_info, &pipeline_key) != vk::Result::eSuccess ||
            !pipeline_key.keySize || pipeline_key.keySize > VK_MAX_PIPELINE_BINARY_KEY_SIZE_KHR) {
            return CreatePlain(device, pipeline_cache, base_info, is_compute);
        }

        if (auto cached = Load(global_key, pipeline_key)) {
            auto created = CreateFromBinaries(device, base_info, *cached, is_compute);
            if (created.result == vk::Result::eSuccess) {
                ++g_pipeline_binary_hits;
                return created;
            }
            Remove(pipeline_key);
        }

        auto captured = CaptureAndCreate(device, base_info, global_key, pipeline_key, is_compute);
        if (captured.result == vk::Result::eSuccess) {
            return captured;
        }
        return CreatePlain(device, pipeline_cache, base_info, is_compute);
    }

private:
    struct StoredBinary {
        vk::PipelineBinaryKeyKHR key{};
        std::vector<u8> data;
    };

    static constexpr std::array<char, 4> Magic = {'G', 'P', 'B', '1'};
    static constexpr u32 FormatVersion = 1;
    static constexpr u32 MaxBinariesPerPipeline = 8;
    static constexpr u32 MaxBinarySize = 32 * 1024 * 1024;

#pragma pack(push, 1)
    struct FileHeader {
        char magic[4];
        u32 version;
        u64 checksum;
        u32 global_key_size;
        u8 global_key[VK_MAX_PIPELINE_BINARY_KEY_SIZE_KHR];
        u32 pipeline_key_size;
        u8 pipeline_key[VK_MAX_PIPELINE_BINARY_KEY_SIZE_KHR];
        u32 binary_count;
    };

    struct EntryHeader {
        u32 key_size;
        u8 key[VK_MAX_PIPELINE_BINARY_KEY_SIZE_KHR];
        u32 data_size;
    };
#pragma pack(pop)

    static u64 HashBytes(u64 seed, const void* ptr, size_t size) {
        const auto* bytes = static_cast<const u8*>(ptr);
        for (size_t i = 0; i < size; ++i) {
            seed ^= bytes[i];
            seed *= 1099511628211ull;
        }
        return seed;
    }

    static bool KeysEqual(const vk::PipelineBinaryKeyKHR& a, u32 b_size, const u8* b_data) {
        return a.keySize == b_size && a.keySize <= VK_MAX_PIPELINE_BINARY_KEY_SIZE_KHR &&
               std::memcmp(a.key.data(), b_data, a.keySize) == 0;
    }

    static std::string KeyFileStem(const vk::PipelineBinaryKeyKHR& key) {
        static constexpr char Hex[] = "0123456789abcdef";
        std::string out;
        out.reserve(size_t(key.keySize) * 2);
        for (u32 i = 0; i < key.keySize && i < VK_MAX_PIPELINE_BINARY_KEY_SIZE_KHR; ++i) {
            const u8 byte = key.key[i];
            out.push_back(Hex[byte >> 4]);
            out.push_back(Hex[byte & 0xf]);
        }
        return out;
    }

    std::filesystem::path EnsureDirectory() {
        std::scoped_lock lk{dir_mutex};
        const auto serial = Common::ElfInfo::Instance().GameSerial();
        if (serial.empty()) {
            return {};
        }
        if (dir_serial != serial || binary_dir.empty()) {
            binary_dir = Common::FS::GetUserPath(Common::FS::PathType::CacheDir) / serial /
                         "pipeline-binaries";
            dir_serial = serial;
            std::error_code ec;
            std::filesystem::create_directories(binary_dir, ec);
        }
        return binary_dir;
    }

    std::filesystem::path PathForKey(const vk::PipelineBinaryKeyKHR& key) {
        const auto dir = EnsureDirectory();
        if (dir.empty()) {
            return {};
        }
        return dir / (KeyFileStem(key) + ".bin");
    }

    void Remove(const vk::PipelineBinaryKeyKHR& key) {
        const auto path = PathForKey(key);
        if (!path.empty()) {
            std::error_code ec;
            std::filesystem::remove(path, ec);
        }
    }

    std::optional<std::vector<StoredBinary>> Load(const vk::PipelineBinaryKeyKHR& global_key,
                                                  const vk::PipelineBinaryKeyKHR& pipeline_key) {
        const auto path = PathForKey(pipeline_key);
        if (path.empty()) {
            return std::nullopt;
        }
        Common::FS::IOFile file{path, Common::FS::FileAccessMode::Read};
        if (!file.IsOpen()) {
            return std::nullopt;
        }
        const u64 file_size = file.GetSize();
        if (file_size < sizeof(FileHeader) ||
            file_size > sizeof(FileHeader) + u64(MaxBinariesPerPipeline) * MaxBinarySize) {
            return std::nullopt;
        }
        std::vector<u8> bytes(static_cast<size_t>(file_size));
        if (file.ReadRaw<u8>(bytes.data(), bytes.size()) != bytes.size()) {
            return std::nullopt;
        }
        file.Close();

        FileHeader header{};
        std::memcpy(&header, bytes.data(), sizeof(header));
        if (std::memcmp(header.magic, Magic.data(), Magic.size()) != 0 ||
            header.version != FormatVersion || header.binary_count == 0 ||
            header.binary_count > MaxBinariesPerPipeline ||
            !KeysEqual(global_key, header.global_key_size, header.global_key) ||
            !KeysEqual(pipeline_key, header.pipeline_key_size, header.pipeline_key)) {
            Remove(pipeline_key);
            return std::nullopt;
        }

        FileHeader check_header = header;
        check_header.checksum = 0;
        u64 checksum = HashBytes(14695981039346656037ull, &check_header, sizeof(check_header));
        checksum = HashBytes(checksum, bytes.data() + sizeof(FileHeader),
                             bytes.size() - sizeof(FileHeader));
        if (checksum != header.checksum) {
            Remove(pipeline_key);
            return std::nullopt;
        }

        size_t offset = sizeof(FileHeader);
        std::vector<StoredBinary> out;
        out.reserve(header.binary_count);
        for (u32 i = 0; i < header.binary_count; ++i) {
            if (offset + sizeof(EntryHeader) > bytes.size()) {
                Remove(pipeline_key);
                return std::nullopt;
            }
            EntryHeader entry{};
            std::memcpy(&entry, bytes.data() + offset, sizeof(entry));
            offset += sizeof(entry);
            if (!entry.key_size || entry.key_size > VK_MAX_PIPELINE_BINARY_KEY_SIZE_KHR ||
                !entry.data_size || entry.data_size > MaxBinarySize ||
                offset + entry.data_size > bytes.size()) {
                Remove(pipeline_key);
                return std::nullopt;
            }
            StoredBinary binary{};
            binary.key.keySize = entry.key_size;
            std::memcpy(binary.key.key.data(), entry.key, entry.key_size);
            binary.data.assign(bytes.data() + offset, bytes.data() + offset + entry.data_size);
            offset += entry.data_size;
            out.push_back(std::move(binary));
        }
        if (offset != bytes.size()) {
            Remove(pipeline_key);
            return std::nullopt;
        }
        return out;
    }

    void Save(const vk::PipelineBinaryKeyKHR& global_key,
              const vk::PipelineBinaryKeyKHR& pipeline_key,
              const std::vector<StoredBinary>& binaries) {
        if (binaries.empty() || binaries.size() > MaxBinariesPerPipeline) {
            return;
        }
        const auto path = PathForKey(pipeline_key);
        if (path.empty()) {
            return;
        }

        size_t payload_size = 0;
        for (const auto& binary : binaries) {
            if (!binary.key.keySize || binary.key.keySize > VK_MAX_PIPELINE_BINARY_KEY_SIZE_KHR ||
                binary.data.empty() || binary.data.size() > MaxBinarySize) {
                return;
            }
            payload_size += sizeof(EntryHeader) + binary.data.size();
        }

        FileHeader header{};
        std::memcpy(header.magic, Magic.data(), Magic.size());
        header.version = FormatVersion;
        header.global_key_size = global_key.keySize;
        std::memcpy(header.global_key, global_key.key.data(), global_key.keySize);
        header.pipeline_key_size = pipeline_key.keySize;
        std::memcpy(header.pipeline_key, pipeline_key.key.data(), pipeline_key.keySize);
        header.binary_count = static_cast<u32>(binaries.size());

        std::vector<u8> bytes(sizeof(FileHeader) + payload_size);
        size_t offset = sizeof(FileHeader);
        for (const auto& binary : binaries) {
            EntryHeader entry{};
            entry.key_size = binary.key.keySize;
            std::memcpy(entry.key, binary.key.key.data(), binary.key.keySize);
            entry.data_size = static_cast<u32>(binary.data.size());
            std::memcpy(bytes.data() + offset, &entry, sizeof(entry));
            offset += sizeof(entry);
            std::memcpy(bytes.data() + offset, binary.data.data(), binary.data.size());
            offset += binary.data.size();
        }

        u64 checksum = HashBytes(14695981039346656037ull, &header, sizeof(header));
        checksum = HashBytes(checksum, bytes.data() + sizeof(FileHeader), payload_size);
        header.checksum = checksum;
        std::memcpy(bytes.data(), &header, sizeof(header));

        const auto tid = std::hash<std::thread::id>{}(std::this_thread::get_id());
        auto temp = path;
        temp += fmt::format(".{}.tmp", tid);
        {
            Common::FS::IOFile file{temp, Common::FS::FileAccessMode::Create};
            if (!file.IsOpen() || file.WriteRaw<u8>(bytes.data(), bytes.size()) != bytes.size()) {
                file.Close();
                std::error_code ec;
                std::filesystem::remove(temp, ec);
                return;
            }
        }
        std::error_code ec;
        std::filesystem::remove(path, ec);
        ec.clear();
        std::filesystem::rename(temp, path, ec);
        if (ec) {
            std::filesystem::remove(temp, ec);
            return;
        }
        ++g_pipeline_binary_saves;
    }

    template <typename CreateInfo>
    static vk::ResultValue<vk::UniquePipeline> CreatePlain(vk::Device device,
                                                           vk::PipelineCache pipeline_cache,
                                                           const CreateInfo& info,
                                                           bool is_compute) {
        if constexpr (std::is_same_v<CreateInfo, vk::ComputePipelineCreateInfo>) {
            (void)is_compute;
            return device.createComputePipelineUnique(pipeline_cache, info);
        } else {
            (void)is_compute;
            return device.createGraphicsPipelineUnique(pipeline_cache, info);
        }
    }

    template <typename CreateInfo>
    static vk::ResultValue<vk::UniquePipeline> CreateFromBinaries(
        vk::Device device, const CreateInfo& base_info, const std::vector<StoredBinary>& stored,
        bool is_compute) {
        const auto& dispatch = VULKAN_HPP_DEFAULT_DISPATCHER;
        const VkDevice raw_dev = static_cast<VkDevice>(device);
        boost::container::small_vector<VkPipelineBinaryDataKHR, MaxBinariesPerPipeline> data_items;
        boost::container::small_vector<VkPipelineBinaryKeyKHR, MaxBinariesPerPipeline> key_items;
        for (const auto& item : stored) {
            VkPipelineBinaryKeyKHR raw_key{VK_STRUCTURE_TYPE_PIPELINE_BINARY_KEY_KHR};
            raw_key.keySize = item.key.keySize;
            std::memcpy(raw_key.key, item.key.key.data(), item.key.keySize);
            key_items.push_back(raw_key);
            data_items.push_back(VkPipelineBinaryDataKHR{
                .dataSize = item.data.size(),
                .pData = const_cast<u8*>(item.data.data()),
            });
        }
        VkPipelineBinaryKeysAndDataKHR keys_and_data{
            .binaryCount = static_cast<u32>(data_items.size()),
            .pPipelineBinaryKeys = key_items.data(),
            .pPipelineBinaryData = data_items.data(),
        };
        boost::container::small_vector<VkPipelineBinaryKHR, MaxBinariesPerPipeline> binaries(
            data_items.size(), VK_NULL_HANDLE);
        VkPipelineBinaryHandlesInfoKHR handles{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_BINARY_HANDLES_INFO_KHR,
            .pNext = nullptr,
            .pipelineBinaryCount = static_cast<u32>(binaries.size()),
            .pPipelineBinaries = binaries.data(),
        };
        VkPipelineBinaryCreateInfoKHR binary_ci{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_BINARY_CREATE_INFO_KHR,
            .pNext = nullptr,
            .pKeysAndDataInfo = &keys_and_data,
            .pipeline = VK_NULL_HANDLE,
            .pPipelineCreateInfo = nullptr,
        };
        const VkResult bin_res =
            dispatch.vkCreatePipelineBinariesKHR(raw_dev, &binary_ci, nullptr, &handles);
        auto destroy_binaries = [&]() {
            for (const auto binary : binaries) {
                if (binary != VK_NULL_HANDLE) {
                    dispatch.vkDestroyPipelineBinaryKHR(raw_dev, binary, nullptr);
                }
            }
        };
        if (bin_res != VK_SUCCESS) {
            destroy_binaries();
            return vk::ResultValue<vk::UniquePipeline>{static_cast<vk::Result>(bin_res),
                                                       vk::UniquePipeline{}};
        }

        CreateInfo info = base_info;
        VkPipelineBinaryInfoKHR binary_info{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_BINARY_INFO_KHR,
            .pNext = info.pNext,
            .binaryCount = static_cast<u32>(binaries.size()),
            .pPipelineBinaries = binaries.data(),
        };
        info.pNext = &binary_info;
        auto created = CreatePlain(device, vk::PipelineCache{}, info, is_compute);
        destroy_binaries();
        return created;
    }

    template <typename CreateInfo>
    vk::ResultValue<vk::UniquePipeline> CaptureAndCreate(
        vk::Device device, const CreateInfo& base_info, const vk::PipelineBinaryKeyKHR& global_key,
        const vk::PipelineBinaryKeyKHR& pipeline_key, bool is_compute) {
        const auto& dispatch = VULKAN_HPP_DEFAULT_DISPATCHER;
        const VkDevice raw_dev = static_cast<VkDevice>(device);
        CreateInfo info = base_info;
        vk::PipelineCreateFlags2CreateInfoKHR flags2{
            .pNext = info.pNext,
            .flags = vk::PipelineCreateFlags2{static_cast<VkPipelineCreateFlags>(info.flags)} |
                     vk::PipelineCreateFlagBits2::eCaptureDataKHR,
        };
        info.pNext = &flags2;
        // Per VK_KHR_pipeline_binary (VUID-09609), pipelineCache must be VK_NULL_HANDLE when
        // VK_PIPELINE_CREATE_2_CAPTURE_DATA_BIT_KHR is specified.
        auto created = CreatePlain(device, vk::PipelineCache{}, info, is_compute);
        if (created.result != vk::Result::eSuccess || !created.value) {
            return created;
        }

        const VkPipeline raw_pipe = static_cast<VkPipeline>(*created.value);
        VkReleaseCapturedPipelineDataInfoKHR release_info{
            .sType = VK_STRUCTURE_TYPE_RELEASE_CAPTURED_PIPELINE_DATA_INFO_KHR,
            .pNext = nullptr,
            .pipeline = raw_pipe,
        };
        SCOPE_EXIT {
            (void)dispatch.vkReleaseCapturedPipelineDataKHR(raw_dev, &release_info, nullptr);
        };

        VkPipelineBinaryCreateInfoKHR binary_ci{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_BINARY_CREATE_INFO_KHR,
            .pNext = nullptr,
            .pKeysAndDataInfo = nullptr,
            .pipeline = raw_pipe,
            .pPipelineCreateInfo = nullptr,
        };
        VkPipelineBinaryHandlesInfoKHR count_info{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_BINARY_HANDLES_INFO_KHR,
        };
        std::vector<StoredBinary> stored;
        if (dispatch.vkCreatePipelineBinariesKHR(raw_dev, &binary_ci, nullptr, &count_info) ==
                VK_SUCCESS &&
            count_info.pipelineBinaryCount > 0 &&
            count_info.pipelineBinaryCount <= MaxBinariesPerPipeline) {
            boost::container::small_vector<VkPipelineBinaryKHR, MaxBinariesPerPipeline> binaries(
                count_info.pipelineBinaryCount, VK_NULL_HANDLE);
            count_info.pPipelineBinaries = binaries.data();
            if (dispatch.vkCreatePipelineBinariesKHR(raw_dev, &binary_ci, nullptr, &count_info) ==
                VK_SUCCESS) {
                stored.reserve(binaries.size());
                bool ok = true;
                for (const auto binary : binaries) {
                    if (binary == VK_NULL_HANDLE) {
                        ok = false;
                        break;
                    }
                    VkPipelineBinaryDataInfoKHR data_info{
                        .sType = VK_STRUCTURE_TYPE_PIPELINE_BINARY_DATA_INFO_KHR,
                        .pNext = nullptr,
                        .pipelineBinary = binary,
                    };
                    VkPipelineBinaryKeyKHR binary_key{VK_STRUCTURE_TYPE_PIPELINE_BINARY_KEY_KHR};
                    size_t data_size = 0;
                    if (dispatch.vkGetPipelineBinaryDataKHR(raw_dev, &data_info, &binary_key,
                                                            &data_size, nullptr) != VK_SUCCESS ||
                        !data_size || data_size > MaxBinarySize) {
                        ok = false;
                        break;
                    }
                    StoredBinary item{};
                    item.data.resize(data_size);
                    VkPipelineBinaryKeyKHR out_key{VK_STRUCTURE_TYPE_PIPELINE_BINARY_KEY_KHR};
                    if (dispatch.vkGetPipelineBinaryDataKHR(raw_dev, &data_info, &out_key,
                                                            &data_size, item.data.data()) !=
                        VK_SUCCESS) {
                        ok = false;
                        break;
                    }
                    item.key.keySize = out_key.keySize;
                    std::memcpy(item.key.key.data(), out_key.key,
                                std::min<u32>(out_key.keySize, VK_MAX_PIPELINE_BINARY_KEY_SIZE_KHR));
                    item.data.resize(data_size);
                    stored.push_back(std::move(item));
                }
                if (!ok) {
                    stored.clear();
                }
            }
            for (const auto binary : binaries) {
                if (binary != VK_NULL_HANDLE) {
                    dispatch.vkDestroyPipelineBinaryKHR(raw_dev, binary, nullptr);
                }
            }
        }

        if (!stored.empty()) {
            Save(global_key, pipeline_key, stored);
        }
        return created;
    }

    std::mutex dir_mutex;
    std::string dir_serial;
    std::filesystem::path binary_dir;
};

} // namespace

vk::ResultValue<vk::UniquePipeline> CreateCachedGraphicsPipeline(
    const Instance& instance, vk::PipelineCache pipeline_cache,
    const vk::GraphicsPipelineCreateInfo& pipeline_info) {
    return PipelineBinaryCache::Get().CreatePipeline(instance, pipeline_cache, pipeline_info,
                                                     false);
}

vk::ResultValue<vk::UniquePipeline> CreateCachedComputePipeline(
    const Instance& instance, vk::PipelineCache pipeline_cache,
    const vk::ComputePipelineCreateInfo& pipeline_info) {
    return PipelineBinaryCache::Get().CreatePipeline(instance, pipeline_cache, pipeline_info,
                                                     true);
}

void RegisterPipelineData(const ComputePipelineKey& key,
                          ComputePipeline::SerializationSupport& sdata) {
    if (!Storage::DataBase::Instance().IsOpened()) {
        return;
    }

    Serialization::Archive ar{};
    Serialization::Writer pldata{ar};

    pldata.Write(Serialization::PipelineKeyVersion);
    pldata.Write(u32{1}); // compute

    key.Serialize(ar);
    sdata.Serialize(ar);

    Storage::DataBase::Instance().Save(Storage::BlobType::PipelineKey,
                                       fmt::format("c_{:#018x}", key.value), ar.TakeOff());
}

void RegisterPipelineData(const GraphicsPipelineKey& key, u64 hash,
                          GraphicsPipeline::SerializationSupport& sdata) {
    if (!Storage::DataBase::Instance().IsOpened()) {
        return;
    }

    Serialization::Archive ar{};
    Serialization::Writer pldata{ar};

    pldata.Write(Serialization::PipelineKeyVersion);
    pldata.Write(u32{0}); // graphics

    key.Serialize(ar);
    sdata.Serialize(ar);

    Storage::DataBase::Instance().Save(Storage::BlobType::PipelineKey,
                                       fmt::format("g_{:#018x}", hash), ar.TakeOff());
}

void RegisterShaderMeta(const Shader::Info& info,
                        const std::optional<Shader::Gcn::FetchShaderData>& fetch_shader_data,
                        const Shader::StageSpecialization& spec, size_t perm_hash,
                        size_t perm_idx) {
    if (!Storage::DataBase::Instance().IsOpened()) {
        return;
    }

    Serialization::Archive ar;
    Serialization::Writer meta{ar};

    meta.Write(Serialization::ShaderMetaVersion);
    meta.Write(Serialization::ShaderBinaryVersion);

    meta.Write(perm_hash);
    meta.Write(perm_idx);

    spec.Serialize(ar);
    info.Serialize(ar);

    Storage::DataBase::Instance().Save(Storage::BlobType::ShaderMeta,
                                       fmt::format("{:#018x}", perm_hash), ar.TakeOff());
}

void RegisterShaderBinary(std::vector<u32>&& spv, u64 pgm_hash, size_t perm_idx) {
    if (!Storage::DataBase::Instance().IsOpened()) {
        return;
    }

    Storage::DataBase::Instance().Save(Storage::BlobType::ShaderBinary,
                                       Serialization::ShaderBinaryName(pgm_hash, perm_idx),
                                       std::move(spv));
}

bool LoadShaderMeta(Serialization::Archive& ar, Shader::Info& info,
                    std::optional<Shader::Gcn::FetchShaderData>& fetch_shader_data,
                    Shader::StageSpecialization& spec, size_t& perm_idx) {
    Serialization::Reader meta{ar};

    u32 meta_version{};
    meta.Read(meta_version);
    if (meta_version != Serialization::ShaderMetaVersion) {
        return false;
    }

    u32 binary_version{};
    meta.Read(binary_version);
    if (binary_version != Serialization::ShaderBinaryVersion) {
        return false;
    }

    u64 perm_hash_ar{};
    meta.Read(perm_hash_ar);
    meta.Read(perm_idx);

    spec.Deserialize(ar);
    info.Deserialize(ar);

    // Motion vertex shaders embed session-local buffer device addresses. They must be
    // recompiled for the current allocation, never loaded from a previous process.
    if (info.hw_stage == Shader::HwStage::Vertex && spec.runtime_info.hw.vs.motion_vectors) {
        return false;
    }

    fetch_shader_data = spec.fetch_shader_data;
    return true;
}

void ComputePipelineKey::Serialize(Serialization::Archive& ar) const {
    Serialization::Writer key{ar};
    key.Write(value);
}

bool ComputePipelineKey::Deserialize(Serialization::Archive& ar) {
    Serialization::Reader key{ar};
    key.Read(value);
    return true;
}

void ComputePipeline::SerializationSupport::Serialize(Serialization::Archive& ar) const {
    // Nothing here yet
    return;
}

bool ComputePipeline::SerializationSupport::Deserialize(Serialization::Archive& ar) {
    // Nothing here yet
    return true;
}

bool PipelineCache::LoadComputePipeline(Serialization::Archive& ar) {
    sel.infos.fill(nullptr);
    sel.modules.fill(vk::ShaderModule{});
    sel.fetch_shader.reset();
    sel.runtime_infos = {};
    SCOPE_EXIT {
        sel.infos.fill(nullptr);
        sel.modules.fill(vk::ShaderModule{});
        sel.fetch_shader.reset();
    };

    compute_key.Deserialize(ar);

    ComputePipeline::SerializationSupport sdata{};
    sdata.Deserialize(ar);

    std::vector<u8> meta_blob;
    Storage::DataBase::Instance().Load(Storage::BlobType::ShaderMeta,
                                       fmt::format("{:#018x}", compute_key.value), meta_blob);
    if (meta_blob.empty()) {
        return false;
    }

    Serialization::Archive meta_ar{std::move(meta_blob)};

    if (!LoadPipelineStage(meta_ar, 0)) {
        return false;
    }

    const auto [it, is_new] = compute_pipelines.try_emplace(compute_key);
    if (!is_new) {
        return it.value() != nullptr;
    }

    it.value() =
        std::make_unique<ComputePipeline>(instance, scheduler, desc_heap, profile, *pipeline_cache,
                                          compute_key, *sel.infos[0], sel.modules[0], sdata, true);

    // bbport: a cached pipeline the driver cannot create (a new driver, or one saved by a build
    // that stopped on it) stopped every start here. It is compiled again when the game uses it.
    if (!it.value()->IsValid()) {
        compute_pipelines.erase(compute_key);
        return false;
    }
    return true;
}

void GraphicsPipelineKey::Serialize(Serialization::Archive& ar) const {
    Serialization::Writer key{ar};

    key.Write(this, sizeof(*this));
}

bool GraphicsPipelineKey::Deserialize(Serialization::Archive& ar) {
    Serialization::Reader key{ar};

    key.Read(this, sizeof(*this));
    return true;
}

void GraphicsPipeline::SerializationSupport::Serialize(Serialization::Archive& ar) const {
    Serialization::Writer sdata{ar};

    sdata.Write(&vertex_attributes, sizeof(vertex_attributes));
    sdata.Write(&vertex_bindings, sizeof(vertex_bindings));
    sdata.Write(&divisors, sizeof(divisors));
    sdata.Write(multisampling);
    sdata.Write(tcs);
    sdata.Write(tes);
}

bool GraphicsPipeline::SerializationSupport::Deserialize(Serialization::Archive& ar) {
    Serialization::Reader sdata{ar};

    sdata.Read(&vertex_attributes, sizeof(vertex_attributes));
    sdata.Read(&vertex_bindings, sizeof(vertex_bindings));
    sdata.Read(&divisors, sizeof(divisors));
    sdata.Read(multisampling);
    sdata.Read(tcs);
    sdata.Read(tes);
    return true;
}

bool PipelineCache::LoadGraphicsPipeline(Serialization::Archive& ar) {
    sel.infos.fill(nullptr);
    sel.modules.fill(vk::ShaderModule{});
    sel.fetch_shader.reset();
    sel.runtime_infos = {};
    SCOPE_EXIT {
        sel.infos.fill(nullptr);
        sel.modules.fill(vk::ShaderModule{});
        sel.fetch_shader.reset();
    };

    sel.graphics_key.Deserialize(ar);

    // bbport: motion-vector pipelines are never preloaded. Their vertex shader embeds session-
    // local buffer addresses (see LoadShaderMeta), and the shader metadata is stored once per
    // program, not per permutation: the meta found for a motion pipeline's stages could be the
    // plain permutation, so LoadShaderMeta's motion check missed it and the preload built the
    // 8-attachment motion key with plain shaders. The AMD driver refused some of these
    // (ErrorUnknown: the startup crash), and the ones it accepted were registered under the
    // motion key without motion output. Built at first use instead, like the vertex shaders.
    if (sel.graphics_key.motion_vectors) {
        return false;
    }

    GraphicsPipeline::SerializationSupport sdata{};
    sdata.Deserialize(ar);

    for (int stage_idx = 0; stage_idx < MaxShaderStages; ++stage_idx) {
        const auto& hash = sel.graphics_key.stage_hashes[stage_idx];
        if (!hash) {
            continue;
        }

        std::vector<u8> meta_blob;
        Storage::DataBase::Instance().Load(Storage::BlobType::ShaderMeta,
                                           fmt::format("{:#018x}", hash), meta_blob);
        if (meta_blob.empty()) {
            return false;
        }

        Serialization::Archive meta_ar{std::move(meta_blob)};

        if (!LoadPipelineStage(meta_ar, stage_idx)) {
            return false;
        }
    }

    const auto [it, is_new] = graphics_pipelines.try_emplace(sel.graphics_key);
    if (!is_new) {
        return it.value() != nullptr;
    }

    it.value() = std::make_unique<GraphicsPipeline>(
        instance, scheduler, desc_heap, profile, sel.graphics_key, *pipeline_cache, sel.infos,
        sel.runtime_infos, sel.fetch_shader, sel.modules, sdata, true);

    // bbport: as for compute pipelines, a failed one is left to compile when the game uses it.
    if (!it.value()->IsValid()) {
        graphics_pipelines.erase(sel.graphics_key);
        return false;
    }
    return true;
}

bool PipelineCache::LoadPipelineStage(Serialization::Archive& ar, size_t stage) {
    auto program = std::make_unique<Program>();
    Shader::StageSpecialization spec{};
    spec.info = &program->info;
    size_t perm_idx{};
    std::optional<Shader::Gcn::FetchShaderData> stage_fetch;
    if (!LoadShaderMeta(ar, program->info, stage_fetch, spec, perm_idx)) {
        return false;
    }

    std::vector<u32> spv{};
    Storage::DataBase::Instance().Load(Storage::BlobType::ShaderBinary,
                                       Serialization::ShaderBinaryName(program->info.pgm_hash, perm_idx),
                                       spv);
    if (spv.empty()) {
        return false;
    }

    // Permutation hash depends on shader variation index. To prevent collisions, we need insert it
    // at the exact position rather than append

    vk::ShaderModule module{};
    bool need_insert = true;

    auto [it_pgm, new_program] = program_cache.try_emplace(program->info.pgm_hash);
    if (new_program) {
        module = CompileSPV(spv, instance.GetDevice());
        if (!module) {
            program_cache.erase(it_pgm);
            return false;
        }
        it_pgm.value() = std::move(program);
    } else {
        spec.info = &it_pgm.value()->info;
        const auto& it = std::ranges::find(it_pgm.value()->modules, spec, &Program::Module::spec);
        if (it != it_pgm.value()->modules.end()) {
            // A matching permutation is valid only at its original index. A different index means
            // the store holds entries from more than one cache generation, so this pipeline is
            // left to compile at runtime.
            const auto idx = std::distance(it_pgm.value()->modules.begin(), it);
            if (perm_idx != idx) {
                LOG_WARNING(Render_Vulkan,
                            "Cached permutation {} of {}_{:x} conflicts with index {}, skipping "
                            "preload",
                            perm_idx, program->info.hw_stage, program->info.pgm_hash, idx);
                return false;
            }
            module = it->module;
            need_insert = false;
        } else {
            module = CompileSPV(spv, instance.GetDevice());
            if (!module) {
                return false;
            }
        }
    }

    sel.runtime_infos[stage] = spec.runtime_info;
    if (stage_fetch) {
        sel.fetch_shader = stage_fetch;
    }
    if (need_insert) {
        spec.info = &it_pgm.value()->info;
        it_pgm.value()->InsertPermut(module, std::move(spec), perm_idx);
    }

    sel.infos[stage] = &it_pgm.value()->info;
    sel.modules[stage] = module;

    return true;
}

void PipelineCache::WarmUp(const WarmUpProgressCallback& progress) {
    if (!EmulatorSettings.IsPipelineCacheEnabled()) {
        return;
    }

    Storage::DataBase::Instance().Open();

    // Check if cache is compatible
    std::vector<u8> profile_data{};
    Storage::DataBase::Instance().Load(Storage::BlobType::ShaderProfile, "profile", profile_data);
    if (profile_data.empty()) {
        Storage::DataBase::Instance().FinishPreload();

        profile_data.resize(sizeof(profile));
        std::memcpy(profile_data.data(), &profile, sizeof(profile));
        Storage::DataBase::Instance().Save(Storage::BlobType::ShaderProfile, "profile",
                                           std::move(profile_data));
        Storage::DataBase::Instance().Flush();
        return;
    }
    if (profile_data.size() != sizeof(Shader::Profile)) {
        LOG_WARNING(Render, "Pipeline cache profile has unexpected size ({} != {})",
                    profile_data.size(), sizeof(Shader::Profile));
    }
    Shader::Profile cached_profile{};
    if (profile_data.size() == sizeof(Shader::Profile)) {
        std::memcpy(&cached_profile, profile_data.data(), sizeof(cached_profile));
    }
    if (profile_data.size() != sizeof(Shader::Profile) || cached_profile != profile) {
        // bbport: upstream closed the cache for the session here, so it was never rewritten
        // and every later session compiled every shader again (stutters on each new area).
        // Start a fresh cache for this build and GPU instead.
        LOG_WARNING(Render, "Pipeline cache isn't compatible with current system: rebuilding it");
        Storage::DataBase::Instance().Clear();
        Storage::DataBase::Instance().FinishPreload();
        profile_data.resize(sizeof(profile));
        std::memcpy(profile_data.data(), &profile, sizeof(profile));
        Storage::DataBase::Instance().Save(Storage::BlobType::ShaderProfile, "profile",
                                           std::move(profile_data));
        Storage::DataBase::Instance().Flush();
        return;
    }

    const u32 expected_total =
        static_cast<u32>(Storage::DataBase::Instance().CountBlobs(Storage::BlobType::PipelineKey));
    if (progress && expected_total > 0) {
        progress(0, expected_total, 0);
    }

    const u32 hits_before = g_pipeline_binary_hits.load(std::memory_order_relaxed);
    const u32 saves_before = g_pipeline_binary_saves.load(std::memory_order_relaxed);
    u32 num_pipelines{};
    u32 num_total_pipelines{};

    Storage::DataBase::Instance().ForEachBlob(
        Storage::BlobType::PipelineKey, [&](std::vector<u8>&& data) {
            ++num_total_pipelines;

            Serialization::Archive ar{std::move(data)};
            Serialization::Reader pldata{ar};

            u32 version{};
            pldata.Read(version);
            if (version == Serialization::PipelineKeyVersion) {
                u32 is_compute{};
                pldata.Read(is_compute);

                const bool result =
                    is_compute ? LoadComputePipeline(ar) : LoadGraphicsPipeline(ar);
                if (result) {
                    ++num_pipelines;
                }
            }

#ifdef BB_UWP
            if ((num_total_pipelines % 32u) == 0u) {
                _heapmin();
                if (const HANDLE heap = GetProcessHeap()) {
                    HeapCompact(heap, 0);
                }
            }
#endif
            if (progress && ((num_total_pipelines % 4u) == 0u ||
                             num_total_pipelines == expected_total)) {
                progress(num_total_pipelines, std::max(expected_total, num_total_pipelines),
                         num_pipelines);
            }
        });

#ifdef BB_UWP
    _heapmin();
    if (const HANDLE heap = GetProcessHeap()) {
        HeapCompact(heap, 0);
    }
#endif

    const u32 bin_hits = g_pipeline_binary_hits.load(std::memory_order_relaxed) - hits_before;
    const u32 bin_saves = g_pipeline_binary_saves.load(std::memory_order_relaxed) - saves_before;
    LOG_INFO(Render, "Preloaded {} / {} pipelines (binary hits={}, newly captured={})",
             num_pipelines, num_total_pipelines, bin_hits, bin_saves);
    std::printf("GPU: WarmUp preloaded %u / %u pipelines (VK_KHR_pipeline_binary hits=%u, saved=%u)\n",
                num_pipelines, num_total_pipelines, bin_hits, bin_saves);
    if (num_total_pipelines > num_pipelines) {
        LOG_WARNING(Render, "{} stale pipelines were found. Consider re-generating the cache",
                    num_total_pipelines - num_pipelines);
    }

    Storage::DataBase::Instance().FinishPreload();
    if (bin_saves > 0 || num_pipelines > 0) {
        FlushCaches();
    }
}

void PipelineCache::FlushCaches() {
    if (!EmulatorSettings.IsPipelineCacheEnabled()) {
        return;
    }
    auto& db = Storage::DataBase::Instance();
    if (!db.IsOpened()) {
        return;
    }
    if (pipeline_cache &&
        (num_new_pipelines != num_flushed_pipelines || num_flushed_pipelines == 0)) {
        auto [res, data] = instance.GetDevice().getPipelineCacheData(*pipeline_cache);
        if (res == vk::Result::eSuccess && !data.empty()) {
            db.Save(Storage::BlobType::DriverCache, "driver", std::move(data));
            num_flushed_pipelines = num_new_pipelines;
        }
    }
    db.Flush();
}

void PipelineCache::Sync() {
    FlushCaches();
    Storage::DataBase::Instance().Close();
}

} // namespace Vulkan

namespace Shader {

void Info::Serialize(Serialization::Archive& ar) const {
    Serialization::Writer info{ar};

    info.Write(this, sizeof(InfoPersistent));
    info.Write(flattened_ud_buf);
    srt_info.Serialize(ar);
}

bool Info::Deserialize(Serialization::Archive& ar) {
    Serialization::Reader info{ar};

    info.Read(this, sizeof(Shader::InfoPersistent));
    info.Read(flattened_ud_buf);

    return srt_info.Deserialize(ar);
}

void Gcn::FetchShaderData::Serialize(Serialization::Archive& ar) const {
    Serialization::Writer fetch{ar};
    ar.Grow(6 + attributes.size() * sizeof(VertexAttribute));

    fetch.Write(size);
    fetch.Write(vertex_offset_sgpr);
    fetch.Write(instance_offset_sgpr);
    fetch.Write(attributes);
}

bool Gcn::FetchShaderData::Deserialize(Serialization::Archive& ar) {
    Serialization::Reader fetch{ar};

    fetch.Read(size);
    fetch.Read(vertex_offset_sgpr);
    fetch.Read(instance_offset_sgpr);
    fetch.Read(attributes);

    return true;
}

void PersistentSrtInfo::Serialize(Serialization::Archive& ar) const {
    Serialization::Writer srt{ar};

    srt.Write(this, sizeof(*this));
    if (walker_func_size) {
        srt.Write(reinterpret_cast<void*>(walker_func), walker_func_size);
    }
}

bool PersistentSrtInfo::Deserialize(Serialization::Archive& ar) {
    Serialization::Reader srt{ar};

    srt.Read(this, sizeof(*this));

    if (walker_func_size) {
        walker_func = RegisterWalkerCode(ar.CurrPtr(), walker_func_size);
        ar.Advance(walker_func_size);
    }

    return true;
}

void StageSpecialization::Serialize(Serialization::Archive& ar) const {
    Serialization::Writer spec{ar};

    spec.Write(start);
    spec.Write(runtime_info);

    spec.Write(bitset.to_string());

    if (fetch_shader_data) {
        spec.Write(sizeof(*fetch_shader_data));
        fetch_shader_data->Serialize(ar);
    } else {
        spec.Write(size_t{0});
    }

    spec.Write(vs_attribs);
    spec.Write(buffers);
    spec.Write(images);
    spec.Write(fmasks);
    spec.Write(samplers);
}

bool StageSpecialization::Deserialize(Serialization::Archive& ar) {
    Serialization::Reader spec{ar};

    spec.Read(start);
    spec.Read(runtime_info);

    std::string bits{};
    spec.Read(bits);
    bitset = std::bitset<MaxStageResources>(bits);

    u64 fetch_data_size{};
    spec.Read(fetch_data_size);

    if (fetch_data_size) {
        Gcn::FetchShaderData fetch_data;
        fetch_data.Deserialize(ar);
        fetch_shader_data = fetch_data;
    }

    spec.Read(vs_attribs);
    spec.Read(buffers);
    spec.Read(images);
    spec.Read(fmasks);
    spec.Read(samplers);

    return true;
}

} // namespace Shader
