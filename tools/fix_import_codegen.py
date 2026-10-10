"""Preserve PSP import boundaries across local and cross-unit AOT calls."""
from apply_framework_patches import ROOT, replace_once
import re

IMPORT_LABEL_OLD = '''        body << "L_" << psprecomp::hex32(block_start).substr(2) << ":\\n";
        std::uint32_t pc = block_start;'''
IMPORT_LABEL_NEW = '''        body << "L_" << psprecomp::hex32(block_start).substr(2) << ":\\n";
        // A same-unit JAL/JALR can arrive here without leaving native code.
        // Import stubs must yield to the registered HLE wrapper, never execute
        // their on-disc placeholder JR RA and silently skip the service.
        if (function.import_stubs != nullptr && function.import_stubs->contains(block_start)) {
            body << "    ctx.pc = " << psprecomp::hex32(block_start) << "u;\\n"
                 << "    return;\\n";
            continue;
        }
        std::uint32_t pc = block_start;'''

def repair_existing_corpus(folder):
    """Apply the identical emitter change without rebuilding unchanged units."""
    registry = (folder / 'generated_registry.cpp').read_text(encoding='utf-8')
    stubs = set(re.findall(r'runtime.register_function\(0x([0-9A-Fa-f]{8})u, &import_', registry))
    if not stubs:
        raise RuntimeError('No authoritative generated import registry found')
    repaired = 0
    for path in sorted(folder.glob('generated_unit_*.cpp')):
        source = path.read_text(encoding='utf-8')
        for stub in stubs:
            label = 'L_' + stub.upper()
            pattern = rf'({label}:\n).*?(?=\nL_|\n\}})'
            replacement = rf'\g<1>    ctx.pc = 0x{stub.upper()}u;\n    return;'
            source, count = re.subn(pattern, replacement, source, flags=re.S)
            repaired += count
        if source != path.read_text(encoding='utf-8'):
            path.write_text(source, encoding='utf-8', newline='\n')
    print('Verified import labels in existing corpus:', repaired)

if __name__ == '__main__':
    replace_once(ROOT / 'third_party/PSPRecomp/tools/codegen_main.cpp',
        '''                            body << "    ctx.pc = " << psprecomp::hex32(target) << "u;\\n"
                                 << "    return;\\n";
                            continue;''',
        '''                            body << "    ctx.pc = " << psprecomp::hex32(target) << "u;\\n"
                                 << "    return;\\n";
                            // This guest block is complete. Continuing the emitter
                            // loop keeps pc unchanged and emits this call forever.
                            break;''')
    replace_once(ROOT / 'third_party/PSPRecomp/tools/codegen_main.cpp', IMPORT_LABEL_OLD, IMPORT_LABEL_NEW)
    corpus = ROOT / 'work/recompiled_corrected'
    if (corpus / 'generated_registry.cpp').exists():
        repair_existing_corpus(corpus)
