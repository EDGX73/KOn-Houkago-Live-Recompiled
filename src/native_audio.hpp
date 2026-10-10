#pragma once
#include <cstdlib>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <mmsystem.h>
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
// One continuous Windows stream. The native mixer owns PCM, copies channels
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
    ~NativeAudio(){stopping_.store(true);if(worker_.joinable())worker_.join();if(device_)waveOutClose(device_);}
    NativeAudio(const NativeAudio&)=delete;NativeAudio& operator=(const NativeAudio&)=delete;
    std::uint32_t reserve(std::int32_t requested,unsigned frames,unsigned format){
        std::lock_guard lock(mutex_);check_failure();
        if(requested<0){requested=-1;for(unsigned n=0;n<8;++n)if(!mixer_.channels[n].reserved && mixer_.channels[n].queue.empty()){requested=n;break;}if(requested<0)return 0x80260005u;}
        if(static_cast<unsigned>(requested)>=8 || mixer_.channels[requested].reserved || !mixer_.channels[requested].queue.empty())return invalid_channel;
        if(!valid_frames(frames))return 0x80260006u;if(format!=0 && format!=0x10)return 0x80260007u;
        mixer_.reserve(requested,frames,format);
        if(!device_){
            WAVEFORMATEX wave{};wave.wFormatTag=WAVE_FORMAT_PCM;wave.nChannels=2;wave.nSamplesPerSec=44100;wave.wBitsPerSample=16;wave.nBlockAlign=4;wave.nAvgBytesPerSec=176400;
            checked(waveOutOpen(&device_,WAVE_MAPPER,&wave,reinterpret_cast<DWORD_PTR>(&completion),0,CALLBACK_FUNCTION),"Open continuous Windows audio");
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
    struct DeviceBuffer {WAVEHDR header{};std::vector<std::int16_t> samples;std::atomic<bool> done{false};};
    mutable std::mutex mutex_;NativeAudioMixer mixer_;HWAVEOUT device_=nullptr;std::thread worker_;std::atomic<bool> stopping_{false};std::exception_ptr failure_;std::atomic<std::uint64_t> underruns_{0};
    static std::uint64_t now_us(){return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());}
    static bool valid_frames(unsigned n){return n>0 && n<=65472 && !(n&63u);}
    void check_failure()const{if(failure_)std::rethrow_exception(failure_);}
    std::uint32_t volume(unsigned channel,int left,int right){if(channel>=8)return invalid_channel;if(left>0xFFFF || right>0xFFFF)return 0x8026000Bu;auto& c=mixer_.channels[channel];if(!c.reserved)return not_reserved;if(left>=0)c.left=left;if(right>=0)c.right=right;return 0;}
    static void checked(MMRESULT error,const char* operation){if(error!=MMSYSERR_NOERROR)throw std::runtime_error(std::string(operation)+" Windows audio error="+std::to_string(error));}
    static void CALLBACK completion(HWAVEOUT,UINT message,DWORD_PTR,DWORD_PTR first,DWORD_PTR){if(message==WOM_DONE){auto* h=reinterpret_cast<WAVEHDR*>(first);reinterpret_cast<DeviceBuffer*>(h->dwUser)->done.store(true,std::memory_order_release);}}
    void run(){
        std::deque<std::unique_ptr<DeviceBuffer>> queue;std::uint64_t submitted=0,position_high=0;std::uint32_t previous_position=0;
        const auto retire=[&]{while(!queue.empty() && queue.front()->done.load(std::memory_order_acquire)){checked(waveOutUnprepareHeader(device_,&queue.front()->header,sizeof(WAVEHDR)),"Retire continuous PCM buffer");queue.pop_front();}};
        const auto send=[&](std::span<const std::int16_t> samples){
            auto b=std::make_unique<DeviceBuffer>();if(muted())b->samples.assign(samples.size(),0);else b->samples.assign(samples.begin(),samples.end());b->header.lpData=reinterpret_cast<LPSTR>(b->samples.data());b->header.dwBufferLength=b->samples.size()*2u;b->header.dwUser=reinterpret_cast<DWORD_PTR>(b.get());
            checked(waveOutPrepareHeader(device_,&b->header,sizeof(WAVEHDR)),"Prepare continuous PCM buffer");const auto result=waveOutWrite(device_,&b->header,sizeof(WAVEHDR));
            if(result!=MMSYSERR_NOERROR){waveOutUnprepareHeader(device_,&b->header,sizeof(WAVEHDR));checked(result,"Submit continuous PCM buffer");}
            submitted+=samples.size()/2u;queue.push_back(std::move(b));
        };
        const auto playback=[&]{MMTIME time{};time.wType=TIME_SAMPLES;checked(waveOutGetPosition(device_,&time,sizeof(time)),"Read continuous audio sample clock");if(time.wType!=TIME_SAMPLES)throw std::runtime_error("Audio driver did not provide its sample clock");const auto current=time.u.sample;if(current<previous_position)position_high+=0x100000000ull;previous_position=current;const auto played=position_high+current;if(played>submitted)throw std::runtime_error("Audio driver played beyond submitted PCM");std::lock_guard lock(mutex_);mixer_.played(played);};
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
                    while(queue.size()>=4 && !stopping_.load()){wait.wait_for(500);retire();playback();}
                    if(stopping_.load())break;retire();if(queue.empty())underruns_.fetch_add(1,std::memory_order_relaxed);
                    send(staging);timeline+=block;cursor=0;
                }
            }
        }catch(...){std::lock_guard lock(mutex_);failure_=std::current_exception();}
        // Buffers remain alive until the real driver has relinquished them.
        waveOutReset(device_);for(auto& b:queue)waveOutUnprepareHeader(device_,&b->header,sizeof(WAVEHDR));
    }
};
}
