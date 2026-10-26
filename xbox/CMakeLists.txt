cmake_minimum_required(VERSION 3.24)
project(bloodborne_xbox LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_C_STANDARD_LIBRARIES "")
set(CMAKE_CXX_STANDARD_LIBRARIES "")

if(NOT CMAKE_C_COMPILER_ID MATCHES "Clang")
  message(FATAL_ERROR "Use clang-cl: guest PS4 x86-64 code uses the System V AMD64 ABI")
endif()

set(BB_ROOT "${CMAKE_CURRENT_LIST_DIR}/..")
set(BB_DEPS_ROOT "${CMAKE_CURRENT_LIST_DIR}/.local/deps" CACHE PATH
    "Pinned third-party dependencies fetched/built by xbox/scripts/build-deps.ps1")
set(BB_PTHREAD "${BB_DEPS_ROOT}/winpthreads" CACHE PATH
    "UCRT winpthreads headers and import library")

# xbox/platform/compat/unistd.h defines mode_t; prevent winpthreads from redefining a narrower one.
add_compile_definitions(_MODE_T_)
add_compile_options(/clang:-Wno-unused-command-line-argument)

file(STRINGS "${CMAKE_CURRENT_LIST_DIR}/prepared-format.txt" BB_PREPARED_FORMAT LIMIT_COUNT 1)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_LIST_DIR}/prepared-format.txt")

# Build bbgpu + bbnet static libraries in UWP/Dozen mode.
set(BB_UWP ON)
set(BBPORT_UWP ON)
set(BB_LTO OFF CACHE BOOL "" FORCE)
add_subdirectory("${BB_ROOT}/gpu" "${CMAKE_BINARY_DIR}/gpu")

file(GLOB BB_ATRAC9 CONFIGURE_DEPENDS "${BB_ROOT}/third_party/LibAtrac9/C/src/*.c")
add_library(bb_atrac9 STATIC ${BB_ATRAC9})
target_compile_options(bb_atrac9 PRIVATE /w)
set_target_properties(bb_atrac9 PROPERTIES C_STANDARD 99 INTERPROCEDURAL_OPTIMIZATION OFF)

set(BB_RUNTIME_SOURCES
  probe
  compat_win
  runtime
  runtime_ajm
  runtime_audio
  runtime_content
  runtime_file
  runtime_kernel
  runtime_memory
  runtime_mutex
  runtime_pad
  runtime_rebirth
  runtime_rtc
  runtime_rwlock
  runtime_savedata
  runtime_sema
  runtime_services
  runtime_thread
  runtime_time
)
list(TRANSFORM BB_RUNTIME_SOURCES PREPEND "${BB_ROOT}/src/")
list(TRANSFORM BB_RUNTIME_SOURCES APPEND ".c")

add_executable(bloodborne WIN32
  platform/main.cpp
  platform/prepare_game.cpp
  platform/runtime_compat.c
  platform/audio.cpp
  platform/input.cpp
  platform/desktop_shims.cpp
  ${BB_RUNTIME_SOURCES}
)

set_source_files_properties("${BB_ROOT}/src/runtime_ajm.c" PROPERTIES
    COMPILE_DEFINITIONS COMPILING_DLL)

target_include_directories(bloodborne PRIVATE
  "${BB_ROOT}"
  "${BB_ROOT}/src"
  platform
  platform/compat
  "${BB_PTHREAD}/include"
)

target_compile_definitions(bloodborne PRIVATE
  BB_UWP
  BBPORT_UWP
  VK_USE_PLATFORM_WIN32_KHR
  NOMINMAX
  WIN32_LEAN_AND_MEAN
  _CRT_SECURE_NO_WARNINGS
  BB_PREPARED_FORMAT="${BB_PREPARED_FORMAT}"
)

target_compile_options(bloodborne PRIVATE
  /W3
  $<$<COMPILE_LANGUAGE:C>:/clang:-Wno-deprecated-declarations>
  $<$<COMPILE_LANGUAGE:C>:/clang:-masm=att>
  $<$<COMPILE_LANGUAGE:CXX>:/EHsc>
)

# Keep 64 GiB - 1 TiB clear for the PS4 guest address space (/HIGHENTROPYVA:NO) and commit
# a 16 MiB default stack inside the AppContainer.
target_link_options(bloodborne PRIVATE
  /APPCONTAINER
  /HIGHENTROPYVA:NO
  /STACK:16777216,16777216
  /OPT:REF
)
target_link_directories(bloodborne BEFORE PRIVATE "$ENV{VCToolsInstallDir}/lib/x64/store")
target_link_libraries(bloodborne PRIVATE
  bbgpu
  bbnet
  bb_atrac9
  PkgConfig::SDL3
  PkgConfig::FFMPEG
  Vulkan::Headers
  WindowsApp
  xaudio2
  d3d12
  dxgi
  dxguid
  "${BB_PTHREAD}/lib/libwinpthread.dll.a"
)

set(BB_LAYOUT "${CMAKE_BINARY_DIR}/layout")
set(BB_DOZEN_DLL "${BB_MESA_ROOT}/build/dozen-uwp/src/microsoft/vulkan/vulkan_dzn.dll" CACHE FILEPATH
    "Mesa Dozen UWP DLL (vulkan_dzn.dll)")
