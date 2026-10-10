"""BREAK is a trap and can occur in a branch delay slot, including divide checks."""
from apply_framework_patches import ROOT, replace_once
if __name__ == '__main__':
    replace_once(ROOT/'third_party/PSPRecomp/src/decoder.cpp',
        '\n    case OpcodeKind::Jr: case OpcodeKind::Jalr: case OpcodeKind::Syscall: case OpcodeKind::Break:\n',
        '\n    case OpcodeKind::Jr: case OpcodeKind::Jalr: case OpcodeKind::Syscall:\n')
