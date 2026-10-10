#pragma once
#include "psprecomp/guest_memory.hpp"
#include <bit>
#include <cstdint>
#include <limits>
#include <map>
#include <stdexcept>
#include <string>
#include <utility>
namespace kon {
struct LwMutexResult {std::uint32_t error=0,uid=0;bool blocked=false;};
class KernelLwMutexes {
public:
    struct Object {std::string name;std::uint32_t area,attributes;std::int32_t initial_count;};
    std::map<std::uint32_t,Object> objects;
    explicit KernelLwMutexes(psprecomp::GuestMemory& memory):memory_(memory){}
    static constexpr std::uint32_t missing=0x800201CAu,not_locked=0x800201CCu,overflow=0x800201CDu,underflow=0x800201CEu,already_locked=0x800201CFu;
    std::uint32_t create(std::uint32_t uid,std::uint32_t area,std::string name,std::uint32_t attributes,std::int32_t count,std::int32_t thread){
        if(!valid(area))return 0x800200D3u;
        if(attributes>=0x400u)return 0x80020191u;
        if(count<0 || (count>1 && !(attributes&0x200u)))return 0x800201BDu;
        if(!objects.emplace(uid,Object{std::move(name),area,attributes,count}).second)throw std::runtime_error("Duplicate native lightweight mutex UID");
        memory_.zero(area,32);memory_.store32(area,static_cast<std::uint32_t>(count));
        memory_.store32(area+4,count?static_cast<std::uint32_t>(thread):0u);
        memory_.store32(area+8,attributes);memory_.store32(area+16,uid);return 0;
    }
    LwMutexResult acquire(std::uint32_t area,std::int32_t thread,std::int32_t count){
        auto result=lookup(area);if(result.error)return result;
        const auto current=read_count(area),owner=read_count(area+4);const auto attributes=memory_.load32(area+8);
        if(count<=0 || (count>1 && !(attributes&0x200u))){result.error=0x800201BDu;return result;}
        if(current<0)throw std::runtime_error("Negative lightweight mutex counter in game memory");
        if(count>std::numeric_limits<std::int32_t>::max()-current){result.error=overflow;return result;}
        if(current && owner==thread && !(attributes&0x200u)){result.error=already_locked;return result;}
        if(current && owner!=thread){result.blocked=true;return result;}
        memory_.store32(area,static_cast<std::uint32_t>(current+count));memory_.store32(area+4,static_cast<std::uint32_t>(thread));return result;
    }
    LwMutexResult release(std::uint32_t area,std::int32_t thread,std::int32_t count){
        auto result=lookup(area);if(result.error)return result;
        const auto current=read_count(area),owner=read_count(area+4);const auto attributes=memory_.load32(area+8);
        if(count<=0 || (count>1 && !(attributes&0x200u))){result.error=0x800201BDu;return result;}
        if(current<=0 || owner!=thread){result.error=not_locked;return result;}
        if(count>current){result.error=underflow;return result;}
        memory_.store32(area,static_cast<std::uint32_t>(current-count));if(current==count)memory_.store32(area+4,0u);return result;
    }
    LwMutexResult erase(std::uint32_t area){
        auto result=lookup(area);if(result.error)return result;
        objects.erase(result.uid);memory_.store32(area,0);memory_.store32(area+4,0xFFFFFFFFu);memory_.store32(area+16,0xFFFFFFFFu);return result;
    }
    void set_waiters(std::uint32_t uid,std::uint32_t count){
        const auto found=objects.find(uid);if(found==objects.end())return;
        const auto area=found->second.area;
        if(memory_.load32(area+16)==uid)memory_.store32(area+12,count);
    }
private:
    psprecomp::GuestMemory& memory_;
    bool valid(std::uint32_t area) const{return !(area&3u) && memory_.contains(area,32);}
    std::int32_t read_count(std::uint32_t area) const{return std::bit_cast<std::int32_t>(memory_.load32(area));}
    LwMutexResult lookup(std::uint32_t area) const{
        if(!valid(area))return {0x8000020Du,0,false};
        const auto uid=memory_.load32(area+16);const auto found=objects.find(uid);
        if(found==objects.end() || psprecomp::GuestMemory::canonical(found->second.area)!=psprecomp::GuestMemory::canonical(area))return {missing,uid,false};
        return {0,uid,false};
    }
};
}
