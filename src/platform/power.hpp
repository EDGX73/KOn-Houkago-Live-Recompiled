#pragma once
// Power state for scePower callbacks (platform layer): 0x1000 external power,
// 0x80 battery present, 0x100 charging, low byte = battery percentage.
#include <cstdint>
#include <stdexcept>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <filesystem>
#include <fstream>
#include <string>
#endif
namespace kon::platform {
inline std::uint32_t host_power_flags() {
#ifdef _WIN32
    SYSTEM_POWER_STATUS status{};
    if(!GetSystemPowerStatus(&status))throw std::runtime_error("Cannot query Windows power status");
    std::uint32_t flags=status.ACLineStatus==1?0x1000u:0u;
    if(status.BatteryFlag!=255 && !(status.BatteryFlag&128)) {
        flags|=0x80u;
        if(status.BatteryFlag&6)flags|=0x100u;
        if(status.BatteryLifePercent<=100)flags|=status.BatteryLifePercent;
    }
    return flags;
#else
    // Linux power_supply class. Android may deny it to apps (SELinux): then
    // report external power and no battery, like a desktop PC without one.
    namespace fs=std::filesystem;
    const auto text=[](const fs::path& p){std::ifstream in(p);std::string s;std::getline(in,s);return s;};
    std::uint32_t flags=0;bool known=false;std::error_code error;
    for(const auto& entry:fs::directory_iterator("/sys/class/power_supply",error)){
        const auto type=text(entry.path()/"type");
        if(type=="Mains" || type=="USB"){known=true;if(text(entry.path()/"online")=="1")flags|=0x1000u;}
        else if(type=="Battery"){
            const auto capacity=text(entry.path()/"capacity");if(capacity.empty())continue;
            known=true;flags|=0x80u;
            const auto status=text(entry.path()/"status");if(status=="Charging" || status=="Full")flags|=0x100u;
            try{const auto percent=std::stoul(capacity);if(percent<=100)flags|=static_cast<std::uint32_t>(percent);}catch(const std::exception&){}
        }
    }
    return known?flags:0x1000u;
#endif
}
}
