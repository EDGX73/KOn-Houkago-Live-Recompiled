#include "ge_queue.hpp"
#include <iostream>
#include <stdexcept>
void check(bool c,const char* message){if(!c)throw std::runtime_error(message);}
int main(){try {
    psprecomp::Runtime rt;auto& mem=rt.memory();kon::GeQueue ge(mem);
    constexpr std::uint32_t list=0x08810000u;
    const std::uint32_t words[]={0x10080000u,0x01012340u,0x3E000000u,0x3F3F8000u,0x0F00002Au,0x0C000000u};
    for(unsigned n=0;n<6;n++)mem.store32(list+n*4u,words[n]);
    unsigned draws=0,finishes=0;auto draw=[&](auto&,auto,unsigned,unsigned){++draws;};
    auto finish=[&](const auto& l,auto arg,auto pc){check(l.status==0 && arg==42 && pc==list+24,"Wrong completed list/finish payload");++finishes;};
    const auto id=ge.enqueue(list,list+8u,-1);ge.process(draw,finish);
    check(ge.list_status(id)==3 && finishes==0 && ge.state.vertex_address==0x08012340u,"Stall should preserve state and pending completion");
    check(ge.update_stall(id,0)==0,"Stall update failed");ge.process(draw,finish);
    check(ge.list_status(id)==0 && ge.draw_status()==0 && finishes==1 && draws==0 && ge.state.projection[0]==1.f,"State-only list/matrix did not complete");
    mem.store32(list,0x04030003u);mem.store32(list+4u,0x0F000000u);mem.store32(list+8u,0x0C000000u);
    const auto primitive=ge.enqueue(list,0,-1);unsigned count=0;
    ge.process([&](auto&,auto pc,unsigned kind,unsigned vertices){check(pc==list && kind==3 && vertices==3,"Primitive request changed");count=vertices;},[](auto&,auto,auto){});
    check(count==3 && ge.list_status(primitive)==0,"Draw sink was not actually invoked");
    mem.store32(list,0x0A810020u);mem.store32(list+4u,0x0F000000u);mem.store32(list+8u,0x0C000000u);
    mem.store32(list+32u,0x5500A55Au);mem.store32(list+36u,0x0B000000u);
    ge.enqueue(list,0,-1);ge.process(draw,[](auto&,auto,auto){});
    check(ge.state.registers[0x55]==0x5500A55Au,"GE CALL/RET failed");
    check(ge.enqueue(list+1,0,-1)==0x80000103u,"Unaligned GE list permitted");
    constexpr std::uint32_t palette=0x08812000u;
    for(unsigned n=0;n<64;++n)mem.store8(palette+n,static_cast<std::uint8_t>(n+1));
    const std::uint32_t palette_list[]={0xB0812000u,0xB1080000u,0xC4000002u,0x0F000000u,0x0C000000u};
    for(unsigned n=0;n<5;++n)mem.store32(list+n*4u,palette_list[n]);ge.enqueue(list,0,-1);ge.process(draw,[](auto&,auto,auto){});
    check(ge.state.clut_loads==1 && ge.state.clut_loaded_bytes==64 && ge.state.clut[0]==1 && ge.state.clut[63]==64,"Palette load did not copy original guest bytes");
    mem.store8(palette,99);mem.store32(list+8,0xC4000001u);ge.enqueue(list,0,-1);ge.process(draw,[](auto&,auto,auto){});
    check(ge.state.clut[0]==99 && ge.state.clut[63]==64 && ge.state.clut_loaded_bytes==32,"Partial palette load destroyed retained entries");
    mem.store32(list+8,0xC4000040u);ge.enqueue(list,0,-1);ge.process(draw,[](auto&,auto,auto){});
    check(ge.state.clut_loads==2 && ge.state.clut[0]==99,"Masked zero-size palette command did not remain a no-op");
    const auto before=ge.state.vertex_address;mem.store32(list,0xFF123456u);mem.store32(list+4,0x0F000000u);mem.store32(list+8,0x0C000000u);
    unsigned nop_finishes=0;ge.enqueue(list,0,-1);ge.process(draw,[&](auto&,auto,auto){++nop_finishes;});
    check(nop_finishes==1 && ge.state.vertex_address==before,"NOP FF changed execution or blocked completion");
    // Host quantum boundaries preserve CALL/RET, vertex consumption, primitive
    // order, and the one actual completion after FINISH/END.
    kon::GeQueue full(mem),sliced(mem);constexpr unsigned slice_list=0x08820000u;
    const std::array<unsigned,9> slice_words{0x10080000u,0x01012340u,0x04030003u,0x0A820040u,0x04030006u,0x0F00002Au,0x0C000000u,0u,0u};
    for(unsigned n=0;n<slice_words.size();++n)mem.store32(slice_list+n*4u,slice_words[n]);
    mem.store32(slice_list+64,0x04030009u);mem.store32(slice_list+68,0x0B000000u);
    full.enqueue(slice_list,0,-1);const auto sliced_id=sliced.enqueue(slice_list,0,-1);
    std::vector<unsigned> full_order,sliced_order;unsigned full_done=0,sliced_done=0;
    full.process([&](auto& s,auto pc,auto,auto n){full_order.push_back(pc);s.vertex_address+=n*12u;},[&](auto&,auto,auto){++full_done;});
    sliced.process([&](auto& s,auto pc,auto,auto n){sliced_order.push_back(pc);s.vertex_address+=n*12u;},[&](auto&,auto,auto){++sliced_done;},[]{return true;});
    check(sliced.list_status(sliced_id)==2 && sliced_done==0 && sliced_order.size()==1 && sliced.lists.at(sliced_id).pc==slice_list+12,"Quantum invented list completion or lost PC");
    for(unsigned n=0;n<10 && !sliced.queue.empty();++n)sliced.process([&](auto& s,auto pc,auto,auto count){sliced_order.push_back(pc);s.vertex_address+=count*12u;},[&](auto&,auto,auto){++sliced_done;},[]{return true;});
    check(full_order==sliced_order && full_done==1 && sliced_done==1 && sliced.state.registers==full.state.registers && sliced.state.vertex_address==full.state.vertex_address && sliced.lists.at(sliced_id).stack.empty(),"Slicing changed command state/order or callbacks");
    // A yielding infinite command loop still reaches the diagnostic guard.
    kon::GeQueue looping(mem);mem.store32(slice_list,0x10080000u);mem.store32(slice_list+4,0x08820004u);looping.enqueue(slice_list,0,-1);
    bool loop_rejected=false;try{for(unsigned n=0;n<600;++n)looping.process(draw,[](auto&,auto,auto){},[]{return true;});}catch(const std::runtime_error& e){loop_rejected=std::string(e.what()).find("budget exhausted")!=std::string::npos;}
    check(loop_rejected,"Host yields disabled the GE infinite-loop guard");
    std::cout<<"PASS: GE queue/state, matrices, draw dispatch, CALL/RET, NOP FF retained palette uploads and resumable host quanta\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<"\n";return 1;}}
