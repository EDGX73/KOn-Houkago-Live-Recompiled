#pragma once
#include "native_files.hpp"
#include "native_save_container.hpp"
#include "psprecomp/guest_memory.hpp"
#include <array>
#include <string>

namespace kon {
// Native AUTOLOAD/AUTOSAVE lifecycle. The game state lives in a versioned
// .konpc container; existing encrypted PSP files remain intact and explicit.
class NativeSavedata {
public:
    static constexpr std::uint32_t wrong_type=0x80110005u,invalid_status=0x80110001u,
        invalid_size=0x80110004u,no_data=0x80110307u,file_missing=0x80110309u;
    NativeSavedata(psprecomp::GuestMemory& memory,NativeFiles& files):memory_(memory),files_(files){}
    std::uint32_t init(std::uint32_t pointer,std::uint64_t now){
        advance(now);
        if(status_)return invalid_status;
        if((pointer&3u) || !memory_.contains(pointer,4))return 0x80000103u;
        const auto size=memory_.load32(pointer);
        if(size!=1480u && size!=1500u && size!=1536u)return invalid_size;
        if(!memory_.contains(pointer,size))return 0x80000103u;
        request_.fill(0);request_size_=size;
        for(unsigned n=0;n<size;++n)request_[n]=memory_.load8(pointer+n);
        mode_=word(48);
        if(mode_>1u)throw std::runtime_error("Savedata mode "+std::to_string(mode_)+" requires native storage/dialog implementation");
        game_=name(60,13);save_=name(76,20);file_=name(100,13);
        if(game_.empty() || save_.empty() || file_.empty())throw std::runtime_error("Savedata name-list selection requires native implementation");
        bool disc=false;directory_=files_.resolve("ms0:/PSP/SAVEDATA/"+game_+save_,disc);
        path_=files_.resolve("ms0:/PSP/SAVEDATA/"+game_+save_+"/"+file_,disc);
        native_path_=files_.resolve("ms0:/PSP/SAVEDATA/"+game_+save_+"/"+file_+".konpc",disc);
        pointer_=pointer;started_=true;status_=1u;deadline_=now+200000u;
        return 0;
    }
    std::uint32_t get_status(std::uint64_t now){advance(now);return started_?status_:wrong_type;}
    std::uint32_t update(std::uint64_t now){
        if(!started_)return wrong_type;
        advance(now);if(status_!=2u)return invalid_status;
        if(mode_==1u){save_native();result_=0;memory_.store32(pointer_+28u,result_);status_=3u;return 0;}
        if(std::filesystem::exists(native_path_)){load_native();result_=0;memory_.store32(pointer_+28u,result_);status_=3u;return 0;}
        // Never disguise an existing PSP save as absent, or copy encrypted
        // bytes into the unencrypted buffer supplied by the original game.
        const auto attributes=GetFileAttributesW(directory_.c_str());
        if(attributes==INVALID_FILE_ATTRIBUTES){
            const auto error=GetLastError();
            if(error!=ERROR_FILE_NOT_FOUND && error!=ERROR_PATH_NOT_FOUND)
                throw std::runtime_error("Savedata directory inspection failed: "+std::to_string(error));
            result_=no_data;
        }else{
            if(!(attributes&FILE_ATTRIBUTE_DIRECTORY))throw std::runtime_error("Savedata slot is not a directory");
            const auto data_attributes=GetFileAttributesW(path_.c_str());
            if(data_attributes!=INVALID_FILE_ATTRIBUTES)throw std::runtime_error("Existing savedata needs SFO validation and native decoding");
            const auto error=GetLastError();
            if(error!=ERROR_FILE_NOT_FOUND && error!=ERROR_PATH_NOT_FOUND)
                throw std::runtime_error("Savedata file inspection failed: "+std::to_string(error));
            result_=file_missing;
        }
        memory_.store32(pointer_+28u,result_);status_=3u;return 0;
    }
    std::uint32_t shutdown(std::uint64_t now){
        if(!started_)return wrong_type;
        advance(now);if(status_!=3u)return invalid_status;
        status_=4u;deadline_=now+2000u;return 0;
    }
    std::uint32_t mode()const{return mode_;}
    std::uint32_t result()const{return result_;}
    bool startup_complete()const{return started_ && status_==0u && mode_==0u && (result_==no_data || result_==0u);}
    const std::filesystem::path& path()const{return path_;}
    const std::filesystem::path& native_path()const{return native_path_;}
private:
    psprecomp::GuestMemory& memory_;NativeFiles& files_;
    std::array<std::uint8_t,1536> request_{};
    std::uint32_t pointer_=0,mode_=0,status_=0,result_=0,request_size_=0;
    std::uint64_t deadline_=0;bool started_=false;
    std::string game_,save_,file_;std::filesystem::path path_,directory_,native_path_;
    void validate_identity(const NativeSaveContainer& save)const{
        if(save.game!=game_ || save.slot!=save_ || save.file!=file_)throw std::runtime_error("Native savedata identity mismatch");
        if(request_size_>=1516u)for(unsigned n=0;n<16;++n)if(save.key[n]!=request_[1500u+n])throw std::runtime_error("Native savedata game key mismatch");
    }
    std::vector<std::uint8_t> capture(unsigned pointer,unsigned capacity,unsigned length)const{
        if(length>capacity || length>NativeSaveContainer::max_section || (length && !memory_.contains(pointer,length)))throw std::runtime_error("Native savedata buffer outside mapped memory");
        std::vector<std::uint8_t> data(length);for(unsigned n=0;n<length;++n)data[n]=memory_.load8(pointer+n);return data;
    }
    void save_native(){
        NativeSaveContainer save;save.game=game_;save.slot=save_;save.file=file_;
        save.sections[0]=capture(word(116),word(120),word(124));
        std::copy_n(request_.begin()+128,save.metadata.size(),save.metadata.begin());
        if(request_size_>=1516u)std::copy_n(request_.begin()+1500,16,save.key.begin());
        if(request_size_>=1520u)save.secure_version=word(1516);
        for(unsigned n=0;n<4;++n){const auto descriptor=1412u+n*16u;save.sections[n+1]=capture(word(descriptor),word(descriptor+4u),word(descriptor+8u));}
        save.write(native_path_,word(56)!=0u);
    }
    void load_native(){
        const auto save=NativeSaveContainer::read(native_path_);validate_identity(save);
        const auto validate=[&](unsigned pointer,unsigned capacity,const auto& bytes){if(bytes.size()>capacity || (!bytes.empty() && !memory_.contains(pointer,bytes.size())))throw std::runtime_error("Native savedata destination too small or unmapped");};
        validate(word(116),word(120),save.sections[0]);
        for(unsigned n=0;n<4;++n){const auto descriptor=1412u+n*16u;if(word(descriptor))validate(word(descriptor),word(descriptor+4),save.sections[n+1]);}
        const auto restore=[&](unsigned pointer,const auto& bytes){for(unsigned n=0;n<bytes.size();++n)memory_.store8(pointer+n,bytes[n]);};
        restore(word(116),save.sections[0]);memory_.store32(pointer_+124,static_cast<unsigned>(save.sections[0].size()));restore(pointer_+128,save.metadata);
        for(unsigned n=0;n<4;++n){const auto descriptor=1412u+n*16u;if(word(descriptor)){restore(word(descriptor),save.sections[n+1]);memory_.store32(pointer_+descriptor+8,static_cast<unsigned>(save.sections[n+1].size()));}}
    }
    void advance(std::uint64_t now){if(now>=deadline_){if(status_==1u)status_=2u;else if(status_==4u)status_=0;}}
    std::uint32_t word(unsigned offset)const{return request_[offset]|(std::uint32_t(request_[offset+1])<<8u)|(std::uint32_t(request_[offset+2])<<16u)|(std::uint32_t(request_[offset+3])<<24u);}
    std::string name(unsigned offset,unsigned limit)const{
        std::string value;
        for(unsigned n=0;n<limit && request_[offset+n];++n){const auto ch=request_[offset+n];
            if(ch<' ' || ch=='/' || ch=='\\' || ch==':' || ch=='<' || ch=='>' || ch=='"' || ch=='|' || ch=='?' || ch=='*')
                throw std::runtime_error("Unsafe savedata name");
            value.push_back(static_cast<char>(ch));
        }
        if(value=="." || value==".." || (!value.empty() && (value.back()=='.' || value.back()==' ')))throw std::runtime_error("Unsafe savedata name");
        return value;
    }
};
}
