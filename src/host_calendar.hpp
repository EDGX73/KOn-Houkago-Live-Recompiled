#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <cstdint>
#include <stdexcept>
#include "host_clock.hpp"
namespace kon {
inline std::uint64_t utc_to_local_tick(std::uint64_t tick,const TIME_ZONE_INFORMATION* zone=nullptr) {
    constexpr std::uint64_t file_epoch=50491123200000000ull;
    constexpr std::uint64_t calendar_end=315537897600000000ull;
    if(tick<file_epoch || tick>=calendar_end)throw std::out_of_range("UTC calendar outside Windows time-zone conversion range");
    const auto fraction=tick%1000u;
    const auto units=(tick-file_epoch)*10u;
    FILETIME input{static_cast<DWORD>(units),static_cast<DWORD>(units>>32u)},output{};
    SYSTEMTIME utc{},local{};
    if(!FileTimeToSystemTime(&input,&utc) || !(zone?SystemTimeToTzSpecificLocalTime(zone,&utc,&local):SystemTimeToTzSpecificLocalTimeEx(nullptr,&utc,&local)) || !SystemTimeToFileTime(&local,&output))
        throw std::runtime_error("Windows UTC to local calendar conversion failed");
    const auto result=(static_cast<std::uint64_t>(output.dwHighDateTime)<<32u)|output.dwLowDateTime;
    return file_epoch+result/10u+fraction;
}
}
