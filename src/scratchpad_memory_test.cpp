#include "psprecomp/guest_memory.hpp"
#include "psprecomp/common.hpp"
#include <array>
#include <iostream>
#include <stdexcept>
void check(bool ok,const char* message){if(!ok)throw std::runtime_error(message);}
template<class F> void invalid(F f){bool failed=false;try{f();}catch(const psprecomp::Error&){failed=true;}check(failed,"Invalid scratchpad range accepted");}
int main(){try{
    psprecomp::GuestMemory mem; constexpr auto base=psprecomp::GuestMemory::kScratchpadBase;
    constexpr auto size=psprecomp::GuestMemory::kScratchpadSize;
    check(mem.contains(base,size) && !mem.contains(base-1) && !mem.contains(base+size),"Scratchpad boundaries wrong");
    check(!mem.contains(0) && !mem.contains(base+size-3,4),"Scratchpad extended into unmapped addresses");
    mem.aot_store32(0x00011800u,0x20u);mem.aot_store32(0x00011808u,0x00011000u);
    check(mem.aot_load32(0x00011800u)==0x20 && mem.aot_load32(0x40011808u)==0x11000,"Original allocator scratchpad addresses lost");
    auto fast=mem.aot_fast_view();fast.aot_store16(base+1,0xBEEF);fast.aot_store8(base+3,0xAC);
    check(fast.aot_load16(0x80010001u)==0xBEEF && fast.aot_load8(base+3)==0xAC,"Native fast-view fallback not mapped");
    std::array<std::uint8_t,5> bytes{1,2,3,4,5},copy{};
    mem.copy_in(base+size-5,bytes);mem.copy_out(0x40010000u+size-5,copy);check(bytes==copy,"Scratchpad copy/alias failed");
    check(mem.raw_pointer(base,size)!=nullptr && mem.raw_pointer(base+size-1,2)==nullptr,"Raw range escapes scratchpad");
    invalid([&]{mem.store32(base+size-3,0xDEADBEEFu);});invalid([&]{mem.zero(base+size-1,2);});
    check(mem.load8(base+size-1)==5,"Failed store partially modified scratchpad");
    mem.zero(base,size);for(auto n: {0u,1u,0x1800u,size-1})check(mem.load8(base+n)==0,"Scratchpad zero failed");
    mem.store32(0x08810000u,0xAABBCCDDu);mem.store32(0x04000000u,0x11223344u);
    check(mem.load32(0x08810000u)==0xAABBCCDDu && mem.load32(0x04000000u)==0x11223344u,"Scratchpad aliases another storage region");
    std::cout<<"PASS: real 16 KiB scratchpad, aliases, native loads/stores, strict bounds and independent regions\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<"\n";return 1;}}
