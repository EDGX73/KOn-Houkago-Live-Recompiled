#pragma once
#include "native_frame_stream.hpp"
#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>
namespace kon {
// A separate owner for its GPU, private VRAM and render deadlines. Publication
// never waits for rasterization. Original tasks/audio/input are not accessible
// to this worker; consumers receive immutable completed output only.
class NativeRenderWorker {
public:
 using Clock=std::function<std::uint64_t()>;
 using Factory=std::function<std::unique_ptr<NativeGpuRaster>()>;
 struct Output {
  std::shared_ptr<const NativeRenderFrame> source;
  std::vector<std::uint8_t> vram;
  std::uint64_t sequence=0,presentation_us=0;
  double fraction=1;
 };
 struct Stats {std::uint64_t completed=0,replaced_inputs=0,skipped_deadlines=0;};
 NativeRenderWorker(Factory factory,Clock clock,unsigned fps):factory_(std::move(factory)),clock_(std::move(clock)),fps_(fps){
  NativeRenderTimeline validate(fps);if(!factory_ || !clock_)throw std::invalid_argument("Render worker requires a GPU factory and clock");
  thread_=std::thread([this]{run();});
  std::unique_lock lock(mutex_);condition_.wait(lock,[this]{return ready_;});
  if(!error_.empty()){const auto error=error_;lock.unlock();thread_.join();throw std::runtime_error(error);}
 }
 ~NativeRenderWorker(){stop();}
 NativeRenderWorker(const NativeRenderWorker&)=delete;
 NativeRenderWorker& operator=(const NativeRenderWorker&)=delete;
 void submit(std::shared_ptr<const NativeRenderFrame> frame){
  if(!frame || !frame->complete || frame->initial_vram.size()!=psprecomp::GuestMemory::kVramSize)throw std::invalid_argument("Render worker requires a complete immutable frame");
  std::lock_guard lock(mutex_);check();
  if(current_ && frame->timestamp_us<=current_->timestamp_us)throw std::invalid_argument("Render input timestamps must increase");
  if(current_ && current_->timestamp_us>consumed_timestamp_)++stats_.replaced_inputs;
  previous_=std::move(current_);current_=std::move(frame);++revision_;condition_.notify_all();
 }
 void set_rate(unsigned fps){NativeRenderTimeline validate(fps);std::lock_guard lock(mutex_);check();fps_=fps;++rate_revision_;condition_.notify_all();}
 std::shared_ptr<const Output> latest()const{std::lock_guard lock(mutex_);check();return output_;}
 Stats stats()const{std::lock_guard lock(mutex_);check();return stats_;}
 void stop(){
  {std::lock_guard lock(mutex_);stopping_=true;condition_.notify_all();}
  if(thread_.joinable())thread_.join();
 }
private:
 Factory factory_;Clock clock_;unsigned fps_;mutable std::mutex mutex_;std::condition_variable condition_;std::thread thread_;
 bool stopping_=false,ready_=false;std::string error_;
 std::uint64_t revision_=0,rate_revision_=0,consumed_timestamp_=0;
 std::shared_ptr<const NativeRenderFrame> previous_,current_;
 std::shared_ptr<const Output> output_;Stats stats_;
 void check()const{if(!error_.empty())throw std::runtime_error(error_);if(stopping_)throw std::runtime_error("Render worker is stopped");}
 void run()noexcept{try{
  auto gpu=factory_();if(!gpu)throw std::runtime_error("Render factory returned no GPU");
  std::unique_lock lock(mutex_);NativeRenderTimeline timeline(fps_);ready_=true;condition_.notify_all();
  std::uint64_t applied_rate=rate_revision_,skipped_total=0,last_source=~0ull;double last_fraction=-1;
  while(!stopping_){
   if(applied_rate!=rate_revision_){skipped_total+=timeline.skipped_deadlines();timeline.set_rate(fps_);applied_rate=rate_revision_;}
   if(!current_){condition_.wait(lock,[this]{return stopping_ || bool(current_);});continue;}
   const auto now=clock_();
   if(!timeline.due(now)){
    const auto revision=revision_,rate=rate_revision_;
    condition_.wait_for(lock,std::chrono::microseconds(timeline.until_due(now)),[&]{return stopping_ || revision_!=revision || rate_revision_!=rate;});continue;
   }
   const auto previous=previous_,current=current_;
   const auto rate=applied_rate;consumed_timestamp_=current->timestamp_us;
   // Interpolation uses one source period of buffering. A transition that
   // cannot match resources/topology preserves current geometry explicitly.
   double alpha=1;
   if(previous && previous->timestamp_us<current->timestamp_us)
    alpha=NativeRenderTimeline::fraction(now,previous->timestamp_us,current->timestamp_us,current->timestamp_us-previous->timestamp_us);
   if(current->timestamp_us==last_source && alpha==last_fraction){
    stats_.skipped_deadlines=skipped_total+timeline.skipped_deadlines();continue;
   }
   lock.unlock();
   auto frame=previous && alpha<1?interpolate_native_frame(*previous,*current,alpha):*current;
   // Count actual new geometry work. Repainting a frozen source does not
   // manufacture FPS or keep a queue of obsolete interpolation deadlines.
   bool changed=false;
   if(previous && alpha<1)for(std::size_t n=0;n<frame.commands.size() && !changed;++n){
    const auto& a=frame.commands[n].triangles;const auto& b=current->commands[n].triangles;
    for(std::size_t k=0;k<a.size();++k)if(a[k].xy!=b[k].xy || a[k].attributes!=b[k].attributes){changed=true;break;}
   }
   const auto fraction=changed?alpha:1;
   std::shared_ptr<Output> result;
   if(current->timestamp_us!=last_source || fraction!=last_fraction){
    result=std::make_shared<Output>();result->source=current;result->presentation_us=now;result->fraction=fraction;result->vram=replay_native_frame(frame,*gpu);
    last_source=current->timestamp_us;last_fraction=fraction;
   }
   lock.lock();stats_.skipped_deadlines=skipped_total+timeline.skipped_deadlines();
   if(result && !stopping_ && rate==rate_revision_){result->sequence=++stats_.completed;output_=std::move(result);}
  }
  lock.unlock(); // Destroy the GPU on its owning thread after its last replay.
 }catch(const std::exception& e){std::lock_guard lock(mutex_);error_=e.what();ready_=true;condition_.notify_all();}
 catch(...){std::lock_guard lock(mutex_);error_="Unknown render worker failure";ready_=true;condition_.notify_all();}}
};
}
