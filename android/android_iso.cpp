// Install the disc assets from the player's own ISO over SAF.
//
// The Activity opens the ISO with ACTION_OPEN_DOCUMENT and passes its file
// descriptor here. The ISO9660 image is read directly with that descriptor
// (no mount, no root): every file is extracted to
// <root>/analysis/extracted/<jp|en>, the std.cpk SHA-256 is checked against
// the known Japanese/English discs, and the shared sound bank is made
// available under the Japanese folder as on PC.
#include "psprecomp/sha256.hpp"

#include <jni.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
struct IsoFile {std::string path;std::uint32_t lba=0,size=0;};

class IsoImage {
public:
    explicit IsoImage(int fd){
        file_=fdopen(dup(fd),"rb");
        if(!file_)throw std::runtime_error("No se puede abrir la ISO");
        std::array<std::uint8_t,2048> pvd{};read(16,pvd.data(),2048);
        if(pvd[0]!=1 || std::memcmp(pvd.data()+1,"CD001",5))throw std::runtime_error("No es una imagen ISO 9660");
        walk(u32(pvd.data()+156+2),u32(pvd.data()+156+10),"",0);
    }
    ~IsoImage(){if(file_)std::fclose(file_);}
    IsoImage(const IsoImage&)=delete;IsoImage& operator=(const IsoImage&)=delete;
    const std::vector<IsoFile>& files()const{return files_;}
    const IsoFile* find(const std::string& path)const{for(const auto& f:files_)if(f.path==path)return &f;return nullptr;}
    void extract(const IsoFile& f,const std::filesystem::path& to){
        std::filesystem::create_directories(to.parent_path());
        std::ofstream out(to,std::ios::binary|std::ios::trunc);if(!out)throw std::runtime_error("No se puede escribir "+to.string());
        std::vector<char> buffer(4u<<20);std::uint64_t left=f.size,offset=std::uint64_t(f.lba)*2048u;
        if(fseeko(file_,static_cast<off_t>(offset),SEEK_SET)!=0)throw std::runtime_error("ISO incompleta");
        while(left){const auto n=std::size_t(std::min<std::uint64_t>(left,buffer.size()));if(std::fread(buffer.data(),1,n,file_)!=n)throw std::runtime_error("ISO incompleta");out.write(buffer.data(),std::streamsize(n));left-=n;}
        if(!out)throw std::runtime_error("No hay espacio para "+to.string());
    }
private:
    FILE* file_=nullptr;std::vector<IsoFile> files_;
    static std::uint32_t u32(const std::uint8_t* p){return std::uint32_t(p[0])|(std::uint32_t(p[1])<<8)|(std::uint32_t(p[2])<<16)|(std::uint32_t(p[3])<<24);}
    void read(std::uint32_t lba,std::uint8_t* to,std::size_t size){
        if(fseeko(file_,static_cast<off_t>(std::uint64_t(lba)*2048u),SEEK_SET)!=0)throw std::runtime_error("ISO incompleta");
        if(std::fread(to,1,size,file_)!=size)throw std::runtime_error("ISO incompleta");
    }
    void walk(std::uint32_t lba,std::uint32_t size,const std::string& prefix,int depth){
        if(depth>16 || size>(64u<<20))throw std::runtime_error("Directorio de la ISO no válido");
        std::vector<std::uint8_t> dir((size+2047u)/2048u*2048u);read(lba,dir.data(),dir.size());
        for(std::size_t at=0;at<size;){
            const unsigned length=dir[at];if(!length){at=(at/2048u+1u)*2048u;continue;}
            if(at+length>dir.size() || length<34)break;
            const auto* r=dir.data()+at;const unsigned name_length=r[32];
            if(33u+name_length<=length && !(name_length==1 && (r[33]==0 || r[33]==1))){
                std::string name(reinterpret_cast<const char*>(r+33),name_length);
                if(const auto semicolon=name.find(';');semicolon!=std::string::npos)name.resize(semicolon);
                if(!name.empty() && name.back()=='.')name.pop_back();
                if(name.find("..")!=std::string::npos || name.find('/')!=std::string::npos || name.find('\\')!=std::string::npos)throw std::runtime_error("Nombre no válido en la ISO");
                const auto path=prefix.empty()?name:prefix+"/"+name;
                if(r[25]&2u)walk(u32(r+2),u32(r+10),path,depth+1);else files_.push_back({path,u32(r+2),u32(r+10)});
            }
            at+=length;
        }
    }
};

