// Writes the PT_LOAD bytes of a PSP PRX exactly as PSPRecomp relocates them when
// the game is loaded: the reference that tools/psp_prx_to_exec.py is compared with.
//   prx_image_dump <prx> <out.bin> [base, default 0x08804000]
#include "psprecomp/elf32.hpp"
#include "psprecomp/guest_memory.hpp"
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <vector>

int main(int argc, char **argv) {
    if (argc < 3) {
        std::cerr << "usage: prx_image_dump <prx> <out.bin> [base]\n";
        return 2;
    }
    try {
        const std::uint32_t base = argc > 3 ? static_cast<std::uint32_t>(std::strtoul(argv[3], nullptr, 0)) : 0x08804000u;
        const auto elf = psprecomp::Elf32Image::from_file(argv[1]);
        psprecomp::GuestMemory memory;
        const auto stats = elf.load_and_relocate(memory, base);
        std::ofstream out(argv[2], std::ios::binary);
        for (const auto &segment : elf.segments()) {
            if (segment.type != 1u) continue;
            std::vector<std::uint8_t> bytes(segment.file_size);
            memory.copy_out(elf.is_psp_prx() ? base + segment.vaddr : segment.vaddr, bytes);
            out.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        }
        std::cout << "relocations total=" << stats.total << " r32=" << stats.r_mips_32 << " r26=" << stats.r_mips_26
                  << " hi16=" << stats.r_mips_hi16 << " lo16=" << stats.r_mips_lo16 << " unsupported=" << stats.unsupported
                  << " invalid=" << stats.invalid << '\n';
        return stats.invalid || stats.unsupported ? 1 : 0;
    } catch (const std::exception &e) {
        std::cerr << "prx_image_dump: " << e.what() << '\n';
        return 1;
    }
}
