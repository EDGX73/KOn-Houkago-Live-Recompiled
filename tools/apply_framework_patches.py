"""Apply independently authored PSP ELF analysis fixes to the pinned framework."""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

MODULE_OLD = '''    if (section_it == sections_.end() || section_it->size < sizeof(ModuleInfo32)) return std::nullopt;
    const std::uint32_t addr = section_runtime_address(*section_it, load_base);
    if (!memory.contains(addr, sizeof(ModuleInfo32))) return std::nullopt;'''
MODULE_NEW = '''    // PSP retail PRX files may omit every section name. In that case the
    // first load header's p_paddr supplies the module-info FILE offset (its
    // high bit is the kernel-module marker). Translate through PT_LOAD,
    // rather than treating that file offset as a virtual address.
    std::optional<std::uint32_t> module_addr;
    if (section_it != sections_.end() && section_it->size >= sizeof(ModuleInfo32)) {
        module_addr = section_runtime_address(*section_it, load_base);
    } else if (is_psp_prx()) {
        const auto first_load = std::find_if(segments_.begin(), segments_.end(), [](const ElfSegment &segment) {
            return segment.type == kPtLoad;
        });
        if (first_load != segments_.end()) {
            const std::uint32_t file_offset = first_load->paddr & 0x7FFFFFFFu;
            for (std::size_t i = 0; i < segments_.size(); ++i) {
                const auto &segment = segments_[i];
                if (segment.type != kPtLoad || file_offset < segment.offset) continue;
                const std::uint64_t delta = static_cast<std::uint64_t>(file_offset) - segment.offset;
                if (delta + sizeof(ModuleInfo32) > segment.file_size) continue;
                const std::uint64_t address = static_cast<std::uint64_t>(segment_runtime_address(i, load_base)) + delta;
                if (address <= 0xFFFFFFFFu) module_addr = static_cast<std::uint32_t>(address);
                break;
            }
        }
    }
    if (!module_addr || !memory.contains(*module_addr, sizeof(ModuleInfo32))) return std::nullopt;
    const std::uint32_t addr = *module_addr;'''

RANGE_OLD = '''    std::vector<ExecutableRange> ranges;
    for (std::size_t i = 0; i < elf.segments().size(); ++i) {'''
RANGE_NEW = '''    std::vector<ExecutableRange> ranges;
    // PSP RWX load segments can contain code, strings, textures and tables.
    // Section flags retain code/data boundaries even when names are stripped.
    // Use allocated, executable, file-backed sections when available.
    for (const auto &section : elf.sections()) {
        if (section.type != 1u || (section.flags & 6u) != 6u || section.size == 0u) continue;
        const std::uint32_t start = elf.section_runtime_address(section, load_base);
        const std::uint64_t end64 = static_cast<std::uint64_t>(start) + section.size;
        if (end64 > std::numeric_limits<std::uint32_t>::max()) continue;
        ranges.push_back({start, static_cast<std::uint32_t>(end64)});
    }
    if (!ranges.empty()) {
        std::sort(ranges.begin(), ranges.end(), [](const auto &a, const auto &b) { return a.start < b.start; });
        return ranges;
    }
    for (std::size_t i = 0; i < elf.segments().size(); ++i) {'''

def replace_once(path, old, new):
    data = path.read_text(encoding='utf-8')
    if new in data:
        print('Already applied:', path.name)
        return
    if data.count(old) != 1:
        raise RuntimeError(f'Pinned source mismatch: {path}')
    path.write_text(data.replace(old, new, 1), encoding='utf-8', newline='\n')
    print('Applied:', path.name)

if __name__ == '__main__':
    source = ROOT / 'third_party/PSPRecomp/src'
    replace_once(source / 'elf32.cpp', MODULE_OLD, MODULE_NEW)
    replace_once(source / 'program_analysis.cpp', RANGE_OLD, RANGE_NEW)
