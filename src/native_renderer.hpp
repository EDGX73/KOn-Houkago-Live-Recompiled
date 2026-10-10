#pragma once
#include "ge_queue.hpp"
#include "native_texture.hpp"
#include "native_upscale.hpp"
#include "native_geometry.hpp"
#include "native_blend.hpp"
#include "native_raster_pool.hpp"
#include "native_gpu_clear.hpp"
#include "native_gpu_triangles.hpp"
#include "native_gpu_raster.hpp"
#include "native_frame_stream.hpp"
#include "native_vram_ownership.hpp"
#include <iostream>
#include <deque>
#include <functional>
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <stdexcept>
#include <optional>
#include "native_fixed_raster.hpp"
#include <chrono>
namespace kon {
// Initial native drawing backend. Only exact supported operations are accepted;
// other primitive/state combinations remain explicit incomplete-port stops.
class NativeRenderer {
public:
    // Geometry of one flat-triangle draw: vertex fetch, transform, clipping,
    // projection and the GPU jobs. A pure function of the draw's state and
    // guest memory, so queued draws are built in parallel (process_pending).
    struct FlatGeometry {
        bool ok=false,bounded_through_uv=true,jobs_ready=false;unsigned vertex_stride=0;
        std::vector<std::array<GeometryVertex,3>> triangles;std::vector<NativeGpuRasterTriangle> jobs;
        std::uint64_t projections=0,nonfinite=0;int x0=0,y0=0,x1=0,y1=0;std::exception_ptr error;
        std::vector<GeometryVertex> vertices;std::vector<unsigned char> projected; // reused scratch
        std::vector<std::uint32_t> encoded; // jobs in GPU packet form
        void reset(){ok=jobs_ready=false;bounded_through_uv=true;vertex_stride=0;triangles.clear();jobs.clear();encoded.clear();projections=nonfinite=0;x0=y0=x1=y1=0;error=nullptr;}
    };
    explicit NativeRenderer(psprecomp::GuestMemory& memory,unsigned lanes=4,bool texture_cache=true,bool projection_cache=true,bool persistent_texture_cache=false,bool prepared_raster=false,bool direct_vertex_reads=true,bool fast_blend=true,unsigned parallel_threshold=8192,bool balanced_rows=true):memory_(memory),raster_pool_(lanes),texture_cache_enabled_(texture_cache),projection_cache_enabled_(projection_cache),persistent_texture_cache_enabled_(persistent_texture_cache),prepared_raster_enabled_(prepared_raster),direct_vertex_reads_enabled_(direct_vertex_reads),fast_blend_enabled_(fast_blend),parallel_threshold_(parallel_threshold),balanced_rows_(balanced_rows){}
    void set_gpu_clear(NativeGpuClear* backend){gpu_clear_=backend;}
    // The backend records into its other staging slot while a submitted batch
    // is still in flight, so the next batch opens without waiting.
    void begin_gpu_batch(){if(async_main()){push({~std::uint64_t{0},[this]{begin_gpu_batch();},false});return;}if(gpu_raster_ && !gpu_batch_open_){gpu_batch_open_=true;gpu_batch_has_draws_=false;gpu_batch_colors_bytes_=gpu_batch_depth_bytes_=0;gpu_batch_zrect_=kEmptyRect;gpu_raster_->begin_batch();}}
    // Synchronous end: publish everything and close the batch, even if empty.
    void end_gpu_batch(){
        if(async_main())drain();
        process_pending();submit_gpu_batch();collect_gpu_batch();
        if(gpu_batch_open_){gpu_batch_open_=false;NativeVramInternalScope ownership;gpu_raster_->end_batch();}
    }
    // Submit the open batch without waiting. Its destination stays owned by
    // the GPU (pending_vram_access) until collect_gpu_batch publishes it.
    // Diagnostic (KON_RENDER_TIMELINE=<start ms>): render-thread events of 3000
    // batches with host microseconds and each batch's GPU time.
    void timeline(char kind,std::uint64_t value=0){
        static const char* env=std::getenv("KON_RENDER_TIMELINE");if(!env)return;
        static const auto origin=std::chrono::steady_clock::now();static const std::uint64_t start=std::strtoull(env,nullptr,10)*1000u;
        static std::mutex mutex;std::lock_guard lock(mutex);static std::vector<std::array<std::uint64_t,3>> events;static bool written=false;if(written)return;
        const auto now=std::uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now()-origin).count());if(now<start)return;
        events.push_back({now,std::uint64_t(kind),value});
        if(events.size()>=6000){written=true;for(const auto& e:events)std::cout<<"tl "<<e[0]<<' '<<char(e[1])<<' '<<e[2]<<'\n';}
    }
    void submit_gpu_batch(){
        // An open batch without draws has nothing to submit: keep it open and
        // leave any in-flight batch running instead of waiting for it.
        if(async_main()){push({~std::uint64_t{0},[this]{submit_gpu_batch();},false});return;}
        process_pending();
        if(!gpu_batch_open_ || !gpu_batch_has_draws_)return;
        if(gpu_inflight_){NativeVramInternalScope ownership;gpu_raster_->prepare_batch();} // overlaps the batch in flight
        collect_gpu_batch();gpu_batch_open_=false;
        timeline('s');{NativeVramInternalScope ownership;gpu_raster_->submit_batch();}timeline('S');
        if(gpu_batch_has_draws_){gpu_inflight_=true;gpu_inflight_identity_=gpu_batch_identity_;gpu_inflight_colors_bytes_=gpu_batch_colors_bytes_;gpu_inflight_depth_bytes_=gpu_batch_depth_bytes_;gpu_inflight_zrect_=gpu_batch_zrect_;gpu_inflight_depth_superseded_=false;}
    }
    void collect_gpu_batch(){
        if(async_main())drain();
        collect_gpu_batch_now();
    }
    void collect_gpu_batch_now(){
        if(gpu_inflight_){gpu_inflight_=false;
            NativeVramInternalScope ownership;const auto start=std::chrono::steady_clock::now();timeline('c');const auto before=gpu_raster_->profile();const auto result=gpu_raster_->collect_batch();
            {const auto after=gpu_raster_->profile();timeline('C',(after[0]+after[1]+after[2]-before[0]-before[1]-before[2])/1000u);}
            gpu_collect_wait_us+=std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now()-start).count();
            if(result.draws){
                const auto& p=result.parameters;const auto bytes=p[9]==3?4u:2u;
                const auto color_words=result.color_span(),depth_words=result.depth_span();
                NativeMemoryView colors(memory_,p[28],color_words.size()*bytes,true),depths(memory_,p[29],depth_words.size()*2u,true);
                colors.write_words(color_words,bytes);
                // A later full depth clear already replaced every depth word this batch changed.
                if(!gpu_inflight_depth_superseded_)depths.write_words(depth_words,2);
                draw_covered_pixels+=result.stats.covered;alpha_rejected_pixels+=result.stats.alpha;depth_rejected_pixels+=result.stats.depth;draw_pixels+=result.stats.drawn;
                gpu_raster_primitives+=result.draws;gpu_raster_pixels+=result.stats.drawn;
            }
        }
        run_pending_show(); // the buffer it waited for is complete in VRAM now
    }
    // `queued`: also draws produced but not recorded yet. The render thread
    // passes false for its own commands: such draws come after them.
    bool pending_vram_access(std::uint32_t address,std::size_t length,bool queued=true)const{
        if(!length)return false;
        // With work still queued for the render thread the access waits for it.
        if(async_main() && !render_idle())return true;
        // Queued draws own their color and depth buffers until recorded (their
        // vertices and textures are never in VRAM: such draws are not queued).
        if(queued)for(auto n=recorded_.load();n<produced_;++n){const auto& r=slot(n).state.registers;
            const unsigned bytes=(r[0xD2]&3u)==3?4u:2u,stride=r[0x9D]&0x7FCu,zstride=r[0x9F]&0x7FCu,rows=((r[0xD5]>>10u)&1023u)+1u;
            if(vram_overlap(address,length,0x04000000u|(r[0x9C]&0x1FFFF0u),std::size_t(std::max(stride,512u))*rows*bytes))return true;
            if((r[0x23]&1u) && vram_overlap(address,length,0x04000000u|(r[0x9E]&0x1FFFF0u),std::size_t(std::max(zstride,512u))*rows*2u))return true;
        }
        const auto offset=std::size_t(address&(psprecomp::GuestMemory::kVramSize-1));
        const auto overlaps=[&](unsigned base,std::size_t size){const auto first=std::size_t(base&(psprecomp::GuestMemory::kVramSize-1));return size && offset<first+size && first<offset+length;};
        const bool open=gpu_batch_open_ && gpu_batch_has_draws_;
        if(!open && !gpu_inflight_)return false;
        if(length>psprecomp::GuestMemory::kVramSize-offset)return true;
        return (open && (overlaps(gpu_batch_identity_[3],gpu_batch_colors_bytes_) || overlaps(gpu_batch_identity_[4],gpu_batch_depth_bytes_))) ||
            (gpu_inflight_ && (overlaps(gpu_inflight_identity_[3],gpu_inflight_colors_bytes_) || (!gpu_inflight_depth_superseded_ && overlaps(gpu_inflight_identity_[4],gpu_inflight_depth_bytes_))));
    }
    std::uint64_t gpu_collect_wait_us=0,vram_texture_flushes=0;
    // Called before GE work whenever original code may have run since the
    // previous call; main-RAM textures are then revalidated once per epoch.
    void begin_guest_epoch(){process_pending();std::lock_guard lock(texture_mutex_);++texture_epoch_;decoded_texture_cache_.set_epoch(texture_epoch_);gpu_decoded_texture_cache_.set_epoch(texture_epoch_);}
    std::array<std::uint64_t,6> gpu_profile()const{return gpu_raster_?gpu_raster_->profile():std::array<std::uint64_t,6>{};}
    void flush_gpu_batch(unsigned reason=0){if(async_main())drain();process_pending();if(gpu_batch_open_){if(gpu_batch_has_draws_)++flush_reasons[std::min(reason,4u)];end_gpu_batch();begin_gpu_batch();}}
    std::array<std::uint64_t,5> flush_reasons{}; // external, clear/VRAM source, unsafe, identity, capacity
    void set_frame_stream(NativeFrameStream* stream){drain();process_pending();frame_stream_=stream;}
    void set_gpu_raster(NativeGpuRaster* backend){gpu_raster_=backend;}
    std::uint64_t gpu_raster_primitives=0,gpu_raster_pixels=0,gpu_resident_words_reused=0;
    void set_gpu_triangles(NativeGpuTriangles* backend){gpu_triangles_=backend;}
    std::uint64_t gpu_triangle_primitives=0,gpu_triangle_pixels=0;
    std::uint64_t gpu_clear_primitives=0,gpu_clear_pixels=0;
    std::uint64_t clear_primitives=0,clear_pixels=0;
    std::uint64_t texture_cache_preparations=0,texture_cache_hits=0,vertex_projection_evaluations=0;
    std::uint64_t draw_primitives=0,draw_pixels=0;
    std::uint64_t draw_covered_pixels=0,alpha_rejected_pixels=0,depth_rejected_pixels=0;
    std::uint64_t nonfinite_triangles=0;
    // Draws that waited for the in-flight batch: other path/VRAM source, color,
    // depth, clear color, partial clear depth; and full depth clears superseding it.
    mutable std::array<std::uint64_t,6> overlap_waits{};
    ~NativeRenderer(){try{drain();}catch(...){}stop_render_thread();stop_builders();}
