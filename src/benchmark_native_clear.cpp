#include "native_vulkan_clear.hpp"
#include "native_renderer.hpp"
#include <chrono>
#include <cstring>
#include <fstream>
#include <iostream>
#include <algorithm>
#include <memory>
int main(int argc,char** argv){try{
    if(argc!=3)throw std::runtime_error("Usage: benchmark_native_clear cpu|gpu|gpu-uncached timing.json");
    const bool cached_reads=std::string(argv[1])!="gpu-uncached";
    const bool use_gpu=std::string(argv[1])=="gpu" || !cached_reads;
    if(!use_gpu && std::string(argv[1])!="cpu")throw std::runtime_error("Unknown clear benchmark backend");
    psprecomp::GuestMemory memory,reference_memory;kon::NativeRenderer renderer(memory,1),reference(reference_memory,1);
    std::unique_ptr<kon::NativeVulkanClear> gpu;if(use_gpu){gpu=std::make_unique<kon::NativeVulkanClear>(true,cached_reads);renderer.set_gpu_clear(gpu.get());}
    constexpr unsigned vram=0x04000000u,bytes=2u*1024u*1024u,vertex=0x08810000u;
    for(auto* m:{&memory,&reference_memory}){
        for(unsigned n=0;n<bytes/4u;++n)m->store32(vram+n*4u,n*0x937B61A3u+0xA1786539u);
        for(unsigned n=0;n<8u;++n){
            m->store16(vertex+n*24u+4u,n*60u);m->store16(vertex+n*24u+6u,0);
            m->store32(vertex+n*24u+12u,0xBF716935u);
            m->store16(vertex+n*24u+16u,(n+1u)*60u);m->store16(vertex+n*24u+18u,272u);m->store16(vertex+n*24u+20u,0x9713u);
        }
    }
    kon::GeState state;state.vertex_address=vertex;auto& r=state.registers;
    r[0x12]=0x80011Cu;r[0x9D]=r[0x9F]=512u;r[0x9E]=0x100000u;r[0xD2]=3u;r[0xD3]=0x701u;r[0xD5]=(271u<<10u)|479u;
    auto expected=state;if(!reference.draw(expected,6,16))throw std::runtime_error("CPU reference clear rejected");
    std::vector<long long> times;constexpr unsigned warmup=8u,iterations=100u;
    for(unsigned n=0;n<warmup+iterations;++n){state.vertex_address=vertex;
        const auto before=std::chrono::steady_clock::now();
        if(!renderer.draw(state,6,16))throw std::runtime_error("Clear benchmark rejected");
        const auto elapsed=std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now()-before).count();
        if(n>=warmup)times.push_back(elapsed);
    }
    if(std::memcmp(memory.raw_pointer(vram,bytes),reference_memory.raw_pointer(vram,bytes),bytes))throw std::runtime_error("Benchmark framebuffer/depth changed from CPU reference");
    std::sort(times.begin(),times.end());std::ofstream report(argv[2]);
    report<<"{\"backend\":\""<<(use_gpu?(cached_reads?"gpu-cached":"gpu-uncached"):"cpu")<<"\",\"warmup\":"<<warmup<<",\"iterations\":"<<iterations<<",\"width\":480,\"height\":272,\"stripes\":8,\"color_depth\":true,\"median_us\":"<<times[times.size()/2u]/1000.0<<",\"p95_us\":"<<times[times.size()*95u/100u]/1000.0<<",\"cpu_reference_vram_exact\":true,\"whole_game_fps_verified\":false,\"gpu_clear_primitives\":"<<renderer.gpu_clear_primitives<<"}\n";
    if(gpu){
        const auto& t=gpu->timings();
        std::ofstream breakdown(std::string(argv[2])+".stages.json");const auto divisor=(warmup+iterations)*1000.0;
        breakdown<<"{\"averages_us_per_draw_including_warmup\":true,\"draws\":"<<(warmup+iterations)<<",\"memory_flags\":"<<gpu->memory_flags()<<",\"host_upload_us\":"<<t.upload_ns/divisor<<",\"command_record_us\":"<<t.record_ns/divisor<<",\"queue_submit_us\":"<<t.submit_ns/divisor<<",\"completion_wait_us\":"<<t.wait_ns/divisor<<",\"host_download_us\":"<<t.download_ns/divisor<<",\"gpu_timestamp_us\":"<<t.gpu_elapsed_ns/divisor<<",\"gpu_timestamp_samples\":"<<t.timestamp_samples<<",\"actual_submissions\":"<<gpu->submissions()<<"}\n";
        if(!breakdown)throw std::runtime_error("Cannot record GPU stage timing");
    }
    if(!report)throw std::runtime_error("Cannot record clear benchmark");
    std::cout<<(use_gpu?"GPU":"CPU")<<" exact clear; median_us="<<times[times.size()/2u]/1000.0<<" p95_us="<<times[times.size()*95u/100u]/1000.0<<'\n';return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
