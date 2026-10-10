#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <cstdint>
#include <stdexcept>
#include <string>
namespace kon {
// Private waitable timer: no change to the global timer resolution, no spin
// loop, and no invented guest clock. A fallback is reported on older Windows.
class NativeWait {
public:
    NativeWait(){
        timer_=CreateWaitableTimerExW(nullptr,nullptr,0x00000002u,TIMER_MODIFY_STATE|SYNCHRONIZE);
        high_resolution_=timer_!=nullptr;
        if(!timer_ && GetLastError()==ERROR_INVALID_PARAMETER)timer_=CreateWaitableTimerExW(nullptr,nullptr,0,TIMER_MODIFY_STATE|SYNCHRONIZE);
        if(!timer_)fail("Create native scheduler timer");
    }
    ~NativeWait(){if(timer_)CloseHandle(timer_);}
    NativeWait(const NativeWait&)=delete;NativeWait& operator=(const NativeWait&)=delete;
    bool high_resolution()const{return high_resolution_;}
    void wait_for(std::uint64_t microseconds){
        if(!microseconds)return;
        if(microseconds>60000000u)throw std::invalid_argument("Native scheduler wait exceeds one minute");
        LARGE_INTEGER due;due.QuadPart=-static_cast<LONGLONG>(microseconds*10u);
        if(!SetWaitableTimer(timer_,&due,0,nullptr,nullptr,FALSE))fail("Arm native scheduler timer");
        if(WaitForSingleObject(timer_,INFINITE)!=WAIT_OBJECT_0)fail("Wait for native scheduler timer");
    }
private:
    HANDLE timer_=nullptr;bool high_resolution_=false;
    [[noreturn]]static void fail(const char* operation){throw std::runtime_error(std::string(operation)+" Windows error="+std::to_string(GetLastError()));}
};
}
