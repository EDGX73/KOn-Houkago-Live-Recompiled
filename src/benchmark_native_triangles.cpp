#include "native_vulkan_clear.hpp"
#include "native_renderer.hpp"
#include <chrono>
#include <cstring>
#include <fstream>
#include <iostream>
#include <algorithm>
#include <memory>
int main(int argc,char** argv){try{
    if(argc!=4)throw std::runtime_error("Usage: benchmark_native_triangles cpu|gpu triangle-pairs timing.json");
    const bool use_gpu=std::string(argv[1])=="gpu";if(!use_gpu && std::string(argv[1])!="cpu")throw std::runtime_error("Unknown backend");
    const unsigned pairs=std::stoul(argv[2]);if(!pairs || pairs>64)throw std::runtime_error("Triangle pairs must be 1-64");
    psprecomp::GuestMemory memory,reference_memory;kon::NativeRenderer renderer(memory,4),reference(reference_memory,4);
    std::unique_ptr<kon::NativeVulkanClear> gpu;if(use_gpu){gpu=std::make_unique<kon::NativeVulkanClear>();renderer.set_gpu_triangles(gpu.get());}
    constexpr unsigned vram=0x04000000u,bytes=2u*1024u*1024u,vertices=0x08810000u;
    const unsigned xy[6][2]={{0,0},{480,0},{0,272},{480,0},{480,272},{0,272}};
    for(auto* m:{&memory,&reference_memory}){
        for(unsigned n=0;n<bytes/4u;++n)m->store32(vram+n*4u,n*0x937B61A3u+0xA1786539u);
        for(unsigned n=0;n<pairs*6u;++n){const auto address=vertices+n*12u;m->store32(address,0xBF716935u+(n/6u)*0x7135u);m->store16(address+4u,xy[n%6u][0]);m->store16(address+6u,xy[n%6u][1]);m->store16(address+8u,100);}
    }
    kon::GeState state;state.vertex_address=vertices;auto& r=state.registers;r[0x12]=0x80011Cu;r[0x9D]=512;r[0xD2]=3;r[0x16]=r[0xD5]=(271u<<10u)|479u;
    auto expected=state;if(!reference.draw(expected,3,pairs*6u))throw std::runtime_error("CPU reference rejected");
    std::vector<long long> times;constexpr unsigned warmup=4,iterations=40;
    for(unsigned n=0;n<warmup+iterations;++n){state.vertex_address=vertices;const auto before=std::chrono::steady_clock::now();if(!renderer.draw(state,3,pairs*6u))throw std::runtime_error("Triangle benchmark rejected");const auto elapsed=std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now()-before).count();if(n>=warmup)times.push_back(elapsed);}
    if(std::memcmp(memory.raw_pointer(vram,bytes),reference_memory.raw_pointer(vram,bytes),bytes))throw std::runtime_error("Triangle benchmark differs from CPU");
    if(renderer.draw_covered_pixels!=(warmup+iterations)*reference.draw_covered_pixels)throw std::runtime_error("Benchmark coverage differs");
    if(use_gpu && gpu->triangle_submissions()!=warmup+iterations)throw std::runtime_error("GPU benchmark used CPU fallback");
    std::sort(times.begin(),times.end());std::ofstream out(argv[3]);out<<"{\"backend\":\""<<(use_gpu?"gpu":"cpu-four-lanes")<<"\",\"width\":480,\"height\":272,\"triangles\":"<<pairs*2u<<",\"iterations\":"<<iterations<<",\"warmup\":"<<warmup<<",\"median_us\":"<<times[times.size()/2u]/1000.0<<",\"p95_us\":"<<times[times.size()*95u/100u]/1000.0<<",\"cpu_reference_vram_exact\":true,\"actual_gpu_triangle_submissions\":"<<(gpu?gpu->triangle_submissions():0)<<",\"textured\":false,\"depth_test\":false,\"whole_game_fps_verified\":false}\n";
    if(!out)throw std::runtime_error("Cannot save triangle benchmark");std::cout<<(use_gpu?"GPU":"CPU")<<" exact flat triangles="<<pairs*2u<<" median_us="<<times[times.size()/2u]/1000.0<<" p95_us="<<times[times.size()*95u/100u]/1000.0<<'\n';return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
