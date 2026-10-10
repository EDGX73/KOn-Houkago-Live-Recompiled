#include "native_draw_capture.hpp"
#include "native_renderer.hpp"
#include "native_vulkan_clear.hpp"
#include "native_display.hpp"
#include <chrono>
#include <iostream>
int main(int argc,char** argv){try{
 if(argc!=8)throw std::runtime_error("Usage: benchmark_native_raster capture iterations mode(cpu/gpu) threads output.bmp timing.json full-vram.bin");
 unsigned iterations=std::stoul(argv[2]),lanes=std::stoul(argv[4]);if(!iterations||iterations>10000||!lanes||lanes>16)throw std::runtime_error("Invalid bounded benchmark");
 bool gpu=std::string(argv[3])=="gpu";if(!gpu && std::string(argv[3])!="cpu")throw std::runtime_error("Unknown benchmark mode");
 psprecomp::GuestMemory memory;const auto capture=kon::NativeDrawCapture::read(argv[1],memory);std::vector<std::uint8_t> initial(memory.vram_bytes().begin(),memory.vram_bytes().end());
 std::unique_ptr<kon::NativeVulkanClear> backend;if(gpu)backend=std::make_unique<kon::NativeVulkanClear>(true);kon::NativeRenderer renderer(memory,lanes);if(backend)renderer.set_gpu_raster(backend.get());
 std::vector<double> times;times.reserve(iterations);
 for(unsigned i=0;i<iterations+5;++i){memory.copy_in(psprecomp::GuestMemory::kVramPhysicalBase,initial);auto state=capture.state;const auto start=std::chrono::steady_clock::now();if(!renderer.draw(state,capture.kind,capture.count))throw std::runtime_error("Original draw unsupported");const auto elapsed=std::chrono::duration<double,std::micro>(std::chrono::steady_clock::now()-start).count();if(i>=5)times.push_back(elapsed);}
 std::sort(times.begin(),times.end());kon::NativeDisplay display(memory);const auto& r=capture.state.registers;if(display.set_buffer(0x04000000u|(r[0x9C]&0x1FFFF0u),r[0x9D]&0x7FCu,r[0xD2]&3u,0))throw std::runtime_error("Invalid captured display");display.save_bitmap(argv[5]);
 std::ofstream binary(argv[7],std::ios::binary);binary.write(reinterpret_cast<const char*>(memory.vram_bytes().data()),memory.vram_bytes().size());
 std::ofstream report(argv[6]);report<<"{\"mode\":\""<<(gpu?"gpu":"cpu")<<"\",\"iterations\":"<<iterations<<",\"lanes\":"<<lanes<<",\"median_us\":"<<times[times.size()/2]<<",\"p95_us\":"<<times[times.size()*95/100]<<",\"gpu_submissions\":"<<(backend?backend->raster_submissions():0)<<",\"gpu_raster_primitives\":"<<renderer.gpu_raster_primitives<<",\"full_game_fps_verified\":false}\n";
 if(backend){const auto& t=backend->timings();std::cout<<"per-draw us bin="<<t.bin_ns/(1000.0*(iterations+5))<<" upload="<<t.upload_ns/(1000.0*(iterations+5))<<" record="<<t.record_ns/(1000.0*(iterations+5))<<" submit="<<t.submit_ns/(1000.0*(iterations+5))<<" wait="<<t.wait_ns/(1000.0*(iterations+5))<<" download="<<t.download_ns/(1000.0*(iterations+5))<<'\n';}
 std::cout<<(gpu?"GPU":"CPU")<<" whole draw median_us="<<times[times.size()/2]<<" p95_us="<<times[times.size()*95/100]<<'\n';return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
