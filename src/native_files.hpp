#pragma once
#include "content_mount.hpp"
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <map>
#include <span>
#include <stdexcept>
namespace kon {
class NativeFiles {
public:
    static constexpr std::uint32_t bad_fd=0x80020323u,invalid=0x80010016u;
    struct File {HANDLE handle;std::uint32_t flags;std::filesystem::path path;bool disc;};
    std::map<std::uint32_t,File> files;
    explicit NativeFiles(const ContentMount& content,std::filesystem::path storage={})
        :content_(content),storage_(std::filesystem::weakly_canonical(storage.empty()?content.root()/"userdata/ms0":storage)) {
        const auto relative=storage_.lexically_relative(content.root());
        if(relative.empty() || *relative.begin()=="..")throw std::invalid_argument("Storage must stay inside project");
    }
    ~NativeFiles(){for(const auto& [id,f]:files)CloseHandle(f.handle);}
    NativeFiles(const NativeFiles&)=delete;
    NativeFiles& operator=(const NativeFiles&)=delete;
    static std::int64_t signed_error(std::uint32_t error){return static_cast<std::int32_t>(error);}
    std::filesystem::path resolve(std::string guest,bool& disc) const {
        std::replace(guest.begin(),guest.end(),'\\','/');
        const auto colon=guest.find(':');
        const auto device=colon==std::string::npos?"disc0":guest.substr(0,colon);
        if(device=="disc0" || device=="umd0") {
            disc=true;
            if(colon==std::string::npos)guest="disc0:/PSP_GAME/USRDIR/"+guest;
            return content_.resolve(guest);
        }
        if(device!="ms0" && device!="fatms0")throw std::invalid_argument("Unsupported file device: "+device);
        disc=false;guest.erase(0,colon+1);while(!guest.empty() && guest.front()=='/')guest.erase(guest.begin());
        std::filesystem::path relative;
        for(const auto& part:std::filesystem::path(guest)) {
            const auto name=part.string();
            if(name==".." || name.find(':')!=std::string::npos)throw std::invalid_argument("Unsafe storage path");
            if(name!=".")relative/=part;
        }
        const auto result=std::filesystem::weakly_canonical(storage_/relative);
        const auto within=result.lexically_relative(storage_);
        if(within.empty() || *within.begin()=="..")throw std::invalid_argument("Storage path escapes mount");
        return result;
    }
    std::uint32_t open(const std::string& guest,std::uint32_t flags) {
        // NBUF bypasses any port-side data cache: reads go straight to ReadFile.
        // Other scheduling/encryption flags need explicit implementations.
        if(flags&~0x4F03u)throw std::invalid_argument("Unsupported file flags");
        const auto mode=flags&3u;if(!mode)return invalid;
        bool disc=false;std::filesystem::path path;
        try{path=resolve(guest,disc);}catch(const std::runtime_error&){return 0x80010002u;}
        if(disc && (mode!=1 || flags&0xF00u))return 0x8001000Du;
        std::uint32_t id=3;while(id<64 && files.contains(id))++id;
        if(id==64)return 0x80020320u;
        const auto access=((mode&1)?GENERIC_READ:0)|((mode&2)?GENERIC_WRITE:0);
        DWORD creation=OPEN_EXISTING;
        if(flags&0x200u)creation=(flags&0x800u)?CREATE_NEW:(flags&0x400u)?CREATE_ALWAYS:OPEN_ALWAYS;
        else if(flags&0x400u)creation=TRUNCATE_EXISTING;
        const auto handle=CreateFileW(path.c_str(),access,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,
                                     nullptr,creation,FILE_ATTRIBUTE_NORMAL,nullptr);
        if(handle==INVALID_HANDLE_VALUE)return host_error();
        files.emplace(id,File{handle,flags,path,disc});return id;
    }
    std::uint32_t close(std::uint32_t id) {
        const auto it=files.find(id);if(it==files.end())return bad_fd;
        const auto ok=CloseHandle(it->second.handle);files.erase(it);return ok?0u:host_error();
    }
    std::int64_t size(std::uint32_t id) const {
        const auto it=files.find(id);if(it==files.end())return signed_error(bad_fd);
        LARGE_INTEGER size{};return GetFileSizeEx(it->second.handle,&size)?size.QuadPart:signed_error(host_error());
    }
    std::int64_t seek(std::uint32_t id,std::int64_t offset,std::uint32_t origin) {
        const auto it=files.find(id);if(it==files.end())return signed_error(bad_fd);
        if(origin>2)return signed_error(invalid);
        LARGE_INTEGER distance{};distance.QuadPart=offset;LARGE_INTEGER result{};
        return SetFilePointerEx(it->second.handle,distance,&result,origin)?result.QuadPart:signed_error(host_error());
    }
    std::uint32_t read(std::uint32_t id,std::span<std::uint8_t> data) {
        const auto it=files.find(id);if(it==files.end() || !(it->second.flags&1u))return bad_fd;
        if(data.size()>static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max()))return invalid;
        DWORD count=0;return ReadFile(it->second.handle,data.data(),static_cast<DWORD>(data.size()),&count,nullptr)?count:host_error();
    }
    std::uint32_t write(std::uint32_t id,std::span<const std::uint8_t> data) {
        const auto it=files.find(id);if(it==files.end() || !(it->second.flags&2u))return bad_fd;
        if(data.size()>static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max()))return invalid;
        if(it->second.flags&0x100u){const auto pos=seek(id,0,2);if(pos<0)return static_cast<std::uint32_t>(pos);}
        DWORD count=0;return WriteFile(it->second.handle,data.data(),static_cast<DWORD>(data.size()),&count,nullptr)?count:host_error();
    }
    std::uint32_t sync(std::uint32_t id) {
        const auto it=files.find(id);if(it==files.end())return bad_fd;
        if(!(it->second.flags&2u))return 0;
        return FlushFileBuffers(it->second.handle)?0u:host_error();
    }
    const std::filesystem::path& storage() const{return storage_;}
    // sceIoGetstat: the 0x58-byte SceIoStat (mode, attributes, size and
    // creation/access/modification times as ScePspDateTime in local time).
    std::uint32_t stat(const std::string& guest,std::array<std::uint8_t,0x58>& out) const {
        bool disc=false;std::filesystem::path path;
        try{path=resolve(guest,disc);}catch(const std::exception&){return 0x80010002u;}
        WIN32_FILE_ATTRIBUTE_DATA data{};
        if(!GetFileAttributesExW(path.c_str(),GetFileExInfoStandard,&data))return host_error();
        out.fill(0);const bool directory=data.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY;
        const auto put32=[&](std::size_t at,std::uint32_t v){for(unsigned n=0;n<4;++n)out[at+n]=static_cast<std::uint8_t>(v>>(8*n));};
        const auto put16=[&](std::size_t at,std::uint16_t v){out[at]=static_cast<std::uint8_t>(v);out[at+1]=static_cast<std::uint8_t>(v>>8);};
        put32(0,directory?0x11FFu:0x21FFu);put32(4,directory?0x10u:0x20u);
        put32(8,data.nFileSizeLow);put32(12,data.nFileSizeHigh);
        const auto put_time=[&](std::size_t at,FILETIME utc){
            FILETIME local{};SYSTEMTIME s{};
            if(!FileTimeToLocalFileTime(&utc,&local) || !FileTimeToSystemTime(&local,&s))return;
            put16(at,s.wYear);put16(at+2,s.wMonth);put16(at+4,s.wDay);put16(at+6,s.wHour);put16(at+8,s.wMinute);put16(at+10,s.wSecond);put32(at+12,s.wMilliseconds*1000u);
        };
        put_time(16,data.ftCreationTime);put_time(32,data.ftLastAccessTime);put_time(48,data.ftLastWriteTime);
        return 0;
    }
private:
    const ContentMount& content_;std::filesystem::path storage_;
    static std::uint32_t host_error() {
        switch(GetLastError()) {
        case ERROR_FILE_NOT_FOUND:case ERROR_PATH_NOT_FOUND:return 0x80010002u;
        case ERROR_ACCESS_DENIED:case ERROR_WRITE_PROTECT:return 0x8001000Du;
        case ERROR_FILE_EXISTS:case ERROR_ALREADY_EXISTS:return 0x80010011u;
        case ERROR_INVALID_PARAMETER:case ERROR_NEGATIVE_SEEK:return invalid;
        case ERROR_DISK_FULL:case ERROR_HANDLE_DISK_FULL:return 0x8001001Cu;
        default:return 0x80010005u;
        }
    }
};
}
