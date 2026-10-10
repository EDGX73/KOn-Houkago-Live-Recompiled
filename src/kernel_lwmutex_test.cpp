#include "kernel_lwmutex.hpp"
#include <iostream>
#include <limits>
#include <stdexcept>
void check(bool c,const char* message){if(!c)throw std::runtime_error(message);}
int main(){try{
    psprecomp::GuestMemory memory;kon::KernelLwMutexes locks(memory);
    constexpr std::uint32_t area=0x08812000u,alias=0x48812000u;
    check(locks.create(34,area,"CRI Wave out",0x100,0,2)==0,"Game lightweight mutex creation failed");
    check(memory.load32(area)==0 && memory.load32(area+4)==0 && memory.load32(area+8)==0x100 && memory.load32(area+12)==0 && memory.load32(area+16)==34,"PSP workarea ABI wrong");
    for(unsigned offset=20;offset<32;offset+=4)check(memory.load32(area+offset)==0,"Workarea padding not initialized");
    check(!locks.acquire(alias,2,1).blocked && memory.load32(area+4)==2,"Aliased workarea ownership lost");
    check(locks.acquire(area,33,1).blocked,"Competing wave task did not wait");
    check(locks.release(area,33,1).error==kon::KernelLwMutexes::not_locked,"Wrong task released audio lock");
    check(locks.acquire(area,2,1).error==kon::KernelLwMutexes::already_locked,"Nonrecursive lightweight lock recursed");
    check(locks.release(area,2,1).error==0 && memory.load32(area+4)==0,"Final release did not clear owner");
    // The recompiled SDK fast path can change these words directly. Host state
    // must observe its counter/owner rather than overwriting a stale mirror.
    memory.store32(area,1);memory.store32(area+4,33);
    check(locks.acquire(area,2,1).blocked && locks.release(area,33,1).error==0,"Original fast-path writes were ignored");
    check(locks.erase(area).error==0 && memory.load32(area+4)==0xFFFFFFFFu && memory.load32(area+16)==0xFFFFFFFFu,"Deletion workarea contract wrong");
    check(locks.acquire(area,2,1).error==kon::KernelLwMutexes::missing,"Deleted mutex was reacquired");
    check(locks.create(35,area,"recursive",0x200,1,2)==0,"Recursive creation failed");
    check(locks.acquire(area,2,3).error==0 && memory.load32(area)==4,"Recursive counter lost");
    check(locks.release(area,2,5).error==kon::KernelLwMutexes::underflow,"Recursive underflow accepted");
    memory.store32(area,0x7FFFFFFFu);check(locks.acquire(area,2,1).error==kon::KernelLwMutexes::overflow,"Signed lock counter overflow accepted");
    check(locks.acquire(area+1,2,1).error==0x8000020Du && locks.create(36,0,"invalid",0,0,2)==0x800200D3u,"Invalid workarea accepted");
    std::cout<<"PASS: PSP lightweight workarea ABI, actual shared-memory fast path, contention, ownership, recursion and lifecycle errors\n";return 0;
}catch(const std::exception& error){std::cerr<<error.what()<<"\n";return 1;}}
