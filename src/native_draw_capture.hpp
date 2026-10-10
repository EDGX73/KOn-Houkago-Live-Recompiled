#pragma once
#include "ge_queue.hpp"
#include <filesystem>
#include <fstream>
#include <type_traits>
namespace kon {
// Private development snapshot, tied to this Windows GeState ABI. It is not a
// gameplay save or proof that the original game reached a playable scene.
struct NativeDrawCapture {
    std::uint32_t pc=0,kind=0,count=0;GeState state;
    static void write(const std::filesystem::path& path,const psprecomp::GuestMemory& memory,const GeState& state,unsigned pc,unsigned kind,unsigned count){
        static_assert(std::is_trivially_copyable_v<GeState>);
        std::ofstream out(path,std::ios::binary);const std::array<std::uint32_t,8> header{0x444E4F4Bu,1,sizeof(GeState),memory.size(),memory.vram_size(),pc,kind,count};
        out.write(reinterpret_cast<const char*>(header.data()),sizeof(header));out.write(reinterpret_cast<const char*>(&state),sizeof(state));
        out.write(reinterpret_cast<const char*>(memory.bytes().data()),memory.bytes().size());out.write(reinterpret_cast<const char*>(memory.vram_bytes().data()),memory.vram_bytes().size());
        if(!out)throw std::runtime_error("Cannot record native draw snapshot");
    }
    static NativeDrawCapture read(const std::filesystem::path& path,psprecomp::GuestMemory& memory){
        std::ifstream in(path,std::ios::binary);std::array<std::uint32_t,8> header{};
        if(!in.read(reinterpret_cast<char*>(header.data()),sizeof(header)) || header[0]!=0x444E4F4Bu || header[1]!=1 || header[2]!=sizeof(GeState) || header[3]!=memory.size() || header[4]!=memory.vram_size())throw std::runtime_error("Native draw snapshot version/size mismatch");
        NativeDrawCapture capture{header[5],header[6],header[7]};
        if(!in.read(reinterpret_cast<char*>(&capture.state),sizeof(capture.state)))throw std::runtime_error("Truncated native draw state");
        std::vector<std::uint8_t> ram(memory.size()),vram(memory.vram_size());
        if(!in.read(reinterpret_cast<char*>(ram.data()),ram.size()) || !in.read(reinterpret_cast<char*>(vram.data()),vram.size()) || in.peek()!=std::char_traits<char>::eof())throw std::runtime_error("Native draw memory snapshot incomplete");
        memory.copy_in(psprecomp::GuestMemory::kPhysicalBase,ram);memory.copy_in(psprecomp::GuestMemory::kVramPhysicalBase,vram);return capture;
    }
};
}
