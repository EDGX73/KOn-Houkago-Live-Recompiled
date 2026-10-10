"""Provide the real 16 KiB Allegrex scratchpad used by the original game.

Independently authored storage and range checks. Hardware addresses verified in
https://uofw.github.io/upspd/docs/hardware/psp_doc.pdf (memory map) and
https://raw.githubusercontent.com/hrydgard/ppsspp/master/Core/MemMap.h (constants).
No CPU fallback is introduced: generated native loads use the existing slow path.
"""
from apply_framework_patches import ROOT, replace_once

if __name__ == '__main__':
    header=ROOT/'third_party/PSPRecomp/include/psprecomp/guest_memory.hpp'
    source=ROOT/'third_party/PSPRecomp/src/guest_memory.cpp'
    replace_once(header,
        '    static constexpr std::uint32_t kPhysicalBase = 0x08000000u;\n',
        '    static constexpr std::uint32_t kPhysicalBase = 0x08000000u;\n'
        '    static constexpr std::uint32_t kScratchpadBase = 0x00010000u;\n'
        '    static constexpr std::uint32_t kScratchpadSize = 0x00004000u;\n')
    replace_once(header,'    enum class Region { Vram, Ram };\n',
        '    enum class Region { Vram, Ram, Scratchpad };\n')
    replace_once(header,'    bool write_watch_enabled_{};\n};\n',
        '    bool write_watch_enabled_{};\n'
        '    std::vector<std::uint8_t> scratchpad_;\n};\n')
    replace_once(source,
        'write_watch_enabled_(std::getenv("PSPRECOMP_WATCH_WRITE") != nullptr) {\n',
        'write_watch_enabled_(std::getenv("PSPRECOMP_WATCH_WRITE") != nullptr), scratchpad_(kScratchpadSize, 0u) {\n')
    replace_once(source,
        '    const std::uint32_t c = canonical(address);\n'
        '    const std::uint64_t end = static_cast<std::uint64_t>(c) + static_cast<std::uint64_t>(length);\n',
        '    const std::uint32_t c = canonical(address);\n'
        '    if (c >= kScratchpadBase && c < kScratchpadBase + kScratchpadSize &&\n'
        '        length <= kScratchpadSize - (c - kScratchpadBase)) return true;\n'
        '    const std::uint64_t end = static_cast<std::uint64_t>(c) + static_cast<std::uint64_t>(length);\n')
    replace_once(source,
        '    if (is_vram_window(c))\n'
        '        return {Region::Vram, vram_offset(c)};\n',
        '    if (c >= kScratchpadBase && c < kScratchpadBase + kScratchpadSize)\n'
        '        return {Region::Scratchpad, static_cast<std::size_t>(c - kScratchpadBase)};\n'
        '    if (is_vram_window(c))\n'
        '        return {Region::Vram, vram_offset(c)};\n')
    for prefix in ('const std::vector<std::uint8_t> &', 'std::vector<std::uint8_t> &'):
        old = prefix + 'GuestMemory::region_bytes(Region region)' + (' const' if prefix.startswith('const') else '') + ' noexcept {\n    return region == Region::Vram ? vram_ : bytes_;\n}'
        new = old.replace('    return region', '    if (region == Region::Scratchpad) return scratchpad_;\n    return region')
        replace_once(source, old, new)
    replace_once(source,'    if (c < kPhysicalBase) return nullptr;\n',
        '    if (c >= kScratchpadBase && c < kScratchpadBase + kScratchpadSize) {\n'
        '        const auto offset = static_cast<std::size_t>(c - kScratchpadBase);\n'
        '        return length <= scratchpad_.size() - offset ? scratchpad_.data() + offset : nullptr;\n'
        '    }\n'
        '    if (c < kPhysicalBase) return nullptr;\n')
