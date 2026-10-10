#include "guest_arena.hpp"
#include <iostream>
#include <random>
#include <vector>
static void require(bool c,const char* message){if(!c)throw std::runtime_error(message);}
int main(){try {
    using kon::GuestArena;
    GuestArena arena(0x09000000u,0x09010000u);
    auto a=arena.allocate(3,1000,4096);auto b=arena.allocate(4,1000,4096);
    require(arena.block(a)->address==0x09000000u,"low aligned allocation");
    require(arena.block(b)->address==0x0900F000u,"high aligned allocation");
    auto at=arena.allocate(2,512,0x09004234u);
    require(arena.block(at)->address==0x09004200u,"fixed address allocation");
    require(arena.allocate(2,512,0x09004200u)==GuestArena::AllocationFailed,"overlap allowed");
    require(arena.allocate(3,512,3072)==GuestArena::IllegalAlignment,"nonpower alignment allowed");
    require(arena.allocate(6,512)==GuestArena::IllegalType,"bad type allowed");
    require(arena.allocate(0,0xFFFFFFFFu)==GuestArena::AllocationFailed,"size overflow allowed");
    require(arena.release(a)==0 && arena.release(a)==GuestArena::UnknownUid,"double free behavior");
    require(arena.release(b)==0 && arena.release(at)==0,"valid free failed");
    require(arena.total_free()==65536 && arena.max_free()==65536,"free blocks did not coalesce");
    std::mt19937 random(42);std::vector<std::uint32_t> ids;
    for(unsigned step=0;step<3000;step++) {
        if(!ids.empty() && (random()%3)==0) {
            const auto index=random()%ids.size();arena.release(ids[index]);ids.erase(ids.begin()+index);
        } else {
            const auto uid=arena.allocate(random()%2,random()%4096+1);
            if(uid<0x80000000u) ids.push_back(uid);
        }
        std::uint32_t allocated=0;
        for(auto id:ids) {
            const auto block=*arena.block(id);allocated+=block.size;
            require(block.address>=0x09000000u && block.address+block.size<=0x09010000u,"allocation escaped arena");
            for(auto other:ids) if(other!=id) {
                const auto rhs=*arena.block(other);
                require(block.address+block.size<=rhs.address || rhs.address+rhs.size<=block.address,"allocated ranges overlap");
            }
        }
        require(allocated+arena.total_free()==65536,"allocator leaked or created memory");
    }
    for(auto id:ids)arena.release(id);
    require(arena.max_free()==65536,"fragmentation survived complete free");
    std::cout<<"PASS: partition allocation, alignment, bounds, coalescing and 3000 allocation/free steps\n";
    return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
