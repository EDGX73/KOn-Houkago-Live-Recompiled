#pragma once
#include <chrono>
#include <array>
#include <cstdint>
#include <stdexcept>

namespace kon {
// PSP calendar ticks are microseconds from 0001-01-01, UTC. Relative time
// advances from a steady clock so an OS clock adjustment cannot reverse it.
class HostClock {
public:
    static constexpr std::uint64_t unix_epoch_tick=62135596800000000ull;
    static constexpr std::uint32_t resolution=1000000u;
    using Steady=std::chrono::steady_clock;
    HostClock():start_(Steady::now()),base_(from_unix_microseconds(
        std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::system_clock::now().time_since_epoch()).count())) {}
    explicit HostClock(std::uint64_t tick,Steady::time_point start=Steady::now()):start_(start),base_(tick) {}
    static std::uint64_t from_unix_microseconds(std::int64_t unix_us) {
        if(unix_us < -static_cast<std::int64_t>(unix_epoch_tick)) throw std::out_of_range("Calendar before PSP epoch");
        if(unix_us<0) return unix_epoch_tick-static_cast<std::uint64_t>(-unix_us);
        return unix_epoch_tick+static_cast<std::uint64_t>(unix_us);
    }
    static std::array<std::uint32_t,7> calendar_date(std::uint64_t tick) {
        if(tick>=315537897600000000ull)throw std::out_of_range("PSP calendar year exceeds 9999");
        constexpr std::uint64_t day_us=86400000000ull;
        const auto days=static_cast<std::int64_t>(tick/day_us)-719162;
        const std::chrono::year_month_day date{std::chrono::sys_days{std::chrono::days{days}}};
        const auto within=tick%day_us;
        return {static_cast<std::uint32_t>(static_cast<int>(date.year())),static_cast<unsigned>(date.month()),static_cast<unsigned>(date.day()),
            static_cast<std::uint32_t>(within/3600000000ull),static_cast<std::uint32_t>((within/60000000ull)%60),
            static_cast<std::uint32_t>((within/1000000ull)%60),static_cast<std::uint32_t>(within%1000000ull)};
    }
    std::uint64_t system_microseconds() const {
        const auto elapsed=std::chrono::duration_cast<std::chrono::microseconds>(Steady::now()-start_).count();
        return elapsed>0?static_cast<std::uint64_t>(elapsed):0;
    }
    std::uint64_t rtc_tick() const {return base_+system_microseconds();}
private:
    Steady::time_point start_;
    std::uint64_t base_;
};
}
