#include "native_vulkan_clear.hpp"
#include "native_renderer.hpp"
#include <cstring>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <array>
#include <vector>
void check(bool value,const char* message){if(!value)throw std::runtime_error(message);}
namespace {
struct FailingDepth final : kon::NativeGpuClear {
    unsigned calls=0;
    void clear(std::span<std::uint32_t> pixels,unsigned,std::span<const kon::NativeGpuClearRectangle>)override{
        if(++calls==2)throw std::runtime_error("Expected diagnostic GPU failure");
        pixels[0]^=0xFFFFFFFFu;
    }
};
}
int main(int argc,char** argv){try{
    if(argc!=2)throw std::runtime_error("Usage: native_vulkan_renderer_test verification.json");
    kon::NativeVulkanClear backend;
    psprecomp::GuestMemory cpu_memory,gpu_memory;
    kon::NativeRenderer cpu(cpu_memory,1),gpu(gpu_memory,1);gpu.set_gpu_clear(&backend);
    constexpr unsigned vram=0x04000000u,bytes=2u*1024u*1024u,vertices=0x08810000u;
    for(unsigned n=0;n<bytes/4u;++n){const auto value=(n*0x9E3779B9u)^0xA173592Bu;cpu_memory.store32(vram+n*4u,value);gpu_memory.store32(vram+n*4u,value);}
    const auto same=[&]{check(std::memcmp(cpu_memory.raw_pointer(vram,bytes),gpu_memory.raw_pointer(vram,bytes),bytes)==0,"Integrated GPU clear differs from CPU reference VRAM");};
    unsigned comparisons=0;
    const auto draw=[&](kon::GeState state,unsigned count=6u){
        auto reference=state;const auto old_pixels=cpu.clear_pixels;
        check(cpu.draw(reference,6,count) && gpu.draw(state,6,count),"Clear operation rejected");
        check(reference.vertex_address==state.vertex_address,"GPU clear changed vertex consumption");
        check(cpu.clear_pixels==gpu.clear_pixels && cpu.clear_pixels>=old_pixels,"GPU clear diagnostics changed coverage");
        same();++comparisons;
    };
    // Distinct and overlapping sprites, reversed endpoint order, clipped at the
    // scissor. The full VRAM comparison checks pitch, depth and untouched guards.
    const unsigned geometry[6][2]={{7,9},{121,56},{97,45},{31,17},{55,7},{128,64}};
    for(unsigned n=0;n<6;++n)for(auto* memory:{&cpu_memory,&gpu_memory}){
        memory->store32(vertices+n*12u,0xB07193D5u+n*0x103071u);
        memory->store16(vertices+n*12u+4u,geometry[n][0]);memory->store16(vertices+n*12u+6u,geometry[n][1]);memory->store16(vertices+n*12u+8u,0x7213u+n*217u);
    }
    kon::GeState state;auto& r=state.registers;state.vertex_address=vertices;
    r[0x12]=0x80011Cu;r[0x9C]=0x10u;r[0x9D]=128u;r[0x9E]=0x100000u;r[0x9F]=132u;r[0xD4]=(3u<<10u)|2u;r[0xD5]=(63u<<10u)|127u;
    for(unsigned format=0;format<4u;++format){r[0xD2]=format;
        for(unsigned mask:{0x101u,0x201u,0x301u,0x401u,0x501u,0x601u,0x701u,0x1u}){
            r[0xD3]=mask;draw(state);
            // A direct compiled-game memory write between submissions must be
            // uploaded; persistent resources must not retain an old VRAM copy.
            for(auto* memory:{&cpu_memory,&gpu_memory})memory->store32(vram+4096u,0x71F379BDu^format^mask);
            draw(state);
        }
    }
    const auto gpu_before_fallback=gpu.gpu_clear_primitives;
    r[0xD2]=3u;r[0xD3]=0x701u;r[0x9C]=r[0x9E]=0;r[0x9F]=128u;
    draw(state);check(gpu.gpu_clear_primitives==gpu_before_fallback,"Aliased color/depth clear lost CPU ordering fallback");
    r[0x9C]=0x1FFF00u;r[0xD3]=0x301u;
    draw(state);check(gpu.gpu_clear_primitives==gpu_before_fallback,"Mirrored framebuffer clear bypassed CPU fallback");
    r[0x9C]=0x10u;r[0x9E]=0x100000u;r[0xD4]=(63u<<10u)|127u;r[0xD5]=0;
    draw(state);check(gpu.gpu_clear_primitives==gpu_before_fallback,"Fully clipped clear reached GPU");
    r[0xD4]=0;r[0xD5]=(63u<<10u)|127u;r[0x9D]=64u;
    const auto submissions_before_invalid=backend.submissions();bool invalid=false;
    try{auto malformed=state;gpu.draw(malformed,6,6);}catch(const std::exception&){invalid=true;}
    check(invalid && submissions_before_invalid==backend.submissions(),"Invalid clear reached GPU before validation");same();
    std::vector<std::uint32_t> guard(8192u,0xA1726394u);const auto original=guard;
    const std::array<kon::NativeGpuClearRectangle,2> invalid_jobs{{{0,0,2,2,0u,~0u},{0,0,129,64,0u,~0u}}};
    invalid=false;try{backend.clear(guard,128u,invalid_jobs);}catch(const std::exception&){invalid=true;}
    check(invalid && guard==original && submissions_before_invalid==backend.submissions(),"Invalid GPU job changed destination or submitted work");
    // Reuse the same persistent resources for a full-frame destination.
    guard.resize(8192u,0xA1726394u);
    const std::array<kon::NativeGpuClearRectangle,1> bounded_job{{{0,0,128,64,0x5AF07913u,~0u}}};
    // An oversized rectangle is rejected by the public API. Use the isolated
    // shader test for deliberately rounded invocations beyond an active prefix.
    backend.clear(guard,128u,bounded_job);
    check(guard.front()==0x5AF07913u && guard.back()==0x5AF07913u,"Persistent GPU destination did not update");
    // Rounded X groups can extend into the next row's addresses on narrow
    // strides. They must not load/store those pixels or race valid invocations.
    for(unsigned stride:{4u,12u,28u,60u}){
        constexpr unsigned height=17u,initial=0xA1726394u;
        std::vector<std::uint32_t> narrow(stride*height+17u,initial);
        const std::array<kon::NativeGpuClearRectangle,2> jobs{{
            {0,0,stride,height,0x5AF07913u,~0u},
            {1,1,stride-1u,height-1u,0xC33751A9u,0xFF00FFFFu}}};
        backend.clear(narrow,stride,jobs);
        for(unsigned n=0;n<narrow.size();++n){
            const auto x=n%stride,y=n/stride;
            const auto expected=n>=stride*height?initial:
                (x>=1u && x<stride-1u && y>=1u && y<height-1u?0xC3F051A9u:0x5AF07913u);
            check(narrow[n]==expected,"Rounded narrow GPU dispatch touched another row or guards");
        }
    }
    FailingDepth failing;gpu.set_gpu_clear(&failing);r[0x9D]=128u;r[0x9F]=132u;r[0xD3]=0x701u;
    invalid=false;try{auto copy=state;gpu.draw(copy,6,6);}catch(const std::exception&){invalid=true;}
    check(invalid && failing.calls==2,"Expected second-pass GPU failure missing");same();
    gpu.set_gpu_clear(&backend);
    std::ofstream report(argv[1]);
    report<<"{\"native_renderer_clear_integration_verified\":true,\"game_run_verified\":false,\"all_color_formats_verified\":4,\"full_vram_comparisons\":"<<comparisons<<",\"bytes_compared_each_time\":"<<bytes<<",\"gpu_clear_primitives\":"<<gpu.gpu_clear_primitives<<",\"actual_gpu_submissions\":"<<backend.submissions()<<",\"rounded_narrow_stride_cases_verified\":4,\"depth_color_masks_verified\":true,\"compiled_vram_write_refresh_verified\":true,\"feedback_and_mirror_cpu_fallback_verified\":true,\"failure_before_vram_commit_verified\":true,\"game_fps_gain_verified\":false,\"selected_adapter\":\""<<backend.adapter()<<"\"}\n";
    check(static_cast<bool>(report),"Cannot save integrated GPU renderer evidence");
    std::cout<<"PASS: integrated native Vulkan clears match CPU for "<<comparisons<<" full VRAM comparisons, all four color formats, depth, masks, overlap and host writes; actual submissions="<<backend.submissions()<<" on "<<backend.adapter()<<"\n";
    return 0;
}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}}
