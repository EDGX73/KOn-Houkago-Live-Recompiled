#pragma once
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>
// Resource mods by folders, without touching the ISO data:
//   mods/<name>/disc/<path on the disc>   replaces that disc file
//                                         (e.g. disc/PSP_GAME/USRDIR/movie/op.pmf);
//   mods/<name>/textures/<hash>.png|.bmp  replaces a decoded texture of the
//                                         same size (<hash> as written by
//                                         KON_TEXTURE_DUMP: 16 hex digits).
// Folders apply in name order and a later folder wins over an earlier one
// (prefix numbers set the priority: 10-..., 20-...). A folder whose name
// starts with '-' is disabled.
namespace kon {
struct ModFolder {std::string name;std::filesystem::path path;bool enabled=true;};
inline std::vector<ModFolder> list_mods(const std::filesystem::path& root){
    std::vector<ModFolder> mods;std::error_code error;
    const auto directory=root/"mods";
    if(!std::filesystem::is_directory(directory,error))return mods;
    for(const auto& entry:std::filesystem::directory_iterator(directory,error)){
        if(!entry.is_directory(error))continue;
        auto name=entry.path().filename().string();
        mods.push_back({name,entry.path(),!name.starts_with("-")});
    }
    std::sort(mods.begin(),mods.end(),[](const ModFolder& a,const ModFolder& b){return a.name<b.name;});
    return mods;
}
// Texture replacements: an index built at start, pixels decoded on first use
// (by the decoder the host installs, so this header needs no image library).
class TextureMods {
public:
    using Decoder=std::vector<std::uint32_t>(*)(const std::filesystem::path&,unsigned& width,unsigned& height);
    static TextureMods& instance(){static TextureMods mods;return mods;}
    // Later folders override earlier ones; returns the number of textures indexed.
    std::size_t load(const std::vector<ModFolder>& mods,Decoder decoder){
        std::lock_guard lock(mutex_);files_.clear();cache_.clear();decoder_=decoder;std::error_code error;
        for(const auto& mod:mods){
            if(!mod.enabled)continue;
            const auto folder=mod.path/"textures";
            if(!std::filesystem::is_directory(folder,error))continue;
            for(const auto& entry:std::filesystem::directory_iterator(folder,error)){
                const auto stem=entry.path().stem().string();auto ext=entry.path().extension().string();
                std::transform(ext.begin(),ext.end(),ext.begin(),[](unsigned char c){return char(std::tolower(c));});
                if(ext!=".png" && ext!=".bmp")continue;
                const auto hash=parse_hash(stem);if(hash)files_[*hash]=entry.path();
            }
        }
        active_.store(!files_.empty());return files_.size();
    }
    bool active()const{return active_.load(std::memory_order_relaxed);}
    // Replacement pixels for a texture (same size only), or nullptr.
    std::shared_ptr<const std::vector<std::uint32_t>> find(std::uint64_t hash,unsigned width,unsigned height){
        if(!active())return nullptr;
        std::lock_guard lock(mutex_);
        const auto file=files_.find(hash);if(file==files_.end())return nullptr;
        auto cached=cache_.find(hash);
        if(cached==cache_.end()){
            unsigned w=0,h=0;std::vector<std::uint32_t> pixels;
            try{pixels=decoder_?decoder_(file->second,w,h):std::vector<std::uint32_t>{};}catch(...){pixels.clear();}
            Loaded loaded;
            if(!pixels.empty() && pixels.size()==std::size_t(w)*h){loaded.width=w;loaded.height=h;loaded.pixels=std::make_shared<const std::vector<std::uint32_t>>(std::move(pixels));}
            else std::fprintf(stderr,"mod_texture_unreadable=%s\n",file->second.string().c_str());
            cached=cache_.emplace(hash,std::move(loaded)).first;
        }
        if(!cached->second.pixels || cached->second.width!=width || cached->second.height!=height)return nullptr;
        return cached->second.pixels;
    }
    static std::optional<std::uint64_t> parse_hash(const std::string& text){
        if(text.size()<16)return std::nullopt;
        std::uint64_t value=0;
        for(std::size_t i=0;i<16;++i){
            const char c=text[i];unsigned digit;
            if(c>='0' && c<='9')digit=unsigned(c-'0');else if(c>='a' && c<='f')digit=unsigned(c-'a'+10);else if(c>='A' && c<='F')digit=unsigned(c-'A'+10);else return std::nullopt;
            value=(value<<4)|digit;
        }
        if(text.size()>16 && text[16]!='_')return std::nullopt; // "<hash>_<w>x<h>" as dumped is accepted
        return value;
    }
private:
    struct Loaded {unsigned width=0,height=0;std::shared_ptr<const std::vector<std::uint32_t>> pixels;};
    std::mutex mutex_;std::map<std::uint64_t,std::filesystem::path> files_;std::map<std::uint64_t,Loaded> cache_;
    Decoder decoder_=nullptr;std::atomic<bool> active_{false};
};
}
