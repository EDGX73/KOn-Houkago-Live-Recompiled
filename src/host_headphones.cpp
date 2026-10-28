#include "host_headphones.hpp"
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmdeviceapi.h>
#include <memory>
#include <stdexcept>
#include <string>
namespace kon {
namespace {
void checked(HRESULT result,const char* operation){
    if(FAILED(result))throw std::runtime_error(std::string(operation)+" HRESULT="+std::to_string(static_cast<unsigned>(result)));
}
template<class T>struct Release {void operator()(T* pointer)const{if(pointer)pointer->Release();}};
struct Apartment {
    HRESULT result=CoInitializeEx(nullptr,COINIT_MULTITHREADED);
    Apartment(){if(result!=RPC_E_CHANGED_MODE)checked(result,"Initialize Core Audio query");}
    ~Apartment(){if(SUCCEEDED(result))CoUninitialize();}
};
}
HostHeadphoneState query_host_headphones(){
    Apartment apartment;IMMDeviceEnumerator* raw_enumerator=nullptr;
    checked(CoCreateInstance(__uuidof(MMDeviceEnumerator),nullptr,CLSCTX_INPROC_SERVER,
        __uuidof(IMMDeviceEnumerator),reinterpret_cast<void**>(&raw_enumerator)),"Create Core Audio enumerator");
    std::unique_ptr<IMMDeviceEnumerator,Release<IMMDeviceEnumerator>> enumerator(raw_enumerator);
    IMMDevice* raw_device=nullptr;
    const auto found=enumerator->GetDefaultAudioEndpoint(eRender,eConsole,&raw_device);
    if(found==HRESULT_FROM_WIN32(ERROR_NOT_FOUND))return {};
    checked(found,"Query default Windows playback endpoint");
    std::unique_ptr<IMMDevice,Release<IMMDevice>> device(raw_device);
    IPropertyStore* raw_properties=nullptr;
    checked(device->OpenPropertyStore(STGM_READ,&raw_properties),"Read Windows playback properties");
    std::unique_ptr<IPropertyStore,Release<IPropertyStore>> properties(raw_properties);
    // SDK PKEY_AudioEndpoint_FormFactor; copied identifier, not implementation.
    const PROPERTYKEY form_key{{0x1DA5D803,0xD492,0x4EDD,{0x8C,0x23,0xE0,0xC0,0xFF,0xEE,0x7F,0x0E}},0};
    PROPVARIANT value{};const auto read=properties->GetValue(form_key,&value);
    const unsigned form=value.vt==VT_UI4?value.uintVal:static_cast<unsigned>(UnknownFormFactor);
    PropVariantClear(&value);checked(read,"Read Windows endpoint form factor");
    return {headphone_form_factor(form),true,form};
}
}
#else
// Elsewhere the playback route is not known yet (Android: AudioManager through
// JNI, a later step): report no endpoint, as Windows does without one.
namespace kon {
HostHeadphoneState query_host_headphones(){return {};}
}
#endif
