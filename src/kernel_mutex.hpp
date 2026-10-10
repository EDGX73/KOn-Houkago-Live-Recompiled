#pragma once
#include <cstdint>
#include <limits>
#include <map>
#include <string>

namespace kon {
struct MutexAcquisition {std::uint32_t error=0;bool blocked=false;};
class KernelMutexes {
public:
    struct Mutex {std::string name;std::uint32_t attributes;std::int32_t count,owner;};
    std::map<std::uint32_t,Mutex> objects;
    static constexpr std::uint32_t recursive=0x200u;
    std::uint32_t create(std::uint32_t uid,std::string name,std::uint32_t attributes,std::int32_t count,std::int32_t thread) {
        if(attributes&~0xBFFu)return 0x80020191u;
        if(count<0 || (count>1 && !(attributes&recursive)))return 0x800201BDu;
        objects.emplace(uid,Mutex{name.substr(0,31),attributes,count,count?thread:-1});return 0;
    }
    MutexAcquisition acquire(std::uint32_t uid,std::int32_t thread,std::int32_t count) {
        const auto found=objects.find(uid);if(found==objects.end())return {0x800201C3u,false};
        auto& m=found->second;
        if(count<=0 || (count>1 && !(m.attributes&recursive)))return {0x800201BDu,false};
        if(count>std::numeric_limits<std::int32_t>::max()-m.count)return {0x800201C6u,false};
        if(m.owner==thread && !(m.attributes&recursive))return {0x800201C8u,false};
        if(m.count && m.owner!=thread)return {0,true};
        m.count+=count;m.owner=thread;return {};
    }
    std::uint32_t release(std::uint32_t uid,std::int32_t thread,std::int32_t count) {
        const auto found=objects.find(uid);if(found==objects.end())return 0x800201C3u;
        auto& m=found->second;
        if(count<=0 || (count>1 && !(m.attributes&recursive)))return 0x800201BDu;
        if(!m.count || m.owner!=thread)return 0x800201C5u;
        if(count>m.count)return 0x800201C7u;
        m.count-=count;if(!m.count)m.owner=-1;return 0;
    }
};
}
