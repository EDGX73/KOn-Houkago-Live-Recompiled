#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <wincodec.h>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <stdexcept>
#include <vector>
#pragma comment(lib,"windowscodecs.lib")
#pragma comment(lib,"ole32.lib")
// PNG/BMP (any format WIC reads) to texels as the texture decoder stores them
// (R in the low byte, alpha in the high byte), straight alpha.
namespace kon {
// 32-bit uncompressed BMP as written by KON_TEXTURE_DUMP: read directly so
// its alpha survives (WIC treats it as opaque BGR).
inline bool read_dump_bmp(const std::filesystem::path& path,std::vector<std::uint32_t>& pixels,unsigned& width,unsigned& height){
    FILE* f=_wfopen(path.wstring().c_str(),L"rb");if(!f)return false;
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
    const HRESULT init=CoInitializeEx(nullptr,COINIT_MULTITHREADED);
    struct Uninit {bool on;~Uninit(){if(on)CoUninitialize();}} uninit{SUCCEEDED(init)};
    const auto ok=[](HRESULT hr,const char* what){if(FAILED(hr))throw std::runtime_error(what);};
    IWICImagingFactory* factory=nullptr;IWICBitmapDecoder* decoder=nullptr;IWICBitmapFrameDecode* frame=nullptr;IWICFormatConverter* converter=nullptr;
    struct Release {IUnknown** items[4];~Release(){for(auto** p:items)if(*p)(*p)->Release();}} release{{reinterpret_cast<IUnknown**>(&converter),reinterpret_cast<IUnknown**>(&frame),reinterpret_cast<IUnknown**>(&decoder),reinterpret_cast<IUnknown**>(&factory)}};
    ok(CoCreateInstance(CLSID_WICImagingFactory,nullptr,CLSCTX_INPROC_SERVER,IID_IWICImagingFactory,reinterpret_cast<void**>(&factory)),"WIC factory");
    ok(factory->CreateDecoderFromFilename(path.wstring().c_str(),nullptr,GENERIC_READ,WICDecodeMetadataCacheOnDemand,&decoder),"Image open");
    ok(decoder->GetFrame(0,&frame),"Image frame");
    ok(factory->CreateFormatConverter(&converter),"WIC converter");
    ok(converter->Initialize(frame,GUID_WICPixelFormat32bppRGBA,WICBitmapDitherTypeNone,nullptr,0.0,WICBitmapPaletteTypeCustom),"Image conversion");
    UINT w=0,h=0;ok(converter->GetSize(&w,&h),"Image size");
    if(!w || !h || w>4096 || h>4096)throw std::runtime_error("Image size out of range");
    std::vector<std::uint32_t> pixels(std::size_t(w)*h);
    ok(converter->CopyPixels(nullptr,w*4u,static_cast<UINT>(pixels.size()*4u),reinterpret_cast<BYTE*>(pixels.data())),"Image pixels");
    width=w;height=h;return pixels;
}
}
