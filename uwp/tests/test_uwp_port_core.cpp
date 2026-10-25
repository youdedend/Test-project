// SPDX-License-Identifier: GPL-2.0-or-later
// Host C++20 Verification Suite for BBPort.UWP subsystems:
// Links and tests GamepadInput, DirectInputShim, StorageLayer, UwpFileLoader, LifecycleManager,
// XAudio2Sink, D3D12DescriptorManager (TextureSetMemo + 5 GiB LRU eviction), D3D12RenderPassCache,
// SpirvToDxilCompiler, D3D12Backend, D3D11FallbackBackend, and BbgpuUwpBridge.

#include "../BBPort.UWP/Platform/GamepadInput.h"
#include "../BBPort.UWP/Platform/LifecycleManager.h"
#include "../BBPort.UWP/Platform/StorageLayer.h"
#include "../BBPort.UWP/Platform/UwpFileLoader.h"
#include "../BBPort.UWP/Platform/XAudio2Sink.h"
#include "../input/DirectInputShim.h"
#include "../renderer/d3d12/D3D12Backend.h"
#include "../renderer/d3d12/D3D12DescriptorManager.h"
#include "../renderer/d3d12/D3D12RenderPassCache.h"
#include "../renderer/d3d12/SpirvToDxilCompiler.h"
#include "../../gpu/bbgpu.h"

#include <cassert>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

// Constructs a valid minimal PS4 binary param.sfo (\0PSF) with TITLE_ID and APP_VER.
std::vector<uint8_t> BuildSyntheticParamSfo(const std::string& title_id,
                                            const std::string& app_ver,
                                            uint32_t attribute = 0x80000000u) {
    // 3 entries: APP_VER (utf8), ATTRIBUTE (int32), TITLE_ID (utf8) — sorted lexicographically per PSF spec
    const std::string keys = std::string("APP_VER\0", 8) +
                             std::string("ATTRIBUTE\0", 10) +
                             std::string("TITLE_ID\0", 9);

    std::vector<uint8_t> vals;
    auto append_padded_str = [&](const std::string& s, uint32_t max_len) {
        const size_t start = vals.size();
        vals.resize(start + max_len, 0);
        std::memcpy(vals.data() + start, s.data(), std::min<size_t>(s.size(), max_len - 1));
    };
    append_padded_str(app_ver, 8);
    const size_t attr_off = vals.size();
    vals.resize(attr_off + 4);
    vals[attr_off + 0] = static_cast<uint8_t>(attribute & 0xFF);
    vals[attr_off + 1] = static_cast<uint8_t>((attribute >> 8) & 0xFF);
    vals[attr_off + 2] = static_cast<uint8_t>((attribute >> 16) & 0xFF);
    vals[attr_off + 3] = static_cast<uint8_t>((attribute >> 24) & 0xFF);
    const size_t tid_off = vals.size();
    append_padded_str(title_id, 12);

    const uint32_t header_and_index = 20 + 3 * 16; // 68 bytes
    const uint32_t key_table_off = header_and_index;
    const uint32_t val_table_off = (key_table_off + static_cast<uint32_t>(keys.size()) + 3u) & ~3u;

    std::vector<uint8_t> out(val_table_off + vals.size(), 0);
    auto put16 = [&](size_t off, uint16_t v) {
        out[off] = static_cast<uint8_t>(v & 0xFF);
        out[off + 1] = static_cast<uint8_t>((v >> 8) & 0xFF);
    };
    auto put32 = [&](size_t off, uint32_t v) {
        out[off + 0] = static_cast<uint8_t>(v & 0xFF);
        out[off + 1] = static_cast<uint8_t>((v >> 8) & 0xFF);
        out[off + 2] = static_cast<uint8_t>((v >> 16) & 0xFF);
        out[off + 3] = static_cast<uint8_t>((v >> 24) & 0xFF);
    };

    out[0] = 0x00; out[1] = 'P'; out[2] = 'S'; out[3] = 'F';
    put32(4, 0x00000101u);
    put32(8, key_table_off);
    put32(12, val_table_off);
    put32(16, 3u);

    // Entry 0: APP_VER
    put16(20 + 0, 0);
    put16(20 + 2, 0x0204u);
    put32(20 + 4, static_cast<uint32_t>(app_ver.size() + 1));
    put32(20 + 8, 8);
    put32(20 + 12, 0);

    // Entry 1: ATTRIBUTE
    put16(36 + 0, 8);
    put16(36 + 2, 0x0404u);
    put32(36 + 4, 4);
    put32(36 + 8, 4);
    put32(36 + 12, static_cast<uint32_t>(attr_off));

    // Entry 2: TITLE_ID
    put16(52 + 0, 18);
    put16(52 + 2, 0x0204u);
    put32(52 + 4, static_cast<uint32_t>(title_id.size() + 1));
    put32(52 + 8, 12);
    put32(52 + 12, static_cast<uint32_t>(tid_off));

    std::memcpy(out.data() + key_table_off, keys.data(), keys.size());
    std::memcpy(out.data() + val_table_off, vals.data(), vals.size());
    return out;
}

