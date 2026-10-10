#include "native_renderer.hpp"
#include <iostream>
void require(bool value,const char* why){if(!value)throw std::runtime_error(why);}
int main(){try{
    kon::NativeRasterPool pool;std::array<unsigned,4> visited{};
    for(unsigned pass=0;pass<8;++pass){pool.run([&](unsigned id){visited[id]=pass+1;});for(auto n:visited)require(n==pass+1,"Raster barrier lost a job");}
    bool caught=false;try{pool.run([](unsigned id){if(id==2)throw std::runtime_error("worker failure");});}catch(const std::runtime_error&){caught=true;}
    require(caught,"Raster worker error was swallowed");pool.run([&](unsigned id){visited[id]=99;});for(auto n:visited)require(n==99,"Raster pool could not recover after error");
    // Every supported worker count must complete all jobs, propagate failures,
    // and recover. Disjoint row ownership is tested below against serial VRAM.
    for(unsigned lanes:{1u,2u,4u,8u,16u}){kon::NativeRasterPool wide(lanes);std::array<unsigned,16> seen{};
        for(unsigned pass=1;pass<=12;++pass){wide.run([&](unsigned id){seen[id]=pass;});for(unsigned id=0;id<lanes;++id)require(seen[id]==pass,"Wide raster pool lost a job");}
        bool failed=false;try{wide.run([&](unsigned id){if(id==lanes-1)throw std::runtime_error("Expected worker failure");});}catch(const std::runtime_error&){failed=true;}
        require(failed,"Wide pool swallowed an error");wide.run([&](unsigned id){seen[id]=99;});for(unsigned id=0;id<lanes;++id)require(seen[id]==99,"Wide pool did not recover");
    }
    psprecomp::GuestMemory serial_memory,parallel_memory;kon::NativeRenderer serial(serial_memory,1,true,true,false,false,false,false),parallel(parallel_memory,8,true,true,true,false,true);
    constexpr unsigned vertices=0x08810000u,image=0x08820000u,depth=0x04088000u;
    kon::GeState state;auto& r=state.registers;
    r[0x12]=0x80011Fu;r[0x1E]=1;r[0x21]=1;r[0x23]=1;r[0x9D]=r[0x9F]=512;r[0x9E]=0x88000;
    r[0x16]=r[0xD5]=(127u<<10u)|127u;r[0xDF]=0x32;r[0xDE]=6;
    r[0xA0]=0x820000;r[0xA8]=0x080008;r[0xB8]=0x0303;r[0xC3]=3;r[0xC9]=0x100;r[0xC6]=0x101;
    for(unsigned n=0;n<64;++n)serial_memory.store32(image+n*4u,((n*37u%256u)<<24u)|((n*71u%256u)<<16u)|((n*13u%256u)<<8u)|(n*23u%256u));
    for(unsigned n=0;n<4;++n){const auto p=vertices+n*20u;const unsigned x=(n/2)*128u,y=(n%2)*128u;
        serial_memory.store32(p,std::bit_cast<unsigned>(x/16.f));serial_memory.store32(p+4,std::bit_cast<unsigned>(y/16.f));serial_memory.store32(p+8,0xFF80C040u);
        serial_memory.store16(p+12,x);serial_memory.store16(p+14,y);serial_memory.store16(p+16,100u+n*10u);
    }
    for(float invalid:{std::numeric_limits<float>::quiet_NaN(),std::numeric_limits<float>::infinity(),10000000.f}){
        for(unsigned n=0;n<4;++n)serial_memory.store32(vertices+n*20u,std::bit_cast<unsigned>(invalid));
        parallel_memory.copy_in(psprecomp::GuestMemory::kPhysicalBase,serial_memory.bytes());
        auto a=state,b=state;a.vertex_address=b.vertex_address=vertices;bool reference_error=false,fast_error=false;
        try{serial.draw(a,4,4);}catch(const std::runtime_error&){reference_error=true;}
        try{parallel.draw(b,4,4);}catch(const std::runtime_error&){fast_error=true;}
        require(reference_error && fast_error,"Optimized through-mode sampling lost invalid/extreme UV diagnostics");
    }
    for(unsigned n=0;n<4;++n)serial_memory.store32(vertices+n*20u,std::bit_cast<unsigned>((n/2)*8.f));
    for(unsigned format=0;format<4;++format)for(unsigned alpha_test:{0u,1u}){
        r[0xD2]=format;r[0x22]=alpha_test;r[0xDB]=0xFF8006u;r[0xE8]=format&1u?0xFF00u:0;
        for(unsigned n=0;n<128u*512u;++n){serial_memory.store32(0x04000000u+n*4u,0x7F234567u);serial_memory.store16(depth+n*2u,n%3u?65535u:50u);}
        parallel_memory.copy_in(psprecomp::GuestMemory::kPhysicalBase,serial_memory.bytes());parallel_memory.copy_in(psprecomp::GuestMemory::kVramPhysicalBase,serial_memory.vram_bytes());
        for(unsigned pass=0;pass<3;++pass){auto a=state,b=state;a.vertex_address=b.vertex_address=vertices;
            require(serial.draw(a,4,4) && parallel.draw(b,4,4),"Parallel fixture draw rejected");
            require(std::equal(serial_memory.vram_bytes().begin(),serial_memory.vram_bytes().end(),parallel_memory.vram_bytes().begin()),"Parallel raster changed framebuffer/depth/masks/blend or draw order");
            require(serial.draw_pixels==parallel.draw_pixels && serial.draw_covered_pixels==parallel.draw_covered_pixels && serial.depth_rejected_pixels==parallel.depth_rejected_pixels && serial.alpha_rejected_pixels==parallel.alpha_rejected_pixels && a.vertex_address==b.vertex_address,"Parallel raster changed diagnostic counts or consumed vertices");
        }
    }
    // Many small overlapping triangles exercise the per-draw scheduling
    // threshold, crossing all row strips and using different blend/depth states.
    constexpr unsigned small_vertices=0x08830000u;
    for(unsigned format=0;format<4;++format)for(unsigned depth_enabled:{0u,1u}){
        r[0xD2]=format;r[0x23]=depth_enabled;r[0x22]=1;r[0xDB]=0xFF2006u;r[0xE8]=0;
        for(unsigned n=0;n<360;++n){const auto p=small_vertices+n*20u;const auto tri=n/3u,corner=n%3u;
            const unsigned x=(tri*19u)%100u+(corner==1u?24u:0u),y=(tri*31u)%100u+(corner==2u?24u:0u);
            serial_memory.store32(p,std::bit_cast<unsigned>(x/16.f));serial_memory.store32(p+4,std::bit_cast<unsigned>(y/16.f));
            serial_memory.store32(p+8,(tri&1u)?0x9080C040u:0x608040C0u);
            serial_memory.store16(p+12,x);serial_memory.store16(p+14,y);serial_memory.store16(p+16,100u+tri%20u);
        }
        parallel_memory.copy_in(psprecomp::GuestMemory::kPhysicalBase,serial_memory.bytes());
        parallel_memory.copy_in(psprecomp::GuestMemory::kVramPhysicalBase,serial_memory.vram_bytes());
        auto small_a=state,small_b=state;small_a.vertex_address=small_b.vertex_address=small_vertices;
        require(serial.draw(small_a,3,360) && parallel.draw(small_b,3,360),"Small triangle draw rejected");
        require(std::equal(serial_memory.vram_bytes().begin(),serial_memory.vram_bytes().end(),parallel_memory.vram_bytes().begin()),"Per-draw strips changed overlapping small triangle order");
        require(serial.draw_pixels==parallel.draw_pixels && serial.draw_covered_pixels==parallel.draw_covered_pixels && serial.depth_rejected_pixels==parallel.depth_rejected_pixels && serial.alpha_rejected_pixels==parallel.alpha_rejected_pixels,"Per-draw strips changed small triangle diagnostic counts");
    }
    // Feedback from the destination itself must retain serial ordering.
    r[0xA0]=0;r[0xA8]=0x040008;r[0x22]=r[0x23]=0;r[0xD2]=3;
    serial_memory.store32(0x04000000u,0xFF123456u);parallel_memory.copy_in(psprecomp::GuestMemory::kVramPhysicalBase,serial_memory.vram_bytes());
    auto a=state,b=state;a.vertex_address=b.vertex_address=vertices;
    require(serial.draw(a,4,4) && parallel.draw(b,4,4),"Feedback fixture rejected");
    require(std::equal(serial_memory.vram_bytes().begin(),serial_memory.vram_bytes().end(),parallel_memory.vram_bytes().begin()),"Destination texture feedback ran out of order");
    // Compare cached viewport conversion against the original per-triangle
    // path, including shared strips/fans, near/eye/far crossings, flat color,
    // textured perspective and all framebuffer formats over complete VRAM.
    kon::NativeRenderer reference_projection(serial_memory,1,false,false,false,false,false,false),cached_projection(parallel_memory,16,true,true,true);
    kon::GeState projected;auto& pr=projected.registers;
    projected.world=projected.view={1,0,0,0,1,0,0,0,1,0,0,0};
    projected.projection={1,0,0,0,0,1,0,0,0,0,-1.5f,-1,0,0,-2.5f,0};
    const auto floating=[&](unsigned cmd,float value){pr[cmd]=std::bit_cast<unsigned>(value)>>8u;};
    floating(0x42,32);floating(0x43,-32);floating(0x44,-32767.5f);floating(0x45,2048);floating(0x46,2048);floating(0x47,32767.5f);floating(0x48,1);floating(0x49,1);
    pr[0x4C]=pr[0x4D]=2016u*16u;pr[0x12]=0x19F;pr[0x1C]=1;pr[0x9D]=pr[0x9F]=512;pr[0x9E]=0x88000;
    pr[0x16]=pr[0xD5]=(63u<<10u)|63u;pr[0x23]=1;pr[0xDE]=6;pr[0x21]=1;pr[0xDF]=0x32;
    pr[0xA0]=0x820000;pr[0xA8]=0x080008;pr[0xB8]=0x0303;pr[0xC3]=3;pr[0xC9]=0x100;pr[0xC6]=0x101;
    for(unsigned n=0;n<60;++n){const auto p=vertices+n*24u;
        serial_memory.store32(p,std::bit_cast<unsigned>((n%8)/8.f));serial_memory.store32(p+4,std::bit_cast<unsigned>(((n/8)%8)/8.f));
        serial_memory.store32(p+8,n&1u?0x9080C040u:0x608040C0u);
        serial_memory.store32(p+12,std::bit_cast<unsigned>(static_cast<float>(static_cast<int>(n%7)-3)/4.f));
        serial_memory.store32(p+16,std::bit_cast<unsigned>(static_cast<float>(static_cast<int>((n*3)%7)-3)/4.f));
        const float z=n%13==0?0.5f:n%11==0?-0.5f:n%17==0?-8.f:-1.5f-static_cast<float>(n%3);
        serial_memory.store32(p+20,std::bit_cast<unsigned>(z));
    }
    for(unsigned format=0;format<4;++format)for(unsigned smooth:{0u,1u})for(unsigned primitive:{3u,4u,5u})for(unsigned clipping:{0u,1u}){
        pr[0xD2]=format;pr[0x50]=smooth;pr[0x1C]=clipping;pr[0x1E]=smooth;
        for(unsigned n=0;n<64u*512u;++n){serial_memory.store32(0x04000000u+n*4u,0x7F234567u);serial_memory.store16(depth+n*2u,65535u);}
        parallel_memory.copy_in(psprecomp::GuestMemory::kPhysicalBase,serial_memory.bytes());parallel_memory.copy_in(psprecomp::GuestMemory::kVramPhysicalBase,serial_memory.vram_bytes());
        auto reference_state=projected,cached_state=projected;reference_state.vertex_address=cached_state.vertex_address=vertices;
        require(reference_projection.draw(reference_state,primitive,60) && cached_projection.draw(cached_state,primitive,60),"Projected cache fixture rejected");
        require(std::equal(serial_memory.vram_bytes().begin(),serial_memory.vram_bytes().end(),parallel_memory.vram_bytes().begin()),"Viewport cache changed projection/clipping/provoking color or framebuffer/depth");
        require(reference_projection.draw_pixels==cached_projection.draw_pixels && reference_projection.draw_covered_pixels==cached_projection.draw_covered_pixels && reference_state.vertex_address==cached_state.vertex_address,"Viewport cache changed counters or vertex consumption");
    }
    require(cached_projection.vertex_projection_evaluations<reference_projection.vertex_projection_evaluations,"Shared-vertex viewport cache did not avoid repeated work");
    std::cout<<"PASS: synchronous raster workers, propagated errors, exact checked-vertex/direct-vertex serial/parallel RGB formats, alpha, depth, masks, blending and feedback\n";return 0;
}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}}
