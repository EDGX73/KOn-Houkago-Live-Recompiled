#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdint>
#include <stdexcept>
namespace kon {
inline std::uint32_t host_power_flags() {
    SYSTEM_POWER_STATUS status{};
    if(!GetSystemPowerStatus(&status))throw std::runtime_error("Cannot query Windows power status");
    std::uint32_t flags=status.ACLineStatus==1?0x1000u:0u;
    if(status.BatteryFlag!=255 && !(status.BatteryFlag&128)) {
        flags|=0x80u;
        if(status.BatteryFlag&6)flags|=0x100u;
        if(status.BatteryLifePercent<=100)flags|=status.BatteryLifePercent;
    }
    return flags;
}
}
