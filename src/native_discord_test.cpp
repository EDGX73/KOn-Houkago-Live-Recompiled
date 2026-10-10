#include "native_discord.hpp"
#include "cpk_index.hpp"
#include <algorithm>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <stdexcept>
// Presence from the files the game loads, the Discord IPC frames against a
// private pipe standing in for the Discord client, and (with the extracted
// disc given) the std.cpk member lookup.
using namespace kon;
static void check(bool ok,const char* what){if(!ok)throw std::runtime_error(what);}
static std::string read_frame(HANDLE pipe,std::uint32_t& op){
    std::uint32_t head[2]{};DWORD got=0;
    if(!ReadFile(pipe,head,8,&got,nullptr) || got!=8)throw std::runtime_error("No IPC frame header");
    std::string json(head[1],'\0');if(head[1] && (!ReadFile(pipe,json.data(),head[1],&got,nullptr) || got!=head[1]))throw std::runtime_error("Short IPC frame");
    op=head[0];return json;
}
int main(int argc,char** argv){try{
    GameActivity a;std::vector<Presence> seen;a.on_change=[&](const Presence& p){seen.push_back(p);};
    a.start();check(seen.size()==1 && seen[0].details=="Starting the game","Presence at start");
    a.on_member("PSP_GAME\\USRDIR\\bind/teatime_std.bin");check(seen.back().details=="In the light music club","Club room");
    a.on_member("PSP_GAME/USRDIR/bind/menu.bin");check(seen.back().details=="Choosing a song","Menus");
    a.on_member("PSP_GAME/USRDIR/bind/play_chr_2d_mi.bin");
    a.on_member("PSP_GAME/USRDIR/bind/play_m03_single.bin");
    check(seen.back().details=="Playing Watashi no Koi wa Hotchkiss" && seen.back().state=="Solo · Mio (bass)" && seen.back().start>0,"Song, part and timer");
    const auto count=seen.size();a.on_member("PSP_GAME/USRDIR/bind/play_m03_2.bin");check(seen.size()==count,"The title-card pack changes nothing");
    a.on_member("PSP_GAME/USRDIR/bind/result.bin");check(seen.back().details=="Results: Watashi no Koi wa Hotchkiss","Results");
    a.on_member("PSP_GAME/USRDIR/bind/menu.bin");check(seen.back().state=="1 song played","Session count");
    a.on_member("PSP_GAME/USRDIR/bind/play_m02.bin");check(seen.back().details=="Playing Fuwa Fuwa Time" && seen.back().state.rfind("With the band",0)==0,"Band play");
    // IPC against a private pipe.
    const std::wstring prefix=L"\\\\.\\pipe\\kon-discord-test-"+std::to_wstring(GetCurrentProcessId())+L"-";
    HANDLE server=CreateNamedPipeW((prefix+L"0").c_str(),PIPE_ACCESS_DUPLEX,PIPE_TYPE_BYTE|PIPE_READMODE_BYTE|PIPE_WAIT,1,65536,65536,0,nullptr);
    check(server!=INVALID_HANDLE_VALUE,"Test pipe");
    {
        DiscordPresence discord("123456789012345678","logo",prefix);
        discord.update(seen.back());
        check(ConnectNamedPipe(server,nullptr) || GetLastError()==ERROR_PIPE_CONNECTED,"Client connects");
        std::uint32_t op=9;auto hello=read_frame(server,op);
        check(op==0 && hello=="{\"v\":1,\"client_id\":\"123456789012345678\"}","Handshake");
        const auto ready=discord_frame(1,"{\"cmd\":\"DISPATCH\",\"evt\":\"READY\"}");DWORD put=0;WriteFile(server,ready.data(),DWORD(ready.size()),&put,nullptr);
        auto activity=read_frame(server,op);
        check(op==1 && activity.find("\"cmd\":\"SET_ACTIVITY\"")!=std::string::npos && activity.find("Playing Fuwa Fuwa Time")!=std::string::npos
            && activity.find("\"large_image\":\"logo\"")!=std::string::npos && activity.find("\"start\":")!=std::string::npos,"SET_ACTIVITY with the presence");
        Presence quoted;quoted.details="Playing Don't say \"lazy\"";discord.update(quoted);
        activity=read_frame(server,op);check(activity.find("Don't say \\\"lazy\\\"")!=std::string::npos,"JSON escaping");
    }
    std::uint32_t op=0;const auto bye=read_frame(server,op);check(bye.find("\"nonce\":\"bye\"")!=std::string::npos,"Presence cleared on exit");
    CloseHandle(server);
    unsigned found=0;
    if(argc>1 && std::filesystem::exists(argv[1])){
        const auto index=CpkIndex::load(argv[1]);check(index && index->size()>1000,"std.cpk index");
        // Each song's single-play pack is found at its first and last byte.
        for(unsigned n=0;n<19;++n){
            char name[64];std::snprintf(name,sizeof name,"bind/play_m%02u_single.bin",n);
            const auto it=std::find_if(index->members().begin(),index->members().end(),[&](const auto& m){return m.name.size()>=std::strlen(name) && m.name.compare(m.name.size()-std::strlen(name),std::string::npos,name)==0;});
            check(it!=index->members().end(),"Song pack listed");
            check(index->at(it->offset)==&*it && index->at(it->offset+it->size-1)==&*it,"Member found by offset");
            ++found;
        }
    }
    std::cout<<"PASS: activity from loaded files, Discord IPC handshake/SET_ACTIVITY/clear"<<(found?", "+std::to_string(found)+" song packs found in std.cpk by offset":std::string{})<<"\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<"\n";return 1;}}
