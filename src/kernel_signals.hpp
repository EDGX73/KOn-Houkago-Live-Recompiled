#pragma once
#include <cstdint>
#include <map>
#include <string>
namespace kon {
struct EventResult {std::uint32_t error=0,pattern=0;bool blocked=false;};
struct SemaphoreResult {std::uint32_t error=0;bool blocked=false;};
class KernelSignals {
public:
    struct Event {std::string name;std::uint32_t attributes,pattern;};
    struct Semaphore {std::string name;std::uint32_t attributes;std::int32_t initial,count,maximum;};
    std::map<std::uint32_t,Event> events;
    std::map<std::uint32_t,Semaphore> semaphores;
    std::uint32_t create_event(std::uint32_t uid,std::string name,std::uint32_t attributes,std::uint32_t pattern) {
        if(attributes>=0x300u || (attributes&0x100u))return 0x80020191u;
        events.emplace(uid,Event{name.substr(0,31),attributes,pattern});return 0;
    }
    std::uint32_t set_event(std::uint32_t uid,std::uint32_t bits) {
        const auto f=events.find(uid);if(f==events.end())return 0x8002019Au;
        f->second.pattern|=bits;return 0;
    }
    std::uint32_t clear_event(std::uint32_t uid,std::uint32_t keep_bits) {
        const auto f=events.find(uid);if(f==events.end())return 0x8002019Au;
        f->second.pattern&=keep_bits;return 0;
    }
    EventResult acquire_event(std::uint32_t uid,std::uint32_t bits,std::uint32_t mode) {
        if(mode&~0x31u)return {0x80020195u,0,false};
        if(!bits)return {0x800201B1u,0,false};
        const auto f=events.find(uid);if(f==events.end())return {0x8002019Au,0,false};
        auto& pattern=f->second.pattern;const auto before=pattern;
        const bool match=(mode&1u)?((pattern&bits)!=0):((pattern&bits)==bits);
        if(!match)return {0,before,true};
        if(mode&0x20u)pattern&=~bits;
        if(mode&0x10u)pattern=0;
        return {0,before,false};
    }
    std::uint32_t create_semaphore(std::uint32_t uid,std::string name,std::uint32_t attributes,std::int32_t initial,std::int32_t maximum) {
        if(attributes>=0x200u)return 0x80020191u;
        if(initial<0 || maximum<1 || initial>maximum)return 0x800201BDu;
        semaphores.emplace(uid,Semaphore{name.substr(0,31),attributes,initial,initial,maximum});return 0;
    }
    SemaphoreResult acquire_semaphore(std::uint32_t uid,std::int32_t count) {
        const auto f=semaphores.find(uid);if(f==semaphores.end())return {0x80020199u,false};
        auto& s=f->second;if(count<=0 || count>s.maximum)return {0x800201BDu,false};
        if(s.count<count)return {0,true};
        s.count-=count;return {};
    }
    std::uint32_t cancel_semaphore(std::uint32_t uid,std::int32_t desired) {
        const auto f=semaphores.find(uid);if(f==semaphores.end())return 0x80020199u;
        auto& s=f->second;if(desired>s.maximum)return 0x800201BDu;
        s.count=desired<0?s.initial:desired;return 0;
    }
    std::uint32_t signal_semaphore(std::uint32_t uid,std::int32_t count) {
        const auto f=semaphores.find(uid);if(f==semaphores.end())return 0x80020199u;
        auto& s=f->second;if(count<=0)return 0x800201BDu;
        if(static_cast<std::int64_t>(s.count)+count>s.maximum)return 0x800201AEu;
        s.count+=count;return 0;
    }
};
}
