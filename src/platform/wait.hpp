#pragma once
// High-resolution relative waits for the native scheduler (platform layer).
// Windows: a private high-resolution waitable timer (no change to the global
// timer resolution, no spin loop); elsewhere: clock_nanosleep on the monotonic
// clock. Neither invents a guest clock.
#include <cstdint>
#include <stdexcept>
#include <string>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <cerrno>
#include <time.h>
#endif
namespace kon::platform {
class WaitTimer {
public:
#ifdef _WIN32
    WaitTimer(){
        timer_=CreateWaitableTimerExW(nullptr,nullptr,0x00000002u,TIMER_MODIFY_STATE|SYNCHRONIZE);
        high_resolution_=timer_!=nullptr;
        // A fallback without the high-resolution flag is reported on older Windows.
        if(!timer_ && GetLastError()==ERROR_INVALID_PARAMETER)timer_=CreateWaitableTimerExW(nullptr,nullptr,0,TIMER_MODIFY_STATE|SYNCHRONIZE);
        if(!timer_)fail("Create native scheduler timer");
    }
    ~WaitTimer(){if(timer_)CloseHandle(timer_);}
#else
    WaitTimer()=default;
#endif
    WaitTimer(const WaitTimer&)=delete;WaitTimer& operator=(const WaitTimer&)=delete;
    bool high_resolution()const{return high_resolution_;}
    void wait_for(std::uint64_t microseconds){
        if(!microseconds)return;
        if(microseconds>60000000u)throw std::invalid_argument("Native scheduler wait exceeds one minute");
#ifdef _WIN32
        LARGE_INTEGER due;due.QuadPart=-static_cast<LONGLONG>(microseconds*10u);
        if(!SetWaitableTimer(timer_,&due,0,nullptr,nullptr,FALSE))fail("Arm native scheduler timer");
        if(WaitForSingleObject(timer_,INFINITE)!=WAIT_OBJECT_0)fail("Wait for native scheduler timer");
#else
        timespec now{};if(clock_gettime(CLOCK_MONOTONIC,&now))fail("Read monotonic clock");
        const std::uint64_t due_ns=static_cast<std::uint64_t>(now.tv_sec)*1000000000u+static_cast<std::uint64_t>(now.tv_nsec)+microseconds*1000u;
        const timespec due{static_cast<time_t>(due_ns/1000000000u),static_cast<long>(due_ns%1000000000u)};
        int result;  // absolute deadline: an interrupted wait resumes without drifting
        while((result=clock_nanosleep(CLOCK_MONOTONIC,TIMER_ABSTIME,&due,nullptr))==EINTR){}
        if(result)fail("Wait for native scheduler timer");
#endif
    }
private:
#ifdef _WIN32
    HANDLE timer_=nullptr;bool high_resolution_=false;
    [[noreturn]]static void fail(const char* operation){throw std::runtime_error(std::string(operation)+" Windows error="+std::to_string(GetLastError()));}
#else
    bool high_resolution_=true;
    [[noreturn]]static void fail(const char* operation){throw std::runtime_error(std::string(operation)+" errno="+std::to_string(errno));}
#endif
};
}
