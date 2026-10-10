#pragma once
#include "native_activity.hpp"
#include "kon_version.h"
#include <windows.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>
// Discord Rich Presence through the local Discord client's IPC pipe
// (\\.\pipe\discord-ipc-0..9): a handshake with the application ID, then
// SET_ACTIVITY with the presence. Everything runs on its own thread with
// non-blocking pipe reads, reconnecting every few seconds while Discord is
// closed; the game never waits for it. Nothing is sent anywhere else.
namespace kon {
inline std::string json_string(const std::string& text){
    std::string out="\"";
    for(const unsigned char c:text){
        if(c=='"' || c=='\\'){out+='\\';out+=char(c);}
        else if(c<0x20){char b[8];std::snprintf(b,sizeof b,"\\u%04x",c);out+=b;}
        else out+=char(c);
    }
    return out+"\"";
}
// One IPC frame: opcode and length (little endian), then the JSON payload.
inline std::vector<std::uint8_t> discord_frame(std::uint32_t opcode,const std::string& json){
    std::vector<std::uint8_t> frame(8+json.size());
    const std::uint32_t length=static_cast<std::uint32_t>(json.size());
    std::memcpy(frame.data(),&opcode,4);std::memcpy(frame.data()+4,&length,4);std::memcpy(frame.data()+8,json.data(),json.size());
    return frame;
}
inline std::string discord_activity_json(const Presence& p,unsigned long pid,unsigned nonce,const std::string& large_image){
    std::string activity="{\"details\":"+json_string(p.details.substr(0,120));
    if(!p.state.empty())activity+=",\"state\":"+json_string(p.state.substr(0,120));
    if(p.start)activity+=",\"timestamps\":{\"start\":"+std::to_string(p.start)+"}";
    if(!large_image.empty())activity+=",\"assets\":{\"large_image\":"+json_string(large_image)+",\"large_text\":\"" KON_PRODUCT_NAME " " KON_VERSION_STRING "\"}";
    activity+="}";
    return "{\"cmd\":\"SET_ACTIVITY\",\"args\":{\"pid\":"+std::to_string(pid)+",\"activity\":"+activity+"},\"nonce\":\""+std::to_string(nonce)+"\"}";
}
class DiscordPresence {
public:
    // pipe_prefix: for tests (a private pipe name instead of Discord's).
    DiscordPresence(std::string client_id,std::string large_image={},std::wstring pipe_prefix=L"\\\\.\\pipe\\discord-ipc-")
        :client_id_(std::move(client_id)),large_image_(std::move(large_image)),pipe_prefix_(std::move(pipe_prefix)),thread_([this]{run();}){}
    ~DiscordPresence(){{std::lock_guard lock(mutex_);stop_=true;}wake_.notify_all();thread_.join();}
    void update(const Presence& p){{std::lock_guard lock(mutex_);if(pending_ && *pending_==p)return;pending_=p;dirty_=true;}wake_.notify_all();}
    bool connected()const{return connected_.load();}
    std::uint64_t sent()const{return sent_.load();}
private:
    std::string client_id_,large_image_;std::wstring pipe_prefix_;
    std::mutex mutex_;std::condition_variable wake_;bool stop_=false,dirty_=false;std::optional<Presence> pending_;
    std::atomic<bool> connected_{false};std::atomic<std::uint64_t> sent_{0};HANDLE pipe_=INVALID_HANDLE_VALUE;unsigned nonce_=0;
    std::thread thread_;
    bool write(std::uint32_t op,const std::string& json){
        const auto frame=discord_frame(op,json);DWORD done=0;
        return WriteFile(pipe_,frame.data(),DWORD(frame.size()),&done,nullptr) && done==frame.size();
    }
    // Discards whatever Discord answered (READY, command results); false if the pipe broke.
    bool drain(){
        for(;;){
            DWORD available=0;if(!PeekNamedPipe(pipe_,nullptr,0,nullptr,&available,nullptr))return false;
            if(!available)return true;
            std::vector<char> buffer(available);DWORD read=0;if(!ReadFile(pipe_,buffer.data(),available,&read,nullptr))return false;
        }
    }
    void disconnect(){if(pipe_!=INVALID_HANDLE_VALUE)CloseHandle(pipe_);pipe_=INVALID_HANDLE_VALUE;connected_=false;}
    bool connect(){
        for(int n=0;n<10;++n){
            const auto name=pipe_prefix_+std::to_wstring(n);
            pipe_=CreateFileW(name.c_str(),GENERIC_READ|GENERIC_WRITE,0,nullptr,OPEN_EXISTING,0,nullptr);
            if(pipe_==INVALID_HANDLE_VALUE)continue;
            if(write(0,"{\"v\":1,\"client_id\":"+json_string(client_id_)+"}")){
                // Give Discord a moment to answer (READY) before the first command.
                for(int wait=0;wait<30;++wait){DWORD available=0;if(!PeekNamedPipe(pipe_,nullptr,0,nullptr,&available,nullptr)){disconnect();break;}if(available)break;Sleep(100);}
                if(pipe_==INVALID_HANDLE_VALUE)continue;
                connected_=true;return true;}
            disconnect();
        }
        return false;
    }
    void run(){
        std::unique_lock lock(mutex_);
        while(!stop_){
            if(pipe_==INVALID_HANDLE_VALUE){
                lock.unlock();const bool ok=connect();lock.lock();
                if(!ok){wake_.wait_for(lock,std::chrono::seconds(10),[&]{return stop_;});continue;}
                dirty_=pending_.has_value();
            }
            if(dirty_ && pending_){
                const auto json=discord_activity_json(*pending_,GetCurrentProcessId(),++nonce_,large_image_);dirty_=false;
                lock.unlock();const bool ok=write(1,json);lock.lock();
                if(!ok){disconnect();continue;}
                ++sent_;
            }
            lock.unlock();const bool alive=drain();lock.lock();
            if(!alive){disconnect();continue;}
            wake_.wait_for(lock,std::chrono::seconds(2),[&]{return stop_ || dirty_;});
        }
        if(pipe_!=INVALID_HANDLE_VALUE){
            // Clear the presence on exit.
            write(1,"{\"cmd\":\"SET_ACTIVITY\",\"args\":{\"pid\":"+std::to_string(GetCurrentProcessId())+"},\"nonce\":\"bye\"}");
            disconnect();
        }
    }
};
}
