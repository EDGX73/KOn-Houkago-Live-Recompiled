#include "native_vulkan_clear.hpp"
#include "native_renderer.hpp"
#include <cstring>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <vector>
void check(bool v,const char* m){if(!v)throw std::runtime_error(m);}
struct FailingTriangle final:kon::NativeGpuTriangles {
    kon::NativeGpuTriangleStats triangles(std::span<std::uint32_t> p,unsigned,std::span<const kon::NativeGpuFlatTriangle>,unsigned,unsigned,unsigned,unsigned,std::uint32_t)override{p[0]^=~0u;throw std::runtime_error("Injected triangle GPU failure");}
};
int main(int argc,char** argv){try{
    if(argc!=2)throw std::runtime_error("Usage: native_vulkan_triangle_test verification.json");
    kon::NativeVulkanClear backend;
    psprecomp::GuestMemory cm,gm;kon::NativeRenderer cpu(cm,1),gpu(gm,4);gpu.set_gpu_triangles(&backend);
    constexpr unsigned frame=0x04000000u,vertices=0x08810000u,vram_bytes=2u*1024u*1024u;
    for(unsigned n=0;n<vram_bytes/4u;++n){const unsigned v=(n*0x9E3779B9u)^0xA173592Bu;cm.store32(frame+n*4u,v);gm.store32(frame+n*4u,v);}
    unsigned comparisons=0,fallbacks=0;
    const auto same=[&]{check(std::memcmp(cm.raw_pointer(frame,vram_bytes),gm.raw_pointer(frame,vram_bytes),vram_bytes)==0,"GPU triangle changed VRAM relative to CPU");};
    const auto draw=[&](kon::GeState state,unsigned kind,unsigned count,bool expect_gpu=true){
        state.vertex_address=vertices;auto other=state;const auto before=gpu.gpu_triangle_primitives;
        check(cpu.draw(other,kind,count) && gpu.draw(state,kind,count),"Triangle draw unsupported");
        check(state.vertex_address==other.vertex_address,"GPU triangle vertex consumption differs");
        check(cpu.draw_pixels==gpu.draw_pixels && cpu.draw_covered_pixels==gpu.draw_covered_pixels && cpu.alpha_rejected_pixels==gpu.alpha_rejected_pixels && cpu.depth_rejected_pixels==gpu.depth_rejected_pixels,"GPU triangle diagnostics differ");
        if(!(expect_gpu?gpu.gpu_triangle_primitives>before:gpu.gpu_triangle_primitives==before)){std::cerr<<"comparison="<<comparisons<<" expected_gpu="<<expect_gpu<<" count="<<count<<"\n";throw std::runtime_error("Unexpected triangle GPU eligibility");}
        same();++comparisons;if(!expect_gpu)++fallbacks;
    };
    const auto through=[&](unsigned n,int x,int y,unsigned color){for(auto* m:{&cm,&gm}){const auto p=vertices+n*12u;m->store32(p,color);m->store16(p+4u,static_cast<std::uint16_t>(x));m->store16(p+6u,static_cast<std::uint16_t>(y));m->store16(p+8u,100);}};
    kon::GeState s;auto& r=s.registers;r[0x12]=0x80011Cu;r[0x9D]=64;r[0xD2]=3;r[0x16]=r[0xD5]=(63u<<10u)|63u;
    // Independent coverage expectation: two adjacent strip triangles cover
    // exactly sixteen pixels, with no duplicate shared edge and preserved alpha.
    through(0,0,0,0xFF123456u);through(1,0,4,0xFF123456u);through(2,4,0,0xFF123456u);through(3,4,4,0xFF123456u);
    for(unsigned y=0;y<4;++y)for(unsigned x=0;x<4;++x){cm.store32(frame+(y*64+x)*4u,0xAABBCCDDu);gm.store32(frame+(y*64+x)*4u,0xAABBCCDDu);}
    draw(s,4,4);check(gpu.draw_covered_pixels==16 && gpu.draw_pixels==16,"Independent shared-edge coverage failed");
    for(unsigned y=0;y<4;++y)for(unsigned x=0;x<4;++x)check(gm.load32(frame+(y*64+x)*4u)==0xAA123456u,"Flat color or preserved alpha incorrect");
    const int xy[6][2]={{-7,3},{57,1},{12,61},{3,5},{60,58},{4,55}};
    for(unsigned n=0;n<6;++n)through(n,xy[n][0],xy[n][1],n<3?0x80B07193u:0xD59371B0u);
    for(unsigned format=0;format<4;++format)for(unsigned f=0;f<8;++f)for(unsigned mask:{0u,0x00FF0Fu}){
        r[0xD2]=format;r[0xE8]=mask;r[0x22]=1;r[0xDB]=(0xFFu<<16u)|(0x90u<<8u)|f;
        r[0xD4]=(2u<<10u)|1u;r[0xD5]=(60u<<10u)|61u;
        draw(s,3,6);
        // Direct AOT VRAM modification is uploaded before the next submission.
        cm.store32(frame+84u,0xA13579BDu^f^format);gm.store32(frame+84u,0xA13579BDu^f^format);
    }
    r[0x22]=0;r[0xE8]=0;r[0xD4]=0;r[0xD5]=(63u<<10u)|63u;r[0xD2]=3;
    for(unsigned winding=0;winding<2;++winding){r[0x1D]=1;r[0x9B]=winding;draw(s,3,6,winding==1);}r[0x1D]=0;
    // Seeded off-screen and reversed-winding triangles exercise integer edges.
    std::uint32_t seed=0x17A934B2u;const auto random=[&]{seed=seed*1664525u+1013904223u;return seed;};
    for(unsigned trial=0;trial<48;++trial){for(unsigned n=0;n<6;++n)through(n,static_cast<int>(random()%104)-20,static_cast<int>(random()%104)-20,n<3?0x5137B0D9u:0x9713A257u);draw(s,3,6);}
    // Constant flat provoking color after original float XYZ projection.
    s.world=s.view={1,0,0,0,1,0,0,0,1,0,0,0};s.projection={1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1};
    const auto floating=[&](unsigned command,float value){r[command]=std::bit_cast<unsigned>(value)>>8u;};
    floating(0x42,24);floating(0x43,24);floating(0x44,100);floating(0x45,32);floating(0x46,32);floating(0x47,200);
    r[0x12]=0x19Cu;r[0x50]=0;r[0x1C]=1;
    const float positions[3][3]={{-1,-1,0},{1,-1,0},{-1,1,0}};
    for(unsigned n=0;n<3;++n)for(auto* m:{&cm,&gm}){m->store32(vertices+n*16u,0xA13579BDu+n);for(unsigned axis=0;axis<3;++axis)m->store32(vertices+n*16u+4u+axis*4u,std::bit_cast<unsigned>(positions[n][axis]));}
    draw(s,3,3);check(gpu.vertex_projection_evaluations>0,"Projected GPU path did not use native geometry");
    // Near clipping can interpolate tiny nonconstant colors: retain CPU when
    // they differ instead of rounding and quietly changing original pixels.
    r[0x50]=1;draw(s,3,3,false);
    r[0x50]=0;r[0x21]=1;r[0xDF]=0x32;draw(s,3,3,false);r[0x21]=0;
    r[0x23]=1;r[0x9E]=0x100000u;r[0x9F]=64;r[0xDE]=1;draw(s,3,3,false);r[0x23]=0;
    r[0x9C]=0x1FFF00u;draw(s,3,3,false);r[0x9C]=0;
    // Extreme coordinates are safe CPU fallbacks, never overflowing GPU edges.
    r[0x12]=0x80011Cu;through(0,-32768,-32768,~0u);through(1,32767,0,~0u);through(2,0,32767,~0u);draw(s,3,3,false);
    through(0,0,0,~0u);through(1,4,0,~0u);through(2,0,4,~0u);
    FailingTriangle failing;gpu.set_gpu_triangles(&failing);auto bad=s;bad.vertex_address=vertices;
    const auto old_pixels=gpu.draw_pixels;bool rejected=false;try{gpu.draw(bad,3,3);}catch(const std::exception&){rejected=true;}
    check(rejected && bad.vertex_address==vertices && gpu.draw_pixels==old_pixels,"GPU failure advanced guest state");same();gpu.set_gpu_triangles(&backend);
    // Narrow strides and rounded dispatches: expected colors/coverage computed
    // independently; 17 trailing words must remain untouched.
    unsigned narrow_cases=0;
    for(unsigned stride:{4u,12u,28u,60u}){
        constexpr unsigned h=17;std::vector<std::uint32_t> pixels(stride*h+17u,0xA1726394u);
        const int w=stride*16,y=h*16;
        const std::array<kon::NativeGpuFlatTriangle,2> jobs{{
            {{0,0,w,0,0,y},0x5AF07913u,5u,1u,0,0,stride,h,{}},
            {{w,0,w,y,0,y},0x5AF07913u,4u,1u,0,0,stride,h,{}}}};
        auto stats=backend.triangles(pixels,stride,jobs,0,0,stride,h,~0u);
        check(stats.covered==stride*h && stats.alpha_rejected==0,"Narrow triangle coverage overlapped shared edge");
        for(unsigned n=0;n<pixels.size();++n)check(pixels[n]==(n<stride*h?0x5AF07913u:0xA1726394u),"Rounded triangle invocation changed guards");++narrow_cases;
    }
    std::vector<std::uint32_t> pixels(256,0xA1726394u);const auto original=pixels;
    kon::NativeGpuFlatTriangle valid{{0,0,64,0,0,64},0x11223344u,5u,1u,0,0,4,4,{}};
    for(unsigned n=0;n<5;++n){auto invalid=valid;if(n==0)invalid.xy[0]=-8193;if(n==1)invalid.edge_mask=0;if(n==2)invalid.alpha_pass=2;if(n==3)invalid.right=17;if(n==4)invalid.xy[5]=0;
        const auto submissions=backend.submissions();std::array<kon::NativeGpuFlatTriangle,2> jobs{valid,invalid};rejected=false;
        try{backend.triangles(pixels,16,jobs,0,0,16,16,~0u);}catch(const std::exception&){rejected=true;}
        check(rejected && pixels==original && backend.submissions()==submissions,"Invalid triangle batch mutated/submitted work");
    }
    std::ofstream report(argv[1]);report<<"{\"native_flat_triangle_gpu_verified\":true,\"full_vram_comparisons\":"<<comparisons<<",\"bytes_compared_each_time\":"<<vram_bytes<<",\"cpu_fallback_cases\":"<<fallbacks<<",\"all_color_formats_verified\":4,\"alpha_compare_functions_verified\":8,\"narrow_stride_cases_verified\":"<<narrow_cases<<",\"independent_shared_edge_verified\":true,\"projected_triangle_verified\":true,\"failure_before_vram_commit_verified\":true,\"invalid_batch_validation_verified\":true,\"actual_gpu_triangle_submissions\":"<<backend.triangle_submissions()<<",\"selected_adapter\":\""<<backend.adapter()<<"\",\"textured_triangles_gpu_verified\":false,\"game_run_verified\":false,\"game_fps_gain_verified\":false}\n";
    check(bool(report),"Cannot save GPU triangle verification");
    std::cout<<"PASS: native Vulkan flat triangles match CPU in "<<comparisons<<" full VRAM comparisons; shared edges, masks, eight alpha tests, projection, four narrow strides, fallback and failure atomicity; actual GPU triangle submissions="<<backend.triangle_submissions()<<" on "<<backend.adapter()<<"\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
