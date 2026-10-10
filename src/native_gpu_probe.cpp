// Read-only graphics-device capabilities for the forthcoming native backend.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <fstream>
#include <iostream>
#include <string>
template<class T>struct ComOwner {T* value=nullptr;~ComOwner(){if(value)value->Release();}T** out(){return &value;}};
std::string utf8(const wchar_t* text){const int size=WideCharToMultiByte(CP_UTF8,0,text,-1,nullptr,0,nullptr,nullptr);if(!size)throw std::runtime_error("Cannot decode adapter name");std::string output(size,'\0');WideCharToMultiByte(CP_UTF8,0,text,-1,output.data(),size,nullptr,nullptr);output.pop_back();return output;}
std::string quoted(const std::string& text){std::string out="\"";for(unsigned char c:text){if(c=='"' || c=='\\'){out+='\\';out+=c;}else if(c<32)throw std::runtime_error("Unexpected control character in adapter name");else out+=c;}return out+'"';}
int main(int argc,char** argv){try{
    if(argc!=2)throw std::runtime_error("Usage: native_gpu_probe output.json");
    ComOwner<IDXGIFactory1> factory;auto result=CreateDXGIFactory1(IID_IDXGIFactory1,reinterpret_cast<void**>(factory.out()));
    if(FAILED(result))throw std::runtime_error("Cannot enumerate native Windows graphics adapters");
    std::ofstream out(argv[1]);if(!out)throw std::runtime_error("Cannot write graphics capability report");
    out<<std::boolalpha<<"{\"native_gpu_rendering_integrated\":false,\"adapters\":[";unsigned count=0,devices=0;
    for(unsigned index=0;;++index){
        ComOwner<IDXGIAdapter1> adapter;result=factory.value->EnumAdapters1(index,adapter.out());if(result==DXGI_ERROR_NOT_FOUND)break;
        if(FAILED(result))throw std::runtime_error("Cannot read graphics adapter");DXGI_ADAPTER_DESC1 desc{};if(FAILED(adapter.value->GetDesc1(&desc)))throw std::runtime_error("Cannot describe graphics adapter");
        ComOwner<ID3D11Device> device;D3D_FEATURE_LEVEL level{};const D3D_FEATURE_LEVEL levels[]={D3D_FEATURE_LEVEL_11_1,D3D_FEATURE_LEVEL_11_0};
        result=D3D11CreateDevice(adapter.value,D3D_DRIVER_TYPE_UNKNOWN,nullptr,0,levels,2,D3D11_SDK_VERSION,device.out(),&level,nullptr);
        if(result==E_INVALIDARG)result=D3D11CreateDevice(adapter.value,D3D_DRIVER_TYPE_UNKNOWN,nullptr,0,levels+1,1,D3D11_SDK_VERSION,device.out(),&level,nullptr);
        const bool created=SUCCEEDED(result);if(created)++devices;
        D3D11_FEATURE_DATA_DOUBLES doubles{};D3D11_FEATURE_DATA_D3D11_OPTIONS options{};
        const bool doubles_query=created && SUCCEEDED(device.value->CheckFeatureSupport(D3D11_FEATURE_DOUBLES,&doubles,sizeof(doubles)));
        const bool options_query=created && SUCCEEDED(device.value->CheckFeatureSupport(D3D11_FEATURE_D3D11_OPTIONS,&options,sizeof(options)));
        if(count++)out<<',';
        out<<"{\"index\":"<<index<<",\"name\":"<<quoted(utf8(desc.Description))<<",\"dedicated_video_bytes\":"<<desc.DedicatedVideoMemory
           <<",\"software_adapter\":"<<((desc.Flags&DXGI_ADAPTER_FLAG_SOFTWARE)!=0)<<",\"d3d11_device_created\":"<<created<<",\"feature_level\":"<<static_cast<unsigned>(level)
           <<",\"double_precision_query_succeeded\":"<<doubles_query<<",\"double_precision\":"<<static_cast<bool>(doubles.DoublePrecisionFloatShaderOps)
           <<",\"extended_doubles_query_succeeded\":"<<options_query<<",\"extended_doubles\":"<<static_cast<bool>(options.ExtendedDoublesShaderInstructions)<<"}";
    }
    out<<"],\"devices_created\":"<<devices<<",\"game_fps_verified\":false}\n";if(!out)throw std::runtime_error("Cannot save GPU capability report");
    std::cout<<"Enumerated "<<count<<" adapters; "<<devices<<" real Direct3D devices created. Game GPU rendering is not integrated by this probe.\n";return devices?0:1;
}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}}
