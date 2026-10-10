#include "kernel_signals.hpp"
#include <iostream>
#include <stdexcept>
void check(bool ok,const char* message){if(!ok)throw std::runtime_error(message);}
int main(){try{
    kon::KernelSignals s;
    check(s.create_event(6,"load",0,0)==0,"Create event");
    check(s.acquire_event(6,3,0).blocked,"Unsatisfied AND waits");
    check(s.set_event(6,1)==0,"Signal first bit");
    check(s.acquire_event(6,3,0).blocked,"Partial AND waits");
    check(!s.acquire_event(6,3,1).blocked,"OR accepts one bit");
    s.set_event(6,6);auto r=s.acquire_event(6,3,0x20);
    check(!r.blocked && r.pattern==7 && s.events.at(6).pattern==4,"Output before clear requested bits");
    check(s.clear_event(6,0xFFFFFFFBu)==0 && s.events.at(6).pattern==0,"Keep mask clear semantics");
    s.set_event(6,3);r=s.acquire_event(6,1,0x11);
    check(r.pattern==3 && s.events.at(6).pattern==0,"Clear entire flag");
    check(s.acquire_event(6,0,0).error==0x800201B1u,"Zero mask rejected");
    check(s.acquire_event(6,1,2).error==0x80020195u,"Invalid mode rejected");
    check(s.create_semaphore(7,"queue",0x100,1,3)==0,"Create semaphore");
    check(!s.acquire_semaphore(7,1).blocked,"Consume permit");
    check(s.acquire_semaphore(7,1).blocked,"Empty semaphore waits");
    check(s.signal_semaphore(7,3)==0,"Supply permits");
    check(s.signal_semaphore(7,1)==0x800201AEu,"Semaphore overflow rejected");
    check(!s.acquire_semaphore(7,2).blocked && s.semaphores.at(7).count==1,"Consume exact requested count");
    check(s.acquire_semaphore(7,4).error==0x800201BDu,"Count exceeding maximum rejected");
    check(s.cancel_semaphore(7,0)==0 && s.acquire_semaphore(7,1).blocked,"Cancellation can empty semaphore");
    check(s.cancel_semaphore(7,-5)==0 && !s.acquire_semaphore(7,1).blocked && s.acquire_semaphore(7,1).blocked,"Negative cancellation restores initial permits");
    check(s.cancel_semaphore(7,3)==0,"Cancellation accepts maximum count");
    check(s.cancel_semaphore(7,4)==0x800201BDu && s.semaphores.at(7).count==3,"Invalid cancellation preserves permits");
    check(!s.acquire_semaphore(7,3).blocked,"Restored permits are usable");
    check(s.cancel_semaphore(8,-1)==0x80020199u,"Cancellation rejects unknown semaphore");
    std::cout<<"PASS: event AND/OR, output and clear modes, semaphore contention and cancellation\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<"\n";return 1;}}
