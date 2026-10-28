#pragma once
#include <cstdlib>
#include "platform/audio.hpp"
#include "native_audio_mixer.hpp"
#include "native_wait.hpp"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <exception>
#include <memory>
#include <mutex>
#include <thread>
#include <string>
#include <vector>
namespace kon {
// One continuous host stream (platform/audio.hpp). The native mixer owns PCM, copies channels
// at 44100 Hz, and reports completed copy tokens independently of DAC playback.
// No guest memory or recompiled functions execute on the audio worker.
class NativeAudio {
public:
    static constexpr std::uint32_t busy=0x80260002u,invalid_channel=0x80260003u,not_reserved=0x80260008u;
    struct Output {std::uint32_t value,wait_token=0;};
    using Diagnostics=NativeAudioMixer::Diagnostics;
    // Silent test runs: the device and its sample clock run as usual, but only zeros are played.
    static bool& muted(){static bool value=false;return value;}
    // Host block size in frames (multiple of 64, 256..4096). Two blocks are
    // kept ahead of the device, so output latency is about two blocks:
    // config/settings.ini audio_block_ms or KON_AUDIO_BLOCK_FRAMES; set before
    // the first channel is reserved.
    static unsigned& block_frames(){static unsigned value=[]{const char* f=std::getenv("KON_AUDIO_BLOCK_FRAMES");return f?clamp_block(unsigned(std::strtoul(f,nullptr,10))):kDefaultBlockFrames;}();return value;}
    // 1024 frames: ~46 ms (was 2048, ~93 ms). Full songs at 165 FPS gave no
    // device underruns with 2048, 1024 or 512 frames on the Radeon 780M laptop.
    static constexpr unsigned kDefaultBlockFrames=1024;
    static unsigned clamp_block(unsigned frames){return std::clamp((frames+32u)/64u*64u,256u,4096u);}
    // Times the device had played everything sent (an audible dropout).
    std::uint64_t device_underruns()const{return underruns_.load(std::memory_order_relaxed);}
    // Blocking writers may queue about 24 ms ahead (KON_AUDIO_QUEUE_MS; 0 keeps
    // the original one-buffer-ahead behaviour).
    NativeAudio(){const char* ms=std::getenv("KON_AUDIO_QUEUE_MS");mixer_.min_queued_frames=std::uint64_t((ms?std::stod(ms):24.0)*44.1);}
    ~NativeAudio(){stopping_.store(true);if(worker_.joinable())worker_.join();}  // then device_ closes
    NativeAudio(const NativeAudio&)=delete;NativeAudio& operator=(const NativeAudio&)=delete;
    std::uint32_t reserve(std::int32_t requested,unsigned frames,unsigned format){
        std::lock_guard lock(mutex_);check_failure();
        if(requested<0){requested=-1;for(unsigned n=0;n<8;++n)if(!mixer_.channels[n].reserved && mixer_.channels[n].queue.empty()){requested=n;break;}if(requested<0)return 0x80260005u;}
        if(static_cast<unsigned>(requested)>=8 || mixer_.channels[requested].reserved || !mixer_.channels[requested].queue.empty())return invalid_channel;
        if(!valid_frames(frames))return 0x80260006u;if(format!=0 && format!=0x10)return 0x80260007u;
        mixer_.reserve(requested,frames,format);
        if(!device_.is_open()){
            device_.open();
            worker_=std::thread([this]{run();});
        }
        return requested;
    }
    std::uint32_t release(unsigned channel){std::lock_guard lock(mutex_);check_failure();if(channel>=8)return invalid_channel;auto& c=mixer_.channels[channel];if(!c.reserved)return not_reserved;c.reserved=false;return 0;}
    std::uint32_t set_frames(unsigned channel,unsigned frames){std::lock_guard lock(mutex_);check_failure();if(channel>=8)return invalid_channel;if(!valid_frames(frames))return 0x80260006u;if(!mixer_.channels[channel].reserved)return 0x80260001u;mixer_.channels[channel].frames=frames;return 0;}
    std::uint32_t set_format(unsigned channel,unsigned format){std::lock_guard lock(mutex_);check_failure();if(channel>=8)return invalid_channel;if(!mixer_.channels[channel].reserved)return not_reserved;if(format!=0 && format!=0x10)return 0x80260007u;mixer_.channels[channel].format=format;return 0;}
    std::uint32_t set_volume(unsigned channel,std::int32_t left,std::int32_t right){std::lock_guard lock(mutex_);check_failure();return volume(channel,left,right);}
    unsigned sample_count(unsigned channel)const{std::lock_guard lock(mutex_);check_failure();return channel<8 && mixer_.channels[channel].reserved?mixer_.channels[channel].frames:0;}
    unsigned channel_format(unsigned channel)const{std::lock_guard lock(mutex_);check_failure();return channel<8?mixer_.channels[channel].format:0;}
    Output output(unsigned channel,std::int32_t left,std::int32_t right,std::span<const std::int16_t> source,bool blocking){
        std::lock_guard lock(mutex_);check_failure();const auto error=volume(channel,left,right);if(error)return {error};auto& c=mixer_.channels[channel];
        if(!blocking && !c.queue.empty())return {busy};const auto previous=mixer_.submit(channel,source,now_us());return {c.frames,blocking?previous:0u};
    }
    void poll(){std::lock_guard lock(mutex_);check_failure();}
    bool completed(unsigned channel,std::uint32_t token){std::lock_guard lock(mutex_);check_failure();return channel<8 && mixer_.channels[channel].completed>=token;}
    bool has_pending()const{std::lock_guard lock(mutex_);check_failure();return mixer_.pending();}
    std::uint32_t rest_frames(unsigned channel){std::lock_guard lock(mutex_);check_failure();if(channel>=8)return invalid_channel;if(!mixer_.channels[channel].reserved)return not_reserved;return mixer_.remaining(channel);}
    Diagnostics diagnostics(unsigned channel)const{std::lock_guard lock(mutex_);check_failure();return mixer_.diagnostics(channel);}
private:
    mutable std::mutex mutex_;NativeAudioMixer mixer_;platform::PcmOutput device_;std::thread worker_;std::atomic<bool> stopping_{false};std::exception_ptr failure_;std::atomic<std::uint64_t> underruns_{0};
    static std::uint64_t now_us(){return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());}
    static bool valid_frames(unsigned n){return n>0 && n<=65472 && !(n&63u);}
    void check_failure()const{if(failure_)std::rethrow_exception(failure_);}
    std::uint32_t volume(unsigned channel,int left,int right){if(channel>=8)return invalid_channel;if(left>0xFFFF || right>0xFFFF)return 0x8026000Bu;auto& c=mixer_.channels[channel];if(!c.reserved)return not_reserved;if(left>=0)c.left=left;if(right>=0)c.right=right;return 0;}
    void run(){
        std::vector<std::int16_t> silence;
        const auto retire=[&]{device_.retire();};
        const auto send=[&](std::span<const std::int16_t> samples){
            if(muted()){silence.assign(samples.size(),0);device_.submit(silence);}else device_.submit(samples);
        };
        const auto playback=[&]{const auto played=device_.played_frames();std::lock_guard lock(mutex_);mixer_.played(played);};
        try{
            NativeWait wait;const unsigned block=block_frames();std::vector<std::int16_t> staging(block*2u,0);
            // Prime two host blocks (output latency ~2 blocks: 46 ms at 1024
            // frames); original PCM keeps its exact sample order.
            send(staging);send(staging);std::uint64_t timeline=2u*block,steps=0;unsigned cursor=0;const auto start=now_us();
            while(!stopping_.load()){
                retire();playback();const auto next=start+(steps+1u)*64000000ull/44100u;const auto now=now_us();
                if(now<next){wait.wait_for(std::min<std::uint64_t>(next-now,1000u));continue;}
                {std::lock_guard lock(mutex_);mixer_.mix(std::span(staging).subspan(cursor*2u,128u),timeline+cursor,now);}
                cursor+=64;++steps;
                if(cursor==block){
                    while(device_.queued()>=4 && !stopping_.load()){wait.wait_for(500);retire();playback();}
                    if(stopping_.load())break;retire();if(!device_.queued())underruns_.fetch_add(1,std::memory_order_relaxed);
                    send(staging);timeline+=block;cursor=0;
                }
            }
        }catch(...){std::lock_guard lock(mutex_);failure_=std::current_exception();}
        // Buffers remain alive until the real driver has relinquished them.
        device_.stop();
    }
};
}
