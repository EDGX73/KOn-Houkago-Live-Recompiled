#include "psprecomp/runtime.hpp"
#include <iostream>
#include <stdexcept>
static void returned(psprecomp::Runtime& rt,psprecomp::AllegrexContext&){rt.stop("import fixture returned");}
int main(){try {
    for(const auto indirect:{0u,1u}) {
    psprecomp::Runtime rt;psprecomp::register_generated_functions(rt);
    unsigned calls=0;
    rt.register_hle("ProbeLib",0x12345678u,[&](auto&,auto& ctx){++calls;ctx.set_gpr(2,ctx.gpr[4]+1u);});
    rt.register_function(0x08804300u,returned,"fixture return");
    auto& c=rt.cpu();c.gpr[29]=0x09FFF000u;c.gpr[31]=0x08804300u;c.gpr[5]=indirect;
    rt.run(0x08804000u,32);
    if(calls!=1 || c.gpr[2]!=43 || c.gpr[29]!=0x09FFF000u || rt.stop_reason()!="import fixture returned")
        throw std::runtime_error("Automatic import call corrupted result, stack or return control");
    }
    std::cout<<"PASS: compiled direct/indirect import calls preserve result, stack and return\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
