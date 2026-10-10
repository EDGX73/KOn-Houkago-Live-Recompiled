#pragma once
#include "psprecomp/runtime.hpp"
#include "psprecomp/common.hpp"
#include <cstdint>
namespace kon {
// A callback runs the real generated code on a separate frame of its owning
// task. Import handlers must refuse blocking operations while this bounded
// dispatcher is active; asynchronous callback frames will be added separately.
struct CallbackResult {bool completed;std::uint32_t value;};
inline CallbackResult run_native_callback(psprecomp::Runtime& rt,psprecomp::AllegrexContext& cpu,
        std::uint32_t address,std::uint32_t count,std::uint32_t argument,std::uint32_t common,
        std::uint32_t stack_floor=0) {
    constexpr std::uint32_t callback_return=0x08000140u;
    const auto saved=cpu;
    const auto token=psprecomp::capture_runtime_execution_context();
    if(saved.gpr[29]<stack_floor+0x200u || !rt.memory().contains(saved.gpr[29]-0x200u,0x200u)) {
        rt.stop("Callback stack frame outside owner stack");return {false,0};
    }
    cpu.gpr[29]=(saved.gpr[29]-0x100u)&~15u;
    cpu.gpr[31]=callback_return;cpu.gpr[4]=count;cpu.gpr[5]=argument;cpu.gpr[6]=common;cpu.pc=address;
    for(unsigned dispatch=0;dispatch<10000u && !rt.stopped();++dispatch) {
        if(cpu.pc==callback_return) {
            const auto result=cpu.gpr[2];cpu=saved;return {true,result};
        }
        if(!psprecomp::runtime_execution_context_matches(token)) {
            rt.stop("Callback task changed while isolated frame was active");break;
        }
        if(!rt.invoke_isolated_aot(cpu.pc,cpu)) {
            rt.stop("Missing native callback entry "+psprecomp::hex32(cpu.pc));break;
        }
    }
    if(!rt.stopped())rt.stop("Native callback dispatch budget exhausted");
    // On failure leave its actual PC/registers available to the diagnostic.
    return {false,0};
}
// GE handlers are interrupts, not thread callbacks. They must have a separate
// stack even when interrupting a task whose user stack is only 512 bytes.
inline CallbackResult run_native_interrupt(psprecomp::Runtime& rt,psprecomp::AllegrexContext& cpu,
        std::uint32_t address,std::uint32_t first,std::uint32_t second,std::uint32_t third,
        std::uint32_t stack_base,std::uint32_t stack_size) {
    if(stack_size<1024 || !rt.memory().contains(stack_base,stack_size)){
        rt.stop("Invalid native interrupt stack");return {false,0};
    }
    const auto interrupted=cpu;cpu.gpr[29]=(stack_base+stack_size-16u)&~15u;
    const auto result=run_native_callback(rt,cpu,address,first,second,third,stack_base);
    if(result.completed)cpu=interrupted;
    return result;
}
}
