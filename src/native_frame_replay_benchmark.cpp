// Offline replay of captured original frames on the real GPU. Each replay is
// compared with the original framebuffer the game produced, then GPU shading
// time is reported. Diagnostic tool: no game code, audio or input is run.
#include "native_frame_stream.hpp"
#include "native_vulkan_clear.hpp"
#include <chrono>
#include <iostream>
int main(int argc,char** argv){
 try{
  if(argc<2){std::cerr<<"Usage: native_frame_replay_benchmark frame.konframe... [--repeat N]\n";return 2;}
  unsigned repeat=10;std::vector<std::filesystem::path> paths;
  for(int i=1;i<argc;++i){const std::string arg=argv[i];if(arg=="--repeat" && i+1<argc)repeat=std::stoul(argv[++i]);else paths.emplace_back(arg);}
  kon::NativeVulkanClear gpu(false,true,std::getenv("KON_VULKAN_ADAPTER")?std::getenv("KON_VULKAN_ADAPTER"):"");
  std::cout<<"adapter="<<gpu.adapter()<<"\n";bool exact=true;
  for(const auto& path:paths){
   std::vector<std::uint8_t> expected;const auto frame=kon::load_native_frame(path,expected);
   const std::size_t offset=frame.address%psprecomp::GuestMemory::kVramSize,size=std::size_t(frame.stride)*272u*(frame.format==3?4u:2u);
   std::size_t triangles=0;for(const auto& c:frame.commands)triangles+=c.triangles.size();
   const auto before=gpu.profile();double best=1e30,total=0;
   for(unsigned n=0;n<repeat;++n){
    const auto start=std::chrono::steady_clock::now();const auto output=kon::replay_native_frame(frame,gpu);
    const auto ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();best=std::min(best,ms);total+=ms;
    const bool same=offset+size<=output.size() && std::equal(output.begin()+offset,output.begin()+offset+size,expected.begin()+offset);
    if(!same){exact=false;std::cout<<"MISMATCH "<<path.string()<<" repeat="<<n<<"\n";break;}
   }
   const auto after=gpu.profile();
   std::cout<<path.filename().string()<<" commands="<<frame.commands.size()<<" triangles="<<triangles<<" wall_best_ms="<<best<<" wall_mean_ms="<<total/repeat
    <<" gpu_shade_ms="<<double(after[1]-before[1])/1e6/repeat<<" gpu_upload_ms="<<double(after[0]-before[0])/1e6/repeat<<" dispatches="<<(after[3]-before[3])/repeat
    <<" barriers="<<(after[4]-before[4])/repeat<<" tile_job_pixels="<<(after[5]-before[5])/repeat<<"\n";
  }
  std::cout<<(exact?"PASS: replay equals original framebuffer\n":"FAIL: replay differs from original framebuffer\n");return exact?0:1;
 }catch(const std::exception& e){std::cerr<<"error: "<<e.what()<<"\n";return 1;}
}