struct Disc {const char* folder;const char* language;std::uint64_t std_size;const char* std_sha;};
constexpr Disc kDiscs[]{
    {"jp","ja",451382528u,"f41890e40bf127bba9a357f6828e4d80c7140db5559180b8c55a11633c9e8a08"},
    {"en","en",756264416u,"3dfbffced2b4d1eae7ce2969832e04f55332747018f351a176ae984239e37cfe"}};

std::string install(int fd,const std::filesystem::path& root){
    IsoImage image(fd);
    const auto* std_cpk=image.find("PSP_GAME/USRDIR/std.cpk");
    const auto* eboot=image.find("PSP_GAME/SYSDIR/EBOOT.BIN");
    if(!std_cpk || !eboot)throw std::runtime_error("Esta ISO no es de K-On! Houkago Live!!");
    const Disc* disc=nullptr;for(const auto& d:kDiscs)if(d.std_size==std_cpk->size)disc=&d;
    if(!disc)throw std::runtime_error("ISO de K-On no reconocida");
    const auto extracted=root/"analysis"/"extracted";
    const auto target=extracted/disc->folder;
    if(std::filesystem::is_regular_file(target/"PSP_GAME/USRDIR/std.cpk"))return disc->language;
    const auto staging=extracted/(std::string(disc->folder)+".instalando");
    std::filesystem::remove_all(staging);
    std::uint64_t total=0,done=0;for(const auto& f:image.files())total+=f.size;
    std::ofstream progress(root/"install_progress.txt",std::ios::trunc);progress<<"0 "<<total<<"\n";progress.flush();
    for(const auto& f:image.files()){
        image.extract(f,staging/std::filesystem::path(f.path));done+=f.size;
        progress.seekp(0);progress<<(total?done*100u/total:100u)<<" "<<total<<"\n";progress.flush();
    }
    if(psprecomp::sha256_file(staging/"PSP_GAME/USRDIR/std.cpk")!=disc->std_sha)throw std::runtime_error("std.cpk no coincide con el disco original");
    if(psprecomp::sha256_file(staging/"PSP_GAME/SYSDIR/EBOOT.BIN")!="676ce2a0c9caf276e2bf22511fe46dd0cd6cda8d58b57553a8e4029f4addf29d")throw std::runtime_error("EBOOT no coincide con el original");
    // The target may already exist with only the shared snd.cpk (for example
    // after installing the English disc). Rename cannot replace a non-empty
    // directory, so replace the extracted folder; it contains no saves.
    if(std::filesystem::exists(target))std::filesystem::remove_all(target);
    std::filesystem::rename(staging,target);
    // The game always reads the shared sound bank from the Japanese folder.
    const auto jp=extracted/"jp"/"PSP_GAME/USRDIR/snd.cpk";
    const auto from=target/"PSP_GAME/USRDIR/snd.cpk";
    if(!std::filesystem::is_regular_file(jp) && std::filesystem::is_regular_file(from)){
        std::filesystem::create_directories(jp.parent_path());
        std::error_code error;std::filesystem::create_hard_link(from,jp,error);if(error)std::filesystem::copy_file(from,jp);
    }
    std::filesystem::remove(root/"install_progress.txt");
    return disc->language;
}
} // namespace

extern "C" JNIEXPORT jstring JNICALL
Java_com_edgx73_konrecompiled_MainActivity_nativeInstallIso(JNIEnv* env,jobject,jint fd,jstring destination){
    const char* text=env->GetStringUTFChars(destination,nullptr);
    if(!text)return env->NewStringUTF("ERROR: destino no válido");
    const std::string root(text);env->ReleaseStringUTFChars(destination,text);
    std::string result;
    try{
        result=install(fd,std::filesystem::path(root));
    }catch(const std::exception& error){
        result=std::string("ERROR: ")+error.what();
    }
    return env->NewStringUTF(result.c_str());
}
