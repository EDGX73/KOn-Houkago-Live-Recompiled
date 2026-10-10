#pragma once
#include "psprecomp/sha256.hpp"
#include <windows.h>
#include <commdlg.h>
#include <array>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <optional>
#include <string>
#include <vector>
// Installation from the player's own disc image, with nothing else needed
// (K-On Houkago Live Recompiled.exe asks for it the first time, src/launcher.cpp): reads the
// ISO 9660 image directly (no mounting), recognises the Japanese or English
// disc by its std.cpk, extracts it to analysis/extracted/<jp|en>, checks the
// main files by SHA-256, decrypts the EBOOT with pspdecrypt (next to the
// game in tools/) and checks the executable. One disc is enough: with only
// the English one, the Japanese folder gets the shared sound bank; with only
// the Japanese one, the game starts in Japanese.
namespace kon {
struct IsoFile {std::string path;std::uint32_t lba=0,size=0;};
class IsoImage {
public:
    explicit IsoImage(const std::filesystem::path& path):in_(path,std::ios::binary){
        if(!in_)throw std::runtime_error("No se puede abrir la imagen");
        std::array<std::uint8_t,2048> pvd{};read(16,pvd.data(),2048);
        if(pvd[0]!=1 || std::memcmp(pvd.data()+1,"CD001",5))throw std::runtime_error("No es una imagen ISO 9660 (¿es un .iso de PSP?)");
        walk(u32(pvd.data()+156+2),u32(pvd.data()+156+10),"",0);
    }
    const std::vector<IsoFile>& files()const{return files_;}
    const IsoFile* find(const std::string& path)const{for(const auto& f:files_)if(f.path==path)return &f;return nullptr;}
    void extract(const IsoFile& f,const std::filesystem::path& to){
        std::filesystem::create_directories(to.parent_path());
        std::ofstream out(to,std::ios::binary|std::ios::trunc);if(!out)throw std::runtime_error("No se puede escribir "+to.string());
        std::vector<char> buffer(4u<<20);std::uint64_t left=f.size,offset=std::uint64_t(f.lba)*2048u;
        in_.clear();in_.seekg(std::streamoff(offset));
        while(left){const auto n=std::size_t(std::min<std::uint64_t>(left,buffer.size()));if(!in_.read(buffer.data(),std::streamsize(n)))throw std::runtime_error("La imagen está incompleta");out.write(buffer.data(),std::streamsize(n));left-=n;}
        if(!out)throw std::runtime_error("No hay espacio para "+to.string());
    }
private:
    std::ifstream in_;std::vector<IsoFile> files_;
    static std::uint32_t u32(const std::uint8_t* p){return std::uint32_t(p[0])|(std::uint32_t(p[1])<<8)|(std::uint32_t(p[2])<<16)|(std::uint32_t(p[3])<<24);}
    void read(std::uint32_t lba,std::uint8_t* to,std::size_t size){in_.clear();in_.seekg(std::streamoff(std::uint64_t(lba)*2048u));if(!in_.read(reinterpret_cast<char*>(to),std::streamsize(size)))throw std::runtime_error("La imagen está incompleta");}
    void walk(std::uint32_t lba,std::uint32_t size,const std::string& prefix,int depth){
        if(depth>16 || size>(64u<<20))throw std::runtime_error("Directorio de la imagen no válido");
        std::vector<std::uint8_t> dir((size+2047u)/2048u*2048u);read(lba,dir.data(),dir.size());
        for(std::size_t at=0;at<size;){
            const unsigned length=dir[at];
            if(!length){at=(at/2048u+1u)*2048u;continue;}
            if(at+length>dir.size() || length<34)break;
            const auto* r=dir.data()+at;const unsigned name_length=r[32];
            if(33u+name_length<=length && !(name_length==1 && (r[33]==0 || r[33]==1))){
                std::string name(reinterpret_cast<const char*>(r+33),name_length);
                if(const auto semicolon=name.find(';');semicolon!=std::string::npos)name.resize(semicolon);
                if(!name.empty() && name.back()=='.')name.pop_back();
                if(name.find("..")!=std::string::npos || name.find('/')!=std::string::npos || name.find('\\')!=std::string::npos)throw std::runtime_error("Nombre no válido en la imagen");
                const auto path=prefix.empty()?name:prefix+"/"+name;
                if(r[25]&2u)walk(u32(r+2),u32(r+10),path,depth+1);else files_.push_back({path,u32(r+2),u32(r+10)});
            }
            at+=length;
        }
    }
};
struct DiscInfo {const char* folder;const char* name;std::uint64_t std_size;const char* std_sha;};
inline constexpr DiscInfo kDiscs[]{
    {"jp","japonesa",451382528u,"f41890e40bf127bba9a357f6828e4d80c7140db5559180b8c55a11633c9e8a08"},
    {"en","inglesa",756264416u,"3dfbffced2b4d1eae7ce2969832e04f55332747018f351a176ae984239e37cfe"}};
inline constexpr const char* kEbootSha="676ce2a0c9caf276e2bf22511fe46dd0cd6cda8d58b57553a8e4029f4addf29d";
inline constexpr const char* kElfSha="eda890135ea4aee1a8d0a82b2f00a7159060c58498a6705df7518b36e46b85b1";
// File dialog for the disc images (several may be chosen).
inline std::vector<std::filesystem::path> choose_isos(){
    std::vector<wchar_t> buffer(32768,L'\0');OPENFILENAMEW o{};o.lStructSize=sizeof o;
    o.lpstrFilter=L"Imagen de disco de PSP (*.iso)\0*.iso\0Todos los archivos\0*.*\0";o.lpstrFile=buffer.data();o.nMaxFile=DWORD(buffer.size());
    o.lpstrTitle=L"Elige tu ISO de K-On! Houkago Live!! (japonesa o inglesa)";o.Flags=OFN_FILEMUSTEXIST|OFN_ALLOWMULTISELECT|OFN_EXPLORER;
    if(!GetOpenFileNameW(&o))return {};
    std::vector<std::filesystem::path> out;const wchar_t* p=buffer.data();std::filesystem::path first=p;p+=wcslen(p)+1;
    if(!*p)return {first};
    while(*p){out.push_back(first/p);p+=wcslen(p)+1;}
    return out;
}
// Returns 0 when the game is ready to play.
inline int install_from_isos(std::filesystem::path root,std::vector<std::filesystem::path> isos){
    try{
        if(isos.empty()){std::cout<<"Elige tu ISO en la ventana que se ha abierto...\n";isos=choose_isos();}
        if(isos.empty()){std::cout<<"No se eligió ninguna ISO.\n";return 1;}
        const auto extracted=root/"analysis"/"extracted";
        for(const auto& iso:isos){
            std::cout<<"\nLeyendo "<<iso.filename().string()<<"...\n";
            IsoImage image(iso);
            const auto* std_cpk=image.find("PSP_GAME/USRDIR/std.cpk");const auto* eboot=image.find("PSP_GAME/SYSDIR/EBOOT.BIN");
            if(!std_cpk || !eboot)throw std::runtime_error("Esta ISO no es de K-On! Houkago Live!! (no tiene PSP_GAME/USRDIR/std.cpk)");
            const DiscInfo* disc=nullptr;for(const auto& d:kDiscs)if(d.std_size==std_cpk->size)disc=&d;
            if(!disc)throw std::runtime_error("ISO de K-On no reconocida: solo valen la japonesa original (ULJM05709) y la traducción inglesa \"After School Live!!\"");
            const auto target=extracted/disc->folder;
            if(std::filesystem::is_regular_file(target/"PSP_GAME/USRDIR/std.cpk")){std::cout<<"La versión "<<disc->name<<" ya estaba instalada.\n";continue;}
            const auto staging=extracted/(std::string(disc->folder)+".instalando");std::filesystem::remove_all(staging);
            std::uint64_t total=0,done=0;for(const auto& f:image.files())total+=f.size;
            std::cout<<"Versión "<<disc->name<<": extrayendo "<<image.files().size()<<" archivos ("<<total/1048576u<<" MB)...\n";
            for(const auto& f:image.files()){image.extract(f,staging/std::filesystem::path(f.path));done+=f.size;std::cout<<"\r  "<<(total?done*100u/total:100u)<<" %   "<<std::flush;}
            std::cout<<"\nComprobando los archivos principales...\n";
            if(psprecomp::sha256_file(staging/"PSP_GAME/USRDIR/std.cpk")!=disc->std_sha)throw std::runtime_error("std.cpk no coincide con el disco original (ISO dañada o modificada)");
            if(psprecomp::sha256_file(staging/"PSP_GAME/SYSDIR/EBOOT.BIN")!=kEbootSha)throw std::runtime_error("El EBOOT no coincide con el del juego original (otra versión o ISO modificada)");
            std::filesystem::rename(staging,target);
            std::cout<<"Versión "<<disc->name<<" instalada.\n";
        }
        const bool jp=std::filesystem::is_regular_file(extracted/"jp/PSP_GAME/USRDIR/std.cpk"),en=std::filesystem::is_regular_file(extracted/"en/PSP_GAME/USRDIR/std.cpk");
        if(!jp && !en)throw std::runtime_error("No hay ninguna versión instalada");
        // The game reads the shared sound bank (same in both discs) from the Japanese folder.
        if(!jp){
            const auto from=extracted/"en/PSP_GAME/USRDIR/snd.cpk",to=extracted/"jp/PSP_GAME/USRDIR/snd.cpk";
            if(!std::filesystem::exists(to)){std::filesystem::create_directories(to.parent_path());std::error_code e;std::filesystem::create_hard_link(from,to,e);if(e)std::filesystem::copy_file(from,to);}
        }
        const auto elf=root/"analysis"/"executables"/"pkon.elf";
        if(!std::filesystem::is_regular_file(elf) || psprecomp::sha256_file(elf)!=kElfSha){
            std::cout<<"Descifrando el programa del juego...\n";
            std::filesystem::create_directories(elf.parent_path());
            const auto tool=root/"tools"/"pspdecrypt.exe";
            if(!std::filesystem::is_regular_file(tool))throw std::runtime_error("Falta tools\\pspdecrypt.exe junto al juego");
            const auto eboot=extracted/(jp?"jp":"en")/"PSP_GAME/SYSDIR/EBOOT.BIN";
            std::wstring command=L"\""+tool.wstring()+L"\" \""+eboot.wstring()+L"\" -o \""+elf.wstring()+L"\"";
            STARTUPINFOW si{};si.cb=sizeof si;si.dwFlags=STARTF_USESHOWWINDOW;si.wShowWindow=SW_HIDE;PROCESS_INFORMATION pi{};
            if(!CreateProcessW(nullptr,command.data(),nullptr,nullptr,FALSE,CREATE_NO_WINDOW,nullptr,root.wstring().c_str(),&si,&pi))throw std::runtime_error("No se pudo ejecutar pspdecrypt");
            WaitForSingleObject(pi.hProcess,INFINITE);CloseHandle(pi.hThread);CloseHandle(pi.hProcess);
            if(!std::filesystem::is_regular_file(elf) || psprecomp::sha256_file(elf)!=kElfSha)throw std::runtime_error("El programa descifrado no es el esperado");
        }
        // Only the Japanese disc: start in Japanese (the language can be changed in Settings when both are installed).
        const auto settings=root/"config"/"settings.ini";
        if(!en){std::filesystem::create_directories(settings.parent_path());
            std::string text;{std::ifstream in(settings);std::string line;while(std::getline(in,line))if(line.rfind("language=",0)!=0)text+=line+"\n";}
            std::ofstream(settings,std::ios::trunc)<<text<<"language=ja\n";}
        std::cout<<"\nDisco instalado"<<(jp && en?" (idioma: Ajustes > Video).":".")<<"\nPara jugar online: dentro del juego pulsa F8.\n";
        return 0;
    }catch(const std::exception& e){std::cout<<"\nERROR: "<<e.what()<<"\nNo se ha completado la instalación.\n";return 1;}
}
}