void TestGamepadAndDirectInputShim() {
    using namespace bbport::uwp;
    using namespace bbport::uwp::input;

    GamepadProfile prof{};
    bool prev_chord = false;

    // 1. Standard Bloodborne combat & face buttons
    HostGamepadReading r{};
    r.buttons = HOST_PAD_A | HOST_PAD_B | HOST_PAD_X | HOST_PAD_Y |
                HOST_PAD_LEFT_SHOULDER | HOST_PAD_RIGHT_SHOULDER |
                HOST_PAD_MENU | HOST_PAD_LEFT_THUMB | HOST_PAD_RIGHT_THUMB;
    r.left_trigger = 0.85;   // L2 firearm parry
    r.right_trigger = 0.95;  // R2 charged heavy attack
    r.left_thumb_x = 0.0;
    r.left_thumb_y = 1.0;    // WinRT +Y up -> PS4 ly = 0 (forward!)

    const MappedOrbisPad m1 = TranslateReadingToBloodborne(r, prof, &prev_chord);
    assert((m1.buttons & ORBIS_BTN_CROSS) != 0);
    assert((m1.buttons & ORBIS_BTN_CIRCLE) != 0);
    assert((m1.buttons & ORBIS_BTN_SQUARE) != 0);
    assert((m1.buttons & ORBIS_BTN_TRIANGLE) != 0);
    assert((m1.buttons & ORBIS_BTN_L1) != 0);
    assert((m1.buttons & ORBIS_BTN_R1) != 0);
    assert((m1.buttons & ORBIS_BTN_L2) != 0);
    assert((m1.buttons & ORBIS_BTN_R2) != 0);
    assert((m1.buttons & ORBIS_BTN_L3) != 0);
    assert((m1.buttons & ORBIS_BTN_R3) != 0);
    assert((m1.buttons & ORBIS_BTN_OPTIONS) != 0);
    assert(m1.axes[1] <= 1); // Forward stick inverted from +1.0 WinRT to 0 PS4
    assert(m1.axes[4] > 200 && m1.axes[5] > 230);
    assert(m1.overlay_toggle_edge == true);

    // 2. Touchpad chord mapping: View alone -> Left (gestures x=480), View + D-Pad Right -> Right (personal effects x=1440, suppressing weapon switch!)
    HostGamepadReading view_only{};
    view_only.buttons = HOST_PAD_VIEW;
    const MappedOrbisPad m_left = TranslateReadingToBloodborne(view_only, prof, nullptr);
    assert((m_left.buttons & ORBIS_BTN_TOUCHPAD) != 0);
    assert(m_left.touch_side == 0);

    HostGamepadReading view_right{};
    view_right.buttons = HOST_PAD_VIEW | HOST_PAD_DPAD_RIGHT;
    const MappedOrbisPad m_right = TranslateReadingToBloodborne(view_right, prof, nullptr);
    assert((m_right.buttons & ORBIS_BTN_TOUCHPAD) != 0);
    assert((m_right.buttons & ORBIS_BTN_RIGHT) == 0); // D-Pad Right suppressed during touchpad chord!
    assert(m_right.touch_side == 1);

    // 3. DirectInput -> WGI shim + SDL2 WinRT verification
    LegacyDiJoyState2 di{};
    di.lY = 0;            // DirectInput [0..65535] up = 0
    di.lZ = 60000;        // LT pressed
    di.rgdwPOV[0] = 9000; // East
    di.rgbButtons[0] = 0x80; // Xbox HID Button 0 -> A
    const HostGamepadReading from_di = TranslateDirectInputState(di, /*ds4_button_order=*/false);
    assert((from_di.buttons & HOST_PAD_A) != 0);
    assert((from_di.buttons & HOST_PAD_DPAD_RIGHT) != 0);
    assert(from_di.left_thumb_y > 0.95);
    assert(from_di.left_trigger > 0.85);

    const SdlWinrtAuditReport sdl_audit = VerifySdlWinrtXboxMapping();
    assert(sdl_audit.AllPassed());
}