private:
    // Draw queue: a ring of fixed slots (never reallocated while builders read
    // them). produced_ counts queued slots and claimed_ the slots a builder took
    // (both guarded by queue_mutex_); recorded_ counts slots recorded, in order.
    struct PendingDraw {
        GeState state;unsigned kind=0,count=0;FlatGeometry geo;std::atomic<bool> done{false};
        std::vector<std::uint8_t> vertices; // asynchronous mode: copy of the vertex bytes
        // Asynchronous mode: texture decoded and pinned when the draw was queued.
        bool texture_ready=false;std::span<const std::uint32_t> texels;std::uint32_t token=0;
    };
    static constexpr std::uint64_t kQueueSlots=2048;
    std::vector<PendingDraw> pending_=std::vector<PendingDraw>(kQueueSlots);
    std::uint64_t produced_=0,claimed_=0;std::atomic<std::uint64_t> recorded_{0};
    std::mutex queue_mutex_;std::condition_variable queue_work_,queue_space_;bool builders_stop_=false;
    // Threads actually blocked: signals are skipped (no system call) otherwise.
    unsigned builders_waiting_=0;std::atomic<bool> producer_waiting_{false};
    std::vector<std::thread> builders_;
    bool processing_pending_=false,draw_queue_=false;
    FlatGeometry sync_geometry_;
    PendingDraw* recording_slot_=nullptr; // queued draw being recorded (its prepared texture)
    // gpu_decoded_texture_cache_: the game thread prepares and pins textures,
    // the render thread unpins them after recording.
    std::mutex texture_mutex_;
    PendingDraw& slot(std::uint64_t n){return pending_[n%kQueueSlots];}
    const PendingDraw& slot(std::uint64_t n)const{return pending_[n%kQueueSlots];}
    void build_slot(PendingDraw& p){
        try{build_flat_geometry(p.state,p.kind,p.count,true,p.geo,p.vertices.empty()?nullptr:&p.vertices);}catch(...){p.geo.error=std::current_exception();}
        p.done.store(true,std::memory_order_release);
    }
    bool help_build(){
        std::unique_lock lock(queue_mutex_);if(claimed_>=produced_)return false;
        auto& p=slot(claimed_++);lock.unlock();build_slot(p);return true;
    }
    void start_builders(){
        if(!builders_.empty())return;
        const unsigned count=std::clamp(std::thread::hardware_concurrency(),2u,16u)-1u;
        for(unsigned n=0;n<count;++n)builders_.emplace_back([this]{
            for(;;){
                std::unique_lock lock(queue_mutex_);
                if(!builders_stop_ && claimed_>=produced_){++builders_waiting_;queue_work_.wait(lock,[this]{return builders_stop_ || claimed_<produced_;});--builders_waiting_;}
                if(builders_stop_)return;
                auto& p=slot(claimed_++);lock.unlock();build_slot(p);
            }
        });
    }
    void stop_builders(){{std::lock_guard lock(queue_mutex_);builders_stop_=true;}queue_work_.notify_all();for(auto& b:builders_)b.join();builders_.clear();}
    // Every queued slot up to `upto` has its geometry (guest memory read).
    void wait_built(std::uint64_t upto){
        // A slot the render thread already recorded counts as built (its done
        // flag is reset when the slot is released for reuse).
        for(auto n=recorded_.load(std::memory_order_acquire);n<upto;++n)
            while(n>=recorded_.load(std::memory_order_acquire) && !slot(n).done.load(std::memory_order_acquire))if(!help_build())std::this_thread::yield();
    }
    void release_slot(PendingDraw& p){
        if(p.texture_ready){std::lock_guard lock(texture_mutex_);gpu_decoded_texture_cache_.unpin(p.token);p.texture_ready=false;}
        p.done.store(false,std::memory_order_relaxed);
        recorded_.fetch_add(1); // sequentially consistent with producer_waiting_ (no lost wake-up)
        if(producer_waiting_.load()){std::lock_guard lock(queue_mutex_);queue_space_.notify_all();}
    }
    void record_slot(std::uint64_t n){
        auto& p=slot(n);
        while(!p.done.load(std::memory_order_acquire))if(!help_build())std::this_thread::yield();
        struct Release {NativeRenderer& r;PendingDraw& p;~Release(){r.recording_slot_=nullptr;r.release_slot(p);}} release{*this,p};
        if(!gpu_batch_open_)begin_gpu_batch();
        if(gpu_inflight_ && inflight_conflict(p.state,false))collect_gpu_batch();
        NativeVramInternalScope ownership;recording_slot_=&p;
        if(!draw_flat_triangles(p.state,p.kind,p.count,&p.geo))throw std::runtime_error("Queued GE draw is not supported");
    }
    // Render thread (asynchronous mode): runs queued draws, clears, batch
    // operations and frame presentations in order while original code runs.
    struct Command {std::uint64_t slot=~std::uint64_t{0};std::function<void()> call;bool present=false;};
    std::deque<Command> commands_;mutable std::mutex command_mutex_;std::condition_variable command_ready_,command_done_;
    bool render_busy_=false,render_stop_=false,async_=false,render_waiting_=false;unsigned queued_presents_=0;
    std::thread render_thread_;std::thread::id render_id_;std::exception_ptr render_error_;
    bool on_render_thread()const{return render_thread_.joinable() && std::this_thread::get_id()==render_id_;}
    bool async_main()const{return async_ && !on_render_thread();}
    void start_render_thread(){
        if(render_thread_.joinable())return;
        render_thread_=std::thread([this]{
            auto last_fence_poll=std::chrono::steady_clock::now();
            for(;;){
                Command command;
                // Show a finished frame as soon as its GPU work is done; with
                // nothing else to do, wait for it.
                if(pending_show_){
                    bool idle;{std::lock_guard lock(command_mutex_);idle=commands_.empty();}
                    // The driver's fence query is a system call: at most every 250 us.
                    const auto now=std::chrono::steady_clock::now();
                    bool poll=idle || now-last_fence_poll>=std::chrono::microseconds(250);if(poll && !idle)last_fence_poll=now;
                    // Never block here waiting for the GPU: draws of the next
                    // frame arriving meanwhile are recorded at once (blocking
                    // delayed them all to the end of the batch, and the next
                    // submission past the frame period).
                    static const bool blocking=std::getenv("KON_BLOCKING_SHOW")!=nullptr; // previous behaviour (A/B)
                    if((blocking && idle) || !gpu_raster_ || (poll && gpu_raster_->batch_ready())){
                        try{collect_gpu_batch();run_pending_show();}
                        catch(...){{std::lock_guard lock(command_mutex_);if(!render_error_)render_error_=std::current_exception();pending_show_=nullptr;show_pending_=false;}command_done_.notify_all();}
                    }
                }
                {std::unique_lock lock(command_mutex_);
                 if(!render_stop_ && commands_.empty()){render_waiting_=true;command_ready_.wait_for(lock,std::chrono::microseconds(pending_show_?100:1000000),[this]{return render_stop_ || !commands_.empty();});render_waiting_=false;}
                 if(commands_.empty() && !render_stop_)continue;
                 if(commands_.empty())return;command=std::move(commands_.front());commands_.pop_front();render_busy_=true;}
                bool failed;{std::lock_guard lock(command_mutex_);failed=render_error_!=nullptr;}
                std::exception_ptr error;
                try{
                    if(command.slot!=~std::uint64_t{0}){
                        if(failed){auto& p=slot(command.slot);while(!p.done.load(std::memory_order_acquire))if(!help_build())std::this_thread::yield();release_slot(p);}
                        else record_slot(command.slot);
                    }else if(!failed)command.call();
                }catch(...){error=std::current_exception();}
                {std::lock_guard lock(command_mutex_);if(error && !render_error_)render_error_=error;if(command.present && queued_presents_)--queued_presents_;render_busy_=false;}
                command_done_.notify_all();
            }
        });
        render_id_=render_thread_.get_id();
    }
    void stop_render_thread(){
        if(!render_thread_.joinable())return;
        {std::lock_guard lock(command_mutex_);render_stop_=true;}command_ready_.notify_all();render_thread_.join();
    }
    void push(Command command){
        bool wake;{std::lock_guard lock(command_mutex_);if(command.present)++queued_presents_;commands_.push_back(std::move(command));wake=render_waiting_;}
        if(wake)command_ready_.notify_one();
    }
    void rethrow_render_error(){
        std::exception_ptr error;{std::lock_guard lock(command_mutex_);error=render_error_;render_error_=nullptr;}
        if(error)std::rethrow_exception(error);
    }
    bool render_idle()const{std::lock_guard lock(command_mutex_);return commands_.empty() && !render_busy_ && !show_pending_.load();}
    // A presentation waiting for the batch that writes its buffer (render
    // thread only; show_pending_ lets other threads see that it exists).
    std::function<void()> pending_show_;std::atomic<bool> show_pending_{false};
    void run_pending_show(){
        if(!pending_show_)return;
        auto show=std::move(pending_show_);pending_show_=nullptr;
        struct Done {NativeRenderer& r;~Done(){{std::lock_guard lock(r.command_mutex_);r.show_pending_=false;}r.command_done_.notify_all();}} done{*this};
        NativeVramInternalScope ownership;show();
    }
