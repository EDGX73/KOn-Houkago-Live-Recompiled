#include "native_display.hpp"
#include "psprecomp/runtime.hpp"
#include <iostream>
#include <stdexcept>
void check(bool c,const char* message){if(!c)throw std::runtime_error(message);}
int main(){try {
    check(kon::psp_pixel_bgra(0x001Fu,0)==0xFFFF0000u,"565 red conversion incorrect");
    check(kon::psp_pixel_bgra(0x07E0u,0)==0xFF00FF00u,"565 green conversion incorrect");
    check(kon::psp_pixel_bgra(0xFC00u,1)==0xFF0000FFu,"5551 blue conversion incorrect");
    check(kon::psp_pixel_bgra(0xFA32u,2)==0xFF2233AAu,"4444 conversion incorrect");
    check(kon::psp_pixel_bgra(0xAA332211u,3)==0xAA112233u,"RGBA/BGRA byte order incorrect");
    psprecomp::Runtime rt;kon::NativeDisplay display(rt.memory());
    check(display.set_mode(0,480,272)==0,"Screen mode failed");
    const auto first=0x04000000u,second=0x04088000u;
    rt.memory().store32(first,0xFF0000FFu);rt.memory().store32(second,0xFF00FF00u);
    check(display.set_buffer(first,512,3,0)==0 && display.pixels()[0]==0xFFFF0000u,"Current framebuffer not displayed");
    check(display.submitted_frames()==1,"An immediate presentation must count one submitted frame");
    check(display.set_buffer(second,512,3,1)==0 && display.buffer(0).address==first && display.buffer(1).address==second,"Pending swap was not deferred");
    display.update(16682);check(display.buffer(0).address==first,"Swap occurred before vblank");
    display.update(16684);check(display.buffer(0).address==second && display.pixels()[0]==0xFF00FF00u,"Swap failed at vblank");
    check(display.submitted_frames()==2,"A latched swap must count exactly one submitted frame");
    display.update(16684);display.update(16685);check(display.submitted_frames()==2,"Repeated scanout must not count as a new frame");
    display.update(1000000);check(display.fps_available() && display.measured_fps()==2.0,"Measured FPS counts repaints or misses original frames");
    check(display.until_next_vblank(16684)==16683,"Frame timing wrong");
    for(const auto now:{0ull,16682ull,16683ull,16684ull,1001000ull,1001001ull})for(unsigned count:{1u,2u,3u,60u,0x7FFFFFFFu}){
        const auto delay=display.until_vblank(now,count),due=now+delay;
        check(display.frame_count(due)>=display.frame_count(now)+count && display.frame_count(due-1)==display.frame_count(now)+count-1,"Multi-vblank deadline is not the requested exact boundary");
        if(count==1)check(delay==display.until_next_vblank(now),"Single/multiple vblank disagree");
    }
    for(const auto address:{0x04100000u,0x041FFFF0u})for(unsigned format=0;format<4;++format){
        const unsigned bytes=format==3?4u:2u;
        const auto last=address+(271u*512u+479u)*bytes;
        if(bytes==4){rt.memory().store32(address,0xAA332211u);rt.memory().store32(last,0x446655CCu);}
        else{rt.memory().store16(address,0xA321u);rt.memory().store16(last,0x4C56u);}
        check(display.set_buffer(address,512,format,0)==0,"Mirrored/padded scanout rejected");
        const auto pixels=display.pixels();
        check(pixels.front()==(kon::psp_pixel_bgra(bytes==4?0xAA332211u:0xA321u,format)|0xFF000000u) && pixels.back()==(kon::psp_pixel_bgra(bytes==4?0x446655CCu:0x4C56u,format)|0xFF000000u),"Scanout lost format, last-row pitch or VRAM mirror crossing");
    }
    check(display.set_buffer(first+1,512,3,0)!=0 && display.set_buffer(first,500,3,0)!=0,"Invalid buffers permitted");
    const auto frames=display.submitted_frames();
    check(display.set_buffer(0,512,3,0)==0 && display.pixels()[0]==0xFF000000u,"Display disable failed");
    display.update(50000);check(display.submitted_frames()==frames,"Disabled display or repeated refresh must not fabricate frames");
    std::cout<<"PASS: native pixel conversion, mapped framebuffer scanout and vblank-latched swaps\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<"\n";return 1;}}