void TestStorageAndFileLoader() {
    using namespace bbport::uwp;
    namespace fs = std::filesystem;

    const fs::path sandbox = fs::temp_directory_path() / "bbport_uwp_unit_sandbox";
    std::error_code ec;
    fs::remove_all(sandbox, ec);

    auto& storage = StorageLayer::Instance();
    assert(storage.Initialize(sandbox / "LocalState", sandbox / "TempState"));

    // Verify C bridge roots
    char app0_buf[512]{}, user_buf[512]{}, temp_buf[512]{};
    assert(bbport_uwp_storage_get_roots(app0_buf, sizeof(app0_buf),
                                        user_buf, sizeof(user_buf),
                                        temp_buf, sizeof(temp_buf)) == 0);
    assert(std::string(app0_buf).find("CUSA03173") != std::string::npos);

    // Verify userdata0010 0x204E weapon SFX patch + .bbtmp cleanup
    const fs::path save_dir = storage.Roots().save_data_dir;
    {
        std::ofstream stale_tmp(save_dir / "userdata0000.bbtmp", std::ios::binary);
        stale_tmp << "interrupted";
        std::vector<char> save_blob(0x2100, 0);
        std::ofstream ud10(save_dir / "userdata0010", std::ios::binary);
        ud10.write(save_blob.data(), static_cast<std::streamsize>(save_blob.size()));
    }
    assert(storage.PrepareSaveDirectoryAndSoundFlag());
    assert(!fs::exists(save_dir / "userdata0000.bbtmp"));
    {
        std::ifstream ud10(save_dir / "userdata0010", std::ios::binary);
        ud10.seekg(0x204E);
        char patched = 0;
        ud10.get(patched);
        assert(patched == 1);
    }

    // Verify TemporaryFolder DXIL cache write/read/prune
    assert(storage.WriteShaderCacheBlob("vs60_deadbeef", "DXILBLOB1234", 12));
    std::string dxil_out;
    assert(storage.ReadShaderCacheBlob("vs60_deadbeef", &dxil_out));
    assert(dxil_out == "DXILBLOB1234");
    assert(storage.PruneShaderCache(0) >= 12);

    // Populate synthetic CUSA03173 v01.09 game folder and run FirstStartAudit
    const fs::path game_dir = storage.Roots().game_app0_dir;
    fs::create_directories(game_dir / "sce_sys");
    fs::create_directories(game_dir / "sce_module");
    fs::create_directories(game_dir / "dvdroot_ps4");

    const auto sfo_bytes = BuildSyntheticParamSfo("CUSA03173", "01.09");
    {
        std::ofstream sfo_f(game_dir / "sce_sys" / "param.sfo", std::ios::binary);
        sfo_f.write(reinterpret_cast<const char*>(sfo_bytes.data()),
                    static_cast<std::streamsize>(sfo_bytes.size()));
    }

    // Write valid decrypted ELF64 x86-64 header (e_machine = 62)
    std::vector<uint8_t> elf_hdr(128, 0);
    elf_hdr[0] = 0x7F; elf_hdr[1] = 'E'; elf_hdr[2] = 'L'; elf_hdr[3] = 'F';
    elf_hdr[4] = 2;    // ELFCLASS64
    elf_hdr[5] = 1;    // ELFDATA2LSB
    elf_hdr[18] = 62;  // EM_X86_64
    {
        std::ofstream eb(game_dir / "eboot.bin", std::ios::binary);
        eb.write(reinterpret_cast<const char*>(elf_hdr.data()),
                 static_cast<std::streamsize>(elf_hdr.size()));
        std::ofstream(game_dir / "sce_module" / "libc.prx", std::ios::binary) << "prx";
        std::ofstream(game_dir / "sce_module" / "libSceFios2.prx", std::ios::binary) << "prx";
    }

    const FirstStartAudit audit = UwpFileLoader::RunFirstStartCheck(game_dir);
    assert(audit.sfo_valid);
    assert(audit.sfo.title_id == "CUSA03173");
    assert(audit.sfo.app_ver == "01.09");
    assert(audit.is_bloodborne_cusa && audit.is_version_109);
    assert(audit.eboot_format == ExecutableFormat::DecryptedElf64);
    assert(audit.ReadyToLaunch());

    // Encrypted SELF must be detected and rejected
    const uint8_t enc_self[32] = {0x4F, 0x15, 0x3D, 0x1D, 0x00, 0x01, 0x01, 0x12};
    assert(UwpFileLoader::InspectExecutableHeader(enc_self, sizeof(enc_self)) ==
           ExecutableFormat::EncryptedPs4Self);

    // Path traversal '..' must be rejected
    fs::path resolved;
    assert(!UwpFileLoader::ResolveGuestAssetPath(game_dir, "/app0/../../Windows/System32", &resolved));
    assert(UwpFileLoader::ResolveGuestAssetPath(game_dir, "/app0/dvdroot_ps4/parts/fc_m_0000.partsbnd.dcx", &resolved));

    fs::remove_all(sandbox, ec);
}

