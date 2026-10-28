#pragma once
// Dynamic libraries (the Vulkan loader, FFmpeg): LoadLibraryExW/GetProcAddress on
// Windows, dlopen/dlsym elsewhere. Part of the platform layer (docs/ANDROID.md).
#include <filesystem>
#include <string>
#include <utility>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#endif
namespace kon::platform {
class DynamicLibrary {
public:
    // Where Windows looks: System32 only (system components such as the Vulkan
    // loader), or the library's own folder and then the default directories.
    enum class Search {System,LibraryFolder};
    DynamicLibrary()=default;
    DynamicLibrary(const std::filesystem::path& path,Search search){open(path,search);}
    ~DynamicLibrary(){close();}
    DynamicLibrary(const DynamicLibrary&)=delete;DynamicLibrary& operator=(const DynamicLibrary&)=delete;
    DynamicLibrary(DynamicLibrary&& other) noexcept:handle_(std::exchange(other.handle_,nullptr)),error_(std::move(other.error_)){}
    DynamicLibrary& operator=(DynamicLibrary&& other) noexcept{if(this!=&other){close();handle_=std::exchange(other.handle_,nullptr);error_=std::move(other.error_);}return *this;}
    bool open(const std::filesystem::path& path,Search search){
        close();
#ifdef _WIN32
        handle_=LoadLibraryExW(path.c_str(),nullptr,search==Search::System?LOAD_LIBRARY_SEARCH_SYSTEM32:LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        if(!handle_)error_="Windows error="+std::to_string(GetLastError());
#else
        (void)search;  // the dynamic linker's own search order
        handle_=dlopen(path.c_str(),RTLD_NOW|RTLD_LOCAL);
        if(!handle_){const char* e=dlerror();error_=e?e:"dlopen failed";}
#endif
        return handle_!=nullptr;
    }
    void close(){
#ifdef _WIN32
        if(handle_)FreeLibrary(static_cast<HMODULE>(handle_));
#else
        if(handle_)dlclose(handle_);
#endif
        handle_=nullptr;
    }
    explicit operator bool() const{return handle_!=nullptr;}
    const std::string& error() const{return error_;}
    // nullptr when the library does not export it.
    template<class T>T symbol(const char* name) const {
        if(!handle_)return nullptr;
#ifdef _WIN32
        return reinterpret_cast<T>(GetProcAddress(static_cast<HMODULE>(handle_),name));
#else
        return reinterpret_cast<T>(dlsym(handle_,name));
#endif
    }
private:
    void* handle_=nullptr;std::string error_;
};
// The system's Vulkan loader: vulkan-1.dll from System32, libvulkan.so elsewhere.
inline DynamicLibrary open_vulkan_loader(){
    DynamicLibrary library;
#ifdef _WIN32
    library.open(L"vulkan-1.dll",DynamicLibrary::Search::System);
#else
    if(!library.open("libvulkan.so.1",DynamicLibrary::Search::System))library.open("libvulkan.so",DynamicLibrary::Search::System);
#endif
    return library;
}
}
