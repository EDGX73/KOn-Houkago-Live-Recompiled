"""Game-required Allegrex additions and observable code generation.

Independently written from architectural semantics; PPSSPP tables/interpreter
are references, not copied implementations. Pinned upstream source is checked.
"""
from pathlib import Path
from apply_framework_patches import ROOT, replace_once

if __name__ == '__main__':
    source = ROOT / 'third_party/PSPRecomp'
    replace_once(source / 'include/psprecomp/decoder.hpp',
                 '    Mult, Multu, Div, Divu,',
                 '    Mult, Multu, Div, Divu, Madd, Break,')
    replace_once(source / 'src/decoder.cpp',
                 '        case 0x0F: d.kind = OpcodeKind::Sync;',
                 '        case 0x0D: d.kind = OpcodeKind::Break; d.mnemonic = "break"; break;\n        case 0x0F: d.kind = OpcodeKind::Sync;')
    replace_once(source / 'src/decoder.cpp',
                 '        case 0x20: d.kind = OpcodeKind::Add;',
                 '        case 0x1C: d.kind = OpcodeKind::Madd; d.mnemonic = "madd"; break;\n        case 0x20: d.kind = OpcodeKind::Add;')
    replace_once(source / 'include/psprecomp/allegrex_context.hpp',
                 '    std::uint32_t pc{};',
                 '''    // Signed 32-bit product added to the 64-bit HI:LO accumulator.
    // Unsigned addition defines the required modulo-2^64 result even at overflow.
    PSPRECOMP_CONTEXT_FORCEINLINE void execute_madd(std::uint32_t rs, std::uint32_t rt) noexcept {
        const auto a = std::bit_cast<std::int32_t>(rs == 0u ? 0u : gpr[rs]);
        const auto b = std::bit_cast<std::int32_t>(rt == 0u ? 0u : gpr[rt]);
        const auto product = static_cast<std::int64_t>(a) * static_cast<std::int64_t>(b);
        const auto accumulator = (static_cast<std::uint64_t>(hi) << 32u) | lo;
        const auto result = accumulator + static_cast<std::uint64_t>(product);
        hi = static_cast<std::uint32_t>(result >> 32u);
        lo = static_cast<std::uint32_t>(result);
    }
    std::uint32_t pc{};''')
    replace_once(source / 'tools/codegen_main.cpp',
                 '    case psprecomp::OpcodeKind::Mult:\n',
                 '''    case psprecomp::OpcodeKind::Madd:
        out << "    ctx.execute_madd(" << d.rs << "u, " << d.rt << "u);\\n";
        break;
    case psprecomp::OpcodeKind::Break:
        out << "    ctx.pc = " << psprecomp::hex32(pc) << "u; rt.stop(\\"Allegrex BREAK code "
            << ((d.word >> 6u) & 0xFFFFFu) << " at " << psprecomp::hex32(pc) << "\\"); return;\\n";
        break;
    case psprecomp::OpcodeKind::Mult:
''')
    replace_once(source / 'tools/codegen_main.cpp',
                 '    const auto program = psprecomp::analyze_program(elf, memory, load_base);',
                 '''    std::cerr << "Analyzing relocated executable..." << std::endl;
    const auto program = psprecomp::analyze_program(elf, memory, load_base);
    std::cerr << "Analysis complete: " << program.covered_labels.size() << " unique instructions" << std::endl;''')
    replace_once(source / 'tools/codegen_main.cpp',
                 '        out << lower_constant_vfpu_accesses(\n',
                 '''        std::cerr << "Emitting unit " << unit.bucket << " (" << unit.instructions.size() << " instructions)..." << std::endl;
        if (std::getenv("PSPRECOMP_PLAIN_AUTO") != nullptr) {
            out << emit_function_source(generated_unit, memory, generated_unit.name);
        } else out << lower_constant_vfpu_accesses(
''')
    replace_once(source / 'tools/codegen_main.cpp',
                 '        rewritten_units += write_text_if_changed(path, out.str()) ? 1u : 0u;',
                 '''        rewritten_units += write_text_if_changed(path, out.str()) ? 1u : 0u;
        std::cerr << "Unit " << unit.bucket << " complete" << std::endl;''')
