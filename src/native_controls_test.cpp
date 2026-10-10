#include "control_replay.hpp"
#include "native_controls.hpp"
#include "control_replay.hpp"
#include <iostream>
#include <stdexcept>
void check(bool condition,const char* text){if(!condition)throw std::runtime_error(text);}
int main(){try{
    // Independent expected PSP masks cover every gameplay button, rather than
    // copying the binding table into the expectations.
    const std::array<unsigned,12> codes{8,13,38,39,40,37,'Q','W','S','X','Z','A'};
    const std::array<std::uint32_t,12> masks{1,8,16,32,64,128,256,512,4096,8192,16384,32768};
    for(unsigned n=0;n<codes.size();++n){
        auto pad=kon::keyboard_pad([&](unsigned code){return code==codes[n];});
        check(pad.buttons==masks[n] && pad.x==128 && pad.y==128,"Keyboard gameplay button mapping incorrect");
    }
    auto chord=kon::keyboard_pad([](unsigned code){return code=='Q'||code=='W'||code=='X'||code=='Z'||code=='J'||code=='I';});
    check(chord.buttons==0x6300 && chord.x==0 && chord.y==0,"Simultaneous gameplay buttons or analog diagonal lost");
    auto opposite=kon::keyboard_pad([](unsigned code){return code=='J'||code=='L'||code=='I'||code=='K';});
    check(opposite.buttons==0 && opposite.x==128 && opposite.y==128,"Opposite analog directions must cancel");
    auto lower_right=kon::keyboard_pad([](unsigned code){return code=='L'||code=='K';});
    check(lower_right.x==255 && lower_right.y==255,"Analog right/down axes inverted");
    kon::NativeKeyboard keyboard;keyboard.down('X',false);keyboard.down(13,false);
    check(keyboard.consume()==0x2008 && keyboard.consume()==0,"Short keyboard presses were lost or repeated");
    keyboard.down('X',true);check(keyboard.consume()==0,"Automatic key repeat invented a new press");
    keyboard.down('X',false);keyboard.lose_focus();check(keyboard.consume()==0,"Unfocused key input leaked into game");
    keyboard.down('X',false);keyboard.down(13,false);
    check(keyboard.peek()==0x2008 && keyboard.peek()==0x2008,"Pending press disappeared before delivery");
    keyboard.acknowledge(8);check(keyboard.peek()==0x2000,"Acknowledging one delivered key cleared another");
    kon::NativeControls taps([&]{return kon::HostPad{keyboard.peek(),128,128};});taps.set_cycle(11111,0);
    psprecomp::GuestMemory tap_memory;const auto tap_out=0x08800000u;
    taps.update(11111);taps.update(22222);taps.update(33333);taps.read(tap_memory,tap_out,1);
    keyboard.acknowledge(tap_memory.load32(tap_out+4));
    check(tap_memory.load32(tap_out+4)==0x2000 && keyboard.peek()==0,"Newest sample lost an unread short press");
    taps.update(44444);taps.read(tap_memory,tap_out,1);
    check(tap_memory.load32(tap_out+4)==0,"Delivered tap was repeated in a later sample");
    kon::ControlReplay replay;replay.events={{100,{8,0,255}},{200,{0,128,128}}};
    check(replay.at(99).buttons==0 && replay.at(100).buttons==8 && replay.at(199).y==255 && replay.at(200).buttons==0,"Replay boundaries lost controller presses/releases");
    kon::ControlReplay short_replay;short_replay.events={{15000,{8,128,128}},{35000,{0,128,128}}};
    kon::NativeControls timed([&](std::uint64_t tick){return short_replay.at(tick);});timed.set_cycle(10000,0);
    timed.update(50000);check(timed.read(tap_memory,tap_out,8)==5,"Timed source lost delayed sampling deadlines");
    const std::array<unsigned,5> expected_presses{0,8,8,0,0};
    for(unsigned n=0;n<5;++n)check(tap_memory.load32(tap_out+n*16)==(n+1)*10000 && tap_memory.load32(tap_out+n*16+4)==expected_presses[n],"Render stall erased a replay pulse or backdated its release");
    timed.update(2003333);check(timed.read(tap_memory,tap_out,64)==63,"Timed ring exceeded its bound");
    for(unsigned n=0;n<63;++n)check(tap_memory.load32(tap_out+n*16)==1380000+n*10000,"Ring trimming changed the sampling clock phase");
    psprecomp::GuestMemory memory;kon::HostPad input{0x4000,255,0};kon::NativeControls controls([&]{return input;});
    const auto out=0x08800000u;
    controls.set_cycle(11111,0);controls.set_mode(1);controls.update(11110);
    check(!controls.available(),"Input became readable before sampling deadline");controls.update(11111);
    check(controls.read(memory,out,8)==1,"Read must return actual available samples");
    check(memory.load32(out)==11111 && memory.load32(out+4)==0x4000 && memory.load8(out+8)==255 && memory.load8(out+9)==0,"PSP input layout or analog/button state lost");
    for(unsigned b=10;b<16;++b)check(memory.load8(out+b)==0,"Reserved pad byte not initialized");
    check(!controls.available(),"Reading did not consume samples");input={0x2000,0,255};controls.update(44444);
    check(controls.read(memory,out,2)==2 && memory.load32(out)==33333 && memory.load32(out+16)==44444,"Read did not select newest samples chronologically");
    check(!controls.available(),"Old samples were replayed");controls.set_mode(0);controls.update(55555);controls.read(memory,out,1);
    check(memory.load8(out+8)==128 && memory.load8(out+9)==128,"Digital mode retained analog movement");
    controls.update(11111ull*1000);check(controls.read(memory,out,64)==63,"Unread sampling ring did not stay bounded");
    kon::ControlReplay anchored;anchored.events={{0,{0,128,128}},{1000,{0x2000,128,128}},{2000,{0,128,128}}};
    check(!anchored.at_relative(9999,10000).buttons && !anchored.at_relative(10000,10000).buttons && !anchored.at_relative(10999,10000).buttons && anchored.at_relative(11000,10000).buttons==0x2000 && !anchored.at_relative(12000,10000).buttons,"Audio-relative input underflowed, ran early, lost its first edge or its release");
    std::cout<<"PASS: actual sampling deadlines, newest unread samples, PSP layout, analog mode and bounded history\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<"\n";return 1;}}
