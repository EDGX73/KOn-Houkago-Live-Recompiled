#pragma once
// PCM output (platform layer): one continuous 44100 Hz stereo 16-bit stream.
// Buffers are queued in order; the device's own sample clock reports how many
// frames have actually been played. Windows: waveOut. Elsewhere, until AAudio
// (Android) is added: a silent device that plays in real time, so headless runs
// keep the game's audio clock moving exactly as a device would.
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <deque>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <mmsystem.h>
#include <atomic>
#endif
namespace kon::platform {
#ifdef _WIN32
class PcmOutput {
public:
    PcmOutput()=default;
    ~PcmOutput(){if(device_)waveOutClose(device_);}
    PcmOutput(const PcmOutput&)=delete;PcmOutput& operator=(const PcmOutput&)=delete;
    bool is_open()const{return device_!=nullptr;}
    void open(){
        WAVEFORMATEX wave{};wave.wFormatTag=WAVE_FORMAT_PCM;wave.nChannels=2;wave.nSamplesPerSec=44100;wave.wBitsPerSample=16;wave.nBlockAlign=4;wave.nAvgBytesPerSec=176400;
        checked(waveOutOpen(&device_,WAVE_MAPPER,&wave,reinterpret_cast<DWORD_PTR>(&completion),0,CALLBACK_FUNCTION),"Open continuous Windows audio");
    }
    // Copies the interleaved samples into a new device buffer and queues it.
    void submit(std::span<const std::int16_t> samples){
        auto b=std::make_unique<Buffer>();b->samples.assign(samples.begin(),samples.end());
        b->header.lpData=reinterpret_cast<LPSTR>(b->samples.data());b->header.dwBufferLength=b->samples.size()*2u;b->header.dwUser=reinterpret_cast<DWORD_PTR>(b.get());
        checked(waveOutPrepareHeader(device_,&b->header,sizeof(WAVEHDR)),"Prepare continuous PCM buffer");const auto result=waveOutWrite(device_,&b->header,sizeof(WAVEHDR));
        if(result!=MMSYSERR_NOERROR){waveOutUnprepareHeader(device_,&b->header,sizeof(WAVEHDR));checked(result,"Submit continuous PCM buffer");}
        submitted_+=samples.size()/2u;queue_.push_back(std::move(b));
    }
    // Releases the buffers the device has finished.
    void retire(){while(!queue_.empty() && queue_.front()->done.load(std::memory_order_acquire)){checked(waveOutUnprepareHeader(device_,&queue_.front()->header,sizeof(WAVEHDR)),"Retire continuous PCM buffer");queue_.pop_front();}}
    std::size_t queued()const{return queue_.size();}
    // Frames played according to the driver's sample clock (64-bit, monotonic).
    std::uint64_t played_frames(){
        MMTIME time{};time.wType=TIME_SAMPLES;checked(waveOutGetPosition(device_,&time,sizeof(time)),"Read continuous audio sample clock");
        if(time.wType!=TIME_SAMPLES)throw std::runtime_error("Audio driver did not provide its sample clock");
        const auto current=time.u.sample;if(current<previous_position_)position_high_+=0x100000000ull;previous_position_=current;
        const auto played=position_high_+current;if(played>submitted_)throw std::runtime_error("Audio driver played beyond submitted PCM");
        return played;
    }
    // Stops playback; buffers stay alive until the driver has relinquished them.
    void stop(){waveOutReset(device_);for(auto& b:queue_)waveOutUnprepareHeader(device_,&b->header,sizeof(WAVEHDR));}
private:
    struct Buffer {WAVEHDR header{};std::vector<std::int16_t> samples;std::atomic<bool> done{false};};
    HWAVEOUT device_=nullptr;std::deque<std::unique_ptr<Buffer>> queue_;
    std::uint64_t submitted_=0,position_high_=0;std::uint32_t previous_position_=0;
    static void checked(MMRESULT error,const char* operation){if(error!=MMSYSERR_NOERROR)throw std::runtime_error(std::string(operation)+" Windows audio error="+std::to_string(error));}
    static void CALLBACK completion(HWAVEOUT,UINT message,DWORD_PTR,DWORD_PTR first,DWORD_PTR){if(message==WOM_DONE){auto* h=reinterpret_cast<WAVEHDR*>(first);reinterpret_cast<Buffer*>(h->dwUser)->done.store(true,std::memory_order_release);}}
};
#elif defined(__ANDROID__)
// AAudio output: one blocking stereo 44100 Hz stream. NativeAudio submits a
// block and the write call paces it; frames played come from the device
// timestamp, so the game's audio clock is the real hardware clock.
#include <aaudio/AAudio.h>
#include <time.h>
class PcmOutput {
public:
    PcmOutput()=default;
    ~PcmOutput(){if(stream_)AAudioStream_close(stream_);}
    PcmOutput(const PcmOutput&)=delete;PcmOutput& operator=(const PcmOutput&)=delete;
    bool is_open()const{return stream_!=nullptr || fallback_;}
    void open(){
        AAudioStreamBuilder* builder=nullptr;
        if(AAudio_createStreamBuilder(&builder)!=AAUDIO_OK || !builder)throw std::runtime_error("AAudio builder");
        AAudioStreamBuilder_setDirection(builder,AAUDIO_DIRECTION_OUTPUT);
        AAudioStreamBuilder_setPerformanceMode(builder,AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
        AAudioStreamBuilder_setSharingMode(builder,AAUDIO_SHARING_MODE_SHARED);
        AAudioStreamBuilder_setFormat(builder,AAUDIO_FORMAT_PCM_I16);
        AAudioStreamBuilder_setChannelCount(builder,2);
        AAudioStreamBuilder_setSampleRate(builder,44100);
        AAudioStreamBuilder_setBufferCapacityInFrames(builder,2048);
        const auto result=AAudioStreamBuilder_openStream(builder,&stream_);
        AAudioStreamBuilder_delete(builder);
        if(result!=AAUDIO_OK || !stream_){fallback_=true;open_=true;return;}
        if(AAudioStream_requestStart(stream_)!=AAUDIO_OK){fallback_=true;return;}
    }
    void submit(std::span<const std::int16_t> samples){
        if(fallback_){
            const auto frames=samples.size()/2u;
            std::this_thread::sleep_for(std::chrono::microseconds(frames*1000000u/44100u));
            submitted_+=frames;
            return;
        }
        if(!stream_)throw std::runtime_error("AAudio output is not open");
        const auto frames=static_cast<std::int32_t>(samples.size()/2u);
        std::int32_t written=0;
        while(written<frames){
            const auto n=AAudioStream_write(stream_,samples.data()+std::size_t(written)*2u,frames-written,100000000);
            if(n<0)throw std::runtime_error("Android AAudio write failed");
            if(!n)continue;
            written+=n;
        }
        submitted_+=std::uint64_t(frames);
    }
    void retire(){}
    std::size_t queued()const{return 0;}
    std::uint64_t played_frames(){
        if(fallback_)return submitted_;
        if(!stream_)return 0;
        int64_t position=0,nanoseconds=0;
        if(AAudioStream_getTimestamp(stream_,CLOCK_MONOTONIC,&position,&nanoseconds)!=AAUDIO_OK || position<0)return 0;
        return std::uint64_t(position);
    }
    void stop(){if(stream_)AAudioStream_requestStop(stream_);}
private:
    AAudioStream* stream_=nullptr;
    std::uint64_t submitted_=0;
    bool fallback_=false,open_=false;
};
#else
class PcmOutput {
public:
    bool is_open()const{return open_;}
    void open(){open_=true;}
    void submit(std::span<const std::int16_t> samples){
        advance();submitted_+=samples.size()/2u;ends_.push_back(submitted_);
        if(!running_){running_=true;last_=Clock::now();}
    }
    void retire(){advance();while(!ends_.empty() && ends_.front()<=played_)ends_.pop_front();}
    std::size_t queued()const{return ends_.size();}
    std::uint64_t played_frames(){advance();return played_;}
    void stop(){ends_.clear();running_=false;}
private:
    using Clock=std::chrono::steady_clock;
    // Real-time consumption; like a device, the clock stops when it runs dry
    // and resumes with the next buffer.
    void advance(){
        if(!running_)return;
        const auto now=Clock::now();
        const auto due=static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(now-last_).count())*44100u/1000000u;
        if(!due)return;
        played_=std::min(submitted_,played_+due);
        last_+=std::chrono::microseconds(due*1000000u/44100u);
        if(played_==submitted_){running_=false;}
    }
    bool open_=false,running_=false;std::uint64_t submitted_=0,played_=0;std::deque<std::uint64_t> ends_;Clock::time_point last_{};
};
#endif
}