public:
    // Asynchronous rendering, enabled by the game together with the draw
    // queue: original code only queues GE work; anything that must observe
    // its results first waits for the render thread (drain).
    void set_async(bool enabled){drain();if(enabled)start_render_thread();async_=enabled;}
    bool async()const{return async_;}
    void drain(){
        if(!render_thread_.joinable() || on_render_thread())return;
        const auto start=std::chrono::steady_clock::now();
        {std::unique_lock lock(command_mutex_);if(!commands_.empty() || render_busy_)++async_drains;
         // A presentation still waiting for its batch is finished by the render thread.
         command_done_.wait(lock,[this]{return commands_.empty() && !render_busy_ && !show_pending_.load();});}
        drain_wait_us+=std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now()-start).count();
        rethrow_render_error();
    }
    // Render thread: show `address` once the GPU work writing it is done,
    // while recording continues (the open batch is submitted if it writes it).
    void set_sync_show(bool sync){sync_show_=sync;}bool sync_show_=false;
    // Any thread: change the internal resolution at the next presentation,
    // once no batch is open with draws or in flight.
    void request_internal_scale(unsigned scale){requested_scale_.store(scale);}
    std::atomic<unsigned> requested_scale_{0};
    void present_when_ready(std::uint32_t address,std::size_t bytes,std::function<void()> show){
        timeline('P');
        if(pending_show_){collect_gpu_batch();run_pending_show();}
        if(const auto scale=requested_scale_.exchange(0);scale && gpu_raster_){
            if(gpu_batch_open_ && gpu_batch_has_draws_)submit_gpu_batch();
            collect_gpu_batch_now();
            if(gpu_raster_->set_internal_scale(scale))sync_show_=scale>1 && std::getenv("KON_SYNC_HIRES_SHOW");
        }
        // Draws already queued after this presentation do not delay it.
        if(address && pending_vram_access(address,bytes,false)){
            if(gpu_batch_open_ && gpu_batch_has_draws_)submit_gpu_batch();
            // Deferred until the batch is collected (later draws to other
            // buffers are recorded meanwhile); KON_SYNC_HIRES_SHOW waits here
            // instead at internal resolution (diagnostic).
            if(gpu_inflight_){if(sync_show_)collect_gpu_batch_now();else{pending_show_=std::move(show);show_pending_=true;return;}}
        }
        NativeVramInternalScope ownership;show();
    }
    // Runs `call` on the render thread after everything queued before it
    // (directly when not asynchronous). A presentation counts towards the
    // frames in flight (wait_presents).
    void enqueue(std::function<void()> call,bool present=false){
        if(!async_main()){call();return;}
        rethrow_render_error();push({~std::uint64_t{0},std::move(call),present});
    }
    // Back-pressure: the game may run at most `limit` frames ahead of the screen.
    void wait_presents(unsigned limit){
        if(!async_main())return;
        const auto start=std::chrono::steady_clock::now();timeline('w');
        {std::unique_lock lock(command_mutex_);command_done_.wait(lock,[&]{return queued_presents_<limit || render_error_;});}
        timeline('W');
        present_wait_us+=std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now()-start).count();
        rethrow_render_error();
    }
    std::uint64_t async_drains=0,drain_wait_us=0,present_wait_us=0; // game-thread waits for the render thread
    std::uint64_t batch_clear_primitives=0; // clears shaded inside the open GPU batch
    static unsigned flat_vertex_stride(const GeState& state,unsigned kind,unsigned count){
        const auto& r=state.registers;
        const auto type=r[0x12]&0xFFFFFFu;
        const bool through=type&0x800000u;
        const unsigned position_type=(type>>7u)&3u,normal_type=(type>>5u)&3u;
        const unsigned weight_type=(type>>9u)&3u,weight_count=weight_type?((type>>14u)&7u)+1u:0u;
        if((through?position_type!=2:position_type!=3) || (type&~0x81C7FFu) || (through && weight_type) || kind<3 || kind>5 || count<3 || (kind==3 && count%3))return 0;
        for(unsigned flag:{0x1Fu,0x20u,0x24u,0x25u,0x27u,0x28u})if(r[flag]&1u)return 0;
        const bool lighting=!through && (r[0x17]&1u);
        if(lighting)for(unsigned flag:{0x18u,0x19u,0x1Au,0x1Bu})if(r[flag]&1u)return 0;
        const bool textured=r[0x1E]&1u;
        const unsigned uv_type=type&3u,color_type=(type>>2u)&7u;
        if(color_type && color_type<4)return 0;
        const unsigned min_filter=r[0xC6]&7u;
        if(textured && (!uv_type || (r[0xC3]&15u)>10 || (r[0xC2]&0x70000u) || min_filter==2 || min_filter==3 || ((r[0xC9]&7u)!=0 && (r[0xC9]&7u)!=3) || (!through && (r[0xC0]&3u))))return 0;
        if((r[0x21]&1u) && !NativeBlend::supported(r[0xDF]&0xFFFFFFu))return 0;
        const unsigned uv_bytes=uv_type==1?2u:uv_type==2?4u:uv_type==3?8u:0u,color_bytes=color_type==7?4u:color_type?2u:0u;
        const auto align=[](unsigned value,unsigned boundary){return (value+boundary-1u)&~(boundary-1u);};
        const unsigned weight_component=weight_type==3?4u:weight_type==2?2u:weight_type?1u:0u,uv_component=uv_type==3?4u:uv_type==2?2u:1u;
        const unsigned uv_offset=align(weight_component*weight_count,uv_component);
        const unsigned normal_component=normal_type==3?4u:normal_type==2?2u:normal_type?1u:0u,position_component=position_type==3?4u:2u;
        const unsigned color_offset=align(uv_offset+uv_bytes,color_bytes?color_bytes:1u),normal_offset=align(color_offset+color_bytes,normal_component?normal_component:1u),position_offset=align(normal_offset+normal_component*3u,position_component);
        return align(position_offset+3u*position_component,std::max({position_component,normal_component,color_bytes,uv_component,weight_component}));
    }
    // Records every queued draw in order (synchronous mode). Builders keep
    // working ahead; this thread builds unclaimed ones itself. In asynchronous
    // mode the render thread records them; here only their guest memory reads
    // must finish before original code runs again.
    void process_pending(){
        // Asynchronous mode: queued draws carry their vertices and textures, so
        // original code may continue at once.
        if(async_)return;
        if(processing_pending_ || recorded_.load()==produced_)return;
        processing_pending_=true;struct Reset {bool& flag;~Reset(){flag=false;}} reset{processing_pending_};
        ++pending_flushes;pending_draws+=produced_-recorded_.load();
        while(recorded_.load()<produced_)record_slot(recorded_.load());
    }
    std::uint64_t pending_flushes=0,pending_draws=0;
    // Opt-in (the game enables it): callers that inspect results right after
    // draw() keep the synchronous behaviour.
    void set_draw_queue(bool enabled){drain();process_pending();draw_queue_=enabled;}
    bool draw(GeState& state,unsigned kind,unsigned count) {
        const bool vram_source=((state.vertex_address&0x3F000000u)==0x04000000u) || ((state.index_address&0x3F000000u)==0x04000000u) || ((state.registers[0x1E]&1u) && (((state.registers[0xA8]<<8)&0x0F000000u)==0x04000000u));
        const bool clear_draw=state.registers[0xD3]&1u;
        if(draw_queue_ && !clear_draw && !vram_source && gpu_raster_ && (async_ || gpu_batch_open_) && !frame_stream_ && !native_write_watch_enabled()){
            if(const auto stride=flat_vertex_stride(state,kind,count)){
                bool queueable=true,texture_ready=false;std::span<const std::uint32_t> texels;std::uint32_t token=0;
                if(async_main() && (state.registers[0x1E]&1u)){
                    // The render thread must not read guest memory: decode now and pin.
                    try{
                        NativeVramInternalScope addresses_only; // the views below only compare address ranges
                        const auto& r=state.registers;const unsigned bytes=(r[0xD2]&3u)==3?4u:2u,stride_words=r[0x9D]&0x7FCu,zstride=r[0x9F]&0x7FCu,rows=((r[0xD5]>>10u)&1023u)+1u;
                        NativeMemoryView color(memory_,0x04000000u|(r[0x9C]&0x1FFFF0u),std::size_t(std::max(stride_words,1u))*rows*bytes,true);
                        NativeMemoryView depth(memory_,0x04000000u|(r[0x9E]&0x1FFFF0u),(r[0x23]&1u)?std::size_t(std::max(zstride,1u))*rows*2u:0u,true);
                        std::lock_guard lock(texture_mutex_);NativeTexture texture(memory_,state);
                        if(!texture.parallel_safe(color,depth))queueable=false;
                        else{
                            if(texture.prepare_cache(gpu_decoded_texture_cache_))++texture_cache_preparations;else ++texture_cache_hits;
                            texels=texture.decoded_pixels();token=texture.decoded_token();
                            if(texels.empty() || !token)queueable=false;else{gpu_decoded_texture_cache_.pin(token);texture_ready=true;}
                        }
                    }catch(const std::exception&){queueable=false;}
                }
                if(queueable && async_main() && !memory_.contains(state.vertex_address,static_cast<std::size_t>(count)*stride))queueable=false;
                if(queueable){
                    if(produced_-recorded_.load(std::memory_order_acquire)==kQueueSlots){
                        if(async_main()){std::unique_lock lock(queue_mutex_);producer_waiting_=true;queue_space_.wait(lock,[&]{return produced_-recorded_.load()<kQueueSlots;});producer_waiting_=false;}
                        else process_pending();
                    }
                    start_builders();
                    const auto n=produced_;
                    {auto& p=slot(n);p.state=state;p.kind=kind;p.count=count;p.texture_ready=texture_ready;p.texels=texels;p.token=token;
                     if(async_main()){p.vertices.resize(static_cast<std::size_t>(count)*stride);memory_.copy_out(state.vertex_address,p.vertices);}else p.vertices.clear();
                     std::lock_guard lock(queue_mutex_);++produced_;if(builders_waiting_)queue_work_.notify_one();}
                    if(async_main())push({n,{},false});
                    state.vertex_address+=count*stride;return true;
                }
            }
        }
        if(async_main()){
            const auto type=state.registers[0x12]&0xFFFFFFu;
            if(clear_draw && kind==6 && type==0x80011Cu && !(count&1u) && memory_.contains(state.vertex_address,static_cast<std::size_t>(count)*12u) && !frame_stream_ && !native_write_watch_enabled()){
                // Clears read only their vertex pairs: a copy goes with the command.
                std::vector<std::uint8_t> vertices(static_cast<std::size_t>(count)*12u);memory_.copy_out(state.vertex_address,vertices);
                push({~std::uint64_t{0},[this,copy=state,kind,count,vertices=std::move(vertices)]()mutable{
                    if(!draw_now(copy,kind,count,&vertices))throw std::runtime_error("Queued GE clear is not supported");},false});
                state.vertex_address+=count*12u;return true;
            }
            drain();
        }
        return draw_now(state,kind,count,nullptr);
    }
    bool draw_now(GeState& state,unsigned kind,unsigned count,const std::vector<std::uint8_t>* clear_vertices){
        const bool vram_source=((state.vertex_address&0x3F000000u)==0x04000000u) || ((state.index_address&0x3F000000u)==0x04000000u) || ((state.registers[0x1E]&1u) && (((state.registers[0xA8]<<8)&0x0F000000u)==0x04000000u));
        process_pending();
        // The previous frame's batch may still be shading: only a draw that
        // touches its outputs (or a path outside the open batch) waits for it.
        if(gpu_inflight_ && inflight_conflict(state,vram_source))collect_gpu_batch();
        NativeVramInternalScope ownership;
        if(gpu_batch_open_ && gpu_batch_has_draws_ && (state.registers[0x1E]&1u) && (((state.registers[0xA8]<<8)&0x0F000000u)==0x04000000u))++vram_texture_flushes;
        if(gpu_batch_open_ && (vram_source || ((state.registers[0xD3]&1u) && gpu_batch_has_draws_ && !clear_joins_batch(state))))flush_gpu_batch(1);
        const auto clear=state.registers[0xD3]&0xFFFFFFu;
        const auto type=state.registers[0x12]&0xFFFFFFu;
        if(!(clear&1u))return draw_flat_triangles(state,kind,count);
        const auto vertex16=[&](std::uint32_t address)->int{if(!clear_vertices)return memory_.load16(address);const auto o=address-state.vertex_address;return int((*clear_vertices)[o]|((*clear_vertices)[o+1]<<8));};
        const auto vertex32=[&](std::uint32_t address)->std::uint32_t{if(!clear_vertices)return memory_.load32(address);const auto o=address-state.vertex_address;return std::uint32_t((*clear_vertices)[o])|(std::uint32_t((*clear_vertices)[o+1])<<8)|(std::uint32_t((*clear_vertices)[o+2])<<16)|(std::uint32_t((*clear_vertices)[o+3])<<24);};
        if(kind!=6 || type!=0x80011Cu)return false;
        if(count&1u)throw std::runtime_error("GE clear sprite has an incomplete vertex pair");
        if(!memory_.contains(state.vertex_address,static_cast<std::size_t>(count)*12u))throw std::runtime_error("GE clear vertices outside mapped memory");
        const unsigned format=state.registers[0xD2]&3u,bytes=format==3?4u:2u;
        const unsigned stride=state.registers[0x9D]&0x7FCu,zstride=state.registers[0x9F]&0x7FCu;
        const auto frame=0x04000000u|(state.registers[0x9C]&0x1FFFF0u);
        const auto depth=0x04000000u|(state.registers[0x9E]&0x1FFFF0u);
        const bool rgb=clear&0x100u,alpha=clear&0x200u,zwrite=clear&0x400u;
        const unsigned rgbmask=format==0?0xFFFFu:format==1?0x7FFFu:format==2?0x0FFFu:0xFFFFFFu;
        const unsigned allmask=format==3?0xFFFFFFFFu:0xFFFFu;
        const auto mask=(rgb?rgbmask:0u)|(alpha?(allmask^rgbmask):0u);
        const int sx0=state.registers[0xD4]&1023u,sy0=(state.registers[0xD4]>>10u)&1023u;
        const int sx1=(state.registers[0xD5]&1023u)+1,sy1=((state.registers[0xD5]>>10u)&1023u)+1;
        struct Rectangle {int left,top,right,bottom;std::uint32_t color;std::uint16_t z;};
        std::vector<Rectangle> rectangles;
        for(unsigned n=0;n<count;n+=2u){
            const auto first=state.vertex_address+n*12u,last=first+12u;
            const int ax=vertex16(first+4u),ay=vertex16(first+6u),bx=vertex16(last+4u),by=vertex16(last+6u);
            Rectangle r{std::max(sx0,std::min(ax,bx)),std::max(sy0,std::min(ay,by)),std::min(sx1,std::max(ax,bx)),std::min(sy1,std::max(ay,by)),pack(vertex32(last),format),static_cast<std::uint16_t>(vertex16(last+8u))};
            if(r.left>=r.right || r.top>=r.bottom)continue;
            if(mask && (!stride || r.right>static_cast<int>(stride) || !memory_.contains(frame,(static_cast<std::size_t>(r.bottom-1)*stride+r.right)*bytes)))throw std::runtime_error("GE clear color range outside framebuffer");
            if(zwrite && (!zstride || r.right>static_cast<int>(zstride) || !memory_.contains(depth,(static_cast<std::size_t>(r.bottom-1)*zstride+r.right)*2u)))throw std::runtime_error("GE clear depth range outside framebuffer");
            rectangles.push_back(r);
        }
        // Validate every rectangle before changing either output buffer.
        std::size_t color_length=0,depth_length=0;
        for(const auto& rectangle:rectangles){
            if(mask)color_length=std::max(color_length,(static_cast<std::size_t>(rectangle.bottom-1)*stride+rectangle.right)*bytes);
            if(zwrite)depth_length=std::max(depth_length,(static_cast<std::size_t>(rectangle.bottom-1)*zstride+rectangle.right)*2u);
        }
        if(gpu_batch_open_ && gpu_batch_has_draws_ && clear_joins_batch(state)){
            // The same rectangles with the clear's 8888 colors (last vertex of each pair).
            std::vector<BatchClear> boxes;boxes.reserve(rectangles.size());
            for(unsigned n=0;n<count;n+=2u){
                const auto first=state.vertex_address+n*12u,last=first+12u;
                const int ax=vertex16(first+4u),ay=vertex16(first+6u),bx=vertex16(last+4u),by=vertex16(last+6u);
                const BatchClear b{std::max(sx0,std::min(ax,bx)),std::max(sy0,std::min(ay,by)),std::min(sx1,std::max(ax,bx)),std::min(sy1,std::max(ay,by)),vertex32(last),static_cast<std::uint16_t>(vertex16(last+8u))};
                if(b.left<b.right && b.top<b.bottom)boxes.push_back(b);
            }
            if(clear_in_batch(boxes,mask,zwrite,format,bytes,stride,zstride,frame,depth,color_length,depth_length)){
                if(frame_stream_){std::vector<NativeFrameClear> commands;for(const auto& r:rectangles)commands.push_back({unsigned(r.left),unsigned(r.top),unsigned(r.right),unsigned(r.bottom),r.color,mask,r.z});frame_stream_->clear(frame,depth,stride,zstride,format,zwrite,commands);}
                for(const auto& r:rectangles)clear_pixels+=static_cast<std::uint64_t>(r.right-r.left)*(r.bottom-r.top);
                state.vertex_address+=count*12u;++clear_primitives;++batch_clear_primitives;return true;
            }
            flush_gpu_batch(1);
        }
        if(gpu_inflight_){
            // A clear covering every depth word the in-flight batch changed
            // supersedes its depth result instead of waiting for it.
            // The game clears in 64-pixel strips: the strips spanning z's rows
            // must together span its columns.
            const auto& z=gpu_inflight_zrect_;bool covered=z[0]>=z[2] || z[1]>=z[3];
            if(!covered){
                std::vector<std::pair<int,int>> spans;
                for(const auto& rectangle:rectangles)if(rectangle.top<=z[1] && rectangle.bottom>=z[3])spans.push_back({rectangle.left,rectangle.right});
                std::sort(spans.begin(),spans.end());int reach=z[0];
                for(const auto& [left,right]:spans){if(left>reach)break;reach=std::max(reach,right);}
                covered=reach>=z[2];
            }
            const bool color_owned=color_length && vram_overlap(frame,color_length,gpu_inflight_identity_[3],gpu_inflight_colors_bytes_);
            const bool depth_owned=depth_length && !gpu_inflight_depth_superseded_ && vram_overlap(depth,depth_length,gpu_inflight_identity_[4],gpu_inflight_depth_bytes_);
            static const bool disabled=std::getenv("KON_NO_GPU_OVERLAP")!=nullptr;
            if(depth_owned && !covered && std::getenv("KON_OVERLAP_TRACE") && overlap_waits[4]<8){std::cout<<"overlap_clear z="<<z[0]<<","<<z[1]<<","<<z[2]<<","<<z[3];for(const auto& r:rectangles)std::cout<<" rect="<<r.left<<","<<r.top<<","<<r.right<<","<<r.bottom;std::cout<<" depth="<<std::hex<<depth<<" inflight_depth="<<gpu_inflight_identity_[4]<<std::dec<<" bytes="<<depth_length<<"/"<<gpu_inflight_depth_bytes_<<"\n";}
            if(disabled || color_owned || (depth_owned && !covered)){++overlap_waits[color_owned?3:4];collect_gpu_batch();}else if(depth_owned){++overlap_waits[5];gpu_inflight_depth_superseded_=true;}
        }
        NativeMemoryView color_buffer(memory_,frame,color_length,true),depth_buffer(memory_,depth,depth_length,true);
        if(frame_stream_){std::vector<NativeFrameClear> commands;for(const auto& r:rectangles)commands.push_back({unsigned(r.left),unsigned(r.top),unsigned(r.right),unsigned(r.bottom),r.color,mask,r.z});frame_stream_->clear(frame,depth,stride,zstride,format,zwrite,commands);}
        // A clear with disjoint color/depth buffers can run on the optional
        // native GPU backend. Feedback, mirrored views and write watches retain
        // the original CPU ordering. Guest VRAM is committed only after all GPU
        // passes finish, so the compiled game's next read sees completed pixels.
        // A synchronous GPU clear costs a full round trip; with an open raster
        // batch (pending work already published above) the CPU clear is faster.
        if(gpu_clear_ && !gpu_batch_open_ && !rectangles.empty() && (mask || zwrite) &&
           (!mask || color_buffer.direct()) && (!zwrite || depth_buffer.direct()) &&
           !(mask && zwrite && color_buffer.overlaps(depth_buffer)) && !native_write_watch_enabled()){
            gpu_color_scratch_.resize(color_length/bytes);gpu_depth_scratch_.resize(depth_length/2u);
            color_buffer.read_words(gpu_color_scratch_,bytes);
            depth_buffer.read_words(gpu_depth_scratch_,2);
            gpu_rectangles_.clear();
            if(mask){
                for(const auto& r:rectangles)gpu_rectangles_.push_back({static_cast<unsigned>(r.left),static_cast<unsigned>(r.top),static_cast<unsigned>(r.right),static_cast<unsigned>(r.bottom),r.color,mask});
                gpu_clear_->clear(gpu_color_scratch_,stride,gpu_rectangles_);
            }
            if(zwrite){
                gpu_rectangles_.clear();
                for(const auto& r:rectangles)gpu_rectangles_.push_back({static_cast<unsigned>(r.left),static_cast<unsigned>(r.top),static_cast<unsigned>(r.right),static_cast<unsigned>(r.bottom),r.z,0xFFFFu});
                gpu_clear_->clear(gpu_depth_scratch_,zstride,gpu_rectangles_);
            }
            color_buffer.write_words(gpu_color_scratch_,bytes);
            depth_buffer.write_words(gpu_depth_scratch_,2);
            for(const auto& r:rectangles){const auto pixels=static_cast<std::uint64_t>(r.right-r.left)*(r.bottom-r.top);clear_pixels+=pixels;gpu_clear_pixels+=pixels;}
            state.vertex_address+=count*12u;++clear_primitives;++gpu_clear_primitives;return true;
        }
        // Whole-word writes to direct 16-bit buffers fill each row at once.
        const bool fill_color=!mask || (bytes==2 && mask==allmask && color_buffer.direct());
        const bool fill_depth=!zwrite || depth_buffer.direct();
        if(fill_color && fill_depth){
            auto* color_bytes=mask?const_cast<std::uint8_t*>(color_buffer.data()):nullptr;
            auto* depth_bytes=zwrite?const_cast<std::uint8_t*>(depth_buffer.data()):nullptr;
            std::vector<std::uint16_t> color_row,depth_row;
            for(const auto& r:rectangles){
                const auto width=static_cast<std::size_t>(r.right-r.left);
                color_row.assign(width,static_cast<std::uint16_t>(r.color));depth_row.assign(width,r.z);
                for(int y=r.top;y<r.bottom;y++){
                    if(color_bytes)std::memcpy(color_bytes+(std::size_t(y)*stride+r.left)*2u,color_row.data(),width*2u);
                    if(depth_bytes)std::memcpy(depth_bytes+(std::size_t(y)*zstride+r.left)*2u,depth_row.data(),width*2u);
                }
                clear_pixels+=static_cast<std::uint64_t>(r.right-r.left)*(r.bottom-r.top);
            }
            state.vertex_address+=count*12u;++clear_primitives;return true;
        }
        for(const auto& r:rectangles)for(int y=r.top;y<r.bottom;y++)for(int x=r.left;x<r.right;x++){
            if(mask){
                const auto offset=(static_cast<unsigned>(y)*stride+static_cast<unsigned>(x))*bytes;
                const auto old=bytes==4?color_buffer.load32(offset):color_buffer.load16(offset);
                const auto value=(old&~mask)|(r.color&mask);
                if(bytes==4)color_buffer.store32(offset,value);else color_buffer.store16(offset,static_cast<std::uint16_t>(value));
            }
            if(zwrite)depth_buffer.store16((static_cast<unsigned>(y)*zstride+static_cast<unsigned>(x))*2u,r.z);
            ++clear_pixels;
        }
        state.vertex_address+=count*12u;++clear_primitives;return true;
    }
