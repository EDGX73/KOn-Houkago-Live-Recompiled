#include "native_render_worker.hpp"
#include "native_draw_capture.hpp"
#include "native_renderer.hpp"
#include "native_vulkan_clear.hpp"
#include <fstream>
#include <iostream>
using namespace std::chrono_literals;
void require(bool value,const char* message){if(!value)throw std::runtime_error(message);}
int main(int argc,char** argv){try{
 if(argc!=3)throw std::runtime_error("Usage: native_vulkan_render_worker_test project-root report.json");
 const std::filesystem::path root=argv[1];const auto start=std::chrono::steady_clock::now();auto clock=[&]{return std::uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now()-start).count());};
 psprecomp::GuestMemory original;const auto capture=kon::NativeDrawCapture::read(root/"analysis/native_actual_small_quad_en.kondraw",original);
 kon::NativeFrameStream stream(original);kon::NativeRenderer reference(original,1);reference.set_frame_stream(&stream);auto state=capture.state;
 require(reference.draw(state,capture.kind,capture.count),"Original capture rejected by CPU reference");const auto& r=capture.state.registers;
 auto frame=stream.finish(0x04000000u|(r[0x9C]&0x1FFFF0u),r[0x9D]&0x7FCu,r[0xD2]&3u,clock());require(frame && frame->complete,"Captured immutable frame incomplete");
 const auto expected=original.vram_bytes();std::string adapter;std::thread::id owner;
 kon::NativeRenderWorker worker([&]{auto gpu=std::make_unique<kon::NativeVulkanClear>();require(gpu->raster_supported(),"Private Vulkan renderer unavailable");adapter=gpu->adapter();owner=std::this_thread::get_id();return gpu;},clock,165);
 worker.submit(frame);const auto until=std::chrono::steady_clock::now()+5s;std::shared_ptr<const kon::NativeRenderWorker::Output> output;
 while(!(output=worker.latest())){if(std::chrono::steady_clock::now()>until)throw std::runtime_error("Private Vulkan output timed out");std::this_thread::sleep_for(1ms);}
 require(output->vram==expected,"GPU worker changed complete captured VRAM");require(owner!=std::this_thread::get_id(),"Vulkan worker ran on original producer thread");require(original.vram_bytes()==expected,"GPU worker changed original memory");
 const auto before=worker.stats().completed;for(unsigned fps:{30u,60u,120u,165u,237u})worker.set_rate(fps);std::this_thread::sleep_for(20ms);require(worker.stats().completed==before,"Repeated frozen source counted as new rendering");worker.stop();
 std::ofstream out(argv[2]);out<<"{\"selected_adapter\":\""<<adapter<<"\",\"private_gpu_thread_verified\":true,\"captured_draw_commands\":"<<frame->commands.size()<<",\"full_vram_bytes_compared\":"<<expected.size()<<",\"source_memory_unchanged\":true,\"rate_changes_accepted\":[30,60,120,165,237],\"frozen_source_repaints_excluded\":true,\"live_game_presentation_connected\":false,\"arbitrary_game_fps_verified\":false,\"full_song_verified\":false}\n";require(bool(out),"Could not write GPU worker proof");
 std::cout<<"PASS: private Vulkan worker reconstructs an original captured draw exactly on its own thread; adapter="<<adapter<<". Complete live frame/FPS/sync acceptance remains separate.\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
