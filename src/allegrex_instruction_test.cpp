#include "psprecomp/runtime.hpp"
#include "psprecomp/decoder.hpp"
#include <array>
#include <cstdint>
#include <iostream>
#include <stdexcept>
static void require(bool condition, const char* reason) {
    if (!condition) throw std::runtime_error(reason);
}
static void returned(psprecomp::Runtime& rt, psprecomp::AllegrexContext&) { rt.stop("fixture returned"); }
int main() {
    try {
        using namespace psprecomp;
        require(decode_allegrex(0x010B001Cu).kind == OpcodeKind::Madd, "actual game MADD undecoded");
        require(decode_allegrex(0x0000000Du).kind == OpcodeKind::Break, "actual game BREAK undecoded");
        require(!decode_allegrex(0xDu).has_delay_slot(), "BREAK must not execute a delay slot");
        Runtime rt;
        register_generated_functions(rt);
        rt.register_function(0x08804200u, returned, "fixture return");
        struct Case { std::uint32_t a,b,hi,lo,want_hi,want_lo; };
        const std::array<Case,6> cases{{
            {3,7,0,2,0,23}, {0xFFFFFFFEu,7,0,0,0xFFFFFFFFu,0xFFFFFFF2u},
            {1,1,0xFFFFFFFFu,0xFFFFFFFFu,0,0},
            {1,1,0x7FFFFFFFu,0xFFFFFFFFu,0x80000000u,0},
            {0x80000000u,0x80000000u,0,0,0x40000000u,0},
            {0x80000000u,0xFFFFFFFFu,0,0,0,0x80000000u}
        }};
        for (const auto& c: cases) {
            auto& ctx=rt.cpu(); ctx={}; ctx.gpr[4]=c.a; ctx.gpr[5]=c.b;
            ctx.hi=c.hi; ctx.lo=c.lo; ctx.gpr[31]=0x08804200u;
            rt.run(0x08804000u,8);
            require(rt.stop_reason()=="fixture returned", "compiled MADD did not return");
            require(ctx.hi==c.want_hi && ctx.lo==c.want_lo, "compiled MADD arithmetic mismatch");
        }
        auto& ctx=rt.cpu(); ctx={}; ctx.gpr[2]=0xBADCAFEu;
        rt.run(0x08804100u,8);
        require(ctx.pc==0x08804100u, "BREAK PC lost");
        require(ctx.gpr[2]==0xBADCAFEu, "BREAK modified result register");
        require(rt.stop_reason()=="Allegrex BREAK code 74565 at 0x08804100", "BREAK stop/code mismatch");
        ctx={};ctx.gpr[4]=1;ctx.gpr[31]=0x08804200u;
        rt.run(0x08804120u,8);
        require(rt.stop_reason()=="fixture returned", "Untaken likely branch executed its BREAK delay slot");
        ctx={};ctx.gpr[4]=0;ctx.gpr[31]=0x08804200u;
        rt.run(0x08804120u,8);
        require(ctx.pc==0x08804124u && rt.stop_reason()=="Allegrex BREAK code 7 at 0x08804124",
                "Taken likely branch did not trap at its BREAK delay slot");
        std::cout << "PASS: 6 compiled MADD edge cases and BREAK trap with exact PC/code\n";
        return 0;
    } catch(const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