private:
    unsigned parallel_threshold_=8192;
    bool balanced_rows_=true;
    psprecomp::GuestMemory& memory_;
    NativeRasterPool raster_pool_;
    NativeGpuClear* gpu_clear_=nullptr;
    bool gpu_batch_open_=false,gpu_batch_has_draws_=false,gpu_inflight_=false,gpu_inflight_depth_superseded_=false;
    // Union of the pixel bounds of depth-writing draws (left, top, right, bottom).
    static constexpr std::array<int,4> kEmptyRect{1024,1024,0,0};
    std::array<int,4> gpu_batch_zrect_=kEmptyRect,gpu_inflight_zrect_=kEmptyRect;
    static bool vram_overlap(std::uint32_t a,std::size_t a_size,std::uint32_t b,std::size_t b_size){
        const auto first=std::size_t(a&(psprecomp::GuestMemory::kVramSize-1)),second=std::size_t(b&(psprecomp::GuestMemory::kVramSize-1));
        return a_size && b_size && first<second+b_size && second<first+a_size;
    }
    // Whether a draw must wait for the in-flight batch: any path other than
    // recording into the open batch, VRAM sources, or its color/depth outputs.
    // A clear's depth is decided in the clear path (it may supersede instead).
    // Mid-frame clears (a full depth clear between the 2D background and the
    // 3D scene, for instance) are shaded inside the open GPU batch, in draw
    // order, instead of ending it: ending it waited for the GPU in the middle
    // of every frame. Same destination buffers as the batch; KON_NO_BATCH_CLEAR
    // keeps the previous path (diagnostic A/B).
    struct BatchClear {int left,top,right,bottom;std::uint32_t color;std::uint16_t z;};
    bool clear_joins_batch(const GeState& state)const{
        static const bool disabled=std::getenv("KON_NO_BATCH_CLEAR")!=nullptr;
        if(disabled || !gpu_raster_ || native_write_watch_enabled())return false;
        const auto& r=state.registers;
        const std::array<unsigned,5> identity{r[0x9D]&0x7FCu,r[0x9F]&0x7FCu,r[0xD2]&3u,0x04000000u|(r[0x9C]&0x1FFFF0u),0x04000000u|(r[0x9E]&0x1FFFF0u)};
        return identity==gpu_batch_identity_ && (r[0x12]&0xFFFFFFu)==0x80011Cu;
    }
    bool clear_in_batch(const std::vector<BatchClear>& boxes,unsigned mask,bool zwrite,unsigned format,unsigned bytes,unsigned stride,unsigned zstride,std::uint32_t frame,std::uint32_t depth,std::size_t color_length,std::size_t depth_length){
        if(boxes.empty() || (!mask && !zwrite))return false;
        // Whole batch extent (the batch's own prefix plus this clear's rows).
        color_length=std::max(color_length,gpu_batch_colors_bytes_);if(zwrite)depth_length=std::max(depth_length,gpu_batch_depth_bytes_);
        NativeMemoryView color_buffer(memory_,frame,color_length,true),depth_buffer(memory_,depth,zwrite?depth_length:0u,true);
        if(!color_buffer.direct() || (zwrite && (!depth_buffer.direct() || color_buffer.overlaps(depth_buffer))))return false;
        // The batch in flight (the previous frame's) may still own these words:
        // the words read below must include its result, unless this clear
        // replaces all of its depth (then its depth result is dropped, as on
        // the CPU clear path).
        if(gpu_inflight_){
            const bool color_owned=vram_overlap(frame,color_length,gpu_inflight_identity_[3],gpu_inflight_colors_bytes_);
            const bool depth_owned=zwrite && !gpu_inflight_depth_superseded_ && vram_overlap(depth,depth_length,gpu_inflight_identity_[4],gpu_inflight_depth_bytes_);
            const auto& z=gpu_inflight_zrect_;bool covered=z[0]>=z[2] || z[1]>=z[3];
            if(!covered){std::vector<std::pair<int,int>> spans;for(const auto& b:boxes)if(b.top<=z[1] && b.bottom>=z[3])spans.push_back({b.left,b.right});
                std::sort(spans.begin(),spans.end());int reach=z[0];for(const auto& [l,r]:spans){if(l>reach)break;reach=std::max(reach,r);}covered=reach>=z[2];}
            if(color_owned || (depth_owned && !covered)){++overlap_waits[color_owned?3:4];collect_gpu_batch();}else if(depth_owned){++overlap_waits[5];gpu_inflight_depth_superseded_=true;}
        }
        auto& jobs=gpu_clear_jobs_;jobs.clear();
        int x0=1024,y0=1024,x1=0,y1=0;
        const auto inclusive=[](int ax,int ay,int bx,int by){return by<ay || (by==ay && bx>ax);};
        for(const auto& b:boxes){
            // Two triangles over the rectangle's pixel centers (1/16 pixel units).
            const int l=b.left*16,t=b.top*16,r=b.right*16,d=b.bottom*16;
            const std::array<std::array<int,6>,2> halves{{{l,t,r,t,r,d},{l,t,r,d,l,d}}};
            for(auto xy:halves){
                const auto area=std::int64_t(xy[2]-xy[0])*(xy[5]-xy[1])-std::int64_t(xy[3]-xy[1])*(xy[4]-xy[0]);
                if(!area)continue;
                if(area<0){std::swap(xy[2],xy[4]);std::swap(xy[3],xy[5]);}
                NativeGpuRasterTriangle job;job.xy={xy[0],xy[1],xy[2],xy[3],xy[4],xy[5]};
                for(unsigned v=0;v<3;++v){for(unsigned k=0;k<4;++k)job.attributes[v*8+k]=double((b.color>>(k*8u))&255u);job.attributes[v*8+7]=double(b.z);}
                const unsigned flags=(1u<<1)|(1u<<2)|(unsigned(inclusive(xy[0],xy[1],xy[2],xy[3]))<<3)|(unsigned(inclusive(xy[2],xy[3],xy[4],xy[5]))<<4)|(unsigned(inclusive(xy[4],xy[5],xy[0],xy[1]))<<5);
                job.state={unsigned(b.left),unsigned(b.top),unsigned(b.right),unsigned(b.bottom),flags,b.color,unsigned(b.right<=512 && b.bottom<=512),0};
                jobs.push_back(job);
            }
            x0=std::min(x0,b.left);y0=std::min(y0,b.top);x1=std::max(x1,b.right);y1=std::max(y1,b.bottom);
        }
        if(jobs.empty())return false;
        gpu_color_scratch_.resize(color_buffer.size()/bytes);gpu_depth_scratch_.resize(zwrite?depth_buffer.size()/2u:0u);
        color_buffer.read_words(gpu_color_scratch_,bytes,gpu_batch_colors_bytes_/bytes);
        if(zwrite)depth_buffer.read_words(gpu_depth_scratch_,2,gpu_batch_depth_bytes_/2u);
        const unsigned allmask=format==3?0xFFFFFFFFu:0xFFFFu;
        std::array<std::uint32_t,32> params{};params[7]=stride;params[8]=zstride;params[9]=format;params[10]=allmask^mask;
        // Through mode, flat color, no texture/blend/test; bit 4 writes depth without testing it.
        params[11]=(1u<<1)|(zwrite?(1u<<4):0u);
        params[21]=unsigned(x0);params[22]=unsigned(y0);params[23]=unsigned(x1);params[24]=unsigned(y1);params[28]=frame;params[29]=depth;
        NativeGpuRasterStats result;
        try{result=gpu_raster_->raster(gpu_color_scratch_,gpu_depth_scratch_,{},jobs,params);}
        catch(const NativeGpuBatchFull&){return false;}
        if(!result.deferred)throw std::runtime_error("GPU batch clear was not deferred");
        if(zwrite){auto& z=gpu_batch_zrect_;z={std::min(z[0],x0),std::min(z[1],y0),std::max(z[2],x1),std::max(z[3],y1)};}
        gpu_batch_colors_bytes_=std::max(gpu_batch_colors_bytes_,color_buffer.size());gpu_batch_depth_bytes_=std::max(gpu_batch_depth_bytes_,depth_buffer.size());
        return true;
    }
    std::vector<NativeGpuRasterTriangle> gpu_clear_jobs_;
    bool inflight_conflict(const GeState& state,bool vram_source)const{
        static const bool disabled=std::getenv("KON_NO_GPU_OVERLAP")!=nullptr; // diagnostic A/B
        if(disabled || !gpu_raster_ || !gpu_batch_open_ || vram_source){++overlap_waits[0];return true;}
        const auto& r=state.registers;
        const unsigned bytes=(r[0xD2]&3u)==3?4u:2u,stride=r[0x9D]&0x7FCu,zstride=r[0x9F]&0x7FCu,rows=((r[0xD5]>>10u)&1023u)+1u;
        const auto frame=0x04000000u|(r[0x9C]&0x1FFFF0u),depth=0x04000000u|(r[0x9E]&0x1FFFF0u);
        if(vram_overlap(frame,std::size_t(std::max(stride,512u))*rows*bytes,gpu_inflight_identity_[3],gpu_inflight_colors_bytes_)){++overlap_waits[1];return true;}
        const bool clear=r[0xD3]&1u,depth_test=r[0x23]&1u;
        const bool depth_conflict=!clear && depth_test && !gpu_inflight_depth_superseded_ && vram_overlap(depth,std::size_t(std::max(zstride,512u))*rows*2u,gpu_inflight_identity_[4],gpu_inflight_depth_bytes_);
        if(depth_conflict)++overlap_waits[2];return depth_conflict;
    }
    std::uint64_t texture_epoch_=0;
    std::array<unsigned,5> gpu_inflight_identity_{};std::size_t gpu_inflight_colors_bytes_=0,gpu_inflight_depth_bytes_=0;
    std::array<unsigned,5> gpu_batch_identity_{};
    std::size_t gpu_batch_colors_bytes_=0,gpu_batch_depth_bytes_=0;
    NativeGpuRaster* gpu_raster_=nullptr;
    NativeFrameStream* frame_stream_=nullptr;
    std::vector<NativeGpuRasterTriangle> gpu_raster_jobs_;
    std::vector<std::uint32_t> gpu_texture_scratch_;
    NativeGpuTriangles* gpu_triangles_=nullptr;
    std::vector<NativeGpuFlatTriangle> gpu_triangle_scratch_;
    std::vector<std::uint32_t> gpu_color_scratch_,gpu_depth_scratch_;
    std::vector<NativeGpuClearRectangle> gpu_rectangles_;
    bool texture_cache_enabled_;
    // Guest execution uses one renderer at a time and the draw barrier finishes
    // before reuse. Workers only read these completed geometry buffers.
    bool projection_cache_enabled_=true;
    std::vector<unsigned char> projected_vertex_status_;
    std::vector<GeometryVertex> vertex_scratch_;
    std::vector<std::array<GeometryVertex,3>> triangle_scratch_;
    struct PreparedTriangle {
        std::array<GeometryVertex,3> vertices;
        std::int64_t area=0,step_a=0,step_b=0,step_c=0;
        int left=0,top=0,right=0,bottom=0;
        bool constant_color=false,constant_depth=false,linear=false,include_a=false,include_b=false,include_c=false;
        std::uint32_t flat_color=0;
        NativeFixedTriangle fixed;
    };
    std::vector<PreparedTriangle> prepared_triangle_scratch_;
    bool prepared_raster_enabled_=false;
    bool direct_vertex_reads_enabled_=true;
    bool fast_blend_enabled_=true;
    bool persistent_texture_cache_enabled_=true;
    NativeTexture::Cache decoded_texture_cache_,gpu_decoded_texture_cache_{128,64u*1024u*1024u};
    std::vector<std::uint32_t> decoded_texture_scratch_;
    static bool compare(unsigned function,unsigned value,unsigned reference){
        switch(function&7u){case 0:return false;case 1:return true;case 2:return value==reference;case 3:return value!=reference;case 4:return value<reference;case 5:return value<=reference;case 6:return value>reference;default:return value>=reference;}
    }
    static std::uint32_t unpack(std::uint32_t value,unsigned format){
        if(format==3)return value;
        const auto five=[](unsigned v){return (v<<3u)|(v>>2u);};const auto six=[](unsigned v){return (v<<2u)|(v>>4u);};
        if(format==0)return five(value&31u)|(six((value>>5u)&63u)<<8u)|(five((value>>11u)&31u)<<16u);
        if(format==1)return five(value&31u)|(five((value>>5u)&31u)<<8u)|(five((value>>10u)&31u)<<16u)|((value&0x8000u)?0xFF000000u:0);
        return (value&15u)*17u|(((value>>4u)&15u)*17u<<8u)|(((value>>8u)&15u)*17u<<16u)|(((value>>12u)&15u)*17u<<24u);
    }
    // `snapshot`: the draw's vertex bytes copied when it was queued (asynchronous
    // mode), so builders never read guest memory that original code may change.
    void build_flat_geometry(const GeState& state,unsigned kind,unsigned count,bool gpu_jobs,FlatGeometry& geo,const std::vector<std::uint8_t>* snapshot=nullptr)const{
        geo.reset();
        const auto& r=state.registers;
        const auto type=r[0x12]&0xFFFFFFu;
        const bool through=type&0x800000u;
        const unsigned position_type=(type>>7u)&3u,normal_type=(type>>5u)&3u;
        const unsigned weight_type=(type>>9u)&3u,weight_count=weight_type?((type>>14u)&7u)+1u:0u;
        if((through?position_type!=2:position_type!=3) || (type&~0x81C7FFu) || (through && weight_type) || kind<3 || kind>5 || count<3 || (kind==3 && count%3))return;
        for(unsigned flag:{0x1Fu,0x20u,0x24u,0x25u,0x27u,0x28u})if(r[flag]&1u)return;
        // Ambient-only lighting is independent of normals. Active light channels
        // need their own diffuse/specular/attenuation path and remain a stop.
        const bool lighting=!through && (r[0x17]&1u);
        if(lighting)for(unsigned flag:{0x18u,0x19u,0x1Au,0x1Bu})if(r[flag]&1u)return;
        const bool textured=r[0x1E]&1u;
        const unsigned uv_type=type&3u,color_type=(type>>2u)&7u;
        if(color_type && color_type<4)return;
        const unsigned min_filter=r[0xC6]&7u;
        if(textured && (!uv_type || (r[0xC3]&15u)>10 || (r[0xC2]&0x70000u) || min_filter==2 || min_filter==3 || ((r[0xC9]&7u)!=0 && (r[0xC9]&7u)!=3) || (!through && (r[0xC0]&3u))))return;
        const bool blend=r[0x21]&1u,depth_test=r[0x23]&1u;
        if(blend && !NativeBlend::supported(r[0xDF]&0xFFFFFFu))return;
        const unsigned uv_bytes=uv_type==1?2u:uv_type==2?4u:uv_type==3?8u:0u,color_bytes=color_type==7?4u:color_type?2u:0u;
        const auto align=[](unsigned value,unsigned boundary){return (value+boundary-1u)&~(boundary-1u);};
        const unsigned weight_component=weight_type==3?4u:weight_type==2?2u:weight_type?1u:0u,uv_component=uv_type==3?4u:uv_type==2?2u:1u;
        const unsigned uv_offset=align(weight_component*weight_count,uv_component);
        const unsigned normal_component=normal_type==3?4u:normal_type==2?2u:normal_type?1u:0u,position_component=position_type==3?4u:2u;
        const unsigned color_offset=align(uv_offset+uv_bytes,color_bytes?color_bytes:1u),normal_offset=align(color_offset+color_bytes,normal_component?normal_component:1u),position_offset=align(normal_offset+normal_component*3u,position_component);
        const unsigned vertex_stride=align(position_offset+3u*position_component,std::max({position_component,normal_component,color_bytes,uv_component,weight_component}));
        if(!memory_.contains(state.vertex_address,static_cast<std::size_t>(count)*vertex_stride))throw std::runtime_error("GE triangle vertices outside mapped memory");
        if(snapshot && snapshot->size()<static_cast<std::size_t>(count)*vertex_stride)throw std::logic_error("Queued vertex snapshot is too short");
        std::optional<NativeMemoryView> vertex_data;if(!snapshot)vertex_data.emplace(memory_,state.vertex_address,static_cast<std::size_t>(count)*vertex_stride,false,!direct_vertex_reads_enabled_);
        const auto* bytes_in=snapshot?snapshot->data():nullptr;
        const auto vertex8=[&](unsigned address)->std::uint8_t{const auto o=address-state.vertex_address;return bytes_in?bytes_in[o]:vertex_data->load8(o);};
        const auto vertex16=[&](unsigned address)->std::uint16_t{const auto o=address-state.vertex_address;if(!bytes_in)return vertex_data->load16(o);std::uint16_t v;std::memcpy(&v,bytes_in+o,2);return v;};
        const auto vertex32=[&](unsigned address)->std::uint32_t{const auto o=address-state.vertex_address;if(!bytes_in)return vertex_data->load32(o);std::uint32_t v;std::memcpy(&v,bytes_in+o,4);return v;};
        bool bounded_through_uv=true;
        using Vertex=GeometryVertex;auto& vertices=geo.vertices;vertices.clear();vertices.reserve(count);
        for(unsigned n=0;n<count;++n){
            const auto address=state.vertex_address+n*vertex_stride;
            auto color=color_type==7?vertex32(address+color_offset):color_type?NativeTexture::decode_color(vertex16(address+color_offset),color_type-4u):(r[0x55]&0xFFFFFFu)|((r[0x58]&255u)<<24u);
            if(lighting){
                const auto material=color_type && (r[0x53]&1u)?color:(r[0x55]&0xFFFFFFu)|((r[0x58]&255u)<<24u);
                const auto ambient=(r[0x5C]&0xFFFFFFu)|((r[0x5D]&255u)<<24u);color=0;
                for(unsigned shift:{0u,8u,16u,24u}){
                    const unsigned product=((((material>>shift)&255u)*2u+1u)*(((ambient>>shift)&255u)*2u+1u))>>10u;
                    const unsigned emission=shift==24?0u:(r[0x54]>>shift)&255u;
                    color|=std::min(product+emission,255u)<<shift;
                }
            }
            Vertex vertex;for(unsigned i=0;i<4;++i)vertex.color[i]=(color>>(i*8u))&255u;
            if(through){vertex.x=static_cast<std::int16_t>(vertex16(address+position_offset))*16;vertex.y=static_cast<std::int16_t>(vertex16(address+position_offset+2u))*16;vertex.z=vertex16(address+position_offset+4u);}
            else {
                std::array<double,3> position{};for(unsigned i=0;i<3;++i)position[i]=std::bit_cast<float>(vertex32(address+position_offset+i*4u));
                if(weight_count){std::array<double,8> weights{};for(unsigned n=0;n<weight_count;++n)weights[n]=weight_type==1?vertex8(address+n)/128.0:weight_type==2?vertex16(address+n*2u)/32768.0:std::bit_cast<float>(vertex32(address+n*4u));position=NativeGeometry::skin(state,position,std::span(weights).first(weight_count));}
                vertex.clip=NativeGeometry::transform(state,position);
            }
            if(uv_type==1){vertex.u=vertex8(address+uv_offset);vertex.v=vertex8(address+uv_offset+1u);}
            if(uv_type==2){vertex.u=vertex16(address+uv_offset);vertex.v=vertex16(address+uv_offset+2u);}
            if(uv_type==3){vertex.u=std::bit_cast<float>(vertex32(address+uv_offset));vertex.v=std::bit_cast<float>(vertex32(address+uv_offset+4u));}
            if(!through){
                if(uv_type==1){vertex.u/=128.0;vertex.v/=128.0;}else if(uv_type==2){vertex.u/=32768.0;vertex.v/=32768.0;}
                vertex.u=(vertex.u*GeState::float24(r[0x48])+GeState::float24(r[0x4A]))*(1u<<(r[0xB8]&15u));
                vertex.v=(vertex.v*GeState::float24(r[0x49])+GeState::float24(r[0x4B]))*(1u<<((r[0xB8]>>8u)&15u));
            }
            if(!std::isfinite(vertex.u) || !std::isfinite(vertex.v))throw std::runtime_error("Nonfinite GE texture coordinate");
            if(through)bounded_through_uv=bounded_through_uv && std::abs(vertex.u)<100000.0 && std::abs(vertex.v)<100000.0;
            vertices.push_back(vertex);
        }
        // Complete geometry validation/clipping before any framebuffer mutation.
        auto& raster_triangles=geo.triangles;
        const unsigned triangles=kind==3?count/3:count-2;
        auto& projected_status=geo.projected;if(!through && projection_cache_enabled_)projected_status.assign(count,0);
        const auto project_original=[&](unsigned index,Vertex& target){
            auto& status=projected_status[index];auto& source=vertices[index];
            if(!status){++geo.projections;status=NativeGeometry::project(state,source)?1u:2u;}
            target.x=source.x;target.y=source.y;target.z=source.z;target.q=source.q;
            return status==1u;
        };
        raster_triangles.reserve(triangles*(through?1u:2u));
        for(unsigned n=0;n<triangles;++n){
            unsigned ia=kind==3?n*3u:kind==5?0u:n,ib=kind==3?n*3u+1u:n+1u,ic=kind==3?n*3u+2u:n+2u;
            auto a=vertices[ia],b=vertices[ib],c=vertices[ic];
            if(!through && !(NativeGeometry::finite(a.clip) && NativeGeometry::finite(b.clip) && NativeGeometry::finite(c.clip))){++geo.nonfinite;continue;}
            if(!(r[0x50]&1u))a.color=b.color=c.color;
            if(kind==4 && (n&1u)){std::swap(a,b);std::swap(ia,ib);}
            if(through){raster_triangles.push_back({a,b,c});continue;}
            // A strip/fan reuses vertices across triangles. Cache only their
            // viewport conversion for this draw, retaining the exact operation
            // order and provoking color. Near/eye crossings use full clipping.
            const auto in_front=[](const Vertex& v){return v.clip[3]>0 && v.clip[2]>=-v.clip[3];};
            if(projection_cache_enabled_ && in_front(a) && in_front(b) && in_front(c)){
                if(a.clip[2]>a.clip[3] && b.clip[2]>b.clip[3] && c.clip[2]>c.clip[3])continue;
                bool inside=project_original(ia,a);inside=project_original(ib,b) && inside;inside=project_original(ic,c) && inside;
                if(inside)raster_triangles.push_back({a,b,c});continue;
            }
            auto polygon=NativeGeometry::clip_triangle(state,a,b,c);bool inside=true;
            for(auto& vertex:polygon){++geo.projections;inside=NativeGeometry::project(state,vertex) && inside;}
            if(inside)for(unsigned i=1;i+1<polygon.size();++i)raster_triangles.push_back({polygon[0],polygon[i],polygon[i+1]});
        }
        geo.vertex_stride=vertex_stride;geo.bounded_through_uv=bounded_through_uv;geo.ok=true;
        if(!gpu_jobs)return;
        // GPU jobs: the same triangle setup as the CPU path's prepare_triangle,
        // without its fixed-point form (the backend derives it from each job).
        const int left=std::max(r[0xD4]&1023u,r[0x15]&1023u),top=std::max((r[0xD4]>>10u)&1023u,(r[0x15]>>10u)&1023u);
        const int right=std::min(r[0xD5]&1023u,r[0x16]&1023u)+1,bottom=std::min((r[0xD5]>>10u)&1023u,(r[0x16]>>10u)&1023u)+1;
        const auto edge=[](const Vertex& a,const Vertex& b,int x,int y){return static_cast<std::int64_t>(b.x-a.x)*(y-a.y)-static_cast<std::int64_t>(b.y-a.y)*(x-a.x);};
        const auto inclusive=[](const Vertex& a,const Vertex& b){return b.y<a.y || (b.y==a.y && b.x>a.x);};
        auto& jobs=geo.jobs;jobs.reserve(raster_triangles.size());
        int x0=right,y0=bottom,x1=left,y1=top;
        for(const auto& triangle:raster_triangles){
            std::array<Vertex,3> v=triangle;auto& a=v[0];auto& b=v[1];auto& c=v[2];
            auto area=edge(a,b,c.x,c.y);if(!area)continue;
            if((r[0x1D]&1u) && ((area>0)!=((r[0x9B]&1u)!=0)))continue;
            if(area<0){std::swap(b,c);area=-area;}
            const bool constant_color=a.color==b.color && b.color==c.color,constant_depth=a.z==b.z && b.z==c.z;
            std::uint32_t flat_color=0;for(unsigned n=0;n<4;++n)flat_color|=static_cast<unsigned>(a.color[n])<<(n*8u);
            bool linear=false;
            if(textured){
                const double determinant=area/16.0;
                const auto dx=[&](double au,double bu,double cu){return ((bu-au)*(c.y-a.y)-(cu-au)*(b.y-a.y))/determinant;};
                const auto dy=[&](double au,double bu,double cu){return ((b.x-a.x)*(cu-au)-(c.x-a.x)*(bu-au))/determinant;};
                const auto scale=std::max({std::abs(dx(a.u,b.u,c.u)),std::abs(dy(a.u,b.u,c.u)),std::abs(dx(a.v,b.v,c.v)),std::abs(dy(a.v,b.v,c.v))});
                linear=scale>1.0?(r[0xC6]&1u)!=0:(r[0xC6]&0x100u)!=0;
            }
            const int pl=std::max(left,static_cast<int>(std::floor(std::min({a.x,b.x,c.x})/16.0))),pr=std::min(right,static_cast<int>(std::ceil(std::max({a.x,b.x,c.x})/16.0)));
            const int pt=std::max(top,static_cast<int>(std::floor(std::min({a.y,b.y,c.y})/16.0))),pb=std::min(bottom,static_cast<int>(std::ceil(std::max({a.y,b.y,c.y})/16.0)));
            const bool include_a=inclusive(a,b),include_b=inclusive(b,c),include_c=inclusive(c,a);
            if(pl>=pr || pt>=pb)continue;
            NativeGpuRasterTriangle job;job.xy={a.x,a.y,b.x,b.y,c.x,c.y};
            for(unsigned n=0;n<3;++n){const auto& src=v[n];for(unsigned k=0;k<4;++k)job.attributes[n*8+k]=src.color[k];job.attributes[n*8+4]=src.u;job.attributes[n*8+5]=src.v;job.attributes[n*8+6]=src.q;job.attributes[n*8+7]=src.z;}
            const unsigned flags=unsigned(linear)|(unsigned(constant_color)<<1)|(unsigned(constant_depth)<<2)|(unsigned(include_a)<<3)|(unsigned(include_b)<<4)|(unsigned(include_c)<<5);
            bool narrow=pr<=512 && pb<=512;for(auto value:job.xy)narrow=narrow && value>=-8192 && value<=8191;
            job.state={unsigned(pl),unsigned(pt),unsigned(pr),unsigned(pb),flags,flat_color,unsigned(narrow),0};jobs.push_back(job);
            x0=std::min(x0,pl);y0=std::min(y0,pt);x1=std::max(x1,pr);y1=std::max(y1,pb);
        }
        geo.x0=x0;geo.y0=y0;geo.x1=x1;geo.y1=y1;geo.jobs_ready=true;
        // Validated and encoded here (worker threads), copied as is by the backend.
        geo.encoded.resize(jobs.size()*kFixedJobWords);const bool perspective=textured && !through;
        // A job the backend would reject drops the encoding: the backend then
        // validates again and reports it, only if this draw takes the GPU path.
        try{for(std::size_t n=0;n<jobs.size();++n){validate_raster_job(jobs[n],textured,unsigned(x0),unsigned(y0),unsigned(x1),unsigned(y1));write_fixed_job(geo.encoded.data()+n*kFixedJobWords,jobs[n],perspective);}}
        catch(const std::exception&){geo.encoded.clear();}
        return;
    }
    bool draw_flat_triangles(GeState& state,unsigned kind,unsigned count,FlatGeometry* prebuilt=nullptr){
        if(!prebuilt)build_flat_geometry(state,kind,count,gpu_raster_ || frame_stream_,sync_geometry_);
        FlatGeometry& geo=prebuilt?*prebuilt:sync_geometry_;
        if(geo.error)std::rethrow_exception(geo.error);
        if(!geo.ok)return false;
        vertex_projection_evaluations+=geo.projections;nonfinite_triangles+=geo.nonfinite;
        const auto& r=state.registers;const auto type=r[0x12]&0xFFFFFFu;const bool through=type&0x800000u;
        const bool textured=r[0x1E]&1u,blend=r[0x21]&1u,depth_test=r[0x23]&1u;
        const unsigned vertex_stride=geo.vertex_stride;const bool bounded_through_uv=geo.bounded_through_uv;
        using Vertex=GeometryVertex;auto& raster_triangles=geo.triangles;
        const auto alpha_test=r[0xDB]&0xFFFFFFu;
        std::optional<NativeTexture> texture;if(textured)texture.emplace(memory_,state);
        const unsigned format=r[0xD2]&3u,bytes=format==3?4u:2u,stride=r[0x9D]&0x7FCu,zstride=r[0x9F]&0x7FCu;
        const auto frame=0x04000000u|(r[0x9C]&0x1FFFF0u),depth=0x04000000u|(r[0x9E]&0x1FFFF0u);
        const int left=std::max(r[0xD4]&1023u,r[0x15]&1023u),top=std::max((r[0xD4]>>10u)&1023u,(r[0x15]>>10u)&1023u);
        const int right=std::min(r[0xD5]&1023u,r[0x16]&1023u)+1,bottom=std::min((r[0xD5]>>10u)&1023u,(r[0x16]>>10u)&1023u)+1;
        const bool zwrite=depth_test && !(r[0xE7]&1u);
        const bool early_depth=depth_test && !(r[0x22]&1u);
        if(!stride || right>static_cast<int>(stride) || !memory_.contains(frame,(static_cast<std::size_t>(bottom-1)*stride+right)*bytes))throw std::runtime_error("GE triangle framebuffer outside mapped memory");
        if(depth_test && (!zstride || right>static_cast<int>(zstride) || !memory_.contains(depth,(static_cast<std::size_t>(bottom-1)*zstride+right)*2u)))throw std::runtime_error("GE triangle depth outside mapped memory");
        NativeMemoryView color_buffer(memory_,frame,(static_cast<std::size_t>(bottom-1)*stride+right)*bytes,true);
        NativeMemoryView depth_buffer(memory_,depth,depth_test?(static_cast<std::size_t>(bottom-1)*zstride+right)*2u:0u,true);
        const bool parallel_safe=color_buffer.direct() && (!depth_test || depth_buffer.direct()) && !color_buffer.overlaps(depth_buffer) && (!texture || texture->parallel_safe(color_buffer,depth_buffer));
        if(gpu_batch_open_ && !parallel_safe)flush_gpu_batch(2);
        if(frame_stream_ && !parallel_safe)frame_stream_->reject(frame,"Draw requires direct VRAM feedback or alias semantics");
        const unsigned rgbmask=format==0?0xFFFFu:format==1?0x7FFFu:format==2?0x0FFFu:0xFFFFFFu;
        const unsigned allmask=format==3?0xFFFFFFFFu:0xFFFFu,keep=(allmask^rgbmask)|(pack(r[0xE8]&0xFFFFFFu,format)&rgbmask);
        const auto edge=[](const Vertex& a,const Vertex& b,int x,int y){return static_cast<std::int64_t>(b.x-a.x)*(y-a.y)-static_cast<std::int64_t>(b.y-a.y)*(x-a.x);};
        const auto inclusive=[](const Vertex& a,const Vertex& b){return b.y<a.y || (b.y==a.y && b.x>a.x);};
        // Exact GPU subset: flat constant colors, no sampling/blending/depth.
        // Preserve CPU clipping/projection/culling and commit only on success.
        if(gpu_triangles_ && !gpu_raster_ && !texture && !blend && !depth_test && color_buffer.direct() &&
           !native_write_watch_enabled() && left<right && top<bottom){
            auto& jobs=gpu_triangle_scratch_;jobs.clear();bool eligible=true;
            unsigned roi_left=right,roi_top=bottom,roi_right=left,roi_bottom=top;
            for(auto triangle:raster_triangles){
                auto a=triangle[0],b=triangle[1],c=triangle[2];auto area=edge(a,b,c.x,c.y);
                if(!area || ((r[0x1D]&1u) && ((area>0)!=((r[0x9B]&1u)!=0))))continue;
                if(area<0)std::swap(b,c);
                if(a.color!=b.color || b.color!=c.color){eligible=false;break;}
                for(auto v:{a.x,a.y,b.x,b.y,c.x,c.y})if(v< -8192 || v>8191)eligible=false;
                if(!eligible)break;
                const int x0=std::max(left,static_cast<int>(std::floor(std::min({a.x,b.x,c.x})/16.0))),x1=std::min(right,static_cast<int>(std::ceil(std::max({a.x,b.x,c.x})/16.0)));
                const int y0=std::max(top,static_cast<int>(std::floor(std::min({a.y,b.y,c.y})/16.0))),y1=std::min(bottom,static_cast<int>(std::ceil(std::max({a.y,b.y,c.y})/16.0)));
                if(x0>=x1 || y0>=y1)continue;
                std::uint32_t color=0;for(unsigned i=0;i<4;++i)color|=static_cast<unsigned>(a.color[i])<<(i*8u);
                const auto alpha=color>>24u;
                const bool pass=!(r[0x22]&1u) || compare(alpha_test,alpha&((alpha_test>>16u)&255u),(alpha_test>>8u)&255u);
                jobs.push_back({{a.x,a.y,b.x,b.y,c.x,c.y},pack(color,format),unsigned(inclusive(a,b))|(unsigned(inclusive(b,c))<<1u)|(unsigned(inclusive(c,a))<<2u),unsigned(pass),unsigned(x0),unsigned(y0),unsigned(x1),unsigned(y1),{}});
                roi_left=std::min(roi_left,unsigned(x0));roi_top=std::min(roi_top,unsigned(y0));roi_right=std::max(roi_right,unsigned(x1));roi_bottom=std::max(roi_bottom,unsigned(y1));
            }
            const auto words=static_cast<std::size_t>(bottom-1)*stride+right;
            if(eligible && !jobs.empty() && words*3ull+jobs.size()*16ull<=NativeGpuTriangles::capacity_words){
                gpu_color_scratch_.resize(words);color_buffer.read_words(gpu_color_scratch_,bytes);
                const auto result=gpu_triangles_->triangles(gpu_color_scratch_,stride,jobs,roi_left,roi_top,roi_right,roi_bottom,allmask^keep);
                color_buffer.write_words(gpu_color_scratch_,bytes);
                draw_covered_pixels+=result.covered;alpha_rejected_pixels+=result.alpha_rejected;draw_pixels+=result.covered-result.alpha_rejected;
                ++gpu_triangle_primitives;gpu_triangle_pixels+=result.covered-result.alpha_rejected;
                state.vertex_address+=count*vertex_stride;++draw_primitives;return true;
            }
        }
        struct alignas(64) Stats {std::uint64_t covered=0,alpha=0,depth=0,drawn=0;};std::array<Stats,16> stats{};
        // One barrier for the complete draw, rather than every triangle. Each
        // worker owns a screen-row strip and traverses triangles in original
        // order. This is only safe without output/texture aliasing.
        std::uint64_t work_area=0;int work_top=bottom,work_bottom=top;
        if(parallel_safe && raster_pool_.lanes()>1 && bottom-top>=32){
            for(const auto& t:raster_triangles){
                const int x0=std::max(left,static_cast<int>(std::floor(std::min({t[0].x,t[1].x,t[2].x})/16.0)));
                const int x1=std::min(right,static_cast<int>(std::ceil(std::max({t[0].x,t[1].x,t[2].x})/16.0)));
                const int y0=std::max(top,static_cast<int>(std::floor(std::min({t[0].y,t[1].y,t[2].y})/16.0)));
                const int y1=std::min(bottom,static_cast<int>(std::ceil(std::max({t[0].y,t[1].y,t[2].y})/16.0)));
                if(x1>x0 && y1>y0){work_area+=static_cast<std::uint64_t>(x1-x0)*(y1-y0);work_top=std::min(work_top,y0);work_bottom=std::max(work_bottom,y1);}
            }
        }
        if(!gpu_raster_ && texture_cache_enabled_ && texture && parallel_safe && work_area>=texture->decoded_texel_count()){
            if(persistent_texture_cache_enabled_){if(texture->prepare_cache(decoded_texture_cache_))++texture_cache_preparations;else ++texture_cache_hits;}
            else {texture->prepare_cache(decoded_texture_scratch_);++texture_cache_preparations;}
        }
        // The GPU path rebuilds the fixed-point form from the job (backend), so it skips it here.
        const auto prepare_triangle=[&](const std::array<Vertex,3>& triangle,PreparedTriangle& prepared,bool cpu_fixed=true){
            auto& a=prepared.vertices[0];auto& b=prepared.vertices[1];auto& c=prepared.vertices[2];prepared.vertices=triangle;
            auto area=edge(a,b,c.x,c.y);if(!area)return false;
            if((r[0x1D]&1u) && ((area>0)!=((r[0x9B]&1u)!=0)))return false;
            if(area<0){std::swap(b,c);area=-area;}prepared.area=area;
            prepared.constant_color=a.color==b.color && b.color==c.color;prepared.constant_depth=a.z==b.z && b.z==c.z;
            prepared.flat_color=0;for(unsigned i=0;i<4;++i)prepared.flat_color|=static_cast<unsigned>(a.color[i])<<(i*8u);
            prepared.linear=false;
            if(textured){
                const double determinant=area/16.0;
                const auto dx=[&](double au,double bu,double cu){return ((bu-au)*(c.y-a.y)-(cu-au)*(b.y-a.y))/determinant;};
                const auto dy=[&](double au,double bu,double cu){return ((b.x-a.x)*(cu-au)-(c.x-a.x)*(bu-au))/determinant;};
                const auto scale=std::max({std::abs(dx(a.u,b.u,c.u)),std::abs(dy(a.u,b.u,c.u)),std::abs(dx(a.v,b.v,c.v)),std::abs(dy(a.v,b.v,c.v))});
                prepared.linear=scale>1.0?(r[0xC6]&1u)!=0:(r[0xC6]&0x100u)!=0;
            }
            prepared.left=std::max(left,static_cast<int>(std::floor(std::min({a.x,b.x,c.x})/16.0)));prepared.right=std::min(right,static_cast<int>(std::ceil(std::max({a.x,b.x,c.x})/16.0)));
            prepared.top=std::max(top,static_cast<int>(std::floor(std::min({a.y,b.y,c.y})/16.0)));prepared.bottom=std::min(bottom,static_cast<int>(std::ceil(std::max({a.y,b.y,c.y})/16.0)));
            prepared.step_a=-static_cast<std::int64_t>(b.y-a.y)*16;prepared.step_b=-static_cast<std::int64_t>(c.y-b.y)*16;prepared.step_c=-static_cast<std::int64_t>(a.y-c.y)*16;
            prepared.include_a=inclusive(a,b);prepared.include_b=inclusive(b,c);prepared.include_c=inclusive(c,a);
            if(prepared.left>=prepared.right || prepared.top>=prepared.bottom)return false;
            if(cpu_fixed){
            std::array<double,24> attributes{};for(unsigned v=0;v<3;++v){const auto& s=prepared.vertices[v];for(unsigned i=0;i<4;++i)attributes[v*8+i]=s.color[i];attributes[v*8+4]=s.u;attributes[v*8+5]=s.v;attributes[v*8+6]=s.q;attributes[v*8+7]=s.z;}
            if(textured)validate_fixed_texture(attributes);
            prepared.fixed=make_fixed_triangle(area,attributes,textured && !through);
            }
            return true;
        };
        if((gpu_raster_ || frame_stream_) && parallel_safe && !native_write_watch_enabled()){
            if(!geo.jobs_ready)throw std::logic_error("GPU jobs were not built for this draw");
            auto& jobs=geo.jobs;const int x0=geo.x0,y0=geo.y0,x1=geo.x1,y1=geo.y1;
            if(!jobs.empty()){
                const std::array<unsigned,5> identity{stride,zstride,format,frame,depth};
                if(gpu_batch_open_ && gpu_batch_has_draws_ && identity!=gpu_batch_identity_)flush_gpu_batch(3);

                gpu_color_scratch_.resize(color_buffer.size()/bytes);gpu_depth_scratch_.resize(depth_test?depth_buffer.size()/2:0);
                // The open batch (same identity, flushed above otherwise) holds
                // the destination prefix it uploaded and uses only newly
                // extended words. Guest writes into that prefix publish first.
                const bool resident=gpu_raster_ && gpu_batch_open_ && gpu_batch_has_draws_;
                const std::size_t known_colors=resident?gpu_batch_colors_bytes_/bytes:0,known_depths=resident?gpu_batch_depth_bytes_/2u:0;
                color_buffer.read_words(gpu_color_scratch_,bytes,known_colors);
                if(depth_test)depth_buffer.read_words(gpu_depth_scratch_,2,known_depths);
                gpu_resident_words_reused+=std::min(known_colors,gpu_color_scratch_.size())+std::min(known_depths,gpu_depth_scratch_.size());
                gpu_texture_scratch_.clear();std::span<const std::uint32_t> gpu_texels;
                std::uint32_t texture_token=0;
                if(texture && recording_slot_ && recording_slot_->texture_ready){gpu_texels=recording_slot_->texels;texture_token=recording_slot_->token;}
                else if(texture){gpu_texels=texture->decoded_pixels();if(gpu_texels.empty()){std::lock_guard lock(texture_mutex_);if(texture->prepare_cache(gpu_decoded_texture_cache_))++texture_cache_preparations;else ++texture_cache_hits;gpu_texels=texture->decoded_pixels();}texture_token=texture->decoded_token();}
                std::array<std::uint32_t,32> params{};params[7]=stride;params[8]=zstride;params[9]=format;params[10]=keep;
                params[11]=unsigned(textured)|(unsigned(through)<<1)|(unsigned(blend)<<2)|(unsigned(depth_test)<<3)|(unsigned(zwrite)<<4)|(unsigned(early_depth)<<5)|((r[0x22]&1u)<<6);
                params[12]=alpha_test;params[13]=r[0xDE];params[14]=r[0xDF];params[15]=r[0xE0];params[16]=r[0xE1];params[17]=r[0xC9];
                params[18]=texture?texture->width():0;params[19]=texture?texture->height():0;params[20]=r[0xC7];params[21]=x0;params[22]=y0;params[23]=x1;params[24]=y1;params[27]=r[0xCA];params[28]=frame;params[29]=depth;params[30]=texture_token;
                if(frame_stream_)frame_stream_->raster(params,gpu_texels,jobs);
                if(gpu_raster_){
                NativeGpuRasterStats result;
                // 2D draws at internal resolution sample an enlarged copy of their
                // texture once the background upscaler has made it (native_upscale.hpp).
                std::shared_ptr<const std::vector<std::uint32_t>> hires;unsigned hires_factor=0;
                if(textured && through && texture_token && texture && !gpu_texels.empty()){
                    if(const auto scale=gpu_raster_?gpu_raster_->internal_scale():1u;scale>1u){
                        hires_factor=scale>=3u?4u:2u;hires=TextureUpscaler::instance().find(texture_token,gpu_texels,texture->width(),texture->height(),hires_factor);}}
                {static const bool trace=std::getenv("KON_UPSCALE_TRACE")!=nullptr; // diagnostic: 2D draws and enlarged-texture use
                 if(trace && textured){static std::uint64_t n[4]{};++n[0];if(through)++n[1];if(through && !texture_token)++n[2];if(hires)++n[3];
                  {static std::set<std::uint64_t> pairs;const auto tex=(r[0xA0]&0xFFFFF0u)|((r[0xA8]<<8u)&0x0F000000u);
                   if(pairs.size()<64 && pairs.insert((std::uint64_t(frame)<<32)|tex).second)std::cout<<"upscale_pair frame="<<std::hex<<frame<<" texture="<<tex<<std::dec<<" size="<<texture->width()<<"x"<<texture->height()<<" through="<<through<<" hires="<<bool(hires)<<'\n';}
                  if(n[0]%5000==0)std::cout<<"upscale_trace textured="<<n[0]<<" through="<<n[1]<<" untokened="<<n[2]<<" hires="<<n[3]<<" scale="<<(gpu_raster_?gpu_raster_->internal_scale():0u)<<'\n';}}
                const auto hand_over=[&]{if(geo.encoded.size()==jobs.size()*kFixedJobWords)gpu_raster_->set_encoded_jobs(geo.encoded);
                    if(hires)gpu_raster_->set_hires_texture(*hires,hires_factor,texture_token);};
                try{hand_over();result=gpu_raster_->raster(gpu_color_scratch_,gpu_depth_scratch_,gpu_texels,jobs,params);}
                catch(const NativeGpuBatchFull&){
                    // Capacity is known before recording the new draw. Publish
                    // the earlier ordered group, then refresh the starting VRAM
                    // before retrying once; real GPU errors remain fatal.
                    flush_gpu_batch(4);color_buffer.read_words(gpu_color_scratch_,bytes);
                    if(depth_test)depth_buffer.read_words(gpu_depth_scratch_,2);
                    hand_over();result=gpu_raster_->raster(gpu_color_scratch_,gpu_depth_scratch_,gpu_texels,jobs,params);
                }
                if(result.deferred){gpu_batch_identity_=identity;gpu_batch_has_draws_=true;
                    if(zwrite){auto& z=gpu_batch_zrect_;z={std::min(z[0],x0),std::min(z[1],y0),std::max(z[2],x1),std::max(z[3],y1)};}gpu_batch_colors_bytes_=std::max(gpu_batch_colors_bytes_,color_buffer.size());gpu_batch_depth_bytes_=std::max(gpu_batch_depth_bytes_,depth_buffer.size());++draw_primitives;state.vertex_address+=count*vertex_stride;return true;}
                color_buffer.write_words(gpu_color_scratch_,bytes);
                depth_buffer.write_words(gpu_depth_scratch_,2);
                draw_covered_pixels+=result.covered;alpha_rejected_pixels+=result.alpha;depth_rejected_pixels+=result.depth;draw_pixels+=result.drawn;
                ++gpu_raster_primitives;gpu_raster_pixels+=result.drawn;++draw_primitives;state.vertex_address+=count*vertex_stride;return true;
                }
            }
        }
        if(prepared_raster_enabled_){
            auto& prepared=prepared_triangle_scratch_;prepared.clear();prepared.reserve(raster_triangles.size());
            for(const auto& triangle:raster_triangles){prepared.emplace_back();if(!prepare_triangle(triangle,prepared.back()))prepared.pop_back();}
        }
        const auto rasterize=[&]<bool FastShading,bool BoundedThroughUV>(const PreparedTriangle& prepared,int strip_top,int strip_bottom,Stats& result){
            const auto& a=prepared.vertices[0];const auto& b=prepared.vertices[1];const auto& c=prepared.vertices[2];
            const auto area=prepared.area;const auto constant_color=prepared.constant_color,constant_depth=prepared.constant_depth,linear=prepared.linear;
            const auto flat_color=prepared.flat_color;const auto& fixed=prepared.fixed;const int x0=prepared.left,x1=prepared.right,y0=std::max(strip_top,prepared.top),y1=std::min(strip_bottom,prepared.bottom);
            const auto step_a=prepared.step_a,step_b=prepared.step_b,step_c=prepared.step_c;const bool include_a=prepared.include_a,include_b=prepared.include_b,include_c=prepared.include_c;
            for(int y=y0;y<y1;++y){
                auto ea=edge(a,b,16*x0+8,16*y+8),eb=edge(b,c,16*x0+8,16*y+8),ec=edge(c,a,16*x0+8,16*y+8);
                // Integer edge increments preserve the exact coverage and weights;
                // shader arithmetic and texture quantization remain unchanged.
                for(int x=x0;x<x1;++x,ea+=step_a,eb+=step_b,ec+=step_c){
                if(ea<0 || eb<0 || ec<0 || (!ea&&!include_a) || (!eb&&!include_b) || (!ec&&!include_c))continue;
                ++result.covered;
                // Integer interpolation shared bit-for-bit with the GPU shader.
                const auto weights=fixed_weights(eb,ec,fixed.inverse);
                const auto z=static_cast<std::uint16_t>(constant_depth?fixed.z0:fixed_depth(fixed,weights));
                const auto zoffset=(static_cast<unsigned>(y)*zstride+x)*2u;
                // With alpha testing disabled, rejected depth cannot affect
                // color/stencil/depth or diagnostics. Avoid its texture work.
                if(early_depth && !compare(r[0xDE],z,depth_buffer.load16(zoffset))){++result.depth;continue;}
                std::uint32_t color=flat_color;if(!constant_color)color=fixed_color(fixed,weights);
                if(texture){int ux,vy;fixed_uv(fixed,weights,!through,ux,vy);color=texture->template combine<FastShading>(color,texture->sample_fixed(ux,vy,linear));}
                const auto alpha=color>>24u;
                if((r[0x22]&1u) && !compare(alpha_test,alpha&((alpha_test>>16u)&255u),(alpha_test>>8u)&255u)){++result.alpha;continue;}
                if(depth_test && !early_depth && !compare(r[0xDE],z,depth_buffer.load16(zoffset))){++result.depth;continue;}
                const auto offset=(static_cast<unsigned>(y)*stride+x)*bytes;const auto old=bytes==4?color_buffer.load32(offset):color_buffer.load16(offset);
                if(blend){if constexpr(FastShading)color=NativeBlend::apply(r[0xDF]&0xFFFFFFu,color,unpack(old,format),r[0xE0],r[0xE1]);else color=NativeBlend::apply_generic(r[0xDF]&0xFFFFFFu,color,unpack(old,format),r[0xE0],r[0xE1]);}
                const auto value=(old&keep)|(pack(color,format)&~keep);
                if(bytes==4)color_buffer.store32(offset,value);else color_buffer.store16(offset,static_cast<std::uint16_t>(value));
                if(zwrite)depth_buffer.store16(zoffset,z);++result.drawn;
                }
            }
        };
        // Through-mode attributes are finite and bounded before drawing. Their
        // covered barycentric interpolation is convex with Q=1, so the checked
        // texture-coordinate range cannot be exceeded. Projected/extreme UV
        // draws retain per-sample diagnostics.
        const auto raster_one=[&](const PreparedTriangle& triangle,int strip_top,int strip_bottom,Stats& result){
            if(fast_blend_enabled_){
                if(through && bounded_through_uv)rasterize.template operator()<true,true>(triangle,strip_top,strip_bottom,result);
                else rasterize.template operator()<true,false>(triangle,strip_top,strip_bottom,result);
            }else rasterize.template operator()<false,false>(triangle,strip_top,strip_bottom,result);
        };
        const auto draw_rows=[&](int strip_top,int strip_bottom,Stats& result){
            if(prepared_raster_enabled_){for(const auto& triangle:prepared_triangle_scratch_)if(triangle.top<strip_bottom && triangle.bottom>strip_top)raster_one(triangle,strip_top,strip_bottom,result);}
            else {for(const auto& triangle:raster_triangles){PreparedTriangle prepared;if(prepare_triangle(triangle,prepared))raster_one(prepared,strip_top,strip_bottom,result);}}
        };
        if(work_area>=parallel_threshold_){
            const auto lanes=raster_pool_.lanes();
            const auto first_row=balanced_rows_?work_top:top,last_row=balanced_rows_?work_bottom:bottom;
            // Partition only rows intersecting the validated geometry. Workers
            // still shade disjoint rows in original primitive order; the old
            // screen-wide partition remains available for exact A/B tests.
            raster_pool_.run([&](unsigned id){draw_rows(first_row+(last_row-first_row)*id/lanes,first_row+(last_row-first_row)*(id+1)/lanes,stats[id]);});
        }else draw_rows(top,bottom,stats[0]);
        for(const auto& result:stats){draw_covered_pixels+=result.covered;alpha_rejected_pixels+=result.alpha;depth_rejected_pixels+=result.depth;draw_pixels+=result.drawn;}
        state.vertex_address+=count*vertex_stride;++draw_primitives;return true;
    }
    static std::uint32_t pack(std::uint32_t color,unsigned format){
        const auto r=color&255u,g=(color>>8u)&255u,b=(color>>16u)&255u,a=color>>24u;
        if(format==0)return (r>>3u)|((g>>2u)<<5u)|((b>>3u)<<11u);
        if(format==1)return (r>>3u)|((g>>3u)<<5u)|((b>>3u)<<10u)|((a>>7u)<<15u);
        if(format==2)return (r>>4u)|((g>>4u)<<4u)|((b>>4u)<<8u)|((a>>4u)<<12u);
        return color;
    }
};
}
