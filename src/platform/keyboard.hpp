#pragma once
// Keyboard state (platform layer), by Windows virtual-key code: the codes the
// key bindings use (native_button_map.hpp). Windows asks the system directly;
// elsewhere the window layer reports key events with set_key_state and maps its
// own key codes to these.
#include <array>
#include <atomic>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif
namespace kon::platform {
#ifdef _WIN32
inline bool key_down(int code){return (GetAsyncKeyState(code)&0x8000)!=0;}
#else
inline std::array<std::atomic<bool>,256>& key_table(){static std::array<std::atomic<bool>,256> keys{};return keys;}
inline void set_key_state(int code,bool down){if(code>=0 && code<256)key_table()[static_cast<std::size_t>(code)].store(down,std::memory_order_relaxed);}
inline bool key_down(int code){return code>=0 && code<256 && key_table()[static_cast<std::size_t>(code)].load(std::memory_order_relaxed);}
#endif
}
