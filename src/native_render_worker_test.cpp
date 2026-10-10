#include "native_render_worker.hpp"
#include <atomic>
#include <iostream>
using namespace std::chrono_literals;
void require(bool value,const char* message){if(!value)throw std::runtime_error(message);}
struct Control {std::mutex mutex;std::condition_variable cv;bool entered=false,release=false,fail=false;std::atomic<unsigned> calls=0;std::thread::id owner;};
struct PrivateRaster:kon::NativeGpuRaster {
 std::shared_ptr<Control> control;
 explicit PrivateRaster(std::shared_ptr<Control> state):control(state){control->owner=std::this_thread::get_id();}
 kon::NativeGpuRasterStats raster(std::span<std::uint32_t> colors,std::span<std::uint32_t>,std::span<const std::uint32_t>,std::span<const kon::NativeGpuRasterTriangle> triangles,const std::array<std::uint32_t,32>&)override{
  require(std::this_thread::get_id()==control->owner,"GPU used on a different thread");++control->calls;
  std::unique_lock lock(control->mutex);control->entered=true;control->cv.notify_all();control->cv.wait(lock,[&]{return control->release;});
  if(control->fail)throw std::runtime_error("Injected private raster failure");colors[0]=static_cast<unsigned>(triangles[0].xy[0]);return {};
 }
};
std::shared_ptr<const kon::NativeRenderFrame> frame(std::uint64_t time){
 auto out=std::make_shared<kon::NativeRenderFrame>();out->initial_vram.resize(psprecomp::GuestMemory::kVramSize);out->timestamp_us=time;out->address=psprecomp::GuestMemory::kVramPhysicalBase;out->stride=8;out->format=3;
 kon::NativeFrameCommand c;c.source_pc=0x08800010;c.parameters[7]=8;c.parameters[8]=8;c.parameters[9]=3;c.parameters[23]=2;c.parameters[24]=2;c.parameters[28]=out->address;c.parameters[29]=out->address+1024;
 kon::NativeGpuRasterTriangle t;t.xy[0]=static_cast<int>(time);c.triangles.push_back(t);out->commands.push_back(c);return out;
}
template<class F>void eventually(F check){const auto until=std::chrono::steady_clock::now()+2s;while(!check()){if(std::chrono::steady_clock::now()>until)throw std::runtime_error("Timed out waiting for private worker");std::this_thread::sleep_for(1ms);}}
int main(){try{
 const auto start=std::chrono::steady_clock::now();auto clock=[&]{return std::uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now()-start).count());};
 auto state=std::make_shared<Control>();kon::NativeRenderWorker worker([state]{return std::make_unique<PrivateRaster>(state);},clock,60);
 const auto original=frame(1);worker.submit(original);{std::unique_lock lock(state->mutex);require(state->cv.wait_for(lock,2s,[&]{return state->entered;}),"Private raster did not start");}
 // Hold rasterization explicitly while publishing newer sources and changing
 // rates. A blocked GPU must never hold the producer/logic thread hostage.
 for(unsigned n=2;n<=20;++n)worker.submit(frame(n));
 for(unsigned fps:{30u,60u,120u,165u,237u,1000u})worker.set_rate(fps);
 require(worker.stats().replaced_inputs>=18,"Worker retained obsolete frame backlog");
 {std::lock_guard lock(state->mutex);state->release=true;state->cv.notify_all();}
 eventually([&]{const auto out=worker.latest();return out && out->source->timestamp_us==20;});
 const auto output=worker.latest();unsigned first=0;std::memcpy(&first,output->vram.data(),4);require(first==20,"Worker published obsolete geometry");
 require(original->initial_vram[0]==0 && original->commands[0].triangles[0].xy[0]==1,"Private worker mutated original immutable data");
 require(state->owner!=std::this_thread::get_id(),"Rendering ran on producer thread");
 const auto completed=worker.stats().completed;std::this_thread::sleep_for(25ms);require(worker.stats().completed==completed,"Frozen framebuffer repaints manufactured FPS");
 bool invalid=false;try{worker.set_rate(0);}catch(const std::invalid_argument&){invalid=true;}require(invalid,"Invalid render rate accepted");
 bool backward=false;try{worker.submit(frame(20));}catch(const std::invalid_argument&){backward=true;}require(backward,"Repeated input timestamp accepted");worker.stop();
 // A genuinely moving pair produces a private intermediate frame, then
 // reaches the unchanged endpoint as time advances. Rate changes cannot
 // advance the source clock or mutate the captured original geometry.
 std::atomic<std::uint64_t> source_time=100000;auto moving=std::make_shared<Control>();
 auto a=std::make_shared<kon::NativeRenderFrame>(*frame(100000)),b=std::make_shared<kon::NativeRenderFrame>(*frame(300000));
 auto& ta=a->commands[0].triangles[0];ta.xy={0,0,160,0,0,160};ta.state={0,0,10,10,31,0,0,0};
 for(unsigned v=0;v<3;++v)ta.attributes[v*8+6]=1;
 auto& tb=b->commands[0].triangles[0];tb=ta;for(unsigned n=0;n<6;n+=2)tb.xy[n]+=160;tb.state[0]=10;tb.state[2]=20;
 a->commands[0].parameters[23]=10;a->commands[0].parameters[24]=10;b->commands[0].parameters[23]=20;b->commands[0].parameters[24]=10;
 kon::NativeRenderWorker interpolated([moving]{return std::make_unique<PrivateRaster>(moving);},[&]{return source_time.load();},60);
 interpolated.submit(a);{std::unique_lock lock(moving->mutex);require(moving->cv.wait_for(lock,2s,[&]{return moving->entered;}),"Moving pair raster did not start");}
 interpolated.submit(b);source_time=350000;interpolated.set_rate(120);{std::lock_guard lock(moving->mutex);moving->release=true;moving->cv.notify_all();}
 eventually([&]{const auto out=interpolated.latest();return out && out->source->timestamp_us==300000;});
 auto middle=interpolated.latest();std::memcpy(&first,middle->vram.data(),4);require(middle->fraction==.25 && first==40,"Private worker did not draw intermediate geometry");
 source_time=500000;interpolated.set_rate(30);eventually([&]{return interpolated.latest()->fraction==1;});std::memcpy(&first,interpolated.latest()->vram.data(),4);require(first==160 && ta.xy[0]==0 && tb.xy[0]==160 && source_time==500000,"Rendering changed source time or geometry endpoint");interpolated.stop();
 // A worker failure is observable on its consumer; guest state remains intact.
 auto failure=std::make_shared<Control>();failure->release=true;failure->fail=true;
 kon::NativeRenderWorker failed([failure]{return std::make_unique<PrivateRaster>(failure);},clock,165);failed.submit(original);
 eventually([&]{try{failed.latest();return false;}catch(const std::runtime_error& e){return std::string(e.what())=="Injected private raster failure";}});require(original->initial_vram[0]==0,"Failed worker wrote original VRAM");failed.stop();
 bool startup=false;try{kon::NativeRenderWorker bad([]()->std::unique_ptr<kon::NativeGpuRaster>{throw std::runtime_error("Injected factory failure");},clock,60);}catch(const std::runtime_error& e){startup=std::string(e.what())=="Injected factory failure";}require(startup,"Worker startup failure hidden");
 std::cout<<"PASS: independent render owner, nonblocking immutable publication, bounded latest-frame queue, 30/60/120/165/custom rate changes, real geometry counts and failure isolation. Live presentation/rhythm acceptance pending.\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
