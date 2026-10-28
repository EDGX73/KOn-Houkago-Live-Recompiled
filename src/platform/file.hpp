#pragma once
// Files for the PSP I/O and savedata services (platform layer): handles with the
// Windows semantics the services were written against (shared read/write/delete,
// the five creation dispositions, 64-bit seeks), file attributes in local time,
// and durable write-then-rename transactions. POSIX keeps the same contracts.
#include "process.hpp"
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
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
#include <cerrno>
#include <ctime>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif
namespace kon::platform {
enum class FileError {None,NotFound,AccessDenied,Exists,Invalid,DiskFull,Other};
enum class Disposition {OpenExisting,CreateNew,CreateAlways,OpenAlways,TruncateExisting};
#ifdef _WIN32
inline FileError last_file_error(){
    switch(GetLastError()) {
    case ERROR_FILE_NOT_FOUND:case ERROR_PATH_NOT_FOUND:return FileError::NotFound;
    case ERROR_ACCESS_DENIED:case ERROR_WRITE_PROTECT:return FileError::AccessDenied;
    case ERROR_FILE_EXISTS:case ERROR_ALREADY_EXISTS:return FileError::Exists;
    case ERROR_INVALID_PARAMETER:case ERROR_NEGATIVE_SEEK:return FileError::Invalid;
    case ERROR_DISK_FULL:case ERROR_HANDLE_DISK_FULL:return FileError::DiskFull;
    default:return FileError::Other;
    }
}
inline unsigned long last_error_code(){return GetLastError();}
#else
inline FileError last_file_error(){
    switch(errno) {
    case ENOENT:case ENOTDIR:return FileError::NotFound;
    case EACCES:case EPERM:case EROFS:return FileError::AccessDenied;
    case EEXIST:return FileError::Exists;
    case EINVAL:case EISDIR:return FileError::Invalid;
    case ENOSPC:case EDQUOT:return FileError::DiskFull;
    default:return FileError::Other;
    }
}
inline unsigned long last_error_code(){return static_cast<unsigned long>(errno);}
#endif

class File {
public:
    File()=default;
    ~File(){close();}
    File(const File&)=delete;File& operator=(const File&)=delete;
    File(File&& other) noexcept:handle_(std::exchange(other.handle_,invalid())){}
    File& operator=(File&& other) noexcept{if(this!=&other){close();handle_=std::exchange(other.handle_,invalid());}return *this;}
    explicit operator bool() const{return handle_!=invalid();}
    FileError open(const std::filesystem::path& path,bool read,bool write,Disposition disposition){
        close();
#ifdef _WIN32
        const DWORD access=(read?GENERIC_READ:0)|(write?GENERIC_WRITE:0);
        constexpr DWORD creation[]{OPEN_EXISTING,CREATE_NEW,CREATE_ALWAYS,OPEN_ALWAYS,TRUNCATE_EXISTING};
        handle_=CreateFileW(path.c_str(),access,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,
                            nullptr,creation[static_cast<int>(disposition)],FILE_ATTRIBUTE_NORMAL,nullptr);
#else
        int flags=O_CLOEXEC|(read && write?O_RDWR:write?O_WRONLY:O_RDONLY);
        constexpr int creation[]{0,O_CREAT|O_EXCL,O_CREAT|O_TRUNC,O_CREAT,O_TRUNC};
        flags|=creation[static_cast<int>(disposition)];
        handle_=::open(path.c_str(),flags,0666);
#endif
        return handle_==invalid()?last_file_error():FileError::None;
    }
    FileError close(){
        if(handle_==invalid())return FileError::None;
#ifdef _WIN32
        const bool ok=CloseHandle(handle_);
#else
        const bool ok=::close(handle_)==0;
#endif
        handle_=invalid();return ok?FileError::None:last_file_error();
    }
    FileError size(std::int64_t& out) const {
#ifdef _WIN32
        LARGE_INTEGER size{};if(!GetFileSizeEx(handle_,&size))return last_file_error();out=size.QuadPart;
#else
        struct stat s{};if(fstat(handle_,&s))return last_file_error();out=s.st_size;
#endif
        return FileError::None;
    }
    // origin: 0 start, 1 current position, 2 end (the PSP's and both hosts' values).
    FileError seek(std::int64_t offset,std::uint32_t origin,std::int64_t& position){
#ifdef _WIN32
        LARGE_INTEGER distance{};distance.QuadPart=offset;LARGE_INTEGER result{};
        if(!SetFilePointerEx(handle_,distance,&result,origin))return last_file_error();position=result.QuadPart;
#else
        const auto result=lseek(handle_,static_cast<off_t>(offset),static_cast<int>(origin));
        if(result<0)return last_file_error();position=result;
#endif
        return FileError::None;
    }
    FileError read(std::span<std::uint8_t> data,std::uint32_t& count){
#ifdef _WIN32
        DWORD done=0;if(!ReadFile(handle_,data.data(),static_cast<DWORD>(data.size()),&done,nullptr))return last_file_error();count=done;
#else
        ssize_t done;while((done=::read(handle_,data.data(),data.size()))<0 && errno==EINTR){}
        if(done<0)return last_file_error();count=static_cast<std::uint32_t>(done);
#endif
        return FileError::None;
    }
    FileError write(std::span<const std::uint8_t> data,std::uint32_t& count){
#ifdef _WIN32
        DWORD done=0;if(!WriteFile(handle_,data.data(),static_cast<DWORD>(data.size()),&done,nullptr))return last_file_error();count=done;
#else
        std::size_t total=0;  // a short write is continued: WriteFile completes on files
        while(total<data.size()){
            const auto done=::write(handle_,data.data()+total,data.size()-total);
            if(done<0){if(errno==EINTR)continue;return last_file_error();}
            total+=static_cast<std::size_t>(done);
        }
        count=static_cast<std::uint32_t>(total);
#endif
        return FileError::None;
    }
    FileError flush(){
#ifdef _WIN32
        return FlushFileBuffers(handle_)?FileError::None:last_file_error();
#else
        return fsync(handle_)==0?FileError::None:last_file_error();
#endif
    }
private:
#ifdef _WIN32
    using Handle=HANDLE;static Handle invalid(){return INVALID_HANDLE_VALUE;}
#else
    using Handle=int;static constexpr Handle invalid(){return -1;}
#endif
    Handle handle_=invalid();
};

struct LocalTime {std::uint16_t year,month,day,hour,minute,second;std::uint32_t microsecond;};
struct FileInfo {bool directory=false;std::uint64_t size=0;std::optional<LocalTime> created,accessed,modified;};
// Attributes of a file or directory; times in the host's local time zone.
inline FileError file_info(const std::filesystem::path& path,FileInfo& info){
#ifdef _WIN32
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if(!GetFileAttributesExW(path.c_str(),GetFileExInfoStandard,&data))return last_file_error();
    info.directory=data.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY;
    info.size=(static_cast<std::uint64_t>(data.nFileSizeHigh)<<32)|data.nFileSizeLow;
    const auto local=[](FILETIME utc)->std::optional<LocalTime>{
        FILETIME l{};SYSTEMTIME s{};
        if(!FileTimeToLocalFileTime(&utc,&l) || !FileTimeToSystemTime(&l,&s))return std::nullopt;
        return LocalTime{s.wYear,s.wMonth,s.wDay,s.wHour,s.wMinute,s.wSecond,s.wMilliseconds*1000u};
    };
    info.created=local(data.ftCreationTime);info.accessed=local(data.ftLastAccessTime);info.modified=local(data.ftLastWriteTime);
#else
    struct stat s{};if(::stat(path.c_str(),&s))return last_file_error();
    info.directory=S_ISDIR(s.st_mode);info.size=static_cast<std::uint64_t>(s.st_size);
    const auto local=[](const timespec& t)->std::optional<LocalTime>{
        std::tm l{};const std::time_t seconds=t.tv_sec;if(!localtime_r(&seconds,&l))return std::nullopt;
        return LocalTime{static_cast<std::uint16_t>(l.tm_year+1900),static_cast<std::uint16_t>(l.tm_mon+1),static_cast<std::uint16_t>(l.tm_mday),
                         static_cast<std::uint16_t>(l.tm_hour),static_cast<std::uint16_t>(l.tm_min),static_cast<std::uint16_t>(l.tm_sec),
                         static_cast<std::uint32_t>(t.tv_nsec/1000000)*1000u};  // millisecond precision, as on Windows
    };
    info.created=local(s.st_ctim);info.accessed=local(s.st_atim);info.modified=local(s.st_mtim);  // POSIX has no creation time: status change
#endif
    return FileError::None;
}

// Durable replacement for savedata: write a new temporary file completely,
// flush it to storage, then move it over the target (atomically on one volume).
enum class DurableWrite {Ok,CannotCreate,NotStored};
inline DurableWrite write_new_file_durably(const std::filesystem::path& path,std::span<const std::uint8_t> data){
#ifdef _WIN32
    const auto handle=CreateFileW(path.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(handle==INVALID_HANDLE_VALUE)return DurableWrite::CannotCreate;
    DWORD written=0;const auto ok=WriteFile(handle,data.data(),static_cast<DWORD>(data.size()),&written,nullptr);
    const auto flushed=ok && written==data.size() && FlushFileBuffers(handle);const auto closed=CloseHandle(handle);
    return flushed && closed?DurableWrite::Ok:DurableWrite::NotStored;
#else
    File file;std::uint32_t written=0;
    if(file.open(path,false,true,Disposition::CreateNew)!=FileError::None)return DurableWrite::CannotCreate;
    const bool flushed=file.write(data,written)==FileError::None && written==data.size() && file.flush()==FileError::None;
    return file.close()==FileError::None && flushed?DurableWrite::Ok:DurableWrite::NotStored;
#endif
}
inline bool move_file_durably(const std::filesystem::path& from,const std::filesystem::path& to,bool replace){
#ifdef _WIN32
    return MoveFileExW(from.c_str(),to.c_str(),MOVEFILE_WRITE_THROUGH|(replace?MOVEFILE_REPLACE_EXISTING:0));
#else
    // rename always replaces; without replace, link() fails if the target exists.
    if(replace){if(::rename(from.c_str(),to.c_str()))return false;}
    else{if(::link(from.c_str(),to.c_str()))return false;::unlink(from.c_str());}
    const int directory=::open(to.parent_path().empty()?".":to.parent_path().c_str(),O_RDONLY|O_DIRECTORY|O_CLOEXEC);
    if(directory>=0){fsync(directory);::close(directory);}  // the rename itself reaches storage
    return true;
#endif
}
// Whether a path exists and what it is; Error keeps the host code for messages.
enum class PathKind {Missing,File,Directory,Error};
inline PathKind path_kind(const std::filesystem::path& path,unsigned long& error){
#ifdef _WIN32
    const auto attributes=GetFileAttributesW(path.c_str());
    if(attributes==INVALID_FILE_ATTRIBUTES){
        error=GetLastError();
        return error==ERROR_FILE_NOT_FOUND || error==ERROR_PATH_NOT_FOUND?PathKind::Missing:PathKind::Error;
    }
    return attributes&FILE_ATTRIBUTE_DIRECTORY?PathKind::Directory:PathKind::File;
#else
    struct stat s{};
    if(::stat(path.c_str(),&s)){error=static_cast<unsigned long>(errno);return errno==ENOENT || errno==ENOTDIR?PathKind::Missing:PathKind::Error;}
    return S_ISDIR(s.st_mode)?PathKind::Directory:PathKind::File;
#endif
}
}
