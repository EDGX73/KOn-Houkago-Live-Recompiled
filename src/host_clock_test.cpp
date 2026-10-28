#include "host_clock.hpp"
#include "host_calendar.hpp"
#include <chrono>
#include <cstdlib>
#include <ctime>
#include <iostream>
#include <limits>
#include <stdexcept>

void check(bool condition,const char* message) {if(!condition)throw std::runtime_error(message);}
int main() {
    try {
        using kon::HostClock;
        check(HostClock::from_unix_microseconds(0)==62135596800000000ull,"Unix epoch");
        check(HostClock::from_unix_microseconds(946684800000000ll)==63082281600000000ull,"2000-01-01 calendar");
        check(HostClock::from_unix_microseconds(-62135596800000000ll)==0,"PSP epoch");
        check(HostClock::from_unix_microseconds(-1)==62135596799999999ull,"Before Unix epoch fraction");
        bool rejected=false;
        try {HostClock::from_unix_microseconds(std::numeric_limits<std::int64_t>::min());}
        catch(const std::out_of_range&) {rejected=true;}
        check(rejected,"Invalid calendar overflow");
        const auto anchor=HostClock::Steady::now()-std::chrono::microseconds(1234567);
        HostClock elapsed(0,anchor);
        check(elapsed.system_microseconds()>=1234567 && elapsed.system_microseconds()<2000000,"Microsecond precision");
        const auto winter=HostClock::from_unix_microseconds(1705320000123456ll);
        const auto summer=HostClock::from_unix_microseconds(1721044800123456ll);
#ifdef _WIN32
        TIME_ZONE_INFORMATION zone{};zone.Bias=-60;zone.DaylightBias=-60;
        zone.StandardDate.wMonth=10;zone.StandardDate.wDay=5;zone.StandardDate.wHour=3;
        zone.DaylightDate.wMonth=3;zone.DaylightDate.wDay=5;zone.DaylightDate.wHour=2;
        check(kon::utc_to_local_tick(winter,&zone)==winter+3600000000ull,"Winter time-zone offset and sub-millisecond precision");
        check(kon::utc_to_local_tick(summer,&zone)==summer+7200000000ull,"Summer daylight saving offset");
        zone={};check(kon::utc_to_local_tick(winter,&zone)==winter,"UTC zone does not shift calendar");
#else
        // The same zone (UTC+1 with daylight saving from the last Sunday of March
        // to the last Sunday of October) through the C library's TZ rules.
        setenv("TZ","CET-1CEST,M3.5.0,M10.5.0/3",1);tzset();
        check(kon::utc_to_local_tick(winter)==winter+3600000000ull,"Winter time-zone offset and sub-millisecond precision");
        check(kon::utc_to_local_tick(summer)==summer+7200000000ull,"Summer daylight saving offset");
        setenv("TZ","UTC0",1);tzset();check(kon::utc_to_local_tick(winter)==winter,"UTC zone does not shift calendar");
#endif
        check(HostClock::calendar_date(0)==std::array<std::uint32_t,7>{1,1,1,0,0,0,0},"First PSP calendar day");
        check(HostClock::calendar_date(HostClock::unix_epoch_tick-1)==std::array<std::uint32_t,7>{1969,12,31,23,59,59,999999},"Calendar before Unix epoch retains fraction");
        check(HostClock::calendar_date(HostClock::from_unix_microseconds(951782400123456ll))==std::array<std::uint32_t,7>{2000,2,29,0,0,0,123456},"Gregorian leap day");
        check(HostClock::calendar_date(315537897599999999ull)==std::array<std::uint32_t,7>{9999,12,31,23,59,59,999999},"Last PSP calendar day");
        HostClock actual;
        const auto first=actual.rtc_tick();
        for(int n=0;n<20000;n++)check(actual.rtc_tick()>=first,"Clock reversed");
        check(first>63082281600000000ull,"Host UTC calendar");
        std::cout<<"PASS: calendar epochs, range and steady microsecond clock\n";
        return 0;
    }catch(const std::exception& e){std::cerr<<e.what()<<"\n";return 1;}
}
