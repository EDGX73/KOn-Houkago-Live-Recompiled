#include "native_callback.hpp"
#include <cstring>
#include <iostream>
#include <stdexcept>
int main(){try {
    psprecomp::Runtime rt;psprecomp::register_generated_functions(rt);
    unsigned calls=0;
    rt.register_hle("ProbeLib",0x12345678u,[&](auto&,auto& c){++calls;c.set_gpr(2,c.gpr[4]+1u);});
    auto& c=rt.cpu();c.gpr[29]=0x09FFF000u;c.gpr[31]=0x08804300u;c.gpr[4]=15;
    c.gpr[16]=0xDEAD0001u;c.pc=0x08804400u;c.fpr[7]=0x42C80000u;
    const auto saved=c;psprecomp::set_runtime_thread_identity(10,"callback owner");
    const auto result=kon::run_native_callback(rt,c,0x08804000u,2,1,0xA55Au,0x09FFE000u);
    if(!result.completed || result.value!=43 || calls!=1 || rt.stopped() ||
       std::memcmp(&saved,&c,sizeof(c)) || psprecomp::runtime_thread_uid()!=10)
       throw std::runtime_error("Compiled callback failed to return with full owner context restored");
    if(rt.memory().load32(0x09FFEEF0u)!=0x08000140u)
       throw std::runtime_error("Callback did not use its own native guest stack frame");
    c.gpr[29]=0x09FFF100u;const auto interrupted=c;
    const auto interrupt=kon::run_native_interrupt(rt,c,0x08804000u,2,1,0xA55Au,0x09FFC000u,4096);
    if(!interrupt.completed || interrupt.value!=43 || calls!=2 || rt.stopped() ||
       std::memcmp(&interrupted,&c,sizeof(c)) || psprecomp::runtime_thread_uid()!=10 ||
       rt.memory().load32(0x09FFCEE0u)!=0x08000140u)
       throw std::runtime_error("Interrupt did not use its dedicated stack and preserve the interrupted task");
    std::cout<<"PASS: compiled callback/import executes, returns result and preserves owner CPU context\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<"\n";return 1;}}
