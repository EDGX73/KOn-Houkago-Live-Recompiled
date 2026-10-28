#pragma once
#include "platform/wait.hpp"
namespace kon {
// The native scheduler's wait: a private high-resolution timer, no spin loop and
// no invented guest clock (implementation per platform in platform/wait.hpp).
using NativeWait=platform::WaitTimer;
}
