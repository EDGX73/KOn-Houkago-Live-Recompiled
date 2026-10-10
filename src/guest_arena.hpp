#pragma once
#include <algorithm>
#include <cstdint>
#include <map>
#include <optional>
#include <stdexcept>
namespace kon {
// User partition allocation; all intervals are physical PSP RAM addresses.
// ELF, stacks and host argument pages are excluded by the supplied arena bounds.
class GuestArena {
public:
    static constexpr std::uint32_t AllocationFailed=0x800200D9u;
    static constexpr std::uint32_t IllegalType=0x800200D8u;
    static constexpr std::uint32_t IllegalAlignment=0x800200E4u;
    static constexpr std::uint32_t UnknownUid=0x800200CBu;
    struct Block {std::uint32_t address,size;};
    GuestArena(std::uint32_t begin,std::uint32_t end):begin_(begin),end_(end) {
        if(begin>=end || (begin&255u) || (end&255u)) throw std::invalid_argument("Invalid guest arena bounds");
        free_[begin]=end-begin;
    }
    std::uint32_t allocate(std::uint32_t type,std::uint32_t size,std::uint32_t parameter=0) {
        if(type>4) return IllegalType;
        std::uint64_t alignment=256;
        if(type==3 || type==4) {
            if(!parameter || (parameter&(parameter-1u))) return IllegalAlignment;
            alignment=std::max<std::uint64_t>(alignment,parameter);
        }
        const auto rounded=(static_cast<std::uint64_t>(size)+255u)&~std::uint64_t{255};
        if(!size || rounded>static_cast<std::uint64_t>(end_)-begin_) return AllocationFailed;
        const auto try_interval=[&](auto it)->std::optional<std::uint32_t> {
            const std::uint64_t lo=it->first,hi=lo+it->second;
            std::uint64_t start=(lo+alignment-1u)&~(alignment-1u);
            if(type==1 || type==4) {
                if(hi<rounded) return {};
                start=(hi-rounded)&~(alignment-1u);
            } else if(type==2) start=parameter&~255u;
            if(start<lo || start+rounded>hi) return {};
            const auto addr=static_cast<std::uint32_t>(start);
            free_.erase(it);
            if(start>lo) free_[static_cast<std::uint32_t>(lo)]=static_cast<std::uint32_t>(start-lo);
            if(start+rounded<hi) free_[static_cast<std::uint32_t>(start+rounded)]=static_cast<std::uint32_t>(hi-start-rounded);
            const auto uid=next_uid_++;
            used_[uid]={addr,static_cast<std::uint32_t>(rounded)};
            return uid;
        };
        if(type==1 || type==4) {
            for(auto it=free_.rbegin();it!=free_.rend();++it) {
                auto normal=std::prev(it.base());
                if(const auto uid=try_interval(normal)) return *uid;
            }
        } else {
            for(auto it=free_.begin();it!=free_.end();++it)
                if(const auto uid=try_interval(it)) return *uid;
        }
        return AllocationFailed;
    }
    std::optional<Block> block(std::uint32_t uid) const {
        const auto found=used_.find(uid);if(found==used_.end()) return {};return found->second;
    }
    std::uint32_t release(std::uint32_t uid) {
        const auto found=used_.find(uid);if(found==used_.end()) return UnknownUid;
        auto [address,size]=found->second;used_.erase(found);
        auto next=free_.lower_bound(address);
        if(next!=free_.begin()) {
            auto previous=std::prev(next);
            if(static_cast<std::uint64_t>(previous->first)+previous->second==address) {
                address=previous->first;size+=previous->second;free_.erase(previous);
            }
        }
        if(next!=free_.end() && static_cast<std::uint64_t>(address)+size==next->first) {
            size+=next->second;free_.erase(next);
        }
        free_[address]=size;return 0;
    }
    std::uint32_t total_free() const {std::uint32_t result=0;for(auto [a,size]:free_){(void)a;result+=size;}return result;}
    std::uint32_t max_free() const {std::uint32_t result=0;for(auto [a,size]:free_){(void)a;result=std::max(result,size);}return result;}
private:
    std::uint32_t begin_,end_,next_uid_=0x1000u;
    std::map<std::uint32_t,std::uint32_t> free_;
    std::map<std::uint32_t,Block> used_;
};
}
