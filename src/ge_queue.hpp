#pragma once
#include "psprecomp/runtime.hpp"
#include "psprecomp/common.hpp"
#include <array>
#include <bit>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <stdexcept>
#include <vector>
namespace kon {
struct GeState {
    std::array<std::uint32_t,256> registers{};
    std::array<std::uint8_t,2048> clut{};
    std::uint32_t clut_loads=0,clut_loaded_bytes=0;
    std::array<float,96> bones{};
    std::array<float,12> world{},view{},texture_matrix{};
    std::array<float,16> projection{};
    std::uint32_t vertex_address=0,index_address=0,offset=0;
    unsigned bone_index=0,world_index=0,view_index=0,projection_index=0,texture_index=0;
    static float float24(std::uint32_t data){return std::bit_cast<float>((data&0xFFFFFFu)<<8u);}
    std::uint32_t relative(std::uint32_t data) const {
        return ((((registers[0x10]&0xFF0000u)<<8u)|(data&0xFFFFFFu))+offset)&0x0FFFFFFFu;
    }
};
class GeQueue {
public:
    struct List {
        std::uint32_t id,pc,stall;
        std::int32_t callback;
        unsigned status=1,stack_limit=32;
        bool finished_command=false;
        std::uint32_t finish_argument=0;
        std::vector<std::uint32_t> stack;
        std::uint64_t commands=0;
        unsigned commands_since_stall=0;
    };
    using Draw=std::function<void(GeState&,std::uint32_t,unsigned,unsigned)>;
    using Finish=std::function<void(const List&,std::uint32_t,std::uint32_t)>;
    using Yield=std::function<bool()>;
    GeState state;
    std::map<std::uint32_t,List> lists;
    std::deque<std::uint32_t> queue;
    explicit GeQueue(psprecomp::GuestMemory& memory):memory_(memory){}
    std::uint32_t enqueue(std::uint32_t address,std::uint32_t stall,std::int32_t callback,bool front=false) {
        address=psprecomp::GuestMemory::canonical(address);stall=psprecomp::GuestMemory::canonical(stall);
        if((address&3u) || (stall&3u) || !memory_.contains(address,4))return 0x80000103u;
        std::uint32_t id=0;while(id<64 && lists.contains(id) && lists.at(id).status!=0)++id;
        if(id==64)return 0x80020190u;
        lists.insert_or_assign(id,List{id,address,stall,callback});
        if(front)queue.push_front(id);else queue.push_back(id);return id;
    }
    std::uint32_t update_stall(std::uint32_t id,std::uint32_t stall) {
        const auto it=lists.find(id);if(it==lists.end())return 0x80000100u;
        stall=psprecomp::GuestMemory::canonical(stall);if(stall&3u)return 0x80000103u;
        it->second.stall=stall;if(it->second.status==3)it->second.status=1;return 0;
    }
    unsigned list_status(std::uint32_t id) const {
        const auto it=lists.find(id);return it==lists.end()?0x80000100u:it->second.status;
    }
    unsigned draw_status() const{return queue.empty()?0u:lists.at(queue.front()).status;}
    // An optional host quantum returns only between complete commands. Pending
    // lists keep status/PC/stack/state and do not signal completion early.
    void process(const Draw& draw,const Finish& finish,const Yield& yield={}) {
        unsigned quantum_commands=0;
        while(!queue.empty()) {
            auto& list=lists.at(queue.front());
            if(list.status==0){queue.pop_front();continue;}
            if(list.pc==list.stall){list.status=3;list.commands_since_stall=0;return;}
            // The loop guard spans host yields, so slicing cannot hide a loop.
            if(list.commands_since_stall++==65536u)throw std::runtime_error("GE command budget exhausted without END/stall");
            if(!memory_.contains(list.pc,4))throw std::runtime_error("GE list PC outside mapped memory");
            const auto pc=list.pc,word=memory_.aot_load32(pc),op=word>>24u,data=word&0xFFFFFFu;
            list.pc+=4u;list.status=2;++list.commands;state.registers[op]=word;
            switch(op) {
            case 0:case 0xFF:break; // Both GE NOP encodings have no execution effect.
            case 1:state.vertex_address=state.relative(data);break;
            case 2:state.index_address=state.relative(data);break;
            case 4:
                if(data&0xFFFFu)draw(state,pc,(data>>16u)&7u,data&0xFFFFu);
                break;
            case 5:case 6:case 7:case 9:
                throw std::runtime_error("Unsupported GE curve/bounds instruction "+psprecomp::hex32(word));
            case 8:list.pc=state.relative(data)&~3u;break;
            case 10:
                if(list.stack.size()>=list.stack_limit)throw std::runtime_error("GE CALL stack overflow");
                list.stack.push_back(list.pc);list.pc=state.relative(data)&~3u;break;
            case 11:
                if(list.stack.empty())throw std::runtime_error("GE RET without CALL");
                list.pc=list.stack.back();list.stack.pop_back();break;
            case 12:
                if(!list.finished_command)throw std::runtime_error("GE END without FINISH requires signal semantics");
                list.status=0;finish(list,list.finish_argument,list.pc);queue.pop_front();break;
            case 14:throw std::runtime_error("Unsupported GE signal "+psprecomp::hex32(word));
            case 15:list.finished_command=true;list.finish_argument=data&0xFFFFu;break;
            case 0x10:break;
            case 0x13:state.offset=data<<8u;break;
            case 0x14:state.offset=pc;break;
            case 0x2A:state.bone_index=data&127u;break;
            case 0x2B:
                if(state.bone_index<state.bones.size())state.bones[state.bone_index]=GeState::float24(data);
                state.bone_index=(state.bone_index+1u)&127u;break;
            case 0x3A:state.world_index=data&15u;break;
            case 0x3B:matrix_data(state.world,state.world_index,data);break;
            case 0x3C:state.view_index=data&15u;break;
            case 0x3D:matrix_data(state.view,state.view_index,data);break;
            case 0x3E:state.projection_index=data&15u;break;
            case 0x3F:matrix_data(state.projection,state.projection_index,data);break;
            case 0x40:state.texture_index=data&15u;break;
            case 0x41:matrix_data(state.texture_matrix,state.texture_index,data);break;
            case 0xC4:
                if(data&0x3Fu){
                    const auto bytes=(data&0x3Fu)*32u;
                    const auto source=(state.registers[0xB0]&0x00FFFFF0u)|((state.registers[0xB1]<<8u)&0x0F000000u);
                    if(!memory_.contains(source,bytes))throw std::runtime_error("GE CLUT source outside mapped memory");
                    for(unsigned n=0;n<bytes;++n)state.clut[n]=memory_.aot_load8(source+n); // fast path; the slow one resolves every byte
                    ++state.clut_loads;state.clut_loaded_bytes=bytes;
                }
                break;
            case 0xEA:throw std::runtime_error("GE framebuffer transfer requires renderer");
            default:
                if(!state_register(op))throw std::runtime_error("Unknown GE command "+psprecomp::hex32(word)+" pc="+psprecomp::hex32(pc));
                break;
            }
            ++quantum_commands;
            if(yield && (op==4 || quantum_commands%128u==0) && yield())return;
        }
    }
private:
    psprecomp::GuestMemory& memory_;
    template<std::size_t N>static void matrix_data(std::array<float,N>& matrix,unsigned& index,std::uint32_t data) {
        if(index<N)matrix[index]=GeState::float24(data);index=(index+1u)&15u;
    }
    static bool state_register(unsigned op) {
        return op==0x12 || (op>=0x15 && op<=0x28) || (op>=0x2C && op<=0x33) ||
               (op>=0x36 && op<=0x38) || (op>=0x42 && op<=0x4D) || op==0x50 || op==0x51 ||
               (op>=0x53 && op<=0x58) || (op>=0x5B && op<=0xB5) ||
               (op>=0xB8 && op<=0xD0) || (op>=0xD2 && op<=0xE9) || op==0xEB || op==0xEC || op==0xEE;
    }
};
}
