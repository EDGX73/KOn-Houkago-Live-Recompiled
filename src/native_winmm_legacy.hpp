#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmsystem.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <chrono>
#include <deque>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>
namespace kon {
class NativeLegacyAudio {
public:
    static constexpr std::uint32_t busy=0x80260002u,invalid_channel=0x80260003u,not_reserved=0x80260008u;
    struct Output {std::uint32_t value,wait_token=0;};
    // Passive diagnostics. Gaps are measured from the actual WOM_DONE callback
    // to submission after an empty queue; they are not an audio audition or a
    // hardware latency measurement. Timestamps use the monotonic PC clock.
    struct Diagnostics {std::uint64_t submitted_frames=0,completed_frames=0,empty_queue_submissions=0,gap_count=0,gap_microseconds=0,max_gap_microseconds=0;unsigned queued_buffers=0;};
    Diagnostics diagnostics(unsigned channel)const{
        if(channel>=channels_.size())throw std::out_of_range("Audio diagnostic channel");
        auto out=channels_[channel].diagnostics;out.queued_buffers=static_cast<unsigned>(channels_[channel].queue.size());return out;
    }
    NativeLegacyAudio()=default;~NativeLegacyAudio(){for(auto& c:channels_)close(c,true);}
    NativeLegacyAudio(const NativeLegacyAudio&)=delete;NativeLegacyAudio& operator=(const NativeLegacyAudio&)=delete;
    std::uint32_t reserve(std::int32_t requested,unsigned frames,unsigned format){
        poll();if(requested<0){requested=-1;for(unsigned n=0;n<channels_.size();n++)if(!channels_[n].reserved && channels_[n].queue.empty()){requested=static_cast<std::int32_t>(n);break;}if(requested<0)return 0x80260005u;}
        if(static_cast<unsigned>(requested)>=channels_.size() || channels_[requested].reserved || !channels_[requested].queue.empty())return invalid_channel;
        if(!valid_frames(frames))return 0x80260006u;if(format!=0 && format!=0x10)return 0x80260007u;
        auto& c=channels_[requested];WAVEFORMATEX wave{};wave.wFormatTag=WAVE_FORMAT_PCM;wave.nChannels=2;wave.nSamplesPerSec=44100;wave.wBitsPerSample=16;wave.nBlockAlign=4;wave.nAvgBytesPerSec=176400;
        checked(waveOutOpen(&c.device,WAVE_MAPPER,&wave,reinterpret_cast<DWORD_PTR>(&completion),0,CALLBACK_FUNCTION),"Open Windows audio output");
        c.reserved=true;c.frames=frames;c.format=format;c.left=0;c.right=0;c.sent=0;c.position_high=0;c.previous_position=0;c.completed=0;c.next_token=1;c.diagnostics={};c.last_done_us=0;
        return static_cast<std::uint32_t>(requested);
    }
    std::uint32_t release(unsigned channel){
        if(channel>=channels_.size())return invalid_channel;auto& c=channels_[channel];if(!c.reserved)return not_reserved;
        c.reserved=false;poll();if(c.queue.empty())close(c,false);return 0;
    }
    std::uint32_t set_frames(unsigned channel,unsigned frames){
        if(channel>=channels_.size())return invalid_channel;if(!valid_frames(frames))return 0x80260006u;
        if(!channels_[channel].reserved)return 0x80260001u;channels_[channel].frames=frames;return 0;
    }
    std::uint32_t set_format(unsigned channel,unsigned format){
        if(channel>=channels_.size())return invalid_channel;if(!channels_[channel].reserved)return not_reserved;
        if(format!=0 && format!=0x10)return 0x80260007u;channels_[channel].format=format;return 0;
    }
    std::uint32_t set_volume(unsigned channel,std::int32_t left,std::int32_t right){
        if(channel>=channels_.size())return invalid_channel;if(left>0xFFFF || right>0xFFFF)return 0x8026000Bu;
        auto& c=channels_[channel];if(!c.reserved)return not_reserved;if(left>=0)c.left=left;if(right>=0)c.right=right;return 0;
    }
    unsigned sample_count(unsigned channel) const {return channel<channels_.size() && channels_[channel].reserved?channels_[channel].frames:0;}
    unsigned channel_format(unsigned channel) const {return channel<channels_.size()?channels_[channel].format:0;}
    Output output(unsigned channel,std::int32_t left,std::int32_t right,std::span<const std::int16_t> source,bool blocking){
        poll();const auto error=set_volume(channel,left,right);if(error)return {error};auto& c=channels_[channel];
        if(!blocking && !c.queue.empty())return {busy};if(c.queue.size()>=2)throw std::runtime_error("Native audio has two queued buffers; additional concurrent output requires a waiting writer");
        const unsigned input_channels=c.format==0x10?1u:2u;
        if(source.size()!=static_cast<std::size_t>(c.frames)*input_channels)throw std::invalid_argument("Native audio PCM size does not match reserved channel");
        auto buffer=std::make_unique<Buffer>();buffer->samples.resize(static_cast<std::size_t>(c.frames)*2u);buffer->token=c.next_token++;
        if(!buffer->token)throw std::runtime_error("Native audio completion token exhausted");
        for(unsigned n=0;n<c.frames;n++){
            buffer->samples[n*2u]=gain(source[n*input_channels],c.left);
            buffer->samples[n*2u+1u]=gain(source[n*input_channels+input_channels-1u],c.right);
        }
        buffer->header.lpData=reinterpret_cast<LPSTR>(buffer->samples.data());buffer->header.dwBufferLength=static_cast<DWORD>(buffer->samples.size()*2u);buffer->header.dwUser=reinterpret_cast<DWORD_PTR>(buffer.get());
        checked(waveOutPrepareHeader(c.device,&buffer->header,sizeof(WAVEHDR)),"Prepare original PCM output");
        const auto previous=c.queue.empty()?0u:c.queue.back()->token;
        const auto submitted_us=monotonic_us();
        const auto result=waveOutWrite(c.device,&buffer->header,sizeof(WAVEHDR));
        if(result!=MMSYSERR_NOERROR){waveOutUnprepareHeader(c.device,&buffer->header,sizeof(WAVEHDR));checked(result,"Submit original PCM output");}
        c.sent+=c.frames;c.diagnostics.submitted_frames+=c.frames;
        if(c.queue.empty()){
            ++c.diagnostics.empty_queue_submissions;
            if(c.last_done_us && submitted_us>c.last_done_us){
                const auto gap=submitted_us-c.last_done_us;
                ++c.diagnostics.gap_count;c.diagnostics.gap_microseconds+=gap;
                c.diagnostics.max_gap_microseconds=std::max(c.diagnostics.max_gap_microseconds,gap);
            }
        }
        c.queue.push_back(std::move(buffer));return {c.frames,blocking?previous:0u};
    }
    void poll(){
        for(auto& c:channels_){
            while(!c.queue.empty() && c.queue.front()->done.load(std::memory_order_acquire)){
                const auto& first=c.queue.front();checked(waveOutUnprepareHeader(c.device,&first->header,sizeof(WAVEHDR)),"Retire original PCM buffer");
                c.completed=first->token;c.last_done_us=first->done_us.load(std::memory_order_relaxed);
                c.diagnostics.completed_frames+=first->samples.size()/2u;c.queue.pop_front();
            }
            if(c.device && !c.reserved && c.queue.empty())close(c,false);
        }
    }
    bool completed(unsigned channel,std::uint32_t token){poll();return channel<channels_.size() && channels_[channel].completed>=token;}
    bool has_pending() const {for(const auto& c:channels_)if(!c.queue.empty())return true;return false;}
    std::uint32_t rest_frames(unsigned channel){
        poll();if(channel>=channels_.size())return invalid_channel;auto& c=channels_[channel];if(!c.reserved)return not_reserved;
        MMTIME time{};time.wType=TIME_SAMPLES;checked(waveOutGetPosition(c.device,&time,sizeof(time)),"Read real audio position");
        if(time.wType!=TIME_SAMPLES)throw std::runtime_error("Windows audio driver did not provide sample position");
        const auto current=time.u.sample;if(current<c.previous_position)c.position_high+=0x100000000ull;c.previous_position=current;
        const auto played=c.position_high+current;return static_cast<std::uint32_t>(c.sent>played?c.sent-played:0u);
    }
private:
    struct Buffer {WAVEHDR header{};std::vector<std::int16_t> samples;std::atomic<bool> done{false};std::atomic<std::uint64_t> done_us{0};std::uint32_t token=0;};
    struct Channel {HWAVEOUT device=nullptr;bool reserved=false;unsigned frames=0,format=0;std::int32_t left=0,right=0;std::deque<std::unique_ptr<Buffer>> queue;std::uint32_t next_token=1,completed=0,previous_position=0;std::uint64_t sent=0,position_high=0,last_done_us=0;Diagnostics diagnostics;};
    std::array<Channel,8> channels_;
    static std::uint64_t monotonic_us(){return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());}
    static bool valid_frames(unsigned frames){return frames>0 && frames<=65472 && !(frames&63u);}
    static std::int16_t gain(std::int16_t sample,std::int32_t volume){return static_cast<std::int16_t>(std::clamp(static_cast<std::int32_t>(sample)*volume/0x8000,-32768,32767));}
    static void CALLBACK completion(HWAVEOUT,UINT message,DWORD_PTR,DWORD_PTR first,DWORD_PTR){
        if(message==WOM_DONE){auto* header=reinterpret_cast<WAVEHDR*>(first);auto* buffer=reinterpret_cast<Buffer*>(header->dwUser);buffer->done_us.store(monotonic_us(),std::memory_order_relaxed);buffer->done.store(true,std::memory_order_release);}
    }
    static void checked(MMRESULT error,const char* operation){if(error!=MMSYSERR_NOERROR)throw std::runtime_error(std::string(operation)+" Windows audio error="+std::to_string(error));}
    static void close(Channel& c,bool reset){
        if(!c.device)return;if(reset)waveOutReset(c.device);
        for(auto& b:c.queue)waveOutUnprepareHeader(c.device,&b->header,sizeof(WAVEHDR));c.queue.clear();waveOutClose(c.device);c.device=nullptr;
    }
};
}
