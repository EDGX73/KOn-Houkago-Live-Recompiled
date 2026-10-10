#pragma once
#include <atomic>
#include "ge_queue.hpp"
#include "native_memory_view.hpp"
#include "native_glyphs.hpp"
#include "native_mods.hpp"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <stdexcept>
#include <optional>
#if defined(__SSE2__)
#include <emmintrin.h>
#endif
namespace kon {
class NativeTexture {
public:
    NativeTexture(psprecomp::GuestMemory& memory,const GeState& state):memory_(memory),state_(state){
        const auto& r=state.registers;format_=r[0xC3]&15u;stride_=r[0xA8]&0x7FFu;
        width_=1u<<(r[0xB8]&15u);height_=1u<<((r[0xB8]>>8u)&15u);
        address_=(r[0xA0]&0xFFFFF0u)|((r[0xA8]<<8u)&0x0F000000u);
        if(format_>10 || width_>512 || height_>512)throw std::runtime_error("Unsupported native texture format or dimensions: format="+std::to_string(format_)+" width="+std::to_string(width_)+" height="+std::to_string(height_));
        static constexpr unsigned bits[]={16,16,16,32,4,8,16,32,4,8,8};
        // A buffer width of 0 reads rows 16 bytes apart (as PPSSPP does); a
        // width below the texture's makes wider texels run into later rows.
        if(!stride_)stride_=128u/bits[format_];
        if(format_>=8){
            // DXT1 (8): 8-byte blocks; DXT3 (9) and DXT5 (10): 16-byte blocks.
            if((r[0xC2]&1u) || stride_%4u)throw std::runtime_error("Unsupported compressed texture pitch or swizzle");
            const unsigned block_bytes=format_==8?8u:16u;
            bits_=format_==8?4u:8u;row_bytes_=(stride_/4u)*block_bytes;swizzled_=false;
            const auto size=static_cast<std::size_t>(row_bytes_)*((height_+3u)/4u-1u)+((width_+3u)/4u)*block_bytes;
            if(!memory_.contains(address_,size))throw std::runtime_error("Compressed texture outside mapped memory");
            texels_.emplace(memory_,address_,size);
            return;
        }
        bits_=bits[format_];row_bytes_=(stride_*bits_+7u)/8u;
        swizzled_=r[0xC2]&1u;
        if(swizzled_ && (row_bytes_<16 || row_bytes_%16))throw std::runtime_error("Unsupported narrow swizzled texture pitch");
        // Bytes up to the last texel read (the farthest offset for x,y below width,height).
        const auto size=static_cast<std::size_t>(texel_offset(width_-1u,height_-1u))+(bits_>=8u?bits_/8u:1u);
        if(!memory_.contains(address_,size))throw std::runtime_error("Texture image outside mapped memory");
        texels_.emplace(memory_,address_,size);
    }
    static std::uint32_t decode_color(std::uint32_t raw,unsigned format){
        if(format==3)return raw;
        const auto five=[](unsigned v){return (v<<3u)|(v>>2u);};const auto six=[](unsigned v){return (v<<2u)|(v>>4u);};
        if(format==0)return five(raw&31u)|(six((raw>>5u)&63u)<<8u)|(five((raw>>11u)&31u)<<16u)|0xFF000000u;
        if(format==1)return five(raw&31u)|(five((raw>>5u)&31u)<<8u)|(five((raw>>10u)&31u)<<16u)|((raw&0x8000u)?0xFF000000u:0);
        return (raw&15u)*17u|(((raw>>4u)&15u)*17u<<8u)|(((raw>>8u)&15u)*17u<<16u)|(((raw>>12u)&15u)*17u<<24u);
    }
    template<bool ValidateCoordinates=true> std::uint32_t sample(double u,double v,bool linear) const{
        if constexpr(ValidateCoordinates)if(!std::isfinite(u) || !std::isfinite(v) || std::abs(u)>1000000 || std::abs(v)>1000000)throw std::runtime_error("Invalid native texture coordinates");
        // GE texel positions truncate toward zero to sixteenths before
        // choosing texels, including coordinates slightly below zero.
        return sample_fixed(static_cast<int>(u*16.0),static_cast<int>(v*16.0),linear);
    }
    // Texel position in sixteenths (integer rasterizer path, no validation).
    std::uint32_t sample_fixed(int ux,int vy,bool linear) const{
        if(!linear)return fetch(ux>>4,vy>>4);
        const int x=(ux-8)>>4,y=(vy-8)>>4;
        const unsigned fu=(ux-8)&15,fv=(vy-8)&15;
        const std::array<std::uint32_t,4> samples{fetch(x,y),fetch(x+1,y),fetch(x,y+1),fetch(x+1,y+1)};
        // Two 16-bit lanes each hold an 8-bit channel. Weighted sums stay
        // below 4096, so channels cannot carry into each other. Mask after
        // each stage to retain the GE's horizontal truncation exactly.
        constexpr std::uint32_t lanes=0x00FF00FFu;
        const auto pair=[&](unsigned shift){
            const auto a=(samples[0]>>shift)&lanes,b=(samples[1]>>shift)&lanes;
            const auto c=(samples[2]>>shift)&lanes,d=(samples[3]>>shift)&lanes;
            const auto top=((a*(16u-fu)+b*fu)>>4u)&lanes;
            const auto bottom=((c*(16u-fu)+d*fu)>>4u)&lanes;
            return ((top*(16u-fv)+bottom*fv)>>4u)&lanes;
        };
        return pair(0)|(pair(8)<<8u);
    }
    std::span<const std::uint32_t> decoded_pixels()const{return cached_?std::span<const std::uint32_t>(cached_,decoded_texel_count()):std::span<const std::uint32_t>{};}
    unsigned width()const{return width_;}
    unsigned height()const{return height_;}
    std::size_t decoded_texel_count()const{return static_cast<std::size_t>(width_)*height_;}
    // Content token of the cached decode (0: not from a token-tracking cache).
    // Process-wide so tokens never collide between caches; 0 means unknown.
    static std::uint32_t next_decoded_token(){static std::atomic<std::uint32_t> counter{0};auto token=++counter;if(!token)token=++counter;return token;}
    static std::uint64_t& compared_bytes(){static std::uint64_t bytes=0;return bytes;} // diagnostic
    std::uint32_t decoded_token()const{return cached_?cached_token_:0;}
    void prepare_cache(std::vector<std::uint32_t>& storage){
        // One immutable draw only. The caller excludes framebuffer/depth
        // feedback before decoding; the next draw reconstructs this texture.
        cached_=nullptr;cached_token_=0;storage.resize(decoded_texel_count());
        for(unsigned y=0;y<height_;++y)for(unsigned x=0;x<width_;++x)storage[y*width_+x]=fetch(x,y);
        cached_=storage.data();
    }
    // Cache ownership belongs to one renderer. Each draw finishes before the
    // next lookup/eviction, so workers retain an immutable decoded image.
    class Cache {
        friend class NativeTexture;
        struct Entry {
            std::array<std::uint32_t,7> key;
            decltype(GeState::clut) palette{};
            std::vector<std::uint8_t> source;
            std::vector<std::uint32_t> decoded;
            std::uint64_t last_use=0,validated_epoch=0;
            std::uint32_t token=0; // unique per decode: equal tokens mean equal pixels
            std::uint8_t glyph_style=no_glyphs; // button glyphs redrawn for this style
            // Pinned by draws queued for the render thread: never destroyed
            // while pinned. A pinned entry whose source changed turns stale
            // (lookups skip it) and is destroyed when the last pin is released.
            unsigned pins=0;bool stale=false;
            std::size_t bytes()const{return source.size()+decoded.size()*sizeof(std::uint32_t)+sizeof(palette);}
        };
        std::vector<Entry> entries_;
        std::uint64_t clock_=0,epoch_=0;
        unsigned limit_;
        std::size_t byte_limit_;
    public:
        explicit Cache(unsigned entries=16,std::size_t budget=8u*1024u*1024u):limit_(std::clamp(entries,1u,256u)),byte_limit_(std::clamp<std::size_t>(budget,1024u*1024u,128u*1024u*1024u)){}
        std::size_t entries()const{return entries_.size();}
        // Epoch 0 (default) validates bytes on every lookup. A renderer sets a
        // new epoch whenever original code may have run; within one epoch a
        // main-RAM texture validated once cannot change (only GE work runs).
        void set_epoch(std::uint64_t epoch){epoch_=epoch;}
        void pin(std::uint32_t token){for(auto& e:entries_)if(e.token==token){++e.pins;return;}throw std::logic_error("Pinned texture is not cached");}
        void unpin(std::uint32_t token){
            for(auto it=entries_.begin();it!=entries_.end();++it)if(it->token==token){if(it->pins)--it->pins;if(!it->pins && it->stale)entries_.erase(it);return;}
        }
        std::size_t resident_bytes()const{std::size_t bytes=0;for(const auto& entry:entries_)bytes+=entry.bytes();return bytes;}
    };
    bool prepare_cache(Cache& cache){
        cached_=nullptr;
        const bool indexed=format_>=4u && format_<=7u;
        const std::array<std::uint32_t,7> key{address_&0x1FFFFFFFu,width_,height_,stride_,format_,swizzled_?1u:0u,indexed?state_.registers[0xC5]:0u};
        for(auto it=cache.entries_.begin();it!=cache.entries_.end();++it)if(!it->stale && it->key[0]==key[0] && it->key==key){ // address first: cheap reject
            // Exact byte comparison observes direct original-program writes,
            // DMA/file copies and mods without relying on guessed dirty flags.
            const bool vram=((address_&0x1F000000u)==0x04000000u); // GE output may rewrite VRAM textures
            const bool trusted=cache.epoch_ && !vram && it->validated_epoch==cache.epoch_;
            if(!trusted)compared_bytes()+=texels_->size();
            // A texture showing button glyphs drawn for another device decodes again.
            const bool glyphs_current=it->glyph_style==no_glyphs || it->glyph_style==current_glyph_style();
            if(glyphs_current && (!indexed || bytes_equal(it->palette.data(),state_.clut.data(),state_.clut.size())) && (trusted || texels_->matches(it->source))){
                it->last_use=++cache.clock_;it->validated_epoch=cache.epoch_;cached_=it->decoded.data();cached_token_=it->token;return false;
            }
            if(it->pins)it->stale=true;else cache.entries_.erase(it);break;
        }
        const auto needed=texels_->size()+decoded_texel_count()*sizeof(std::uint32_t)+sizeof(state_.clut);
        if(needed>cache.byte_limit_)throw std::runtime_error("Decoded texture exceeds bounded cache budget");
        while(!cache.entries_.empty() && (cache.entries_.size()>=cache.limit_ || cache.resident_bytes()+needed>cache.byte_limit_)){
            // Oldest unpinned entry; with every entry pinned the budget is exceeded until draws finish.
            auto oldest=cache.entries_.end();
            for(auto it=cache.entries_.begin();it!=cache.entries_.end();++it)if(!it->pins && (oldest==cache.entries_.end() || it->last_use<oldest->last_use))oldest=it;
            if(oldest==cache.entries_.end())break;
            cache.entries_.erase(oldest);
        }
        typename Cache::Entry entry;entry.key=key;if(indexed)entry.palette=state_.clut;
        texels_->snapshot(entry.source);prepare_cache(entry.decoded);entry.last_use=++cache.clock_;entry.validated_epoch=cache.epoch_;
        if(glyph_candidate(width_,height_) || texture_dump_enabled() || TextureMods::instance().active()){
            const auto hash=texture_hash(entry.decoded,width_,height_);
            dump_decoded_texture(entry.decoded,width_,height_,hash);
            if(const auto replacement=TextureMods::instance().find(hash,width_,height_))entry.decoded=*replacement; // mods/<name>/textures
            // Glyphs follow the original layout, also on a replaced texture.
            if(has_button_glyphs(hash,width_,height_)){entry.glyph_style=current_glyph_style();apply_button_glyphs(entry.decoded,width_,height_,hash,entry.glyph_style);}
        }
        entry.token=next_decoded_token();
        cache.entries_.push_back(std::move(entry));cached_=cache.entries_.back().decoded.data();cached_token_=cache.entries_.back().token;return true;
    }
    bool parallel_safe(const NativeMemoryView& color,const NativeMemoryView& depth)const{
        return texels_ && texels_->direct() && !texels_->overlaps(color) && !texels_->overlaps(depth);
    }
    template<bool Fast=true> std::uint32_t combine(std::uint32_t vertex,std::uint32_t texture) const{
#if defined(__SSE2__)
        if constexpr(Fast){
            const auto function=state_.registers[0xC9];
            if((function&7u)==0u){
                const auto zero=_mm_setzero_si128();
                const auto c=_mm_unpacklo_epi8(_mm_cvtsi32_si128(static_cast<int>(vertex)),zero);
                const auto t=_mm_unpacklo_epi8(_mm_cvtsi32_si128(static_cast<int>(texture)),zero);
                const auto product=_mm_mullo_epi16(_mm_add_epi16(c,_mm_set1_epi16(1)),t);
                const bool doubled=function&0x10000u,rgba=function&0x100u;
                const auto scaled=doubled?_mm_srli_epi16(product,7):_mm_srli_epi16(product,8);
                const auto bytes=_mm_packus_epi16(scaled,zero);
                const auto rgb=static_cast<std::uint32_t>(_mm_cvtsi128_si32(bytes))&0xFFFFFFu;
                const auto alpha=rgba?((vertex>>24u)+1u)*(texture>>24u)/256u:vertex>>24u;
                return rgb|(alpha<<24u);
            }
        }
#endif
        return combine_generic(vertex,texture);
    }
    std::uint32_t combine_generic(std::uint32_t vertex,std::uint32_t texture) const{
        const auto mode=state_.registers[0xC9]&7u;const bool rgba=state_.registers[0xC9]&0x100u,doubled=state_.registers[0xC9]&0x10000u;
        // Exact identities avoid repeated channel arithmetic for ordinary
        // white UI modulation and non-doubled replacement draws.
        if(!doubled && mode==3)return (texture&0xFFFFFFu)|(rgba?(texture&0xFF000000u):(vertex&0xFF000000u));
        if(!doubled && mode==0 && vertex==0xFFFFFFFFu)return rgba?texture:(texture|0xFF000000u);
        std::uint32_t color=0;
        for(unsigned shift:{0u,8u,16u}){
            const unsigned c=(vertex>>shift)&255u,t=(texture>>shift)&255u,environment=(state_.registers[0xCA]>>shift)&255u,alpha=texture>>24u;
            unsigned value=0;
            switch(mode){
            case 0:value=(c+1u)*t/(doubled?128u:256u);break;
            case 1:value=rgba?(c*(255u-alpha)+t*alpha)/255u:t;if(doubled)value*=2u;break;
            case 2:value=((255u-t)*c+t*environment+255u)/(doubled?128u:256u);break;
            case 3:value=t*(doubled?2u:1u);break;
            case 4:value=(c+t)*(doubled?2u:1u);break;
            default:throw std::runtime_error("Unsupported native texture function");
            }
            color|=std::min(value,255u)<<shift;
        }
        const unsigned alpha=!rgba || mode==1?vertex>>24u:mode==3?texture>>24u:((vertex>>24u)+1u)*(texture>>24u)/256u;
        return color|(alpha<<24u);
    }
private:
    psprecomp::GuestMemory& memory_;const GeState& state_;unsigned format_,width_,height_,stride_,bits_,row_bytes_;std::uint32_t address_;bool swizzled_;
    std::optional<NativeMemoryView> texels_;
    const std::uint32_t* cached_=nullptr;std::uint32_t cached_token_=0;
    unsigned texel_offset(unsigned x,unsigned y) const{
        const unsigned byte_x=x*bits_/8u;
        return swizzled_?(y/8u)*row_bytes_*8u+(byte_x/16u)*128u+(y%8u)*16u+byte_x%16u:y*row_bytes_+byte_x;
    }
    std::uint32_t fetch(int x,int y) const{
        const auto wrap=state_.registers[0xC7];
        x=wrap&1u?std::clamp(x,0,static_cast<int>(width_)-1):x&static_cast<int>(width_-1u);
        y=wrap&0x100u?std::clamp(y,0,static_cast<int>(height_)-1):y&static_cast<int>(height_-1u);
        if(cached_)return cached_[static_cast<unsigned>(y)*width_+static_cast<unsigned>(x)];
        if(format_>=8){
            // PSP DXT: each block starts with the DXT1 colour part (four selector
            // bytes, then two little-endian RGB565 endpoints). DXT3 adds four
            // 16-bit rows of 4-bit alpha; DXT5 a 48-bit index field (32 bits,
            // then 16) and the two alpha endpoints. DXT3/5 colours always use
            // the four-colour mode, as PPSSPP decodes them.
            const unsigned block_bytes=format_==8?8u:16u;
            const auto block=(static_cast<unsigned>(y)/4u)*row_bytes_+(static_cast<unsigned>(x)/4u)*block_bytes;
            const unsigned selector=(texels_->load8(block+(y&3))>>((x&3)*2u))&3u;
            const unsigned first=texels_->load16(block+4u),second=texels_->load16(block+6u);
            const bool three_colour=format_==8 && first<=second;
            if(selector==3u && three_colour)return 0;
            std::uint32_t alpha=255u;
            if(format_==9)alpha=((texels_->load16(block+8u+(y&3)*2u)>>((x&3)*4u))&15u)*17u;
            else if(format_==10){
                const std::uint64_t indices=std::uint64_t(texels_->load32(block+8u))|(std::uint64_t(texels_->load16(block+12u))<<32u);
                const unsigned a0=texels_->load8(block+14u),a1=texels_->load8(block+15u),i=unsigned((indices>>(((y&3)*4u+(x&3))*3u))&7u);
                alpha=i==0?a0:i==1?a1:a0>a1?((8u-i)*a0+(i-1u)*a1)/7u:i==6?0u:i==7?255u:((6u-i)*a0+(i-1u)*a1)/5u;
            }
            const auto unpack=[](unsigned word){return std::array<unsigned,3>{(word>>8u)&248u,(word>>3u)&252u,(word<<3u)&248u};};
            const auto a=unpack(first),b=unpack(second);std::uint32_t result=alpha<<24u;
            for(unsigned channel=0;channel<3;++channel){
                unsigned value=selector==0?a[channel]:selector==1?b[channel]:three_colour?(a[channel]+b[channel])/2u:selector==2?(2u*a[channel]+b[channel])/3u:(a[channel]+2u*b[channel])/3u;
                result|=value<<(channel*8u);
            }
            return result;
        }
        const auto offset=texel_offset(static_cast<unsigned>(x),static_cast<unsigned>(y));
        std::uint32_t raw=bits_==4?(texels_->load8(offset)>>(static_cast<unsigned>(x)&1u)*4u)&15u:bits_==8?texels_->load8(offset):bits_==16?texels_->load16(offset):texels_->load32(offset);
        if(format_<4)return decode_color(raw,format_);
        const auto palette=state_.registers[0xC5];const unsigned palette_format=palette&3u;
        const auto index=(((raw>>((palette>>2u)&31u))&((palette>>8u)&255u))|(((palette>>16u)&31u)<<4u))&(palette_format==3?255u:511u);
        const auto entry=index*(palette_format==3?4u:2u);
        // The CLUT is an immutable per-draw byte array. Validated indices stay
        // inside it; memcpy preserves unaligned little-endian access without
        // rebuilding each palette word one byte at a time for every sample.
        if(palette_format==3){std::memcpy(&raw,state_.clut.data()+entry,4);return raw;}
        std::uint16_t word;std::memcpy(&word,state_.clut.data()+entry,2);
        return decode_color(word,palette_format);
    }
};
}
