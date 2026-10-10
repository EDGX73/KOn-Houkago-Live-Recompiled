#include "native_renderer.hpp"
#include <iostream>
#include <stdexcept>
void check(bool c,const char* message){if(!c)throw std::runtime_error(message);}
int main(){try{
    // Independent endpoint/channel expectations for the captured song blend.
    check(kon::NativeBlend::supported(0xA2) && kon::NativeBlend::supported(0x32),"Observed blend rejected");
    check(!kon::NativeBlend::supported(0xA6) && !kon::NativeBlend::supported(0x600),"Unsupported blend accepted");
    check(kon::NativeBlend::apply(0xA2,0x000000FFu,0x00204060u,0,0xFFFFFFu)==0x00204060u,"Zero-alpha additive contribution changed destination");
    check(kon::NativeBlend::apply(0xA2,0xFF0000FFu,0x00204060u,0,0xFFFFFFu)==0x002040FFu,"Additive saturation or untouched channels failed");
    check(kon::NativeBlend::apply(0xA2,0x80402010u,0x00204060u,0,0xFFFFFFu)==0x00405068u,"Half-alpha additive colors failed");
    check(kon::NativeBlend::apply(0xAA,0xFFFFFFFFu,0x00FFFFFFu,0x102040u,0x403020u)==0x00505060u,"Fixed factors lost separate RGB channels");
    check(kon::NativeBlend::apply(0x1AA,0x00FFFFFFu,0x00FFFFFFu,0x102040u,0x403020u)==0x00000020u,"Subtract failed to clamp negative channels");
    check(kon::NativeBlend::apply(0x2AA,0x00FFFFFFu,0x00FFFFFFu,0x102040u,0x403020u)==0x00301000u,"Reverse subtract failed");
    check(kon::NativeBlend::apply(0x300,0x001060A0u,0x00403090u,0,0)==0x00103090u,"Minimum incorrectly applied factors");
    check(kon::NativeBlend::apply(0x400,0x001060A0u,0x00403090u,0,0)==0x004060A0u,"Maximum failed");
    check(kon::NativeBlend::apply(0x500,0x001060A0u,0x00403090u,0,0)==0x00303010u,"Absolute difference failed");
    // Every source alpha/channel, adversarial destination values and unrelated
    // RGB lanes verify packed products, truncation and additive saturation.
    for(unsigned alpha=0;alpha<256;++alpha)for(unsigned c=0;c<256;++c)for(unsigned d:{0u,1u,2u,15u,16u,31u,63u,127u,128u,191u,223u,253u,254u,255u}){
        const unsigned src=(alpha<<24u)|(c<<16u)|(((c*71u)&255u)<<8u)|((c*37u)&255u);
        const unsigned dst=((d*13u)&255u)<<16u|d<<8u|((d*19u)&255u);
        for(unsigned mode:{0x32u,0xA2u})check(kon::NativeBlend::apply(mode,src,dst,0,0xFFFFFFu)==kon::NativeBlend::apply_generic(mode,src,dst,0,0xFFFFFFu),"Packed common blend changed GE rounding or crossed channel lanes");
    }
    psprecomp::Runtime rt;auto& mem=rt.memory();kon::NativeRenderer draw(mem);kon::GeState s;
    // Fast and checked views must agree for unaligned little-endian access;
    // a view spanning the VRAM mirror boundary must retain wrapped accesses.
    for(unsigned base:{0x08880000u,0x041FFFF8u}){
        kon::NativeMemoryView view(mem,base,16,true),checked(mem,base,16,true,true);
        view.store32(1,0xA1B2C3D4u);view.store16(8,0x1234u);
        check(checked.load32(1)==0xA1B2C3D4u && checked.load16(8)==0x1234u,"Native view lost unaligned/mirrored bytes");
        checked.store32(10,0x31415926u);check(view.load32(10)==0x31415926u && mem.load32(base+10)==0x31415926u,"Native view bypassed memory aliases");
    }
    for(unsigned bytes:{2u,4u})for(bool checked:{false,true}){
        const auto at=0x08900100u;mem.store32(at-4,0xA5A5A5A5u);mem.store32(at+68,0x5A5A5A5Au);
        std::vector<std::uint32_t> source(17);for(unsigned n=0;n<source.size();++n)source[n]=0xFEDC1234u+n*0x10005u;
        kon::NativeMemoryView packed(mem,at,source.size()*bytes,true,checked);packed.write_words(source,bytes);
        std::vector<std::uint32_t> copy;packed.read_words(copy,bytes);
        for(unsigned n=0;n<copy.size();++n)check(copy[n]==(source[n]&(bytes==2?0xFFFFu:0xFFFFFFFFu)),"Packed GPU transfer changed words");
        check(mem.load32(at-4)==0xA5A5A5A5u && mem.load32(at+68)==0x5A5A5A5Au,"Packed transfer touched guards");
        bool invalid=false;try{packed.write_words(std::vector<std::uint32_t>(18),bytes);}catch(const std::invalid_argument&){invalid=true;}check(invalid,"Packed transfer accepted an oversized source");
    }
    bool invalid_view=false;try{kon::NativeMemoryView invalid(mem,0x09FFFFFEu,4,true);}catch(const std::exception&){invalid_view=true;}
    check(invalid_view,"Native view accepted an unmapped range");
    constexpr std::uint32_t vertices=0x08810000u,frame=0x04000000u,depth=0x04100000u;
    s.vertex_address=vertices;s.registers[0x12]=0x1280011Cu;s.registers[0xD3]=0xD3000701u;
    s.registers[0x9D]=0x9D000200u;s.registers[0x9E]=0x9E100000u;s.registers[0x9F]=0x9F000200u;
    s.registers[0xD2]=0xD2000003u;s.registers[0xD4]=0xD4000000u;s.registers[0xD5]=0xD5043DDFu;
    mem.store32(vertices,0);mem.store16(vertices+4,10);mem.store16(vertices+6,20);
    mem.store32(vertices+12,0xAB563412u);mem.store16(vertices+16,14);mem.store16(vertices+18,23);mem.store16(vertices+20,0x1357u);
    check(draw.draw(s,6,2) && s.vertex_address==vertices+24,"Clear did not consume actual vertices");
    check(mem.load32(frame+(20*512+10)*4)==0xAB563412u && mem.load16(depth+(22*512+13)*2)==0x1357u,"Clear color/depth incorrect");
    check(mem.load32(frame+(20*512+9)*4)==0 && mem.load32(frame+(23*512+10)*4)==0 && draw.clear_pixels==12,"Clear rectangle leaked outside edges");
    // Color-only clear must retain destination alpha, with the scissor applied.
    mem.store32(frame+(20*512+10)*4,0xEE000000u);s.vertex_address=vertices;s.registers[0xD3]=0xD3000101u;s.registers[0xD5]=(20u<<10u)|10u;
    check(draw.draw(s,6,2) && mem.load32(frame+(20*512+10)*4)==0xEE563412u,"Clear masks/scissor lost destination alpha");
    check(mem.load32(frame+(20*512+11)*4)==0xAB563412u,"Scissor leaked");
    for(unsigned format=0;format<3;format++){
        s.vertex_address=vertices;s.registers[0xD2]=format;s.registers[0xD3]=0x701;
        check(draw.draw(s,6,2),"16-bit clear rejected");
        const std::uint16_t expected[]={0x51A2u,0xA8C2u,0xA531u};
        check(mem.load16(frame+(20*512+10)*2)==expected[format],"16-bit channel packing incorrect");
    }
    s.vertex_address=vertices;s.registers[0xD3]=0;check(!draw.draw(s,6,2),"Unsupported ordinary sprite fabricated success");
    s.registers[0xD3]=0x701;check(!draw.draw(s,3,3),"Unsupported triangle fabricated success");
    s={};s.vertex_address=vertices;s.registers[0x12]=0x80011C;s.registers[0x9D]=512;s.registers[0x9E]=0x100000;s.registers[0x9F]=512;
    s.registers[0xD2]=3;s.registers[0x16]=s.registers[0xD5]=(3u<<10u)|3u;
    s.registers[0x21]=1;s.registers[0xDF]=0x32;s.registers[0x22]=1;s.registers[0xDB]=0xFF0006;
    s.registers[0x23]=1;s.registers[0xDE]=4;s.registers[0x1D]=1;s.registers[0x9B]=0;
    for(unsigned n=0;n<4;++n){const auto address=vertices+n*12u;mem.store32(address,0xA9000000u);mem.store16(address+4,(n/2)*4);mem.store16(address+6,(n%2)*4);mem.store16(address+8,100);}
    for(unsigned y=0;y<4;++y)for(unsigned x=0;x<4;++x){mem.store32(frame+(y*512+x)*4,0xEEFFFFFFu);mem.store16(depth+(y*512+x)*2,200);}
    check(draw.draw(s,4,4) && draw.draw_pixels==16 && s.vertex_address==vertices+48,"Triangle strip coverage, culling or vertex consumption failed");
    for(unsigned y=0;y<4;++y)for(unsigned x=0;x<4;++x){check(mem.load32(frame+(y*512+x)*4)==0xEE565656u,"Shared edge was blended twice, RGB blend or preserved stencil failed");check(mem.load16(depth+(y*512+x)*2)==100,"Triangle depth write lost");}
    s.vertex_address=vertices;s.registers[0x9B]=1;check(draw.draw(s,4,4) && draw.draw_pixels==16,"Opposite winding was not culled");
    s.vertex_address=vertices;s.registers[0x9B]=0;check(draw.draw(s,4,4) && draw.draw_pixels==16,"Failed depth comparison drew pixels");
    s.vertex_address=vertices;s.registers[0xDE]=1;s.registers[0xE7]=1;s.registers[0xD4]=(1u<<10u)|1u;s.registers[0xD5]=(2u<<10u)|2u;
    check(draw.draw(s,4,4) && draw.draw_pixels==20,"Triangle scissor leaked");
    s.vertex_address=vertices;s.registers[0xDB]=0xFFAA06;check(draw.draw(s,4,4) && draw.draw_pixels==20,"Rejected alpha wrote pixels");
    s.vertex_address=vertices;s.registers[0x1E]=1;check(!draw.draw(s,4,4) && s.vertex_address==vertices,"Unsupported texture silently drew an untextured primitive");
    // Independent winding contract: a right/down triangle has positive cross
    // product in screen coordinates and must survive GE CULL=1 (GU_CW).
    s.registers[0x1E]=s.registers[0x21]=s.registers[0x22]=s.registers[0x23]=0;
    s.registers[0xD4]=0;s.registers[0xD5]=(3u<<10u)|3u;s.registers[0x9B]=1;
    const unsigned xy[3][2]={{0,0},{4,0},{0,4}};
    for(unsigned n=0;n<3;++n){mem.store32(vertices+n*12u,0xFFFFFFFFu);mem.store16(vertices+n*12u+4,xy[n][0]);mem.store16(vertices+n*12u+6,xy[n][1]);}
    auto before=draw.draw_pixels;s.vertex_address=vertices;
    check(draw.draw(s,3,3) && draw.draw_pixels>before,"GE CULL=1 rejected positive screen winding");
    before=draw.draw_pixels;s.vertex_address=vertices;s.registers[0x9B]=0;
    check(draw.draw(s,3,3) && draw.draw_pixels==before,"GE CULL=0 retained positive screen winding");
    s.vertex_address=vertices;s.registers[0x1D]=0;
    check(draw.draw(s,3,3) && draw.draw_pixels>before,"Disabled culling still discarded winding");
    // Captured additive mode must flow through real packing/masking/depth writes.
    s.registers[0x21]=1;s.registers[0xDF]=0xA2;s.registers[0xE1]=0xFFFFFF;
    s.registers[0x23]=1;s.registers[0xDE]=1;s.registers[0xE7]=0;
    s.registers[0xD2]=3;s.registers[0xE8]=0xFF;
    for(unsigned n=0;n<3;++n)mem.store32(vertices+n*12u,0x80402010u);
    mem.store32(frame,0xEE204060u);mem.store16(depth,200);s.vertex_address=vertices;
    check(draw.draw(s,3,3) && mem.load32(frame)==0xEE405060u && mem.load16(depth)==100,"Additive blend lost RGB mask, stencil or depth output");
    s.registers[0xE8]=0;
    for(unsigned n=0;n<3;++n)mem.store32(vertices+n*12u,0xFF0000FFu);
    for(unsigned format:{0u,2u}){
        s.registers[0xD2]=format;mem.store16(frame,format==0?0x220Cu:0xA246u);s.vertex_address=vertices;
        check(draw.draw(s,3,3) && mem.load16(frame)==(format==0?0x221Fu:0xA24Fu),"Additive RGB565/4444 packing or preserved stencil failed");
    }
    std::cout<<"PASS: clear formats/masks, flat triangle strips, shared-edge coverage, blend/stencil, culling, alpha/depth and scissor\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<"\n";return 1;}}
