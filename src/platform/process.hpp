#pragma once
// Process identity (platform layer).
#include <cstdint>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <unistd.h>
#endif
namespace kon::platform {
inline std::uint32_t current_process_id(){
#ifdef _WIN32
    return GetCurrentProcessId();
#else
    return static_cast<std::uint32_t>(getpid());
#endif
}
}
