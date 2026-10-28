#pragma once
// Image files for texture mods (platform layer): Windows decodes any format WIC
// reads; other hosts decode PNG with the port's own decoder (platform/png.hpp).
// Texels: R in the low byte, alpha in the high byte, straight alpha.
#include <cstdint>
#include <filesystem>
#include <stdexcept>
#include <vector>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <wincodec.h>
#pragma comment(lib,"windowscodecs.lib")
#pragma comment(lib,"ole32.lib")
#else
#include <fstream>
#include <iterator>
#include "png.hpp"
#endif
namespace kon::platform {
inline std::vector<std::uint32_t> decode_image_with_system(const std::filesystem::path& path,unsigned& width,unsigned& height){
#ifdef _WIN32
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
#else
    std::ifstream in(path,std::ios::binary);if(!in)throw std::runtime_error("Image open");
    const std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(in)),std::istreambuf_iterator<char>());
    return decode_png(bytes,width,height);  // PNG (and the dump BMP, read by the caller) on this host
#endif
}
}
