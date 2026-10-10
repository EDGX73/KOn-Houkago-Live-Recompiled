#include "native_draw_capture.hpp"
#include "native_renderer.hpp"
#include "native_display.hpp"
#include <iostream>
#include <fstream>
int main(int argc,char** argv){try{
    if(argc!=3 && argc!=4)throw std::runtime_error("Usage: replay_native_draw snapshot.kondraw output.bmp [full-vram-reference.bin]");
    psprecomp::GuestMemory memory;auto capture=kon::NativeDrawCapture::read(argv[1],memory);kon::NativeRenderer renderer(memory);
    if(!renderer.draw(capture.state,capture.kind,capture.count))throw std::runtime_error("Captured primitive still requires native implementation");
    kon::NativeDisplay display(memory);const auto& r=capture.state.registers;
    if(display.set_buffer(0x04000000u|(r[0x9C]&0x1FFFF0u),r[0x9D]&0x7FCu,r[0xD2]&3u,0)!=0)throw std::runtime_error("Captured framebuffer cannot be scanned out");
    if(argc==4){std::ofstream out(argv[3],std::ios::binary);const auto vram=memory.vram_bytes();out.write(reinterpret_cast<const char*>(vram.data()),vram.size());if(!out)throw std::runtime_error("Cannot save exact full VRAM draw reference");std::cout<<"full_vram_reference_bytes="<<vram.size()<<"\n";}
    display.save_bitmap(argv[2]);std::cout<<"Captured draw replayed, pc="<<psprecomp::hex32(capture.pc)<<" covered="<<renderer.draw_covered_pixels<<" drawn="<<renderer.draw_pixels<<". Original game execution is not part of this check.\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 2;}}
