#include "native_renderer.hpp"
#include "native_vulkan_clear.hpp"
#include "native_draw_capture.hpp"
#include <iostream>
#include <fstream>
#include <cstring>
#include <chrono>
void require(bool value,const char* message){if(!value)throw std::runtime_error(message);}
struct FailingRaster:kon::NativeGpuRaster {
 kon::NativeGpuRasterStats raster(std::span<std::uint32_t> color,std::span<std::uint32_t>,std::span<const std::uint32_t>,std::span<const kon::NativeGpuRasterTriangle>,const std::array<std::uint32_t,32>&)override{color[0]^=~0u;throw std::runtime_error("Injected GPU failure before commit");}
};
int main(int argc,char** argv){try{
 if(argc!=3)throw std::runtime_error("Usage: native_vulkan_raster_test project-root verification.json");
 const std::filesystem::path root=argv[1];kon::NativeVulkanClear backend;require(backend.raster_supported(),"Exact GPU raster unavailable");
 unsigned comparisons=0;std::uint64_t covered=0,drawn=0;
 for(const char* name:{"native_actual_small_quad_en","native_song_stage_main_textured_en","native_first_song_blend_pending_en","native_pending_draw_en","native_song_eye_plane_pending_en"}){
  const auto path=root/"analysis"/(std::string(name)+".kondraw");if(!std::filesystem::exists(path))continue;
  for(unsigned variation=0;variation<30;++variation){
   psprecomp::GuestMemory cm,gm;const auto capture=kon::NativeDrawCapture::read(path,cm);kon::NativeDrawCapture::read(path,gm);
   auto state=capture.state;auto& r=state.registers;
   if(variation>=1 && variation<=4)r[0xD2]=variation-1;
   if(variation==5)r[0xC6]=0;
   if(variation==6)r[0xC6]=0x101;
   if(variation==7)r[0xC7]=0x101;
   if(variation==8)r[0xC7]=0;
   if(variation==9)r[0xC9]=0x100;
   if(variation==10)r[0xC9]=0x103;
   if(variation==11)r[0xC9]|=0x10000;
   if(variation==12){r[0x21]=1;r[0xDF]=0x32;}
   if(variation==13){r[0x21]=1;r[0xDF]=0xA2;r[0xE1]=0xFFFFFF;}
   if(variation>=14 && variation<=21){r[0x22]=1;r[0xDB]=0xFF8000|(variation-14);}
   if(variation>=22){r[0x23]=1;r[0xDE]=variation-22;}
   kon::NativeRenderer cpu(cm,1),gpu(gm,4);gpu.set_gpu_raster(&backend);auto other=state;
   const auto before=backend.raster_submissions();
   require(cpu.draw(other,capture.kind,capture.count),"CPU reference rejected captured draw");
   require(gpu.draw(state,capture.kind,capture.count),"GPU rejected captured draw");
   if(std::memcmp(cm.vram_bytes().data(),gm.vram_bytes().data(),cm.vram_bytes().size())){
    const auto a=cm.vram_bytes(),b=gm.vram_bytes();unsigned count=0;for(std::size_t n=0;n<a.size();++n)if(a[n]!=b[n]){if(count++<8)std::cerr<<"different byte="<<n<<" cpu="<<unsigned(a[n])<<" gpu="<<unsigned(b[n])<<'\n';}
    std::cerr<<"capture="<<name<<" variation="<<variation<<" differing_bytes="<<count<<'\n';throw std::runtime_error("GPU raster changed full VRAM");
   }
   require(cpu.draw_pixels==gpu.draw_pixels && cpu.draw_covered_pixels==gpu.draw_covered_pixels && cpu.alpha_rejected_pixels==gpu.alpha_rejected_pixels && cpu.depth_rejected_pixels==gpu.depth_rejected_pixels,"GPU diagnostic counters differ");
   require(state.vertex_address==other.vertex_address,"GPU vertex consumption differs");
   if(backend.raster_submissions()==before && cpu.draw_covered_pixels){std::cerr<<"CPU coherency fallback: capture="<<name<<" variation="<<variation<<'\n';require(variation!=0,"Original captured draw remained on CPU");}
   if(variation==0){const auto out=root/"analysis"/(std::string("native_gpu_raster_exact_")+name+".vram.bin");std::ofstream stream(out,std::ios::binary);stream.write(reinterpret_cast<const char*>(gm.vram_bytes().data()),gm.vram_bytes().size());}
   ++comparisons;covered+=gpu.draw_covered_pixels;drawn+=gpu.draw_pixels;
  }
  std::cout<<"Exact GPU captured draw: "<<name<<" (30 variations)\n";
 }
 // Batched blending must observe every previous draw while guest VRAM stays
 // unpublished until the complete batch succeeds.
 {
  psprecomp::GuestMemory cm,gm;const auto capture=kon::NativeDrawCapture::read(root/"analysis/native_actual_small_quad_en.kondraw",cm);kon::NativeDrawCapture::read(root/"analysis/native_actual_small_quad_en.kondraw",gm);
  const std::vector<std::uint8_t> initial(gm.vram_bytes().begin(),gm.vram_bytes().end());kon::NativeFrameStream stream(gm);kon::NativeRenderer cpu(cm,1),gpu(gm);gpu.set_frame_stream(&stream);gpu.set_gpu_raster(&backend);gpu.begin_gpu_batch();const auto before=backend.raster_submissions();
  for(unsigned n=0;n<24;++n){
   // A persistent decoded texture must notice changes to source and loaded CLUT.
   if(n==8){const auto& r=capture.state.registers;const auto at=(r[0xA0]&0xFFFFF0u)|((r[0xA8]<<8u)&0x0F000000u);cm.store8(at,cm.load8(at)^0x11u);gm.store8(at,gm.load8(at)^0x11u);}
   auto state=capture.state;if(n>=16)state.clut[0]^=0x55u;state.registers[0x21]=1;state.registers[0xDF]=0x32;state.registers[0xC9]=n&1?0x103:0x100;auto other=state;require(cpu.draw(other,capture.kind,capture.count) && gpu.draw(state,capture.kind,capture.count),"Batch draw rejected");}
  require(std::equal(initial.begin(),initial.end(),gm.vram_bytes().begin()),"Batch published unfinished pixels");gpu.end_gpu_batch();
  require(backend.raster_submissions()==before+1,"Batch still submitted once per draw");
  require(std::memcmp(cm.vram_bytes().data(),gm.vram_bytes().data(),cm.vram_bytes().size())==0,"Batch changed overlapping blend order");
  require(cpu.draw_pixels==gpu.draw_pixels && cpu.draw_covered_pixels==gpu.draw_covered_pixels,"Batch lost actual GPU counters");
  const auto frame=stream.finish(0x04000000u|(capture.state.registers[0x9C]&0x1FFFF0u),capture.state.registers[0x9D]&0x7FCu,capture.state.registers[0xD2]&3u,100000);
  require(frame && frame->complete && frame->commands.size()==24,"Whole frame stream lost ordered packets");
  require(frame->commands[0].texture && frame->commands[0].texture==frame->commands[1].texture,"Immutable frame duplicated an unchanged texture");
  const auto output=kon::replay_native_frame(*frame,backend);
  require(std::equal(output.begin(),output.end(),gm.vram_bytes().begin()),"Private frame replay changed original output");++comparisons;
 }
 // Real GPU output must be published before original compiled reads or writes.
 unsigned ownership_cases=0;
 for(unsigned operation=0;operation<9;++operation){
  psprecomp::GuestMemory cm,gm;const auto capture=kon::NativeDrawCapture::read(root/"analysis/native_actual_small_quad_en.kondraw",cm);kon::NativeDrawCapture::read(root/"analysis/native_actual_small_quad_en.kondraw",gm);
  const auto initial=gm.vram_bytes();kon::NativeRenderer cpu(cm,1),gpu(gm);gpu.set_gpu_raster(&backend);gpu.begin_gpu_batch();auto reference=capture.state,state=capture.state;
  require(cpu.draw(reference,capture.kind,capture.count),"Ownership CPU reference rejected");
  std::size_t changed=0;while(changed<initial.size() && cm.vram_bytes()[changed]==initial[changed])++changed;
  require(changed<initial.size(),"Ownership fixture did not modify pixels");
  struct Pending {kon::NativeRenderer* renderer;unsigned flushes=0;} pending{&gpu};
  psprecomp::set_native_vram_access_hook(&gm,[](void* argument,std::uint32_t at,std::size_t size)noexcept{
   auto& p=*static_cast<Pending*>(argument);if(!p.renderer->pending_vram_access(at,size))return true;
   try{p.renderer->flush_gpu_batch();++p.flushes;return true;}catch(...){return false;}
  },&pending);
  const auto before=backend.raster_submissions();
  require(gpu.draw(state,capture.kind,capture.count),"Ownership GPU draw rejected");
  require(backend.raster_submissions()==before && pending.flushes==0,"Renderer-internal access submitted pending draw");
  std::uint32_t unrelated=0;while(unrelated<initial.size() && gpu.pending_vram_access(0x04000000u+unrelated,1))unrelated+=4096;
  require(unrelated<initial.size(),"No disjoint VRAM region for ownership test");(void)gm.aot_load8(0x44000000u+unrelated);
  require(pending.flushes==0,"Disjoint front-buffer read submitted back-buffer GPU work");
  const auto address=0x44000000u+static_cast<std::uint32_t>(changed);
  switch(operation){
   case 0:require(gm.aot_load8(address)==cm.aot_load8(address),"AOT read stale GPU pixel");break;
   case 1:require(gm.aot_load16(address)==cm.aot_load16(address),"AOT halfword read stale GPU pixel");break;
   case 2:require(gm.aot_load32(address)==cm.aot_load32(address),"AOT word read stale GPU pixel");break;
   case 3:gm.aot_store32(address,0x13579BDF);cm.aot_store32(address,0x13579BDF);break;
   case 4:{const auto* p=gm.raw_pointer(address,4);require(p && *p==cm.load8(address),"Direct pointer stale GPU pixel");break;}
   case 5:{std::array<std::uint8_t,32>a{},b{};gm.copy_out(address,a);cm.copy_out(address,b);require(a==b,"Bulk read stale GPU pixels");break;}
   case 6:{std::array<std::uint8_t,32>a{};a.fill(0x73);gm.copy_in(address,a);cm.copy_in(address,a);break;}
   case 7:gm.zero(address,32);cm.zero(address,32);break;
   case 8:gm.aot_store16(address,0x1357);cm.aot_store16(address,0x1357);break;
  }
  require(pending.flushes==1 && backend.raster_submissions()==before+1,"Original access failed to submit exactly one complete batch");
  psprecomp::set_native_vram_access_hook(nullptr,nullptr,nullptr);gpu.end_gpu_batch();
  require(gm.vram_bytes()==cm.vram_bytes(),"CPU ownership changed full VRAM or lost original store");++comparisons;++ownership_cases;
 }
 // A real song draw fills the fixed packet arena. The renderer must publish
 // ordered groups and retry without losing a draw or reusing stale pixels.
 {
  psprecomp::GuestMemory cm,gm;const auto capture=kon::NativeDrawCapture::read(root/"analysis/native_song_stage_main_textured_en.kondraw",cm);kon::NativeDrawCapture::read(root/"analysis/native_song_stage_main_textured_en.kondraw",gm);
  kon::NativeRenderer cpu(cm,16),gpu(gm);gpu.set_gpu_raster(&backend);gpu.begin_gpu_batch();const auto before=backend.raster_submissions();
  for(unsigned n=0;n<1100;++n){auto reference=capture.state,state=capture.state;require(cpu.draw(reference,capture.kind,capture.count) && gpu.draw(state,capture.kind,capture.count),"Capacity pressure rejected original draw");}
  gpu.end_gpu_batch();require(backend.raster_submissions()>before+1,"Capacity pressure failed to cross a batch boundary");
  require(cm.vram_bytes()==gm.vram_bytes(),"Capacity flush changed ordered full VRAM");
  require(cpu.draw_pixels==gpu.draw_pixels && cpu.draw_covered_pixels==gpu.draw_covered_pixels,"Capacity flush lost drawing counters");++comparisons;
  std::cout<<"Exact bounded GPU capacity flush: 1100 original song draws\n";
 }
 // The same 1100 draws through the game's draw queue: geometry is built on
 // the worker threads, then recorded in order (and across capacity flushes).
 {
  psprecomp::GuestMemory cm,gm;const auto capture=kon::NativeDrawCapture::read(root/"analysis/native_song_stage_main_textured_en.kondraw",cm);kon::NativeDrawCapture::read(root/"analysis/native_song_stage_main_textured_en.kondraw",gm);
  kon::NativeRenderer cpu(cm,16),gpu(gm,16);gpu.set_gpu_raster(&backend);gpu.begin_gpu_batch();gpu.set_draw_queue(true);
  for(unsigned n=0;n<1100;++n){auto reference=capture.state,state=capture.state;require(cpu.draw(reference,capture.kind,capture.count) && gpu.draw(state,capture.kind,capture.count),"Queued draw rejected");
   require(reference.vertex_address==state.vertex_address,"Queued draw did not advance the vertex address");}
  require(gpu.pending_flushes==0,"Queue flushed before the batch end");
  gpu.end_gpu_batch();require(gpu.pending_draws==1100,"Queue lost draws");
  require(cm.vram_bytes()==gm.vram_bytes(),"Queued draws changed ordered full VRAM");
  require(cpu.draw_pixels==gpu.draw_pixels && cpu.draw_covered_pixels==gpu.draw_covered_pixels,"Queued draws lost drawing counters");++comparisons;
  std::cout<<"Exact queued parallel geometry: 1100 original song draws\n";
 }
 // An open batch reads only destination words it does not already own. A
 // guest write beyond the first draw's extent must reach the extended draw.
 {
  psprecomp::GuestMemory cm,gm;const auto capture=kon::NativeDrawCapture::read(root/"analysis/native_song_stage_main_textured_en.kondraw",cm);kon::NativeDrawCapture::read(root/"analysis/native_song_stage_main_textured_en.kondraw",gm);
  kon::NativeRenderer cpu(cm,16),gpu(gm);gpu.set_gpu_raster(&backend);gpu.begin_gpu_batch();const auto before=backend.raster_submissions();
  const auto& r=capture.state.registers;const auto bytes=(r[0xD2]&3u)==3?4u:2u,stride=r[0x9D]&0x7FCu;const auto frame=0x04000000u|(r[0x9C]&0x1FFFF0u);
  const auto clip=[](kon::GeState s,unsigned bottom){s.registers[0xD5]=(s.registers[0xD5]&~(1023u<<10))|((bottom-1)<<10);s.registers[0x16]=(s.registers[0x16]&~(1023u<<10))|((bottom-1)<<10);return s;};
  {auto a=clip(capture.state,133),b=a;require(cpu.draw(a,capture.kind,capture.count) && gpu.draw(b,capture.kind,capture.count),"Partial scissor draw rejected");}
  const auto tail=frame+(150u*stride+16u)*bytes;for(unsigned n=0;n<64;++n){cm.store32(tail+n*4u,0x5A3C0F00u+n);gm.store32(tail+n*4u,0x5A3C0F00u+n);}
  const auto reused=gpu.gpu_resident_words_reused;
  {auto a=capture.state,b=a;require(cpu.draw(a,capture.kind,capture.count) && gpu.draw(b,capture.kind,capture.count),"Extended draw rejected");}
  require(gpu.gpu_resident_words_reused>reused,"Open batch reread its resident destination prefix");
  gpu.end_gpu_batch();require(backend.raster_submissions()==before+1,"Extended destination split the batch");
  require(cm.vram_bytes()==gm.vram_bytes(),"Resident prefix reuse lost a guest write or ordered pixel");++comparisons;
  std::cout<<"Exact resident prefix reuse with extended destination\n";
 }
 // A submitted (in-flight) batch keeps VRAM ownership: an original read must
 // collect it, and a later draw must collect before reusing GPU staging.
 {
  psprecomp::GuestMemory cm,gm;const auto capture=kon::NativeDrawCapture::read(root/"analysis/native_actual_small_quad_en.kondraw",cm);kon::NativeDrawCapture::read(root/"analysis/native_actual_small_quad_en.kondraw",gm);
  const auto initial=gm.vram_bytes();kon::NativeRenderer cpu(cm,1),gpu(gm);gpu.set_gpu_raster(&backend);gpu.begin_gpu_batch();
  {auto a=capture.state,b=a;require(cpu.draw(a,capture.kind,capture.count) && gpu.draw(b,capture.kind,capture.count),"Async draw rejected");}
  std::size_t changed=0;while(changed<initial.size() && cm.vram_bytes()[changed]==initial[changed])++changed;require(changed<initial.size(),"Async fixture did not modify pixels");
  struct Pending {kon::NativeRenderer* renderer;unsigned flushes=0;} pending{&gpu};
  psprecomp::set_native_vram_access_hook(&gm,[](void* argument,std::uint32_t at,std::size_t size)noexcept{
   auto& p=*static_cast<Pending*>(argument);if(!p.renderer->pending_vram_access(at,size))return true;
   try{p.renderer->flush_gpu_batch();++p.flushes;return true;}catch(...){return false;}
  },&pending);
  gpu.submit_gpu_batch();gpu.begin_gpu_batch();
  require(gpu.pending_vram_access(0x04000000u+static_cast<std::uint32_t>(changed),1),"In-flight batch released VRAM ownership early");
  require(gm.aot_load32(0x44000000u+static_cast<std::uint32_t>(changed&~3u))==cm.aot_load32(0x44000000u+static_cast<std::uint32_t>(changed&~3u)) && pending.flushes==1,"Original read missed in-flight GPU pixels");
  gpu.submit_gpu_batch();gpu.begin_gpu_batch();
  {auto a=capture.state,b=a;a.registers[0xDF]=b.registers[0xDF]=0x32;a.registers[0x21]=b.registers[0x21]=1;require(cpu.draw(a,capture.kind,capture.count) && gpu.draw(b,capture.kind,capture.count),"Draw after async submit rejected");}
  gpu.submit_gpu_batch();require(gpu.pending_vram_access(0x04000000u+static_cast<std::uint32_t>(changed),1),"Second in-flight batch lost ownership");
  {auto a=capture.state,b=a;require(cpu.draw(a,capture.kind,capture.count) && gpu.draw(b,capture.kind,capture.count),"Draw while batch in flight rejected");}
  psprecomp::set_native_vram_access_hook(nullptr,nullptr,nullptr);gpu.end_gpu_batch();
  require(cm.vram_bytes()==gm.vram_bytes(),"Asynchronous submission changed ordered VRAM");++comparisons;
  std::cout<<"Exact asynchronous batch submission and lazy collection\n";
 }
 psprecomp::GuestMemory failure_memory;const auto capture=kon::NativeDrawCapture::read(root/"analysis/native_actual_small_quad_en.kondraw",failure_memory);
 std::vector<std::uint8_t> initial(failure_memory.vram_bytes().begin(),failure_memory.vram_bytes().end());FailingRaster failing;kon::NativeRenderer renderer(failure_memory);renderer.set_gpu_raster(&failing);auto state=capture.state;bool caught=false;
 try{renderer.draw(state,capture.kind,capture.count);}catch(const std::runtime_error&){caught=true;}
 require(caught && std::equal(initial.begin(),initial.end(),failure_memory.vram_bytes().begin()),"Failed raster published guest VRAM");
 std::ofstream report(argv[2]);report<<"{\"native_textured_raster_verified\":true,\"full_vram_comparisons\":"<<comparisons<<",\"bytes_compared_each_time\":2097152,\"actual_gpu_submissions\":"<<backend.raster_submissions()<<",\"covered\":"<<covered<<",\"drawn\":"<<drawn<<",\"failure_before_vram_commit_verified\":true,\"selected_adapter\":\""<<backend.adapter()<<"\",\"ownership_access_cases\":"<<ownership_cases<<",\"bounded_capacity_draws_verified\":1100,\"full_song_verified\":false,\"complete_gpu_renderer\":false}\n";
 std::cout<<"PASS: own GPU textures, perspective, filtering, blend, alpha, depth and exact captured VRAM; adapter="<<backend.adapter()<<'\n';return 0;
}catch(const std::exception& error){psprecomp::set_native_vram_access_hook(nullptr,nullptr,nullptr);std::cerr<<error.what()<<'\n';return 1;}}
