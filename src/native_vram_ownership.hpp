#pragma once
#include <cstddef>
#include <cstdint>
#include "psprecomp/guest_memory.hpp"
namespace psprecomp {
using NativeVramAccessHook=bool(*)(void*,std::uint32_t,std::size_t) noexcept;
void set_native_vram_access_hook(const GuestMemory*,NativeVramAccessHook,void*) noexcept;
void suppress_native_vram_access(bool) noexcept;
}
namespace kon {
// Only renderer-internal validated access bypasses the original CPU ownership
// hook. AOT loads, stores and host readers remain observable.
struct NativeVramInternalScope {
 NativeVramInternalScope(){psprecomp::suppress_native_vram_access(true);}
 ~NativeVramInternalScope(){psprecomp::suppress_native_vram_access(false);}
};
}
