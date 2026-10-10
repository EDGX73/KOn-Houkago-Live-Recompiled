#pragma once
#include "psprecomp/guest_memory.hpp"
#include "native_bytes.hpp"
#include <cassert>
#include <bit>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <vector>
#include <span>
namespace kon {
// The diagnostic write watch is fixed for the process; read the environment
// once instead of on every draw and memory view.
inline bool native_write_watch_enabled(){static const bool enabled=std::getenv("PSPRECOMP_WATCH_WRITE")!=nullptr;return enabled;}
// Resolve a validated contiguous region once for native drawing. Callers keep
// each offset inside this range. Mirrored ranges crossing VRAM's end and writes
// with diagnostic watches retain the normal checked GuestMemory accessors.
class NativeMemoryView {
public:
    NativeMemoryView(psprecomp::GuestMemory& memory,std::uint32_t address,std::size_t length,bool writing=false,bool checked_access=false)
        :memory_(memory),address_(address),length_(length){
        static_assert(std::endian::native==std::endian::little);
        if(!memory.contains(address,length))throw std::runtime_error("Native drawing view outside mapped memory");
        if(!checked_access && !(writing && native_write_watch_enabled()))pointer_=memory.raw_pointer(address,length);
    }
    std::uint8_t load8(std::size_t offset)const{bounds(offset,1);return pointer_?pointer_[offset]:memory_.load8(address_+static_cast<unsigned>(offset));}
    std::uint16_t load16(std::size_t offset)const{bounds(offset,2);if(!pointer_)return memory_.load16(address_+static_cast<unsigned>(offset));std::uint16_t value;std::memcpy(&value,pointer_+offset,2);return value;}
    std::uint32_t load32(std::size_t offset)const{bounds(offset,4);if(!pointer_)return memory_.load32(address_+static_cast<unsigned>(offset));std::uint32_t value;std::memcpy(&value,pointer_+offset,4);return value;}
    void store16(std::size_t offset,std::uint16_t value){bounds(offset,2);if(pointer_)std::memcpy(pointer_+offset,&value,2);else memory_.store16(address_+static_cast<unsigned>(offset),value);}
    void store32(std::size_t offset,std::uint32_t value){bounds(offset,4);if(pointer_)std::memcpy(pointer_+offset,&value,4);else memory_.store32(address_+static_cast<unsigned>(offset),value);}
    bool matches(const std::vector<std::uint8_t>& bytes)const{
        if(bytes.size()!=length_)return false;
        if(pointer_)return !length_ || bytes_equal(pointer_,bytes.data(),length_);
        for(std::size_t n=0;n<length_;++n)if(load8(n)!=bytes[n])return false;
        return true;
    }
    void snapshot(std::vector<std::uint8_t>& bytes)const{
        bytes.resize(length_);
        if(pointer_){if(length_)std::memcpy(bytes.data(),pointer_,length_);return;}
        for(std::size_t n=0;n<length_;++n)bytes[n]=load8(n);
    }
    // One range validation allows bulk transfer to GPU staging. Packed 16-bit
    // pixels expand to words; mirrored/watch paths retain checked access.
    // Words before `first` are left unchanged: an open GPU batch already owns
    // that prefix and only consumes the newly extended tail.
    void read_words(std::vector<std::uint32_t>& words,unsigned bytes,std::size_t first=0)const{
        if((bytes!=2 && bytes!=4) || length_%bytes)throw std::invalid_argument("Invalid packed word read");
        words.resize(length_/bytes);if(first>=words.size())return;
        if(pointer_ && bytes==4){std::memcpy(words.data()+first,pointer_+first*4,length_-first*4);return;}
        if(pointer_){const auto* in=pointer_;for(std::size_t n=first;n<words.size();++n){std::uint16_t value;std::memcpy(&value,in+n*2,2);words[n]=value;}return;}
        for(std::size_t n=first;n<words.size();++n){
            if(pointer_){std::uint16_t value;std::memcpy(&value,pointer_+n*2,2);words[n]=value;}
            else words[n]=bytes==4?memory_.load32(address_+unsigned(n*4)):memory_.load16(address_+unsigned(n*2));
        }
    }
    void write_words(std::span<const std::uint32_t> words,unsigned bytes){
        if((bytes!=2 && bytes!=4) || words.size()>length_/bytes)throw std::invalid_argument("Invalid packed word write");
        if(pointer_ && bytes==4){if(!words.empty())std::memcpy(pointer_,words.data(),words.size_bytes());return;}
        if(pointer_){auto* out=pointer_;for(std::size_t n=0;n<words.size();++n){const auto value=static_cast<std::uint16_t>(words[n]);std::memcpy(out+n*2,&value,2);}return;}
        for(std::size_t n=0;n<words.size();++n){
            if(pointer_){const auto value=static_cast<std::uint16_t>(words[n]);std::memcpy(pointer_+n*2,&value,2);}
            else if(bytes==4)memory_.store32(address_+unsigned(n*4),words[n]);else memory_.store16(address_+unsigned(n*2),static_cast<std::uint16_t>(words[n]));
        }
    }
    std::size_t size()const{return length_;}
    bool direct()const{return pointer_!=nullptr;}
    const std::uint8_t* data()const{return pointer_;} // direct views only
    bool overlaps(const NativeMemoryView& other)const{
        if(!length_ || !other.length_)return false;
        if(!pointer_ || !other.pointer_)return true;
        const auto a=reinterpret_cast<std::uintptr_t>(pointer_),b=reinterpret_cast<std::uintptr_t>(other.pointer_);
        return a<b+other.length_ && b<a+length_;
    }
private:
    psprecomp::GuestMemory& memory_;std::uint32_t address_;std::size_t length_;std::uint8_t* pointer_=nullptr;
    void bounds(std::size_t offset,std::size_t width)const{assert(width<=length_ && offset<=length_-width);}
};
}