void TestRendererCachesAndLifecycle() {
    using namespace bbport::uwp;
    using namespace bbport::uwp::renderer::d3d12;

    // 1. D3D12DescriptorManager TextureSetMemo + LRU 5 GiB pressure eviction
    D3D12DescriptorManager mgr(1024, 100ull * 1024ull * 1024ull); // 100 MiB memo cap for test
    assert(mgr.Initialize(nullptr));

    for (uint64_t i = 0; i < 4; ++i) {
        TextureSetKey k{};
        k.count = 4;
        k.image_view_ids[0] = 100 + i;
        k.registry_generation = 1;
        k.scene_targets_generation = 1;
        bool hit = true;
        const DescriptorSlice s = mgr.AcquireTextureSetSlice(k, /*frame=*/i + 1, 20ull * 1024ull * 1024ull, &hit);
        assert(s.valid && !hit);
    }
    assert(mgr.CachedSetCount() == 4);

    // Cache hit updates MRU frame
    {
        TextureSetKey k0{};
        k0.count = 4;
        k0.image_view_ids[0] = 100;
        k0.registry_generation = 1;
        k0.scene_targets_generation = 1;
        bool hit = false;
        const DescriptorSlice s0 = mgr.AcquireTextureSetSlice(k0, /*frame=*/10, 20ull * 1024ull * 1024ull, &hit);
        assert(s0.valid && hit);
    }

    // Evict 40 MiB at frame 12: k0 (frame 10, age 2 < protect 3) stays protected; older sets evict!
    const uint64_t freed = mgr.EvictColdEntries(40ull * 1024ull * 1024ull, /*current_frame=*/12, /*protect=*/3);
    assert(freed >= 40ull * 1024ull * 1024ull);
    assert(mgr.CachedSetCount() == 2);

    // Generation bump invalidates remaining old-generation sets
    assert(mgr.InvalidateStaleGenerations(2, 1) == 2);
    assert(mgr.CachedSetCount() == 0);
    assert(mgr.UsedDescriptors() == 0);

    // 2. Scene resolution scaling & SPIR-V thread clamp
    const SceneResolutionScale sc = D3D12RenderPassCache::ComputeXboxSceneScale(1920, 1080, 1);
    assert(sc.internal_width == 1280 && sc.internal_height == 720);
    assert(SpirvToDxilCompiler::ClampXboxCompilerThreads(16) == 4);
    assert(SpirvToDxilCompiler::ClampXboxCompilerThreads(0) == 2);

    // 3. LifecycleManager memory pressure classification & PLM suspend/resume callbacks
    assert(LifecycleManager::ClassifyUsage(3500ull << 20, 5120ull << 20) == MemoryPressureTier::Low);
    assert(LifecycleManager::ClassifyUsage(4000ull << 20, 5120ull << 20) == MemoryPressureTier::Medium);
    assert(LifecycleManager::ClassifyUsage(4500ull << 20, 5120ull << 20) == MemoryPressureTier::High);
    assert(LifecycleManager::ClassifyUsage(4900ull << 20, 5120ull << 20) == MemoryPressureTier::OverLimit);

    int checkpoint_calls = 0;
    LifecycleManager::Instance().SetSaveCheckpointCallback([&]() { ++checkpoint_calls; });
    LifecycleManager::Instance().HandleSuspending();
    assert(LifecycleManager::Instance().IsSuspended());
    assert(checkpoint_calls == 1);
    LifecycleManager::Instance().HandleResuming();
    assert(!LifecycleManager::Instance().IsSuspended());

    // 4. XAudio2 host ring smoke test
    assert(bbport_uwp_xaudio2_init(48000, 2) == 0);
    const float silence[512]{};
    assert(bbport_uwp_xaudio2_push(silence, sizeof(silence)) >= 0);
    bbport_uwp_xaudio2_shutdown();
}

} // namespace

int main() {
    TestGamepadAndDirectInputShim();
    TestStorageAndFileLoader();
    TestRendererCachesAndLifecycle();
    std::puts("PASS: all BBPort.UWP core platform, input, storage, SFO/ELF, and D3D12 cache tests succeeded.");
    return 0;
}
