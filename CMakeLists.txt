cmake_minimum_required(VERSION 3.20)
project(KonPcPort LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
# No debug information (the release libraries would carry the build folder's
# paths) and source paths relative to the repository in any remaining macro.
if(NOT MSVC)
    add_compile_options(-g0 "-ffile-prefix-map=${CMAKE_SOURCE_DIR}/=")
endif()
option(KON_FULL_AOT "Compile complete generated corpus rather than bootstrap slice" OFF)
set(KON_OPTIMIZED_AOT_UNITS "ALL" CACHE STRING "ALL for native release optimization, a semicolon list of four-digit units, or empty for diagnostic O0")
# The portable PSP core is the same library on every platform. Windows links the
# prebuilt archive produced by build/framework (third_party/PSPRecomp), which is
# a native x86-64 object; Android has no such archive, so the NDK compiles the
# same eight sources here. Neither path changes the other.
if(ANDROID)
    if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/PSPRecomp/src/runtime.cpp")
        message(FATAL_ERROR "The Android build needs the PSPRecomp sources in third_party/PSPRecomp")
    endif()
    add_library(psprecomp_core STATIC
        third_party/PSPRecomp/src/decoder.cpp
        third_party/PSPRecomp/src/deflate.cpp
        third_party/PSPRecomp/src/elf32.cpp
        third_party/PSPRecomp/src/guest_memory.cpp
        third_party/PSPRecomp/src/nid_registry.cpp
        third_party/PSPRecomp/src/program_analysis.cpp
        third_party/PSPRecomp/src/runtime.cpp
        third_party/PSPRecomp/src/sha256.cpp)
    target_include_directories(psprecomp_core PUBLIC third_party/PSPRecomp/include)
    target_compile_options(psprecomp_core PRIVATE -O2 -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
else()
    add_library(psprecomp_core STATIC IMPORTED)
    set_target_properties(psprecomp_core PROPERTIES
        IMPORTED_LOCATION "${CMAKE_CURRENT_SOURCE_DIR}/build/framework/libpsprecomp_core.a"
        INTERFACE_INCLUDE_DIRECTORIES "${CMAKE_CURRENT_SOURCE_DIR}/third_party/PSPRecomp/include")
endif()
# The generated corpus is the game code translated by PSPRecomp; it is portable
# C++ and the Android build always uses all of it (126 units + registry).
if(ANDROID OR KON_FULL_AOT)
    if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/work/recompiled_corrected/auto_codegen_report.json")
        message(FATAL_ERROR "Complete automatic code generation required")
    endif()
    file(GLOB KON_GUEST_SOURCES CONFIGURE_DEPENDS "work/recompiled_corrected/generated_*.cpp")
else()
    set(KON_GUEST_SOURCES work/bootstrap/guest.cpp)
endif()
# Everything below is the Windows port: it includes Win32 headers (windows.h via
# native_display.hpp), links user32/gdi32/winmm/... and embeds a PE resource.
# Phase 0 of the Android port only builds the portable core above, so the whole
# block stays as it was for Windows and is skipped entirely for Android.
if(NOT ANDROID)
# The port's own code (runtime, renderer, audio, online...), without any of the
# game's: the release package ships it as a library and the launcher links it
# with the game code translated on the player's PC (src/native_build.hpp).
add_library(kon_host STATIC src/boot_probe.cpp src/native_network.cpp src/native_adhoc.cpp src/native_online.cpp src/native_controls.cpp src/host_headphones.cpp)
target_include_directories(kon_host PRIVATE src)
target_link_libraries(kon_host PUBLIC psprecomp_core user32 gdi32 winmm iphlpapi ole32 dbghelp hid ws2_32 comdlg32)
# Host renderer/runtime code only (not the recompiled guest corpus): SSE4.1
# inlines floor/ceil/round. No FMA is enabled, so IEEE results are unchanged.
set_source_files_properties(src/boot_probe.cpp PROPERTIES COMPILE_OPTIONS "-msse4.1")
# Development executable: the same library with the corpus generated here.
add_executable(kon_boot_probe ${KON_GUEST_SOURCES})
target_link_libraries(kon_boot_probe PRIVATE kon_host)
# The player's program: installs from the ISO, builds the game and starts it.
add_executable(kon_launcher src/launcher.cpp)
target_include_directories(kon_launcher PRIVATE src)
# comctl32: progress bar of the visible update dialog (src/update_dialog.hpp).
target_link_libraries(kon_launcher PRIVATE psprecomp_core comdlg32 bcrypt winhttp comctl32)
target_link_options(kon_launcher PRIVATE -municode -mwindows)
# Always leave one "Ultima version <fecha hora>.cmd" launcher for the newest build.
add_custom_command(TARGET kon_boot_probe POST_BUILD COMMAND ${CMAKE_COMMAND} "-DROOT=${CMAKE_SOURCE_DIR}" "-DEXE=$<TARGET_FILE:kon_boot_probe>" -P "${CMAKE_SOURCE_DIR}/tools/write_latest_launcher.cmake" VERBATIM)
# Icon and version information: the resource script compiled by zig rc and copied
# into the executable after linking (zig c++ does not link .res files).
get_filename_component(KON_ZIG_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/toolchain/zig-x86_64-windows-0.15.2" ABSOLUTE)
add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/app.res"
    COMMAND "${KON_ZIG_DIR}/zig.exe" rc /fo "${CMAKE_CURRENT_BINARY_DIR}/app.res" "${CMAKE_CURRENT_SOURCE_DIR}/src/app.rc"
    DEPENDS src/app.rc src/kon_version.h assets/branding/icon.ico VERBATIM)
add_custom_target(kon_app_resource DEPENDS "${CMAKE_CURRENT_BINARY_DIR}/app.res")
# Runs after linking and also when only the icon changed (stamp file).
add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/app_res_embedded.stamp"
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tools/embed_resources.py" "${CMAKE_CURRENT_BINARY_DIR}/app.res" "$<TARGET_FILE:kon_boot_probe>"
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tools/embed_resources.py" "${CMAKE_CURRENT_BINARY_DIR}/app.res" "$<TARGET_FILE:kon_launcher>"
    COMMAND "${CMAKE_COMMAND}" -E touch "${CMAKE_CURRENT_BINARY_DIR}/app_res_embedded.stamp"
    DEPENDS "${CMAKE_CURRENT_BINARY_DIR}/app.res" kon_boot_probe kon_launcher VERBATIM)
add_custom_target(kon_embed_resources ALL DEPENDS "${CMAKE_CURRENT_BINARY_DIR}/app_res_embedded.stamp")
target_include_directories(kon_boot_probe PRIVATE work/recompiled_corrected)
# Native release optimization preserves original arithmetic and scheduler semantics.
# Empty or explicit unit lists remain available for bounded diagnostic builds.
if(NOT MSVC)
    set_source_files_properties(${KON_GUEST_SOURCES} PROPERTIES COMPILE_OPTIONS "-O0;-fno-sanitize=undefined")
    if(KON_FULL_AOT)
        if(KON_OPTIMIZED_AOT_UNITS STREQUAL "ALL")
            set_source_files_properties(${KON_GUEST_SOURCES} PROPERTIES COMPILE_OPTIONS "-O2;-fno-sanitize=undefined")
        else()
        foreach(unit IN LISTS KON_OPTIMIZED_AOT_UNITS)
            if(NOT unit MATCHES "^[0-9][0-9][0-9][0-9]$")
                message(FATAL_ERROR "Expected a four-digit AOT unit identifier: ${unit}")
            endif()
            set(native_unit "${CMAKE_CURRENT_SOURCE_DIR}/work/recompiled_corrected/generated_unit_${unit}.cpp")
            if(NOT EXISTS "${native_unit}")
                message(FATAL_ERROR "Missing selected AOT unit: ${native_unit}")
            endif()
            set_source_files_properties("${native_unit}" PROPERTIES COMPILE_OPTIONS "-O2;-fno-sanitize=undefined")
        endforeach()
        endif()
    endif()
endif()
add_executable(content_mount_test src/content_mount_test.cpp)
add_executable(allegrex_instruction_test src/allegrex_instruction_test.cpp work/instruction_fixture/guest.cpp)
target_link_libraries(allegrex_instruction_test PRIVATE psprecomp_core)
enable_testing()
add_test(NAME language_content COMMAND content_mount_test "${CMAKE_CURRENT_SOURCE_DIR}")
add_test(NAME allegrex_execution COMMAND allegrex_instruction_test)

add_executable(guest_arena_test src/guest_arena_test.cpp)
add_test(NAME guest_memory_allocation COMMAND guest_arena_test)
file(GLOB KON_IMPORT_FIXTURE CONFIGURE_DEPENDS "work/import_fixture/generated/generated_*.cpp")
add_executable(import_codegen_test src/import_codegen_test.cpp ${KON_IMPORT_FIXTURE})
target_link_libraries(import_codegen_test PRIVATE psprecomp_core)
target_include_directories(import_codegen_test PRIVATE work/import_fixture/generated)
add_test(NAME automatic_import_execution COMMAND import_codegen_test)
file(GLOB KON_LOCAL_IMPORT_FIXTURE CONFIGURE_DEPENDS "work/import_fixture/local/generated_*.cpp")
add_executable(local_import_codegen_test src/import_codegen_test.cpp ${KON_LOCAL_IMPORT_FIXTURE})
target_link_libraries(local_import_codegen_test PRIVATE psprecomp_core)
target_include_directories(local_import_codegen_test PRIVATE work/import_fixture/local)
add_test(NAME same_unit_import_execution COMMAND local_import_codegen_test)

add_executable(thread_abi_test src/thread_abi_test.cpp)
target_link_libraries(thread_abi_test PRIVATE psprecomp_core)
add_test(NAME thread_work_area COMMAND thread_abi_test)

if(KON_FULL_AOT)
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    add_test(NAME native_game_initialization
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tools/check_native_boot.py"
                --root "${CMAKE_CURRENT_SOURCE_DIR}" --executable $<TARGET_FILE:kon_boot_probe>)
    set_tests_properties(native_game_initialization PROPERTIES TIMEOUT 100)
    add_test(NAME sdk_hle_contract_names
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tools/audit_hle_contracts.py")
    add_test(NAME original_music_chart_layout
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tools/tests/test_music_chart.py")
endif()

add_executable(host_clock_test src/host_clock_test.cpp)
add_test(NAME psp_clock_epochs COMMAND host_clock_test)

add_executable(kernel_mutex_test src/kernel_mutex_test.cpp)
add_test(NAME guest_mutex_ownership COMMAND kernel_mutex_test)

add_executable(kernel_signals_test src/kernel_signals_test.cpp)
add_test(NAME guest_event_signals COMMAND kernel_signals_test)

add_executable(native_context_switch_test src/native_context_switch_test.cpp ${KON_IMPORT_FIXTURE})
target_link_libraries(native_context_switch_test PRIVATE psprecomp_core)
target_include_directories(native_context_switch_test PRIVATE work/import_fixture/generated)
add_test(NAME compiled_task_context_switch COMMAND native_context_switch_test)

add_executable(native_callback_test src/native_callback_test.cpp ${KON_IMPORT_FIXTURE})
target_link_libraries(native_callback_test PRIVATE psprecomp_core)
target_include_directories(native_callback_test PRIVATE work/import_fixture/generated)
add_test(NAME compiled_callback_context COMMAND native_callback_test)

add_executable(native_files_test src/native_files_test.cpp)
add_test(NAME mounted_native_file_io COMMAND native_files_test "${CMAKE_CURRENT_SOURCE_DIR}")

add_executable(ge_queue_test src/ge_queue_test.cpp)
target_link_libraries(ge_queue_test PRIVATE psprecomp_core)
add_test(NAME native_graphics_queue COMMAND ge_queue_test)

add_executable(native_display_test src/native_display_test.cpp)
target_link_libraries(native_display_test PRIVATE psprecomp_core user32 gdi32 hid)
add_test(NAME native_framebuffer_scanout COMMAND native_display_test)

add_executable(scratchpad_memory_test src/scratchpad_memory_test.cpp)
target_link_libraries(scratchpad_memory_test PRIVATE psprecomp_core)
add_test(NAME psp_scratchpad_storage COMMAND scratchpad_memory_test)

add_executable(native_renderer_test src/native_renderer_test.cpp)
target_link_libraries(native_renderer_test PRIVATE psprecomp_core)
add_test(NAME native_clear_rasterization COMMAND native_renderer_test)
add_executable(native_geometry_test src/native_geometry_test.cpp)
target_link_libraries(native_geometry_test PRIVATE psprecomp_core)
add_test(NAME native_projected_rasterization COMMAND native_geometry_test)
add_executable(replay_native_draw src/replay_native_draw.cpp)
target_link_libraries(replay_native_draw PRIVATE psprecomp_core user32 gdi32)
add_executable(benchmark_native_draw src/benchmark_native_draw.cpp)
target_link_libraries(benchmark_native_draw PRIVATE psprecomp_core user32 gdi32)
add_executable(native_parallel_raster_test src/native_parallel_raster_test.cpp)
target_link_libraries(native_parallel_raster_test PRIVATE psprecomp_core)
add_test(NAME native_parallel_raster_exactness COMMAND native_parallel_raster_test)
# Reference relocation of a PSP PRX (compared with tools/psp_prx_to_exec.py).
add_executable(prx_image_dump src/prx_image_dump.cpp)
target_link_libraries(prx_image_dump PRIVATE psprecomp_core)
add_executable(native_gpu_probe src/native_gpu_probe.cpp)
target_link_libraries(native_gpu_probe PRIVATE d3d11 dxgi dxguid)

add_library(kon_native_media STATIC src/native_media.cpp)
target_include_directories(kon_native_media PRIVATE third_party/ffmpeg/include)
target_link_libraries(kon_host PUBLIC kon_native_media)
add_executable(native_media_test src/native_media_test.cpp)
target_link_libraries(native_media_test PRIVATE kon_native_media)
add_test(NAME original_atrac_native_decode COMMAND native_media_test "${CMAKE_CURRENT_SOURCE_DIR}")

add_executable(native_audio_test src/native_audio_test.cpp)
target_link_libraries(native_audio_test PRIVATE winmm)
add_test(NAME windows_pcm_device_output COMMAND native_audio_test)

add_executable(kernel_lwmutex_test src/kernel_lwmutex_test.cpp)
target_link_libraries(kernel_lwmutex_test PRIVATE psprecomp_core)
add_test(NAME guest_lightweight_mutex_workarea COMMAND kernel_lwmutex_test)
add_executable(native_controls_test src/native_controls_test.cpp)
target_link_libraries(native_controls_test PRIVATE psprecomp_core)
add_test(NAME native_controller_sampling COMMAND native_controls_test)
add_executable(native_gamepad_test src/native_gamepad_test.cpp)
add_test(NAME native_gamepad_reports COMMAND native_gamepad_test)
add_executable(native_discord_test src/native_discord_test.cpp)
add_test(NAME native_discord_presence COMMAND native_discord_test "${CMAKE_CURRENT_SOURCE_DIR}/analysis/extracted/en/PSP_GAME/USRDIR/std.cpk")
add_executable(native_image_test src/native_image_test.cpp)
target_link_libraries(native_image_test PRIVATE ole32 windowscodecs)
add_test(NAME native_png_decoder COMMAND native_image_test "${CMAKE_CURRENT_SOURCE_DIR}")
add_executable(native_upscale_test src/native_upscale_test.cpp)
add_test(NAME native_texture_upscale COMMAND native_upscale_test)
add_executable(native_glyph_test src/native_glyph_test.cpp)
add_test(NAME native_button_glyphs COMMAND native_glyph_test .)
add_executable(native_adhoc_test src/native_adhoc_test.cpp src/native_adhoc.cpp src/native_online.cpp src/native_network.cpp)
target_link_libraries(native_adhoc_test PRIVATE ws2_32 iphlpapi)
add_test(NAME native_adhoc_loopback COMMAND native_adhoc_test)
set_tests_properties(native_adhoc_loopback PROPERTIES TIMEOUT 60)
add_executable(native_logic_rate_test src/native_logic_rate_test.cpp)
add_test(NAME native_logic_rate COMMAND native_logic_rate_test)
add_executable(native_texture_test src/native_texture_test.cpp)
target_link_libraries(native_texture_test PRIVATE psprecomp_core)
add_test(NAME native_texture_sampling COMMAND native_texture_test)
add_executable(native_savedata_test src/native_savedata_test.cpp)
target_link_libraries(native_savedata_test PRIVATE psprecomp_core)
add_test(NAME native_savedata_lifecycle COMMAND native_savedata_test "${CMAKE_CURRENT_SOURCE_DIR}")
add_executable(native_atrac_test src/native_atrac_test.cpp)
target_link_libraries(native_atrac_test PRIVATE psprecomp_core kon_native_media)
add_test(NAME original_atrac_streaming COMMAND native_atrac_test "${CMAKE_CURRENT_SOURCE_DIR}")
add_executable(host_headphones_test src/host_headphones_test.cpp src/host_headphones.cpp)
target_link_libraries(host_headphones_test PRIVATE ole32)
add_test(NAME windows_headphone_endpoint COMMAND host_headphones_test)

add_executable(native_sleep_test src/native_sleep_test.cpp src/native_network.cpp src/native_adhoc.cpp src/native_online.cpp src/native_controls.cpp src/host_headphones.cpp)
target_link_libraries(native_sleep_test PRIVATE psprecomp_core kon_native_media user32 gdi32 winmm iphlpapi ole32 dbghelp hid ws2_32)
add_test(NAME production_sleep_callback_scheduler COMMAND native_sleep_test "${CMAKE_CURRENT_SOURCE_DIR}")

add_executable(native_wait_test src/native_wait_test.cpp)
add_test(NAME native_scheduler_timer COMMAND native_wait_test)

add_executable(native_audio_stream_probe src/native_audio_stream_probe.cpp)
target_link_libraries(native_audio_stream_probe PRIVATE winmm)

add_executable(native_audio_mixer_test src/native_audio_mixer_test.cpp)
add_test(NAME native_pcm_mixer COMMAND native_audio_mixer_test)

# Official pinned declarations; runtime uses the installed system Vulkan loader.
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/vulkan_headers/Vulkan-Headers-1.3.290/include/vulkan/vulkan.h")
    add_executable(native_vulkan_probe src/native_vulkan_probe.cpp)
    target_include_directories(native_vulkan_probe PRIVATE third_party/vulkan_headers/Vulkan-Headers-1.3.290/include)
endif()

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/vulkan_headers/Vulkan-Headers-1.3.290/include/vulkan/vulkan.h"
   AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/spirv_headers/SPIRV-Headers-vulkan-sdk-1.3.290.0/include/spirv/unified1/spirv.core.grammar.json")
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    set(KON_SHADER_DIR "${CMAKE_CURRENT_BINARY_DIR}/native_shaders")
    add_custom_command(
        OUTPUT "${KON_SHADER_DIR}/native_gpu_clear.spv" "${KON_SHADER_DIR}/native_gpu_clear_shader.hpp" "${KON_SHADER_DIR}/native_gpu_clear_manifest.json"
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tools/create_vulkan_clear_shader.py" --output-dir "${KON_SHADER_DIR}"
        DEPENDS tools/create_vulkan_clear_shader.py third_party/spirv_headers/SPIRV-Headers-vulkan-sdk-1.3.290.0/include/spirv/unified1/spirv.core.grammar.json
        VERBATIM)
    add_custom_target(native_clear_shader DEPENDS "${KON_SHADER_DIR}/native_gpu_clear.spv" "${KON_SHADER_DIR}/native_gpu_clear_shader.hpp" "${KON_SHADER_DIR}/native_gpu_clear_manifest.json")
    add_custom_command(
        OUTPUT "${KON_SHADER_DIR}/native_gpu_triangle.spv" "${KON_SHADER_DIR}/native_gpu_triangle_shader.hpp" "${KON_SHADER_DIR}/native_gpu_triangle_manifest.json"
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tools/create_vulkan_triangle_shader.py" --output-dir "${KON_SHADER_DIR}"
        DEPENDS tools/create_vulkan_triangle_shader.py third_party/spirv_headers/SPIRV-Headers-vulkan-sdk-1.3.290.0/include/spirv/unified1/spirv.core.grammar.json
        VERBATIM)
    add_custom_target(native_triangle_shader DEPENDS "${KON_SHADER_DIR}/native_gpu_triangle.spv" "${KON_SHADER_DIR}/native_gpu_triangle_shader.hpp" "${KON_SHADER_DIR}/native_gpu_triangle_manifest.json")
    add_custom_command(
        OUTPUT "${KON_SHADER_DIR}/native_gpu_raster.spv" "${KON_SHADER_DIR}/native_gpu_raster_shader.hpp" "${KON_SHADER_DIR}/native_gpu_raster_manifest.json"
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tools/build_vulkan_raster_shader.py" --output-dir "${KON_SHADER_DIR}"
        DEPENDS tools/build_vulkan_raster_shader.py src/shaders/native_raster.comp third_party/glslang/bin/glslang.exe
        VERBATIM)
    add_custom_target(native_raster_shader DEPENDS "${KON_SHADER_DIR}/native_gpu_raster.spv" "${KON_SHADER_DIR}/native_gpu_raster_shader.hpp" "${KON_SHADER_DIR}/native_gpu_raster_manifest.json")
    add_library(kon_native_vulkan STATIC src/native_vulkan_clear.cpp src/native_vulkan_present.cpp "${KON_SHADER_DIR}/native_gpu_clear_shader.hpp")
    target_compile_options(kon_native_vulkan PRIVATE -msse4.1)
    add_dependencies(kon_native_vulkan native_clear_shader native_triangle_shader native_raster_shader)
    target_include_directories(kon_native_vulkan PRIVATE "${KON_SHADER_DIR}" third_party/vulkan_headers/Vulkan-Headers-1.3.290/include)
    target_link_libraries(kon_host PUBLIC kon_native_vulkan)
    target_compile_definitions(kon_host PRIVATE KON_HAS_NATIVE_VULKAN=1)
    add_executable(native_vulkan_shader_test src/native_vulkan_shader_test.cpp)
    add_dependencies(native_vulkan_shader_test native_clear_shader)
    target_include_directories(native_vulkan_shader_test PRIVATE src third_party/vulkan_headers/Vulkan-Headers-1.3.290/include)
    target_link_libraries(native_vulkan_shader_test PRIVATE psprecomp_core)
    add_executable(benchmark_native_clear src/benchmark_native_clear.cpp)
    target_link_libraries(benchmark_native_clear PRIVATE psprecomp_core kon_native_vulkan)
    add_executable(native_vulkan_renderer_test src/native_vulkan_renderer_test.cpp)
    target_link_libraries(native_vulkan_renderer_test PRIVATE psprecomp_core kon_native_vulkan)
    add_executable(benchmark_native_triangles src/benchmark_native_triangles.cpp)
    target_link_libraries(benchmark_native_triangles PRIVATE psprecomp_core kon_native_vulkan)
    add_executable(native_vulkan_triangle_test src/native_vulkan_triangle_test.cpp)
    target_link_libraries(native_vulkan_triangle_test PRIVATE psprecomp_core kon_native_vulkan)
    add_executable(benchmark_native_raster src/benchmark_native_raster.cpp)
    target_link_libraries(benchmark_native_raster PRIVATE psprecomp_core kon_native_vulkan user32 gdi32)
    add_executable(native_vulkan_raster_test src/native_vulkan_raster_test.cpp)
    target_link_libraries(native_vulkan_raster_test PRIVATE psprecomp_core kon_native_vulkan)
    add_executable(native_frame_replay_benchmark src/native_frame_replay_benchmark.cpp)
    target_link_libraries(native_frame_replay_benchmark PRIVATE psprecomp_core kon_native_vulkan)
    add_executable(native_vulkan_render_worker_test src/native_vulkan_render_worker_test.cpp)
    target_link_libraries(native_vulkan_render_worker_test PRIVATE psprecomp_core kon_native_vulkan)
    option(KON_GPU_TESTS "Run actual Vulkan hardware tests; requires an installed compute-capable GPU" OFF)
    if(KON_GPU_TESTS)
        add_test(NAME native_vulkan_triangle_exactness COMMAND native_vulkan_triangle_test "${CMAKE_CURRENT_BINARY_DIR}/vulkan_triangle_verification.json")
        add_test(NAME native_vulkan_shader_exactness COMMAND native_vulkan_shader_test "${KON_SHADER_DIR}/native_gpu_clear.spv" "${CMAKE_CURRENT_BINARY_DIR}/vulkan_shader_verification.json")
        add_test(NAME native_vulkan_renderer_exactness COMMAND native_vulkan_renderer_test "${CMAKE_CURRENT_BINARY_DIR}/vulkan_renderer_verification.json")
    endif()
endif()

add_executable(native_render_timeline_test src/native_render_timeline_test.cpp)
add_test(NAME native_independent_render_timeline COMMAND native_render_timeline_test)

add_executable(native_vram_ownership_test src/native_vram_ownership_test.cpp)
target_link_libraries(native_vram_ownership_test PRIVATE psprecomp_core)
add_test(NAME original_vram_gpu_ownership COMMAND native_vram_ownership_test)

add_executable(native_render_worker_test src/native_render_worker_test.cpp)
target_link_libraries(native_render_worker_test PRIVATE psprecomp_core)
add_test(NAME native_independent_render_worker COMMAND native_render_worker_test)
endif() # NOT ANDROID

# ---------------------------------------------------------------------------
# Android (phase 0): the portable core and the generated corpus, plus headless
# probes. No window, no Activity, no JNI, no APK: these are plain Android
# executables built against bionic and pushed with adb.
# ---------------------------------------------------------------------------
if(ANDROID)
    # All 127 generated translation units (126 units + registry) as one library.
    # The corpus is portable C++: it includes only psprecomp headers, <bit>,
    # <cmath>, <cstdint> and <limits>, with no windows.h and no Win32 API.
    add_library(kon_guest_corpus STATIC ${KON_GUEST_SOURCES})
    target_include_directories(kon_guest_corpus PRIVATE work/recompiled_corrected)
    target_link_libraries(kon_guest_corpus PUBLIC psprecomp_core)
    # Same release optimization the Windows corpus uses; the generated units are
    # huge single functions, so keep the diagnostic sanitizer flags out.
    target_compile_options(kon_guest_corpus PRIVATE -O2 -fno-sanitize=undefined)

    # The patched corpus reads the PC frame-rate host state that Windows defines
    # in src/boot_probe.cpp; this portable mirror provides the same 30 FPS
    # defaults for the Android core (see the file header).
    add_library(kon_android_host STATIC android/host_symbols.cpp)
    target_link_libraries(kon_android_host PUBLIC psprecomp_core)

    # Public runtime: the portable game loop from src/boot_probe.cpp
    # (compiled as kon_main), the Android window/JNI glue and the host
    # services. It contains no generated game code; the private module is
    # loaded from the player's own pack at run time.
    set(KON_FFMPEG_ANDROID "${CMAKE_CURRENT_SOURCE_DIR}/third_party/ffmpeg-android/${ANDROID_ABI}")
    add_library(kon_android_runtime SHARED
        src/boot_probe.cpp
        android/android_game.cpp
        android/android_iso.cpp
        src/native_media.cpp
        src/native_controls.cpp
        src/native_adhoc.cpp
        src/native_online.cpp
        src/native_network.cpp
        src/host_headphones.cpp)
    set_source_files_properties(src/boot_probe.cpp PROPERTIES COMPILE_DEFINITIONS "main=kon_main")
    set_target_properties(kon_android_runtime PROPERTIES POSITION_INDEPENDENT_CODE ON OUTPUT_NAME kon_runtime)
    set_target_properties(psprecomp_core PROPERTIES POSITION_INDEPENDENT_CODE ON)
    set_target_properties(kon_guest_corpus PROPERTIES POSITION_INDEPENDENT_CODE ON)
    set_target_properties(kon_android_host PROPERTIES POSITION_INDEPENDENT_CODE ON)
    target_include_directories(kon_android_runtime PRIVATE src)
    target_link_libraries(kon_android_runtime PRIVATE psprecomp_core android log aaudio dl m atomic)
    if(EXISTS "${KON_FFMPEG_ANDROID}/lib/libavcodec.a")
        target_include_directories(kon_android_runtime PRIVATE "${KON_FFMPEG_ANDROID}/include")
        target_link_libraries(kon_android_runtime PRIVATE
            "${KON_FFMPEG_ANDROID}/lib/libavformat.a"
            "${KON_FFMPEG_ANDROID}/lib/libavcodec.a"
            "${KON_FFMPEG_ANDROID}/lib/libswresample.a"
            "${KON_FFMPEG_ANDROID}/lib/libavutil.a")
    else()
        message(WARNING "FFmpeg for Android is missing at ${KON_FFMPEG_ANDROID}; kon_android_runtime will not link yet")
    endif()

    # Private game module: generated corpus + registration entry point and the
    # 30 FPS host symbols it reads. Built locally from the player's ISO; never
    # packaged in the public APK.
    add_library(kon_android_game_module SHARED
        android/game_module.cpp)
    target_link_libraries(kon_android_game_module PRIVATE kon_guest_corpus kon_android_host psprecomp_core m atomic)
    set_target_properties(kon_android_game_module PROPERTIES
        POSITION_INDEPENDENT_CODE ON OUTPUT_NAME kon_game)
    target_include_directories(kon_android_game_module PRIVATE work/recompiled_corrected)

    # Headless probe: registers every generated unit in a Runtime (this executes
    # the corpus registration code on the device) and checks that the PSP core
    # decoded the game's own MADD opcode. Guest gameplay blocks need the
    # decrypted EBOOT and the mounted content, which is phase 1.
    add_executable(android_probe_test android/probe_main.cpp)
    target_include_directories(android_probe_test PRIVATE work/recompiled_corrected)
    target_link_libraries(android_probe_test PRIVATE kon_guest_corpus kon_android_host psprecomp_core)

    # Same generated-instruction test as the Windows allegrex_execution test: it
    # runs recompiled guest code (MADD edge cases, BREAK trap) on the device ABI.
    add_executable(android_guest_exec_test
        src/allegrex_instruction_test.cpp work/instruction_fixture/guest.cpp)
    target_link_libraries(android_guest_exec_test PRIVATE psprecomp_core)

    enable_testing()
    add_test(NAME android_corpus_registration COMMAND android_probe_test)
    add_test(NAME android_generated_instruction_execution COMMAND android_guest_exec_test)
endif()
