# Android NDK toolchain for the portable core (phase 0: no Activity, no APK).
#
# It drives the clang shipped with the installed NDK (28.2.13676358) directly,
# so the build does not need third_party/android, Gradle or a prebuilt archive:
#
#   cmake -S . -B build/android_arm64 -G Ninja \
#       -DCMAKE_TOOLCHAIN_FILE=cmake/android.toolchain.cmake \
#       -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-26 \
#       -DCMAKE_BUILD_TYPE=Release
#   cmake --build build/android_arm64 --target psprecomp_core kon_guest_corpus
#
# Only the portable targets exist in that configuration: the Windows runtime,
# renderer, launcher and Win32 tests are not part of an Android build yet.
#
# The API level is a cache variable (ANDROID_PLATFORM, "android-26") and the ABI
# is restricted to the two that this phase compiles: arm64-v8a (device) and
# x86_64 (emulator). Both use the NDK's own libc++ and sysroot.

cmake_minimum_required(VERSION 3.20)

set(ANDROID_ABI "arm64-v8a" CACHE STRING "Android ABI to build for (arm64-v8a or x86_64)")
set(ANDROID_PLATFORM "android-26" CACHE STRING "Android API level (android-24 or newer)")
set(ANDROID_NDK "" CACHE PATH "Android NDK root; detected from the environment when empty")

# --- NDK location -----------------------------------------------------------
if(NOT ANDROID_NDK)
    foreach(candidate IN ITEMS "$ENV{ANDROID_NDK_HOME}" "$ENV{ANDROID_NDK_ROOT}")
        if(candidate AND EXISTS "${candidate}/source.properties")
            set(ANDROID_NDK "${candidate}")
            break()
        endif()
    endforeach()
endif()
if(NOT ANDROID_NDK)
    foreach(sdk IN ITEMS "$ENV{ANDROID_HOME}" "$ENV{ANDROID_SDK_ROOT}")
        if(sdk AND EXISTS "${sdk}/ndk/28.2.13676358/source.properties")
            set(ANDROID_NDK "${sdk}/ndk/28.2.13676358")
            break()
        endif()
    endforeach()
endif()
if(NOT ANDROID_NDK)
    message(FATAL_ERROR
        "Android NDK not found. Set ANDROID_NDK, ANDROID_NDK_HOME or ANDROID_HOME.")
endif()
get_filename_component(ANDROID_NDK "${ANDROID_NDK}" ABSOLUTE)
if(NOT EXISTS "${ANDROID_NDK}/source.properties")
    message(FATAL_ERROR "Not an Android NDK: ${ANDROID_NDK}")
endif()
file(STRINGS "${ANDROID_NDK}/source.properties" _kon_ndk_revision_line
     REGEX "^Pkg.Revision")
string(REGEX REPLACE "^Pkg.Revision *= *" "" ANDROID_NDK_REVISION "${_kon_ndk_revision_line}")

# --- ABI / API --------------------------------------------------------------
string(REGEX REPLACE "^android-" "" ANDROID_API_LEVEL "${ANDROID_PLATFORM}")
if(NOT ANDROID_API_LEVEL MATCHES "^[0-9]+$")
    message(FATAL_ERROR "ANDROID_PLATFORM must look like android-26, got: ${ANDROID_PLATFORM}")
endif()

if(ANDROID_ABI STREQUAL "arm64-v8a")
    set(_kon_android_arch "aarch64")
elseif(ANDROID_ABI STREQUAL "x86_64")
    set(_kon_android_arch "x86_64")
else()
    message(FATAL_ERROR
        "Unsupported ANDROID_ABI '${ANDROID_ABI}' for this phase; use arm64-v8a or x86_64")
endif()
set(_kon_android_target "${_kon_android_arch}-linux-android${ANDROID_API_LEVEL}")

if(CMAKE_HOST_WIN32)
    set(_kon_android_host "windows-x86_64")
elseif(APPLE)
    set(_kon_android_host "darwin-x86_64")
else()
    set(_kon_android_host "linux-x86_64")
endif()

set(_kon_android_bin "${ANDROID_NDK}/toolchains/llvm/prebuilt/${_kon_android_host}/bin")
set(ANDROID_NDK_TOOLCHAIN_BIN "${_kon_android_bin}" CACHE INTERNAL "NDK clang directory")

# The real clang driver, not the per-API .cmd/shell shims: CMake passes the
# --target and --sysroot flags itself, which keeps response files and Ninja
# rules identical on every host.
set(CMAKE_C_COMPILER "${_kon_android_bin}/clang${CMAKE_EXECUTABLE_SUFFIX}")
set(CMAKE_CXX_COMPILER "${_kon_android_bin}/clang++${CMAKE_EXECUTABLE_SUFFIX}")
if(CMAKE_HOST_WIN32)
    set(CMAKE_C_COMPILER "${_kon_android_bin}/clang.exe")
    set(CMAKE_CXX_COMPILER "${_kon_android_bin}/clang++.exe")
endif()
if(NOT EXISTS "${CMAKE_CXX_COMPILER}")
    message(FATAL_ERROR
        "NDK ${ANDROID_NDK_REVISION} has no clang++ in ${_kon_android_bin}")
endif()

# --- Target system ----------------------------------------------------------
set(CMAKE_SYSTEM_NAME Android)
set(CMAKE_SYSTEM_VERSION ${ANDROID_API_LEVEL})
set(CMAKE_ANDROID_ARCH_ABI ${ANDROID_ABI})
set(CMAKE_ANDROID_NDK "${ANDROID_NDK}")
set(CMAKE_ANDROID_NDK_VERSION "${ANDROID_NDK_REVISION}")

set(CMAKE_C_COMPILER_TARGET "${_kon_android_target}")
set(CMAKE_CXX_COMPILER_TARGET "${_kon_android_target}")

set(CMAKE_SYSROOT "${ANDROID_NDK}/toolchains/llvm/prebuilt/${_kon_android_host}/sysroot")
set(CMAKE_FIND_ROOT_PATH "${CMAKE_SYSROOT}")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# Android requires position-independent executables and the core uses exceptions
# and RTTI (std::function, std::filesystem). libc++ is linked statically so the
# probe binary can be pushed to /data/local/tmp without extra .so files.
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_C_FLAGS_INIT "-fPIC")
set(CMAKE_CXX_FLAGS_INIT "-fPIC -fexceptions -frtti")
set(CMAKE_EXE_LINKER_FLAGS_INIT "-pie -static-libstdc++")
set(CMAKE_SHARED_LINKER_FLAGS_INIT "-static-libstdc++")

# The NDK clang driver already knows its sysroot and target; keep CMake from
# searching the Windows host for libraries.
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

message(STATUS
    "Android NDK ${ANDROID_NDK_REVISION}: ${ANDROID_ABI}, ${ANDROID_PLATFORM}, ${CMAKE_CXX_COMPILER}")
