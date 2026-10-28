#pragma once
#include "content_mount.hpp"
#include "platform/file.hpp"
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
    struct File {platform::File file;std::uint32_t flags;std::filesystem::path path;bool disc;};
    std::map<std::uint32_t,File> files;  // each platform::File closes itself
    explicit NativeFiles(const ContentMount& content,std::filesystem::path storage={})
        :content_(content),storage_(std::filesystem::weakly_canonical(storage.empty()?content.root()/"userdata/ms0":storage)) {
        const auto relative=storage_.lexically_relative(content.root());
        if(relative.empty() || *relative.begin()=="..")throw std::invalid_argument("Storage must stay inside project");
    }
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
        // NBUF bypasses any port-side data cache: reads go straight to the host.
        // Other scheduling/encryption flags need explicit implementations.
        if(flags&~0x4F03u)throw std::invalid_argument("Unsupported file flags");
        const auto mode=flags&3u;if(!mode)return invalid;
        bool disc=false;std::filesystem::path path;
        try{path=resolve(guest,disc);}catch(const std::runtime_error&){return 0x80010002u;}
        if(disc && (mode!=1 || flags&0xF00u))return 0x8001000Du;
        std::uint32_t id=3;while(id<64 && files.contains(id))++id;
        if(id==64)return 0x80020320u;
        using platform::Disposition;
        auto creation=Disposition::OpenExisting;
        if(flags&0x200u)creation=(flags&0x800u)?Disposition::CreateNew:(flags&0x400u)?Disposition::CreateAlways:Disposition::OpenAlways;
        else if(flags&0x400u)creation=Disposition::TruncateExisting;
        platform::File file;
        if(const auto error=file.open(path,mode&1u,mode&2u,creation);error!=platform::FileError::None)return psp_error(error);
        files.emplace(id,File{std::move(file),flags,path,disc});return id;
    }
    std::uint32_t close(std::uint32_t id) {
        const auto it=files.find(id);if(it==files.end())return bad_fd;
        const auto error=it->second.file.close();files.erase(it);return ok(error)?0u:psp_error(error);
    }
    std::int64_t size(std::uint32_t id) const {
        const auto it=files.find(id);if(it==files.end())return signed_error(bad_fd);
        std::int64_t size=0;const auto error=it->second.file.size(size);return ok(error)?size:signed_error(psp_error(error));
    }
    std::int64_t seek(std::uint32_t id,std::int64_t offset,std::uint32_t origin) {
        const auto it=files.find(id);if(it==files.end())return signed_error(bad_fd);
        if(origin>2)return signed_error(invalid);
        std::int64_t position=0;const auto error=it->second.file.seek(offset,origin,position);
        return ok(error)?position:signed_error(psp_error(error));
    }
    std::uint32_t read(std::uint32_t id,std::span<std::uint8_t> data) {
        const auto it=files.find(id);if(it==files.end() || !(it->second.flags&1u))return bad_fd;
        if(data.size()>static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max()))return invalid;
        std::uint32_t count=0;const auto error=it->second.file.read(data,count);return ok(error)?count:psp_error(error);
    }
    std::uint32_t write(std::uint32_t id,std::span<const std::uint8_t> data) {
        const auto it=files.find(id);if(it==files.end() || !(it->second.flags&2u))return bad_fd;
        if(data.size()>static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max()))return invalid;
        if(it->second.flags&0x100u){const auto pos=seek(id,0,2);if(pos<0)return static_cast<std::uint32_t>(pos);}
        std::uint32_t count=0;const auto error=it->second.file.write(data,count);return ok(error)?count:psp_error(error);
    }
    std::uint32_t sync(std::uint32_t id) {
        const auto it=files.find(id);if(it==files.end())return bad_fd;
        if(!(it->second.flags&2u))return 0;
        const auto error=it->second.file.flush();return ok(error)?0u:psp_error(error);
    }
    const std::filesystem::path& storage() const{return storage_;}
    // sceIoGetstat: the 0x58-byte SceIoStat (mode, attributes, size and
    // creation/access/modification times as ScePspDateTime in local time).
    std::uint32_t stat(const std::string& guest,std::array<std::uint8_t,0x58>& out) const {
        bool disc=false;std::filesystem::path path;
        try{path=resolve(guest,disc);}catch(const std::exception&){return 0x80010002u;}
        platform::FileInfo info;
        if(const auto error=platform::file_info(path,info);!ok(error))return psp_error(error);
        out.fill(0);
        const auto put32=[&](std::size_t at,std::uint32_t v){for(unsigned n=0;n<4;++n)out[at+n]=static_cast<std::uint8_t>(v>>(8*n));};
        const auto put16=[&](std::size_t at,std::uint16_t v){out[at]=static_cast<std::uint8_t>(v);out[at+1]=static_cast<std::uint8_t>(v>>8);};
        put32(0,info.directory?0x11FFu:0x21FFu);put32(4,info.directory?0x10u:0x20u);
        put32(8,static_cast<std::uint32_t>(info.size));put32(12,static_cast<std::uint32_t>(info.size>>32));
        const auto put_time=[&](std::size_t at,const std::optional<platform::LocalTime>& t){
            if(!t)return;
            put16(at,t->year);put16(at+2,t->month);put16(at+4,t->day);put16(at+6,t->hour);put16(at+8,t->minute);put16(at+10,t->second);put32(at+12,t->microsecond);
        };
        put_time(16,info.created);put_time(32,info.accessed);put_time(48,info.modified);
        return 0;
    }
private:
    const ContentMount& content_;std::filesystem::path storage_;
    static bool ok(platform::FileError error){return error==platform::FileError::None;}
    static std::uint32_t psp_error(platform::FileError error) {
        switch(error) {
        case platform::FileError::NotFound:return 0x80010002u;
        case platform::FileError::AccessDenied:return 0x8001000Du;
        case platform::FileError::Exists:return 0x80010011u;
        case platform::FileError::Invalid:return invalid;
        case platform::FileError::DiskFull:return 0x8001001Cu;
        default:return 0x80010005u;
        }
    }
};
}
