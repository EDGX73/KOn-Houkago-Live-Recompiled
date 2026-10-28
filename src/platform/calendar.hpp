#pragma once
// Local calendar for the PSP clock (platform layer): PSP ticks are microseconds
// from 0001-01-01; this converts a UTC tick to the host's local time, keeping the
// sub-millisecond fraction.
#include <cstdint>
#include <stdexcept>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <ctime>
#endif
namespace kon::platform {
#ifdef _WIN32
// zone: a fixed Windows time zone (tests); nullptr = the system's, with its
// historical daylight-saving rules.
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
#else
// The C library's zone rules (TZ, /etc/localtime or Android's tzdata) at that instant.
inline std::uint64_t utc_to_local_tick(std::uint64_t tick) {
    constexpr std::uint64_t unix_epoch=62135596800000000ull;
    constexpr std::uint64_t calendar_end=315537897600000000ull;
    if(tick<unix_epoch || tick>=calendar_end)throw std::out_of_range("UTC calendar outside host time-zone conversion range");
    const std::time_t seconds=static_cast<std::time_t>((tick-unix_epoch)/1000000u);
    std::tm local{};
    if(!localtime_r(&seconds,&local))throw std::runtime_error("Host UTC to local calendar conversion failed");
    const std::int64_t shifted=static_cast<std::int64_t>(tick)+static_cast<std::int64_t>(local.tm_gmtoff)*1000000;
    if(shifted<0 || static_cast<std::uint64_t>(shifted)>=calendar_end)throw std::out_of_range("Local calendar outside PSP range");
    return static_cast<std::uint64_t>(shifted);
}
#endif
}
