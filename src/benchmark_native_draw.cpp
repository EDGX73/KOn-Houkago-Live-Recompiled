#include "native_draw_capture.hpp"
#include "native_renderer.hpp"
#include "native_display.hpp"
#include <chrono>
#include <iostream>
int main(int argc,char** argv){try{
    if(argc<5 || argc>14)throw std::runtime_error("Usage: benchmark_native_draw capture.kondraw iterations output.bmp timing.json [lanes] [texture-cache 0-1] [projection-cache 0-1] [persistent-texture-cache 0-1] [prepared-raster 0-1] [direct-vertex-reads 0-1] [fast-blend 0-1] [parallel-threshold pixels] [balanced-rows 0-1]");
    const unsigned iterations=std::stoul(argv[2]);if(!iterations || iterations>10000)throw std::runtime_error("Iterations must be between 1 and 10000");
    psprecomp::GuestMemory memory;const auto capture=kon::NativeDrawCapture::read(argv[1],memory);
    const std::vector<std::uint8_t> initial(memory.vram_bytes().begin(),memory.vram_bytes().end());
    const unsigned lanes=argc>=6?std::stoul(argv[5]):4u;if(!lanes || lanes>16)throw std::runtime_error("Raster lanes must be between 1 and 16");
    const unsigned cache=argc>=7?std::stoul(argv[6]):1u;if(cache>1)throw std::runtime_error("Texture cache must be 0 or 1");
    const unsigned projection_cache=argc>=8?std::stoul(argv[7]):1u;if(projection_cache>1)throw std::runtime_error("Projection cache must be 0 or 1");
    const unsigned persistent=argc>=9?std::stoul(argv[8]):1u;if(persistent>1)throw std::runtime_error("Persistent texture cache must be 0 or 1");
    const unsigned prepared=argc>=10?std::stoul(argv[9]):0u;if(prepared>1)throw std::runtime_error("Prepared raster must be 0 or 1");
    const unsigned direct=argc>=11?std::stoul(argv[10]):1u;if(direct>1)throw std::runtime_error("Direct vertex reads must be 0 or 1");
    const unsigned fast=argc>=12?std::stoul(argv[11]):1u;if(fast>1)throw std::runtime_error("Fast blend must be 0 or 1");
    const unsigned threshold=argc>=13?std::stoul(argv[12]):8192u;if(!threshold || threshold>10000000u)throw std::runtime_error("Invalid parallel threshold");
    const unsigned balanced=argc>=14?std::stoul(argv[13]):1u;if(balanced>1)throw std::runtime_error("Balanced rows must be 0 or 1");
    kon::NativeRenderer renderer(memory,lanes,cache!=0,projection_cache!=0,persistent!=0,prepared!=0,direct!=0,fast!=0,threshold,balanced!=0);std::vector<double> times;times.reserve(iterations);
    for(unsigned n=0;n<iterations;++n){
        memory.copy_in(psprecomp::GuestMemory::kVramPhysicalBase,initial);auto state=capture.state;
        const auto before=std::chrono::steady_clock::now();
        if(!renderer.draw(state,capture.kind,capture.count))throw std::runtime_error("Captured draw remains unsupported");
        times.push_back(std::chrono::duration<double,std::micro>(std::chrono::steady_clock::now()-before).count());
    }
    kon::NativeDisplay display(memory);const auto& r=capture.state.registers;
    if(display.set_buffer(0x04000000u|(r[0x9C]&0x1FFFF0u),r[0x9D]&0x7FCu,r[0xD2]&3u,0))throw std::runtime_error("Captured framebuffer cannot be displayed");
    display.save_bitmap(argv[3]);std::sort(times.begin(),times.end());
    std::ofstream output(argv[4]);output<<"{\"iterations\":"<<iterations<<",\"balanced_rows\":"<<balanced<<",\"parallel_threshold\":"<<threshold<<",\"lanes\":"<<lanes<<",\"texture_cache_preparations\":"<<renderer.texture_cache_preparations<<",\"texture_cache_hits\":"<<renderer.texture_cache_hits<<",\"persistent_texture_cache\":"<<persistent<<",\"texture_cache\":"<<cache<<",\"fast_blend\":"<<fast<<",\"direct_vertex_reads\":"<<direct<<",\"prepared_raster\":"<<prepared<<",\"projection_cache\":"<<projection_cache<<",\"vertex_projection_evaluations\":"<<renderer.vertex_projection_evaluations<<",\"pc\":"<<capture.pc<<",\"vertices\":"<<capture.count<<",\"draw_only_microseconds\":{\"min\":"<<times.front()<<",\"median\":"<<times[times.size()/2]<<",\"p95\":"<<times[std::min(times.size()-1,times.size()*95/100)]<<"},\"full_game_fps_verified\":false}\n";
    if(!output)throw std::runtime_error("Cannot save timing output");
    std::cout<<"Isolated original draw: median "<<times[times.size()/2]<<" us. Full game FPS is not measured here.\n";return 0;
}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}}
