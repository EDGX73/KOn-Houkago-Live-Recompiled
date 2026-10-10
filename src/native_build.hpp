#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "kon_version.h"
#include <windows.h>
#include <bcrypt.h>
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
// Builds the player's game on their own PC from their own disc, as the release
// package contains none of the game's code: psp_recomp translates the decrypted
// executable (analysis/executables/pkon.elf) into C++, the PC frame-rate patch is
// applied, zig compiles it and links it with the port's runtime libraries.
//
// Everything stays inside the installation folder:
//   recomp/          the toolkit from the package (zig, python, psp_recomp, libs)
//   recomp_cache/    generated code, objects and stamps (rebuilt when needed)
//   bin/             the built game
// Updates replace recomp/; only what changed is rebuilt (a new runtime only
// relinks; a new translator or patch regenerates and recompiles the units
// whose source changed).
namespace kon {
inline std::string sha256_bytes(const void* data,std::size_t size){
    BCRYPT_ALG_HANDLE alg=nullptr;BCRYPT_HASH_HANDLE hash=nullptr;unsigned char digest[32]{};
    if(BCryptOpenAlgorithmProvider(&alg,BCRYPT_SHA256_ALGORITHM,nullptr,0)!=0)throw std::runtime_error("SHA-256 no disponible");
    BCryptCreateHash(alg,&hash,nullptr,0,nullptr,0,0);
    const auto* p=static_cast<const unsigned char*>(data);
    while(size){const ULONG n=ULONG(std::min<std::size_t>(size,1u<<30));BCryptHashData(hash,const_cast<PUCHAR>(p),n,0);p+=n;size-=n;}
    BCryptFinishHash(hash,digest,32,0);BCryptDestroyHash(hash);BCryptCloseAlgorithmProvider(alg,0);
    static const char* hex="0123456789abcdef";std::string out;for(const auto b:digest){out+=hex[b>>4];out+=hex[b&15];}
    return out;
}
inline std::string read_file(const std::filesystem::path& path){
    std::ifstream in(path,std::ios::binary);if(!in)throw std::runtime_error("No se puede leer "+path.string());
    std::ostringstream s;s<<in.rdbuf();return s.str();
}
inline std::string read_text_or_empty(const std::filesystem::path& path){std::ifstream in(path,std::ios::binary);std::ostringstream s;s<<in.rdbuf();return s.str();}
inline void write_text(const std::filesystem::path& path,const std::string& text){std::ofstream(path,std::ios::binary|std::ios::trunc)<<text;}
inline std::string sha256_of_file(const std::filesystem::path& path){const auto s=read_file(path);return sha256_bytes(s.data(),s.size());}

// Runs a program hidden, its output appended to a log; returns the exit code.
inline DWORD run_hidden(std::wstring command,const std::filesystem::path& cwd,const std::filesystem::path& log){
    SECURITY_ATTRIBUTES sa{sizeof sa,nullptr,TRUE};
    HANDLE out=CreateFileW(log.wstring().c_str(),FILE_APPEND_DATA,FILE_SHARE_READ|FILE_SHARE_WRITE,&sa,OPEN_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
    STARTUPINFOW si{};si.cb=sizeof si;si.dwFlags=STARTF_USESTDHANDLES|STARTF_USESHOWWINDOW;si.wShowWindow=SW_HIDE;
    si.hStdOutput=out;si.hStdError=out;si.hStdInput=GetStdHandle(STD_INPUT_HANDLE);
    PROCESS_INFORMATION pi{};
    const BOOL ok=CreateProcessW(nullptr,command.data(),nullptr,nullptr,TRUE,CREATE_NO_WINDOW,nullptr,cwd.wstring().c_str(),&si,&pi);
    if(out!=INVALID_HANDLE_VALUE)CloseHandle(out);
    if(!ok)return DWORD(-1);
    WaitForSingleObject(pi.hProcess,INFINITE);DWORD code=1;GetExitCodeProcess(pi.hProcess,&code);
    CloseHandle(pi.hThread);CloseHandle(pi.hProcess);return code;
}
inline std::wstring quoted(const std::filesystem::path& p){return L"\""+p.wstring()+L"\"";}

// Copies every resource of a compiled .res file (icon, version) into an executable
// (zig c++ does not link .res files); same as tools/embed_resources.py.
inline void embed_resources(const std::filesystem::path& res,const std::filesystem::path& exe){
    const auto data=read_file(res);
    auto u16=[&](std::size_t at){return std::uint16_t(std::uint8_t(data[at])|(std::uint8_t(data[at+1])<<8));};
    auto u32=[&](std::size_t at){return std::uint32_t(u16(at))|(std::uint32_t(u16(at+2))<<16);};
    struct Id{bool number;WORD value;std::wstring name;LPCWSTR get()const{return number?MAKEINTRESOURCEW(value):name.c_str();}};
    auto read_id=[&](std::size_t& at){Id id{};if(u16(at)==0xFFFF){id.number=true;id.value=u16(at+2);at+=4;}else{while(u16(at)){id.name+=wchar_t(u16(at));at+=2;}at+=2;}return id;};
    HANDLE h=BeginUpdateResourceW(exe.wstring().c_str(),FALSE);if(!h)throw std::runtime_error("No se pudo poner el icono al juego");
    for(std::size_t at=0;at+8<=data.size();){
        const auto size=u32(at),header=u32(at+4);std::size_t p=at+8;
        const auto type=read_id(p),name=read_id(p);p=(p+3)&~std::size_t(3);
        const WORD language=u16(p+6);
        if(!(type.number && type.value==0 && name.number && name.value==0) && at+header+size<=data.size())
            if(!UpdateResourceW(h,type.get(),name.get(),language,const_cast<char*>(data.data()+at+header),size)){EndUpdateResourceW(h,TRUE);throw std::runtime_error("No se pudo poner el icono al juego");}
        at=(at+header+size+3)&~std::size_t(3);
    }
    if(!EndUpdateResourceW(h,FALSE))throw std::runtime_error("No se pudo poner el icono al juego");
}

struct GameBuild {
    std::filesystem::path root;
    std::filesystem::path kit()const{return root/"recomp";}
    std::filesystem::path cache()const{return root/"recomp_cache";}
    std::filesystem::path game()const{return root/"bin"/"K-On Houkago Live Recompiled (juego).exe";}
    std::filesystem::path elf()const{return root/"analysis"/"executables"/"pkon.elf";}
    std::filesystem::path log()const{return cache()/"compilacion.log";}
    // The package's identity (written by tools/make_release.py): when it differs
    // from the one the game was built with, something has to be rebuilt.
    std::string kit_id()const{return read_text_or_empty(kit()/"kit_id.txt");}
    bool up_to_date()const{
        return std::filesystem::is_regular_file(game()) && !kit_id().empty() && read_text_or_empty(cache()/"built_with.txt")==kit_id();
    }
    void build(){
        if(!std::filesystem::is_regular_file(kit()/"zig"/"zig.exe"))throw std::runtime_error("Falta la carpeta recomp junto al juego (vuelve a descomprimir el paquete)");
        std::filesystem::create_directories(cache());
        write_text(log(),"K-On! Houkago Live!! Recompiled " KON_VERSION_STRING "\n");
        // zig keeps its caches inside the installation folder too.
        SetEnvironmentVariableW(L"ZIG_GLOBAL_CACHE_DIR",(cache()/"zig-global").wstring().c_str());
        SetEnvironmentVariableW(L"ZIG_LOCAL_CACHE_DIR",(cache()/"zig-local").wstring().c_str());
        const auto corpus=generate();
        const auto objects=compile(corpus);
        link(objects);
        write_text(cache()/"built_with.txt",kit_id());
    }
private:
    // 1. Translation: regenerated when the disc, the translator or the patch changes.
    std::filesystem::path generate(){
        const auto corpus=cache()/"corpus";
        std::string identity=sha256_of_file(elf())+" "+sha256_of_file(kit()/"psp_recomp.exe");
        for(const auto& e:std::filesystem::directory_iterator(kit()/"patches"))if(e.path().extension()==".py")identity+=" "+e.path().filename().string()+"="+sha256_of_file(e.path());
        if(std::filesystem::is_regular_file(corpus/"generated_registry.cpp") && read_text_or_empty(corpus/".identity")==identity){std::cout<<"  Traducción del juego: ya hecha.\n";return corpus;}
        std::cout<<"  Traduciendo el código del juego a PC (1-3 minutos)...\n"<<std::flush;
        const auto staging=cache()/"corpus.nuevo";std::filesystem::remove_all(staging);std::filesystem::create_directories(staging);
        SetEnvironmentVariableW(L"PSPRECOMP_PLAIN_AUTO",L"1");
        if(run_hidden(quoted(kit()/"psp_recomp.exe")+L" "+quoted(elf())+L" --auto "+quoted(staging)+L" 0x08804000 16384",root,log()))
            throw std::runtime_error("La traducción del juego ha fallado (mira recomp_cache\\compilacion.log)");
        if(read_text_or_empty(staging/"auto_codegen_report.json").find("\"import_wrappers\": 238")==std::string::npos)
            throw std::runtime_error("La traducción no es la esperada (mira recomp_cache\\compilacion.log)");
        std::cout<<"  Aplicando el parche de FPS de PC...\n"<<std::flush;
        if(run_hidden(quoted(kit()/"python"/"python.exe")+L" -I "+quoted(kit()/"patches"/"apply_logic_rate_patch.py")+L" "+quoted(staging),root,log()))
            throw std::runtime_error("El parche de FPS ha fallado (mira recomp_cache\\compilacion.log)");
        write_text(staging/".identity",identity);
        std::filesystem::remove_all(corpus);std::filesystem::rename(staging,corpus);
        return corpus;
    }
    // 2. Compilation: one object per unit, recompiled only when its source,
    // the runtime headers or the flags change.
    std::vector<std::filesystem::path> compile(const std::filesystem::path& corpus){
        static const std::wstring flags=L"-target x86_64-windows-gnu -O2 -DNDEBUG -std=c++20 -fno-sanitize=undefined -g0";
        std::string headers;for(const auto& e:std::filesystem::recursive_directory_iterator(kit()/"include"))if(e.is_regular_file())headers+=sha256_of_file(e.path());
        headers+=read_text_or_empty(kit()/"zig"/"version.txt");headers+=sha256_of_file(corpus/"generated_units.hpp");
        const auto header_key=sha256_bytes(headers.data(),headers.size());
        const auto obj=cache()/"obj";std::filesystem::create_directories(obj);
        struct Job{std::filesystem::path source,object,key_file;std::string key;};
        std::vector<Job> all,todo;
        for(const auto& e:std::filesystem::directory_iterator(corpus)){
            const auto& p=e.path();if(p.extension()!=".cpp" || p.filename().string().rfind("generated_",0)!=0)continue;
            const auto source=read_file(p);std::string key=sha256_bytes(source.data(),source.size())+header_key;key=sha256_bytes(key.data(),key.size());
            Job job{p,obj/(p.stem().string()+".o"),obj/(p.stem().string()+".key"),key};
            if(!(std::filesystem::is_regular_file(job.object) && read_text_or_empty(job.key_file)==key))todo.push_back(job);
            all.push_back(job);
        }
        std::sort(all.begin(),all.end(),[](const Job& a,const Job& b){return a.object<b.object;});
        if(todo.empty())std::cout<<"  Compilación: ya hecha.\n";
        else{
            std::cout<<"  Compilando el juego ("<<todo.size()<<" partes; la primera vez tarda varios minutos)...\n"<<std::flush;
            std::atomic<std::size_t> next=0,done=0;std::atomic<bool> failed=false;std::mutex print;
            const unsigned workers=std::max(1u,std::min<unsigned>(std::thread::hardware_concurrency(),unsigned(todo.size())));
            std::vector<std::thread> pool;
            for(unsigned w=0;w<workers;++w)pool.emplace_back([&]{
                for(std::size_t i;!failed && (i=next++)<todo.size();){
                    const auto& job=todo[i];std::filesystem::remove(job.key_file);
                    const auto command=quoted(kit()/"zig"/"zig.exe")+L" c++ "+flags+L" -I"+quoted(corpus)+L" -isystem "+quoted(kit()/"include")+L" -c "+quoted(job.source)+L" -o "+quoted(job.object);
                    if(run_hidden(command,root,log())){failed=true;break;}
                    write_text(job.key_file,job.key);
                    const auto n=++done;std::lock_guard lock(print);std::cout<<"\r  "<<n*100/todo.size()<<" %   "<<std::flush;
                }
            });
            for(auto& t:pool)t.join();
            std::cout<<"\n";
            if(failed)throw std::runtime_error("La compilación ha fallado (mira recomp_cache\\compilacion.log)");
        }
        std::vector<std::filesystem::path> objects;for(const auto& j:all)objects.push_back(j.object);
        return objects;
    }
    // 3. Linking with the port's runtime (recomp/lib), then the icon.
    void link(const std::vector<std::filesystem::path>& objects){
        std::cout<<"  Montando el ejecutable...\n"<<std::flush;
        std::filesystem::create_directories(game().parent_path());
        auto slash=[](std::filesystem::path p){auto s=p.generic_string();return "\""+s+"\"";};
        std::string rsp;for(const auto& o:objects)rsp+=slash(o)+"\n";
        for(const char* lib:{"libkon_host.a","libkon_native_media.a","libkon_native_vulkan.a","libpsprecomp_core.a"})rsp+=slash(kit()/"lib"/lib)+"\n";
        rsp+="-luser32 -lgdi32 -lwinmm -liphlpapi -lole32 -ldbghelp -lhid -lws2_32 -lcomdlg32 -lkernel32 -lshell32 -loleaut32 -luuid -ladvapi32\n";
        write_text(cache()/"link.rsp",rsp);
        // Linked in the cache and then moved, with its symbols (crash reports), next to the
        // data; a failed link never leaves a broken game behind.
        const auto staging=cache()/game().filename();std::filesystem::remove(staging);
        if(run_hidden(quoted(kit()/"zig"/"zig.exe")+L" c++ -target x86_64-windows-gnu -O2 @"+quoted(cache()/"link.rsp")+L" -o "+quoted(staging),root,log()) || !std::filesystem::is_regular_file(staging))
            throw std::runtime_error("No se pudo montar el ejecutable (mira recomp_cache\\compilacion.log)");
        embed_resources(kit()/"app.res",staging);
        std::error_code e;std::filesystem::remove(game(),e);
        if(e)throw std::runtime_error("Cierra el juego antes de actualizarlo");
        std::filesystem::rename(staging,game());
        auto pdb=staging;pdb.replace_extension(".pdb");auto lib=staging;lib.replace_extension(".lib");
        if(std::filesystem::is_regular_file(pdb)){auto to=game();to.replace_extension(".pdb");std::filesystem::remove(to,e);std::filesystem::rename(pdb,to,e);}
        std::filesystem::remove(lib,e);
    }
};
}
