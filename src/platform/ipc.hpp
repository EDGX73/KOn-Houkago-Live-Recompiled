#pragma once
// Local IPC channel to another program on this machine (platform layer): the
// Discord client's IPC endpoint. Windows: a named pipe (\\.\pipe\discord-ipc-N);
// elsewhere: a Unix domain socket ($XDG_RUNTIME_DIR/discord-ipc-N, as Discord on
// Linux). Android has no local Discord client: opening simply fails.
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>
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
#include <cstring>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#endif
namespace kon::platform {
using NativeString=std::filesystem::path::string_type;
inline NativeString native_number(int n){
#ifdef _WIN32
    return std::to_wstring(n);
#else
    return std::to_string(n);
#endif
}
inline NativeString discord_ipc_prefix(){
#ifdef _WIN32
    return L"\\\\.\\pipe\\discord-ipc-";
#else
    for(const char* variable:{"XDG_RUNTIME_DIR","TMPDIR","TMP","TEMP"})
        if(const char* folder=std::getenv(variable);folder && *folder)return std::string(folder)+"/discord-ipc-";
    return "/tmp/discord-ipc-";
#endif
}
class LocalChannel {
public:
    LocalChannel()=default;
    ~LocalChannel(){close();}
    LocalChannel(const LocalChannel&)=delete;LocalChannel& operator=(const LocalChannel&)=delete;
    bool is_open()const{return handle_!=invalid();}
    bool open(const NativeString& name){
        close();
#ifdef _WIN32
        handle_=CreateFileW(name.c_str(),GENERIC_READ|GENERIC_WRITE,0,nullptr,OPEN_EXISTING,0,nullptr);
#else
        sockaddr_un address{};address.sun_family=AF_UNIX;
        if(name.size()>=sizeof address.sun_path)return false;
        std::memcpy(address.sun_path,name.c_str(),name.size()+1);
        handle_=::socket(AF_UNIX,SOCK_STREAM|SOCK_CLOEXEC,0);
        if(handle_>=0 && ::connect(handle_,reinterpret_cast<sockaddr*>(&address),sizeof address)){::close(handle_);handle_=invalid();}
#endif
        return is_open();
    }
    // The whole buffer, or false.
    bool write(const std::vector<std::uint8_t>& bytes){
#ifdef _WIN32
        DWORD done=0;return WriteFile(handle_,bytes.data(),DWORD(bytes.size()),&done,nullptr) && done==bytes.size();
#else
        std::size_t total=0;
        while(total<bytes.size()){
            const auto n=::send(handle_,bytes.data()+total,bytes.size()-total,MSG_NOSIGNAL);
            if(n<0){if(errno==EINTR)continue;return false;}
            total+=static_cast<std::size_t>(n);
        }
        return true;
#endif
    }
    // Bytes waiting to be read without blocking; false if the channel broke.
    bool available(std::size_t& count){
#ifdef _WIN32
        DWORD waiting=0;if(!PeekNamedPipe(handle_,nullptr,0,nullptr,&waiting,nullptr))return false;count=waiting;return true;
#else
        char probe;const auto r=::recv(handle_,&probe,1,MSG_PEEK|MSG_DONTWAIT);
        if(r==0)return false;  // the other side closed
        if(r<0){if(errno==EAGAIN || errno==EWOULDBLOCK){count=0;return true;}return false;}
        int waiting=0;if(ioctl(handle_,FIONREAD,&waiting))return false;count=static_cast<std::size_t>(waiting);return true;
#endif
    }
    // Reads and discards exactly `count` waiting bytes.
    bool discard(std::size_t count){
        std::vector<char> buffer(count);
#ifdef _WIN32
        DWORD read=0;return ReadFile(handle_,buffer.data(),DWORD(count),&read,nullptr);
#else
        return ::recv(handle_,buffer.data(),count,0)>=0;
#endif
    }
    void close(){
#ifdef _WIN32
        if(handle_!=INVALID_HANDLE_VALUE)CloseHandle(handle_);
#else
        if(handle_>=0)::close(handle_);
#endif
        handle_=invalid();
    }
private:
#ifdef _WIN32
    using Handle=HANDLE;static Handle invalid(){return INVALID_HANDLE_VALUE;}
#else
    using Handle=int;static constexpr Handle invalid(){return -1;}
#endif
    Handle handle_=invalid();
};
}