set(BB_MESA_LICENSE "${BB_MESA_ROOT}/mesa/licenses/MIT" CACHE FILEPATH
    "Mesa MIT license")
set(BB_DXC_ROOT "${BB_MESA_ROOT}/tools/dxc" CACHE PATH
    "DirectX Shader Compiler release root (bin/x64/dxil.dll and licenses)")

set(BB_LICENSE_COMMANDS)
foreach(entry IN ITEMS
    "${BB_MESA_LICENSE}|Mesa-MIT.txt"
    "${BB_DXC_ROOT}/LICENCE-MIT.txt|DXC-LICENCE-MIT.txt"
    "${BB_DXC_ROOT}/LICENSE-LLVM.txt|DXC-LICENSE-LLVM.txt"
    "${BB_DXC_ROOT}/LICENSE-MS.txt|DXC-LICENSE-MS.txt"
    "${BB_ROOT}/gpu/shadps4/LICENSE|shadPS4-GPL-2.0.txt"
    "${BB_DEPS_ROOT}/licenses/ffmpeg.txt|FFmpeg-LGPL-2.1.txt"
    "${BB_DEPS_ROOT}/licenses/sdl.txt|SDL3-zlib.txt"
    "${BB_DEPS_ROOT}/licenses/fmt.txt|fmt-MIT.txt"
    "${BB_DEPS_ROOT}/licenses/zydis.txt|Zydis-MIT.txt"
    "${BB_DEPS_ROOT}/licenses/zycore.txt|Zycore-MIT.txt"
    "${BB_DEPS_ROOT}/licenses/xxhash.txt|xxHash-BSD-2.txt"
    "${BB_DEPS_ROOT}/licenses/vma.txt|VulkanMemoryAllocator-MIT.txt"
    "${BB_DEPS_ROOT}/licenses/vulkan-headers.txt|Vulkan-Headers.txt"
    "${BB_DEPS_ROOT}/licenses/robin-map.txt|robin-map-MIT.txt"
    "${BB_DEPS_ROOT}/boost/licenses/config.txt|Boost-BSL-1.0.txt"
    "${BB_ROOT}/third_party/LibAtrac9/LICENSE|LibAtrac9-MIT.txt"
    "${BB_ROOT}/gpu/third_party/imgui/LICENSE.txt|imgui-MIT.txt"
    "${BB_ROOT}/gpu/third_party/miniz/LICENSE|miniz-MIT.txt"
    "${BB_ROOT}/gpu/third_party/magic_enum/LICENSE|magic_enum-MIT.txt"
    "${BB_ROOT}/gpu/third_party/half/LICENSE.txt|half-MIT.txt"
    "${BB_ROOT}/gpu/third_party/sirit/LICENSE.txt|sirit-BSD-3.txt"
    "${BB_ROOT}/gpu/third_party/xbyak/COPYRIGHT|xbyak-BSD-3.txt"
    "${BB_ROOT}/gpu/third_party/fonts/LICENSE-DejaVu.txt|DejaVu-fonts.txt")
  string(REPLACE "|" ";" pair "${entry}")
  list(GET pair 0 source)
  list(GET pair 1 name)
  if(EXISTS "${source}")
    list(APPEND BB_LICENSE_COMMANDS
         COMMAND ${CMAKE_COMMAND} -E copy_if_different "${source}" "${BB_LAYOUT}/ThirdParty/${name}")
  endif()
endforeach()

add_custom_command(TARGET bloodborne POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E make_directory
          "${BB_LAYOUT}/Assets"
          "${BB_LAYOUT}/ThirdParty"
          "${BB_LAYOUT}/patches"
  COMMAND ${CMAKE_COMMAND} -E copy_directory
          "${CMAKE_CURRENT_LIST_DIR}/Assets" "${BB_LAYOUT}/Assets"
  COMMAND ${CMAKE_COMMAND} -E copy "$<TARGET_FILE:bloodborne>" "${BB_LAYOUT}"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different "${BB_DOZEN_DLL}" "${BB_LAYOUT}"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different "${BB_DXC_ROOT}/bin/x64/dxil.dll" "${BB_LAYOUT}"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          "${BB_DEPS_ROOT}/libwinpthread/bin/libwinpthread-1.dll" "${BB_LAYOUT}"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          "${BB_PTHREAD}/share/licenses/winpthreads/COPYING"
          "${BB_LAYOUT}/ThirdParty/winpthread-COPYING.txt"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          "${BB_ROOT}/patches/Bloodborne.xml" "${BB_LAYOUT}/patches/Bloodborne.xml"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          "${BB_ROOT}/patches/redzone.json" "${BB_LAYOUT}/patches/redzone.json"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          "${BB_ROOT}/LICENSE" "${BB_LAYOUT}/LICENSE.txt"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          "${CMAKE_CURRENT_LIST_DIR}/CREDITS.md" "${BB_LAYOUT}/CREDITS.md"
  COMMAND ${CMAKE_COMMAND} -E copy
          "${CMAKE_CURRENT_LIST_DIR}/AppxManifest.xml" "${BB_LAYOUT}/AppxManifest.xml"
  ${BB_LICENSE_COMMANDS}
)
