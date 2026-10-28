#pragma once
#include "platform/image.hpp"
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <stdexcept>
#include <vector>
// Image files of texture mods to texels as the texture decoder stores them
// (R in the low byte, alpha in the high byte), straight alpha. The system
// decoder (WIC on Windows, the port's PNG decoder elsewhere) is in platform/image.hpp.
namespace kon {
// 32-bit uncompressed BMP as written by KON_TEXTURE_DUMP: read directly so
// its alpha survives (WIC treats it as opaque BGR).
inline bool read_dump_bmp(const std::filesystem::path& path,std::vector<std::uint32_t>& pixels,unsigned& width,unsigned& height){
#ifdef _WIN32
    FILE* f=_wfopen(path.wstring().c_str(),L"rb");if(!f)return false;
#else
    FILE* f=std::fopen(path.c_str(),"rb");if(!f)return false;
#endif
    std::uint8_t header[54]{};const bool head=std::fread(header,1,54,f)==54;
    const auto u32=[&](int o){return std::uint32_t(header[o])|(std::uint32_t(header[o+1])<<8)|(std::uint32_t(header[o+2])<<16)|(std::uint32_t(header[o+3])<<24);};
    if(!head || header[0]!='B' || header[1]!='M' || u32(10)!=54 || u32(14)!=40 || (u32(28)&0xFFFF)!=32 || u32(30)!=0){std::fclose(f);return false;}
    const auto w=std::int32_t(u32(18)),h=std::int32_t(u32(22));const unsigned aw=unsigned(w),ah=unsigned(h<0?-h:h);
    if(w<=0 || !ah || aw>4096 || ah>4096){std::fclose(f);return false;}
    std::vector<std::uint8_t> data(std::size_t(aw)*ah*4u);const bool body=std::fread(data.data(),1,data.size(),f)==data.size();std::fclose(f);
    if(!body)return false;
    pixels.resize(std::size_t(aw)*ah);
    for(unsigned y=0;y<ah;++y){const unsigned row=h<0?y:ah-1-y;
        for(unsigned x=0;x<aw;++x){const auto* p=&data[(std::size_t(row)*aw+x)*4u];pixels[std::size_t(y)*aw+x]=std::uint32_t(p[2])|(std::uint32_t(p[1])<<8)|(std::uint32_t(p[0])<<16)|(std::uint32_t(p[3])<<24);}}
    width=aw;height=ah;return true;
}
inline std::vector<std::uint32_t> decode_image_file(const std::filesystem::path& path,unsigned& width,unsigned& height){
    if(std::vector<std::uint32_t> pixels;read_dump_bmp(path,pixels,width,height))return pixels;
    return platform::decode_image_with_system(path,width,height);
}
}
