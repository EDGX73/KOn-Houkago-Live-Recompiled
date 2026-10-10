#include "native_texture.hpp"
#include "native_renderer.hpp"
#include <iostream>
#include <stdexcept>
void check(bool ok,const char* text){if(!ok)throw std::runtime_error(text);}
int main(){try{
    psprecomp::GuestMemory memory;kon::GeState s;auto& r=s.registers;constexpr std::uint32_t image=0x08820000u;
    r[0xA0]=0x820000;r[0xA8]=0x080004;r[0xB8]=0x0102;r[0xC3]=3;
    memory.store32(image,0xFF0000FFu);memory.store32(image+4,0xFF00FF00u);memory.store32(image+16,0xFFFF0000u);memory.store32(image+20,0xFFFFFFFFu);memory.store32(image+12,0xFF123456u);
    // SIMD products are unsigned 16-bit (at most 256*255); saturation occurs
    // after the original /128 or /256 truncation, with alpha never doubled.
    for(unsigned flags:{0u,0x100u,0x10000u,0x10100u}){
        r[0xC9]=flags;kon::NativeTexture combine_texture(memory,s);
        for(unsigned c=0;c<256;++c)for(unsigned t=0;t<256;++t){
            const unsigned vertex=(c<<24u)|(((c*71u)&255u)<<16u)|(((c*37u)&255u)<<8u)|c;
            const unsigned texel=(t<<24u)|(((t*53u)&255u)<<16u)|(((t*19u)&255u)<<8u)|t;
            check(combine_texture.combine(vertex,texel)==combine_texture.combine_generic(vertex,texel),"SIMD texture modulation changed rounding, saturation, RGB or alpha");
        }
    }
    r[0xC9]=0;
    const auto check_cache=[&](kon::NativeTexture& texture){
        const std::array<double,12> coordinates{-2.0625,-.063,-.01,0,.5,.9375,1,1.0625,3.5,4.5,9.5,18.5};
        std::vector<std::uint32_t> reference;
        for(bool linear:{false,true})for(auto v:coordinates)for(auto u:coordinates)reference.push_back(texture.sample(u,v,linear));
        std::vector<std::uint32_t> decoded;texture.prepare_cache(decoded);std::size_t index=0;
        for(bool linear:{false,true})for(auto v:coordinates)for(auto u:coordinates)check(texture.sample(u,v,linear)==reference[index++],"Prepared texture cache changed texels, palette, swizzle, wrap, clamp or filtering");
        check(decoded.size()==texture.decoded_texel_count(),"Decoded texture dimensions changed");
        kon::NativeTexture::Cache cache(2);
        kon::NativeTexture first(memory,s),second(memory,s);
        check(first.prepare_cache(cache) && !second.prepare_cache(cache),"Unchanged texture was decoded twice");
        index=0;for(bool linear:{false,true})for(auto v:coordinates)for(auto u:coordinates)check(second.sample(u,v,linear)==reference[index++],"Persistent decoded texture changed filtering");
        const auto previous=memory.load8(image);memory.store8(image,previous^0x11u);
        kon::NativeTexture changed(memory,s),uncached(memory,s);
        check(changed.prepare_cache(cache),"Original program texture write did not invalidate the cache");
        for(bool linear:{false,true})for(auto v:coordinates)for(auto u:coordinates)check(changed.sample(u,v,linear)==uncached.sample(u,v,linear),"Modified texture reused stale pixels");
        memory.store8(image,previous);
        kon::NativeTexture restored(memory,s);check(restored.prepare_cache(cache),"Restored source bytes reused a different decoded image");
        if((s.registers[0xC3]&15u)>=4u && (s.registers[0xC3]&15u)<=7u){
            s.clut[2]^=0x11u;kon::NativeTexture palette_changed(memory,s),palette_reference(memory,s);
            check(palette_changed.prepare_cache(cache),"Retained palette change failed to invalidate decoded texels");
            for(bool linear:{false,true})for(auto v:coordinates)for(auto u:coordinates)check(palette_changed.sample(u,v,linear)==palette_reference.sample(u,v,linear),"Palette change reused stale colors");
            s.clut[2]^=0x11u;
        }
        const auto old_address=s.registers[0xA0];
        for(unsigned offset:{0x1000u,0x2000u}){
            s.registers[0xA0]=old_address+offset;kon::NativeTexture other(memory,s);check(other.prepare_cache(cache),"Different image address reused the old image");
        }
        s.registers[0xA0]=old_address;
        check(cache.entries()==2 && cache.resident_bytes()<=8u*1024u*1024u,"Texture cache exceeded its entry or byte budget");
        kon::NativeTexture evicted(memory,s);check(evicted.prepare_cache(cache),"Evicted texture reused freed storage");
    };
    kon::NativeTexture rgba(memory,s);
    check(rgba.sample(0.5,0.5,false)==0xFF0000FFu && rgba.sample(0.5,0.5,true)==0xFF0000FFu,"Direct texture channel packing or texel center failed");
    check(rgba.sample(1,1,true)==0xFF7F7F7Fu,"Four-sample bilinear filtering failed");
    check(rgba.sample(-0.01,0.5,false)==0xFF0000FFu && rgba.sample(-0.063,0.5,false)==0xFF123456u,"Negative texel coordinates lost truncation-to-sixteenths semantics");
    check(rgba.sample(4.5,0.5,false)==0xFF0000FFu,"Repeated texture coordinates failed");r[0xC7]=0x101;
    check(rgba.sample(4.5,0.5,false)==0xFF123456u,"Clamped texture coordinates failed");r[0xC9]=0x100;
    check(rgba.combine(0xFFFFFFFFu,0xFF123456u)==0xFF123456u,"White texture modulation changed sampled colors");
    for(unsigned value=0;value<256;++value){
        const auto texel=(value<<24u)|((255u-value)<<16u)|(value<<8u)|value;
        for(const auto alpha_mode:{0u,0x100u}){
            r[0xC9]=alpha_mode;
            check(rgba.combine(0xFFFFFFFFu,texel)==((texel&0xFFFFFFu)|(alpha_mode?(texel&0xFF000000u):0xFF000000u)),"White modulation changed alpha or channel extremes");
            r[0xC9]=alpha_mode|3u;
            check(rgba.combine(0x46123456u,texel)==((texel&0xFFFFFFu)|(alpha_mode?(texel&0xFF000000u):0x46000000u)),"Texture replacement did not retain selected alpha source");
        }
    }
    r[0xC9]=0x10100;
    check(rgba.combine(0xFFFFFFFFu,0x40107F80u)==0x4020FEFFu,"Double-color modulation identity ignored saturation or alpha");
    memory.store32(image,0xFF0D0D0Du);memory.store32(image+4,0xFF565656u);
    memory.store32(image+16,0xFF313131u);memory.store32(image+20,0xFFDBDBDBu);
    check(rgba.sample(0.9375,1.0625,true)==0xFF585858u,"Bilinear horizontal truncation was deferred until the final sum");
    // Extreme independent RGBA channels catch carry between packed lanes.
    const std::array<unsigned,4> extremes{0x00FF00FFu,0xFF00FF00u,0xFE01FD02u,0x03FC04FBu};
    memory.store32(image,extremes[0]);memory.store32(image+4,extremes[1]);memory.store32(image+16,extremes[2]);memory.store32(image+20,extremes[3]);
    for(unsigned fy=0;fy<16;++fy)for(unsigned fx=0;fx<16;++fx){
        unsigned expected=0;
        for(unsigned channel=0;channel<4;++channel){
            const auto value=[&](unsigned n){return (extremes[n]>>(channel*8u))&255u;};
            const auto upper=(value(0)*(16-fx)+value(1)*fx)/16,lower=(value(2)*(16-fx)+value(3)*fx)/16;
            expected|=((upper*(16-fy)+lower*fy)/16)<<(channel*8u);
        }
        check(rgba.sample(.5+fx/16.0,.5+fy/16.0,true)==expected,"Packed bilinear channel carry or stage truncation changed pixels");
    }
    {kon::NativeTexture cached_rgba(memory,s);check_cache(cached_rgba);}
    r[0xA8]=0x080020;r[0xB8]=0x0405;r[0xC3]=5;r[0xC2]=1;r[0xC5]=0xFF02;
    s.clut[2]=0x0F;s.clut[3]=0xF0;memory.store8(image+402,1);
    kon::NativeTexture indexed(memory,s);check(indexed.sample(18.5,9.5,false)==0xFF0000FFu,"Swizzled indexed texture read incorrect tile/4444 palette");
    r[0xC5]=0x01FF02;s.clut[34]=0xF0;s.clut[35]=0xF0;
    check(indexed.sample(18.5,9.5,false)==0xFF00FF00u,"Palette bank offset or retained state failed");
    {kon::NativeTexture cached_indexed(memory,s);check_cache(cached_indexed);}
    r[0xC3]=4;r[0xC2]=0;r[0xC5]=0xFF02;r[0xA8]=0x080020;r[0xB8]=0x0305;
    s.clut[4]=0;s.clut[5]=0xFF;memory.store8(image,0x21);kon::NativeTexture nibbles(memory,s);
    check(nibbles.sample(0.5,0.5,false)==0xFF0000FFu && nibbles.sample(1.5,0.5,false)==0xFFFF0000u,"Four-bit low/high texel nibble order failed");
    {kon::NativeTexture cached_nibbles(memory,s);check_cache(cached_nibbles);}
    // PSP compressed blocks use RGB endpoints in the opposite channel order to GU_PSM_5650.
    r[0xC3]=8;r[0xA8]=0x080008;r[0xB8]=0x0203;
    for(unsigned row=0;row<4;++row)memory.store8(image+row,0xE4);
    memory.store16(image+4,0xF800);memory.store16(image+6,0x001F);
    memory.store32(image+8,0xFFFFFFFF);memory.store16(image+12,0);memory.store16(image+14,0xFFFF);
    kon::NativeTexture compressed(memory,s);
    check(compressed.sample(.5,.5,false)==0xFF0000F8 && compressed.sample(1.5,.5,false)==0xFFF80000,"DXT1 endpoint channel order failed");
    check(compressed.sample(2.5,3.5,false)==0xFF5200A5 && compressed.sample(3.5,2.5,false)==0xFFA50052,"DXT1 four-color interpolation or selector order failed");
    check(compressed.sample(4.5,.5,false)==0,"DXT1 transparent selector or block pitch failed");
    memory.store8(image+8,2);check(compressed.sample(4.5,.5,false)==0xFF7C7E7C,"DXT1 three-color average failed");
    {kon::NativeTexture cached_compressed(memory,s);check_cache(cached_compressed);}
    {
        // DXT3 (9) and DXT5 (10): 16-byte blocks, DXT1 colour part first
        // (always four colours), then the alpha part.
        kon::GeState d=s;auto& dr=d.registers;dr[0xA8]=0x080004;dr[0xB8]=0x0202;
        for(unsigned n=0;n<16;++n)memory.store8(image+n,0);
        memory.store16(image+4,0xF800);memory.store16(image+6,0x001F);
        dr[0xC3]=9;memory.store16(image+8,0xF210);
        kon::NativeTexture dxt3(memory,d);
        check(dxt3.sample(.5,.5,false)==0x000000F8u && dxt3.sample(1.5,.5,false)==0x110000F8u && dxt3.sample(2.5,.5,false)==0x220000F8u && dxt3.sample(3.5,.5,false)==0xFF0000F8u,"DXT3 4-bit alpha rows failed");
        dr[0xC3]=10;memory.store32(image+8,3720u);memory.store16(image+12,0);memory.store8(image+14,200);memory.store8(image+15,100);
        kon::NativeTexture dxt5(memory,d);
        check(dxt5.sample(.5,.5,false)==0xC80000F8u && dxt5.sample(1.5,.5,false)==0x640000F8u && dxt5.sample(2.5,.5,false)==0xB90000F8u && dxt5.sample(3.5,.5,false)==0x720000F8u,"DXT5 interpolated alpha failed");
    }
    {
        // Buffer width 0 (the tutorial screens use it): rows are 16 bytes
        // apart, so a 32-bit texture's texel (5,0) is the one at byte 20.
        kon::GeState zero;auto& zr=zero.registers;zr[0xA0]=0x820000;zr[0xA8]=0x080000;zr[0xB8]=0x0103;zr[0xC3]=3;
        memory.store32(image+20,0xFF445566u);memory.store32(image+16,0xFF778899u);
        kon::NativeTexture narrow(memory,zero);
        check(narrow.sample(5.5,.5,false)==0xFF445566u && narrow.sample(.5,1.5,false)==0xFF778899u,"Zero texture buffer width did not use 16-byte rows");
    }
    {
        // Full-size textures exercise byte-budget eviction independently of
        // the entry limit. Expected pixels come from real modified source RAM.
        kon::GeState large;auto& lr=large.registers;lr[0xA8]=0x080200;lr[0xB8]=0x0909;lr[0xC3]=3;
        kon::NativeTexture::Cache bounded;
        for(unsigned n=0;n<8;++n){const auto address=image+n*0x100000u;
            lr[0xA0]=address&0xFFFFF0u;memory.store32(address,0xFF102030u+n);
            kon::NativeTexture texture(memory,large);check(texture.prepare_cache(bounded),"Full-size distinct texture was not decoded");
            check(texture.sample(.5,.5,false)==0xFF102030u+n,"Budget eviction corrupted retained decoded pixels");
            check(bounded.resident_bytes()<=8u*1024u*1024u && bounded.entries()<=3,"Full-size textures exceeded the byte budget");
        }
    }
    {
        // The GPU cache must retain a working set larger than the old 8 MiB
        // budget, while detecting exact writes and keeping the CPU budget.
        kon::GeState large;auto& lr=large.registers;lr[0xA8]=0x080200;lr[0xB8]=0x0909;lr[0xC3]=3;
        kon::NativeTexture::Cache gpu_working_set(128,64u*1024u*1024u);
        for(unsigned pass=0;pass<2;++pass)for(unsigned n=0;n<8;++n){const auto at=image+n*0x100000u;lr[0xA0]=at&0xFFFFF0u;
            kon::NativeTexture texture(memory,large);check(texture.prepare_cache(gpu_working_set)==(pass==0),"Unchanged GPU working set was decoded again");
            check(texture.sample(.5,.5,false)==0xFF102030u+n,"Retained GPU texture corrupted pixels");
        }
        check(gpu_working_set.entries()==8 && gpu_working_set.resident_bytes()>8u*1024u*1024u && gpu_working_set.resident_bytes()<=64u*1024u*1024u,"GPU cache escaped its larger bounded budget");
        lr[0xA0]=(image+3u*0x100000u)&0xFFFFF0u;memory.store32(image+3u*0x100000u,0x12345678);
        kon::NativeTexture changed(memory,large);check(changed.prepare_cache(gpu_working_set) && changed.sample(.5,.5,false)==0x12345678,"Larger GPU cache ignored original texture write");
    }
    // Draw a real through-mode, UV16/color4444/XYZ16 strip, matching the original layout.
    s={};auto& a=s.registers;a[0x12]=0x80011A;a[0x1E]=1;a[0x9D]=512;a[0xD2]=3;a[0x16]=a[0xD5]=(1u<<10u)|1u;
    a[0xA0]=0x820000;a[0xA8]=0x080002;a[0xB8]=0x0101;a[0xC3]=3;a[0xC9]=0x100;a[0xC6]=0x101;
    const std::uint32_t colors[]={0xFF0000FFu,0xFF00FF00u,0xFFFF0000u,0xFFFFFFFFu};for(unsigned n=0;n<4;++n)memory.store32(image+n*4,colors[n]);
    const auto vertices=0x08810000u;s.vertex_address=vertices;
    for(unsigned n=0;n<4;++n){const unsigned x=(n/2)*2,y=(n%2)*2;const auto p=vertices+n*12;memory.store16(p,x);memory.store16(p+2,y);memory.store16(p+4,0xFFFF);memory.store16(p+6,x);memory.store16(p+8,y);memory.store16(p+10,0);}
    kon::NativeRenderer renderer(memory);check(renderer.draw(s,4,4) && renderer.draw_pixels==4 && s.vertex_address==vertices+48,"Textured strip did not draw actual layout");
    for(unsigned n=0;n<4;++n)check(memory.load32(0x04000000u+((n/2)*512+n%2)*4)==(colors[n]&0xFFFFFFu),"Texture coordinates, channel packing or shared edge failed in framebuffer");
    std::cout<<"PASS: original UV16/4444 layout, swizzled/nibble/palette textures, wrapping, bilinear centers and native textured pixels\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<"\n";return 1;}}
