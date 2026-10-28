#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "kon_version.h"
#include <windows.h>
#include <winhttp.h>
#include <array>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>
// Updates from the official GitHub releases (KON_GITHUB_REPO): the launcher
// asks GitHub for the latest release when it starts (3 s at most; offline it
// just plays), and when a newer version exists offers to install it. The
// update package (-actualizacion.zip, without the compiler) is enough when the
// toolchain did not change (manifest.json); otherwise the complete one. The
// files of the package replace the old ones; the player's data (userdata,
// config, analysis, recomp_cache) is never touched. The new launcher then
// rebuilds the game (native_build.hpp) and starts it.
// config/settings.ini: updates=0 disables the check.
namespace kon {
struct Version {
    int major=0,minor=0,patch=0;
    static std::optional<Version> parse(std::string s){
        if(!s.empty() && (s[0]=='v' || s[0]=='V'))s.erase(0,1);
        Version v;if(std::sscanf(s.c_str(),"%d.%d.%d",&v.major,&v.minor,&v.patch)<2)return std::nullopt;return v;
    }
    bool operator<(const Version& o)const{return std::array{major,minor,patch}<std::array{o.major,o.minor,o.patch};}
};
struct ReleaseInfo {std::string tag,update_url,full_url,manifest_url,notes;};
// Launcher progress (the dialog runs the download in a worker thread; the
// console installation ignores it and keeps printing as before).
enum class InstallPhase {Downloading,Extracting,Installing,Done};
using InstallProgress=std::function<void(InstallPhase,std::uint64_t,std::uint64_t)>;
inline std::wstring widen(const std::string& s){std::wstring w(MultiByteToWideChar(CP_UTF8,0,s.c_str(),int(s.size()),nullptr,0),L'\0');MultiByteToWideChar(CP_UTF8,0,s.c_str(),int(s.size()),w.data(),int(w.size()));return w;}

// HTTPS GET (follows GitHub's redirects to its download servers).
inline std::string https_get(const std::wstring& url,DWORD timeout_ms,const std::function<void(std::uint64_t,std::uint64_t)>& progress={}){
    URL_COMPONENTS parts{};parts.dwStructSize=sizeof parts;wchar_t host[256]{},path[4096]{};
    parts.lpszHostName=host;parts.dwHostNameLength=256;parts.lpszUrlPath=path;parts.dwUrlPathLength=4096;
    std::wstring extra(4096,L'\0');parts.lpszExtraInfo=extra.data();parts.dwExtraInfoLength=4096;
    if(!WinHttpCrackUrl(url.c_str(),0,0,&parts))throw std::runtime_error("URL no válida");
    std::wstring full_path=std::wstring(path,parts.dwUrlPathLength)+std::wstring(extra.c_str(),parts.dwExtraInfoLength);
    HINTERNET session=WinHttpOpen(L"KOnHoukagoLiveRecompiled/" KON_VERSION_WIDE,WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,WINHTTP_NO_PROXY_NAME,WINHTTP_NO_PROXY_BYPASS,0);
    if(!session)throw std::runtime_error("Sin conexión");
    WinHttpSetTimeouts(session,int(timeout_ms),int(timeout_ms),int(timeout_ms),int(timeout_ms));
    struct Close{HINTERNET h;~Close(){if(h)WinHttpCloseHandle(h);}};Close s{session};
    HINTERNET connection=WinHttpConnect(session,std::wstring(host,parts.dwHostNameLength).c_str(),parts.nPort,0);Close c{connection};
    if(!connection)throw std::runtime_error("Sin conexión");
    HINTERNET request=WinHttpOpenRequest(connection,L"GET",full_path.c_str(),nullptr,WINHTTP_NO_REFERER,WINHTTP_DEFAULT_ACCEPT_TYPES,parts.nScheme==INTERNET_SCHEME_HTTPS?WINHTTP_FLAG_SECURE:0);Close r{request};
    if(!request)throw std::runtime_error("Sin conexión");
    const wchar_t* headers=L"Accept: application/vnd.github+json, application/octet-stream\r\n";
    if(!WinHttpSendRequest(request,headers,DWORD(-1),WINHTTP_NO_REQUEST_DATA,0,0,0) || !WinHttpReceiveResponse(request,nullptr))throw std::runtime_error("Sin conexión");
    DWORD status=0,size=sizeof status;WinHttpQueryHeaders(request,WINHTTP_QUERY_STATUS_CODE|WINHTTP_QUERY_FLAG_NUMBER,WINHTTP_HEADER_NAME_BY_INDEX,&status,&size,WINHTTP_NO_HEADER_INDEX);
    if(status!=200)throw std::runtime_error("El servidor respondió "+std::to_string(status));
    std::uint64_t total=0;{wchar_t length[32]{};DWORD n=sizeof length;if(WinHttpQueryHeaders(request,WINHTTP_QUERY_CONTENT_LENGTH,WINHTTP_HEADER_NAME_BY_INDEX,length,&n,WINHTTP_NO_HEADER_INDEX))total=std::wcstoull(length,nullptr,10);}
    std::string body;std::vector<char> buffer(1u<<16);
    for(DWORD read=0;;){
        if(!WinHttpReadData(request,buffer.data(),DWORD(buffer.size()),&read))throw std::runtime_error("Descarga interrumpida");
        if(!read)break;body.append(buffer.data(),read);if(progress)progress(body.size(),total);
    }
    if(total && body.size()!=total)throw std::runtime_error("Descarga incompleta");
    return body;
}
// The few fields needed from GitHub's JSON (no JSON library in the port).
inline std::string json_field(const std::string& json,const std::string& key,std::size_t from=0){
    const auto k=json.find("\""+key+"\"",from);if(k==std::string::npos)return {};
    auto q=json.find('"',json.find(':',k)+1);if(q==std::string::npos)return {};
    std::string out;for(auto i=q+1;i<json.size() && json[i]!='"';++i){if(json[i]=='\\' && i+1<json.size())++i;out+=json[i];}
    return out;
}
inline void append_utf8(std::string& out,std::uint32_t cp){
    if(cp<=0x7F)out+=char(cp);
    else if(cp<=0x7FF){out+=char(0xC0|(cp>>6));out+=char(0x80|(cp&0x3F));}
    else if(cp<=0xFFFF){out+=char(0xE0|(cp>>12));out+=char(0x80|((cp>>6)&0x3F));out+=char(0x80|(cp&0x3F));}
    else{out+=char(0xF0|(cp>>18));out+=char(0x80|((cp>>12)&0x3F));out+=char(0x80|((cp>>6)&0x3F));out+=char(0x80|(cp&0x3F));}
}
inline bool parse_hex4(const std::string& s,std::size_t at,std::uint32_t& value){
    if(at+4>s.size())return false;value=0;
    for(std::size_t i=0;i<4;++i){const char c=s[at+i];value<<=4;
        if(c>='0' && c<='9')value|=std::uint32_t(c-'0');
        else if(c>='a' && c<='f')value|=std::uint32_t(c-'a'+10);
        else if(c>='A' && c<='F')value|=std::uint32_t(c-'A'+10);
        else return false;
    }
    return true;
}
// GitHub release notes (body): decodes \n and \uXXXX so the launcher can show
// short notes. json_field stays byte-for-byte for tags and URLs.
inline std::string json_text_field(const std::string& json,const std::string& key,std::size_t from=0){
    const auto k=json.find("\""+key+"\"",from);if(k==std::string::npos)return {};
    const auto colon=json.find(':',k);if(colon==std::string::npos)return {};
    auto q=colon+1;while(q<json.size() && (json[q]==' ' || json[q]=='\t' || json[q]=='\r' || json[q]=='\n'))++q;
    if(q>=json.size() || json[q]!='"')return {};  // body: null
    std::string out;
    for(std::size_t i=q+1;i<json.size();++i){
        const char c=json[i];
        if(c=='"')break;
        if(c!='\\'){out+=c;continue;}
        if(++i>=json.size())break;
        const char e=json[i];
        if(e=='u'){
            std::uint32_t cp=0;
            if(!parse_hex4(json,i+1,cp)){out+="\\u";continue;}
            i+=4;
            if(cp>=0xD800 && cp<=0xDBFF && i+6<json.size() && json[i+1]=='\\' && json[i+2]=='u'){
                std::uint32_t low=0;
                if(parse_hex4(json,i+3,low) && low>=0xDC00 && low<=0xDFFF){cp=0x10000+((cp-0xD800)<<10)+(low-0xDC00);i+=6;}
            }
            append_utf8(out,cp);continue;
        }
        switch(e){
            case 'n':out+='\n';break;case 'r':out+='\r';break;case 't':out+='\t';break;
            case 'b':out+='\b';break;case 'f':out+='\f';break;
            default:out+=e;  // \" \\ \/ and anything else
        }
    }
    return out;
}
inline std::optional<ReleaseInfo> latest_release(DWORD timeout_ms=3000,std::wstring* error=nullptr){
    if(error)error->clear();
    const std::string repo=KON_GITHUB_REPO;
    // Tests: KON_UPDATE_API points to a local copy of the release JSON.
    const wchar_t* api=_wgetenv(L"KON_UPDATE_API");
    if(repo.empty() && !api){if(error)*error=L"Las actualizaciones no están configuradas.";return std::nullopt;}
    try{
        const auto json=https_get(api?std::wstring(api):L"https://api.github.com/repos/"+std::wstring(repo.begin(),repo.end())+L"/releases/latest",timeout_ms);
        ReleaseInfo info;info.tag=json_field(json,"tag_name");info.notes=json_text_field(json,"body");
        for(std::size_t at=0;(at=json.find("\"browser_download_url\"",at))!=std::string::npos;++at){
            const auto url=json_field(json,"browser_download_url",at);
            if(url.ends_with("-actualizacion.zip"))info.update_url=url;
            else if(url.ends_with("-windows.zip"))info.full_url=url;
            else if(url.ends_with("manifest.json"))info.manifest_url=url;
        }
        if(info.tag.empty() || info.full_url.empty()){if(error)*error=L"La respuesta no incluye un paquete completo de la versión.";return std::nullopt;}
        return info;
    }catch(const std::exception& e){if(error)*error=widen(e.what());return std::nullopt;}
    catch(...){if(error)*error=L"No se pudo comprobar la actualización.";return std::nullopt;}
}

// Downloads and installs the release over this installation; returns true when
// the new launcher has to be started. `progress` is optional: the console
// installation prints as before and the launcher's dialog uses it to update
// its worker-thread progress without blocking the window.
inline bool install_update(const std::filesystem::path& root,const ReleaseInfo& info,const InstallProgress& progress={}){
    const auto work=root/"recomp_cache"/"actualizacion";std::filesystem::remove_all(work);std::filesystem::create_directories(work);
    // The small package when the compiler and Python are the same.
    std::string url=info.full_url;
    if(!info.update_url.empty() && !info.manifest_url.empty())try{
        const auto remote=https_get(widen(info.manifest_url),10000);
        std::ifstream in(root/"manifest.json",std::ios::binary);std::string local((std::istreambuf_iterator<char>(in)),{});
        if(!local.empty() && json_field(remote,"toolchain")==json_field(local,"toolchain"))url=info.update_url;
    }catch(const std::exception&){}
    const bool console=!progress;  // console mode keeps its messages; the dialog has its own progress
    if(console)std::cout<<"Descargando "<<info.tag<<"...\n"<<std::flush;
    if(progress)progress(InstallPhase::Downloading,0,0);
    std::uint64_t last_reported=~0ull;
    const auto zip=https_get(widen(url),30000,[&](std::uint64_t done,std::uint64_t total){
        if(progress)progress(InstallPhase::Downloading,done,total);
        if(!console)return;
        const auto mb=done>>20;if(mb==last_reported)return;last_reported=mb;
        std::cout<<"\r  "<<mb<<" MB"<<(total?" de "+std::to_string(total>>20)+" MB":"")<<"   "<<std::flush;});
    if(console)std::cout<<"\n";
    const auto archive=work/"paquete.zip";{std::ofstream out(archive,std::ios::binary);out.write(zip.data(),std::streamsize(zip.size()));if(!out)throw std::runtime_error("No hay espacio para la actualización. Libera espacio en el disco y vuelve a intentarlo");}
    // Windows 10 and 11 include tar, which extracts zip files.
    if(progress)progress(InstallPhase::Extracting,0,0);
    wchar_t system[MAX_PATH]{};GetSystemDirectoryW(system,MAX_PATH);
    std::wstring command=L"\""+std::wstring(system)+L"\\tar.exe\" -xf \""+archive.wstring()+L"\" -C \""+work.wstring()+L"\"";
    STARTUPINFOW si{};si.cb=sizeof si;PROCESS_INFORMATION pi{};
    if(!CreateProcessW(nullptr,command.data(),nullptr,nullptr,FALSE,CREATE_NO_WINDOW,nullptr,nullptr,&si,&pi))throw std::runtime_error("No se pudo descomprimir la actualización. Vuelve a intentarlo");
    WaitForSingleObject(pi.hProcess,INFINITE);DWORD code=1;GetExitCodeProcess(pi.hProcess,&code);CloseHandle(pi.hThread);CloseHandle(pi.hProcess);
    if(code)throw std::runtime_error("No se pudo descomprimir la actualización. Vuelve a intentarlo");
    std::filesystem::path package;
    for(const auto& e:std::filesystem::directory_iterator(work))if(e.is_directory() && std::filesystem::is_regular_file(e.path()/"manifest.json"))package=e.path();
    if(package.empty())throw std::runtime_error("El paquete descargado no es válido. Vuelve a intentarlo");
    if(console)std::cout<<"Instalando...\n"<<std::flush;
    // The running launcher cannot be overwritten but can be renamed.
    wchar_t module[MAX_PATH]{};GetModuleFileNameW(nullptr,module,MAX_PATH);const std::filesystem::path self=module;
    // The player's data (config, userdata, analysis, recomp_cache) is never
    // copied: saves and settings survive every update.
    std::vector<std::pair<std::filesystem::path,std::filesystem::path>> files;
    for(const auto& e:std::filesystem::recursive_directory_iterator(package)){
        if(!e.is_regular_file())continue;
        const auto rel=std::filesystem::relative(e.path(),package);
        const auto first=rel.begin()->string();
        if(first=="config" || first=="userdata" || first=="analysis" || first=="recomp_cache")continue;
        files.emplace_back(e.path(),rel);
    }
    std::uint64_t copied=0;
    if(progress)progress(InstallPhase::Installing,0,std::uint64_t(files.size()));
    for(const auto& [from,rel]:files){
        const auto to=root/rel;
        std::filesystem::create_directories(to.parent_path());
        if(std::filesystem::equivalent(to.parent_path(),self.parent_path()) && to.filename()==self.filename()){
            auto old=self;old+=L".old";std::error_code ignore;std::filesystem::remove(old,ignore);std::filesystem::rename(self,old);
        }
        std::filesystem::copy_file(from,to,std::filesystem::copy_options::overwrite_existing);
        if(progress)progress(InstallPhase::Installing,++copied,std::uint64_t(files.size()));
    }
    std::filesystem::remove_all(work);
    if(progress)progress(InstallPhase::Done,0,0);
    if(console)std::cout<<"Actualizado a "<<info.tag<<".\n"<<std::flush;
    return true;
}
}
