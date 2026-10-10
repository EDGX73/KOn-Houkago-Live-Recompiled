#pragma once
#include "psprecomp/runtime.hpp"
#include <cstdint>
namespace kon {
inline void reset_guest_context(psprecomp::AllegrexContext& ctx) {
    ctx={};ctx.vfpu_ctrl[0]=0xE4u;ctx.vfpu_ctrl[1]=0xE4u;
    ctx.vfpu_ctrl[3]=0x3Fu;ctx.vfpu_ctrl[7]=0x7772CEABu;
    for(unsigned n=8;n<16;n++)ctx.vfpu_ctrl[n]=0x3F800000u;
    for(unsigned n=0;n<4;n++)ctx.vfpu_ctrl[8+n]|=1u<<n;
}
// User thread ABI: reserve 256 bytes at the stack top for the k0 work area.
inline void initialize_thread_stack(psprecomp::GuestMemory& memory,psprecomp::AllegrexContext& ctx,
                                    std::uint32_t uid,std::uint32_t base,std::uint32_t size) {
    if(size<512u || !memory.contains(base,size)) throw std::invalid_argument("Invalid thread stack extent");
    const auto k0=base+size-256u;
    for(std::uint32_t n=0;n<256u;n++)memory.store8(k0+n,0);
    memory.store32(k0+0xC0u,uid);memory.store32(k0+0xC8u,base);
    memory.store32(k0+0xF8u,0xFFFFFFFFu);memory.store32(k0+0xFCu,0xFFFFFFFFu);
    memory.store32(base,uid);ctx.gpr[26]=k0;ctx.gpr[29]=k0;
}
}
