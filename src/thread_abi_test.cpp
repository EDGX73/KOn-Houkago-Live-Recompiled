#include "thread_abi.hpp"
#include <iostream>
int main(){try {
    psprecomp::Runtime rt;auto& ctx=rt.cpu();kon::reset_guest_context(ctx);
    kon::initialize_thread_stack(rt.memory(),ctx,3,0x09FFC000u,0x2000u);
    if(ctx.gpr[26]!=0x09FFDF00u || ctx.gpr[29]!=ctx.gpr[26] || rt.memory().load32(ctx.gpr[26]+0xC0)!=3 || rt.memory().load32(ctx.gpr[26]+0xC8)!=0x09FFC000u)
        throw std::runtime_error("Thread k0 ABI mismatch");
    const float input[4]{2,3,5,7};float actual[4]{2,3,5,7};
    ctx.apply_vfpu_source_prefix(actual,4,0);
    for(unsigned n=0;n<4;n++) if(actual[n]!=input[n]) throw std::runtime_error("Reset VFPU prefix is not identity");
    std::cout<<"PASS: thread k0 work area and VFPU prefix identity\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
