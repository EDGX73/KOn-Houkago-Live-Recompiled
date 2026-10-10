#include "psprecomp/runtime.hpp"
#include <iostream>
#include <stdexcept>
namespace {
psprecomp::AllegrexContext suspended;
unsigned child_runs=0;
void returned(psprecomp::Runtime& rt,psprecomp::AllegrexContext&){rt.stop("switch fixture returned");}
void child(psprecomp::Runtime&,psprecomp::AllegrexContext& c) {
    if(psprecomp::runtime_thread_uid()!=20 || c.gpr[29]!=0x09FFE000u)throw std::runtime_error("Wrong child context");
    ++child_runs;c=suspended;psprecomp::set_runtime_thread_identity(10,"caller");
}
}
int main(){try {
    psprecomp::Runtime rt;psprecomp::register_generated_functions(rt);
    rt.register_function(0x08804300u,returned,"fixture return");
    rt.register_function(0x08804320u,child,"context switch child");
    unsigned imports=0;
    rt.register_hle("ProbeLib",0x12345678u,[&](auto&,auto& c){
        ++imports;c.set_gpr(2,c.gpr[4]+1u);suspended=c;suspended.pc=c.gpr[31];
        c={};c.pc=0x08804320u;c.gpr[29]=0x09FFE000u;c.gpr[2]=0xBADu;
        psprecomp::set_runtime_thread_identity(20,"child");
    });
    auto& c=rt.cpu();c.gpr[29]=0x09FFF000u;c.gpr[31]=0x08804300u;
    psprecomp::set_runtime_thread_identity(10,"caller");rt.run(0x08804000u,32);
    if(imports!=1 || child_runs!=1 || c.gpr[2]!=43 || c.gpr[29]!=0x09FFF000u ||
       psprecomp::runtime_thread_uid()!=10 || rt.stop_reason()!="switch fixture returned")
       throw std::runtime_error("Compiled caller failed to yield and resume with its own stack/result");
    std::cout<<"PASS: native AOT call yields to another task and resumes without register/stack loss\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<"\n";return 1;}}
