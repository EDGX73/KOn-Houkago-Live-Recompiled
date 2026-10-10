#pragma once
#include <algorithm>
#include <array>
#include <cstdint>
#include <deque>
#include <span>
#include <stdexcept>
#include <vector>
namespace kon {
// Owned PCM only. Host locking serializes access. Copy ownership and real
// device playback have separate counters; neither is an audio audition.
class NativeAudioMixer {
public:
    struct Diagnostics {std::uint64_t submitted_frames=0,completed_frames=0,played_frames=0,empty_queue_submissions=0,gap_count=0,gap_microseconds=0,max_gap_microseconds=0;unsigned queued_buffers=0;};
    struct Buffer {std::vector<std::int32_t> samples;unsigned cursor=0;std::uint32_t token=0;};
    struct Channel {bool reserved=false;unsigned frames=0,format=0;int left=0,right=0;std::uint32_t next_token=1,completed=0,generation=0;std::uint64_t last_copy_us=0;std::deque<Buffer> queue;Diagnostics diagnostics;};
    std::array<Channel,8> channels;
    void reserve(unsigned id,unsigned frames,unsigned format){auto& c=channels.at(id);if(!c.queue.empty())throw std::runtime_error("Cannot reserve a channel with pending PCM");const auto generation=c.generation+1u;if(!generation)throw std::runtime_error("Audio channel generation exhausted");c=Channel{};c.reserved=true;c.frames=frames;c.format=format;c.generation=generation;}
    std::uint32_t submit(unsigned id,std::span<const std::int16_t> source,std::uint64_t now){
        auto& c=channels.at(id);const unsigned input_channels=c.format==0x10?1u:2u;
        if(source.size()!=static_cast<std::size_t>(c.frames)*input_channels)throw std::invalid_argument("Native audio PCM size does not match reserved channel");
        if(c.queue.size()>=kMaxQueued)throw std::runtime_error("Native audio queue is full; additional concurrent output requires a waiting writer");
        if(!c.next_token)throw std::runtime_error("Native audio completion token exhausted");
        Buffer b;b.token=c.next_token++;b.samples.resize(c.frames*2u);
        // Retain eight fractional gain bits across channel accumulation. Signed
        // shifts round negative values down, as the original mixer does.
        for(unsigned n=0;n<c.frames;++n){b.samples[n*2u]=static_cast<std::int32_t>((static_cast<std::int64_t>(source[n*input_channels])*c.left)>>7u);b.samples[n*2u+1u]=static_cast<std::int32_t>((static_cast<std::int64_t>(source[n*input_channels+input_channels-1u])*c.right)>>7u);}
        c.diagnostics.submitted_frames+=c.frames;
        if(c.queue.empty()){
            ++c.diagnostics.empty_queue_submissions;
            if(c.last_copy_us && now>c.last_copy_us){const auto gap=now-c.last_copy_us;++c.diagnostics.gap_count;c.diagnostics.gap_microseconds+=gap;c.diagnostics.max_gap_microseconds=std::max(c.diagnostics.max_gap_microseconds,gap);}
        }
        c.queue.push_back(std::move(b));
        // The token a blocking writer waits for. Originally the previous buffer
        // (one playing, one queued). With min_queued_frames, the writer runs
        // ahead until that many frames are queued after the buffer it waits
        // for: the PSP's audio thread runs the moment it may, while this
        // cooperative scheduler can wake it a few milliseconds late, and small
        // blocks (256 frames, 5.8 ms) then ran dry (audible gaps).
        if(!min_queued_frames){return c.queue.size()>=2?c.queue[c.queue.size()-2].token:0u;}
        std::uint64_t after=0;
        for(std::size_t n=c.queue.size();n-->0;){
            const auto& q=c.queue[n];
            if(after>=min_queued_frames || c.queue.size()-n>=kMaxQueued)return q.token;
            after+=q.samples.size()/2u-q.cursor;
        }
        return 0u;
    }
    static constexpr std::size_t kMaxQueued=16;
    std::uint64_t min_queued_frames=0;
    void mix(std::span<std::int16_t> output,std::uint64_t first_device_frame,std::uint64_t now){
        if(output.size()%2 || output.size()>128)throw std::invalid_argument("Stereo mixer span must contain at most 64 complete frames");
        if(first_device_frame<next_mix_frame_)throw std::runtime_error("Mixer device timeline moved backwards");
        const auto frames=output.size()/2;next_mix_frame_=first_device_frame+frames;std::array<std::int64_t,128> sums{};
        for(unsigned id=0;id<channels.size();++id){auto& c=channels[id];unsigned copied=0;
            while(copied<frames && !c.queue.empty()){
                auto& b=c.queue.front();const auto take=std::min<std::size_t>(frames-copied,b.samples.size()/2u-b.cursor);
                for(unsigned n=0;n<take*2u;++n)sums[copied*2u+n]+=b.samples[b.cursor*2u+n];
                segments_.push_back({first_device_frame+copied,static_cast<unsigned>(take),0,id,c.generation});last_source_end_=std::max(last_source_end_,first_device_frame+copied+take);
                b.cursor+=static_cast<unsigned>(take);copied+=static_cast<unsigned>(take);c.diagnostics.completed_frames+=take;
                if(b.cursor==b.samples.size()/2u){c.completed=b.token;c.last_copy_us=now;c.queue.pop_front();}
            }
        }
        for(unsigned n=0;n<output.size();++n)output[n]=static_cast<std::int16_t>(std::clamp(sums[n]>>8u,std::int64_t{-32768},std::int64_t{32767}));
    }
    void played(std::uint64_t position){
        if(position<device_played_)throw std::runtime_error("Native audio device clock moved backwards");device_played_=position;
        for(auto& segment:segments_){if(segment.first>=position)continue;const auto count=static_cast<unsigned>(std::min<std::uint64_t>(segment.frames,position-segment.first));
            if(channels[segment.channel].generation==segment.generation)channels[segment.channel].diagnostics.played_frames+=count-segment.accounted;
            segment.accounted=count;
        }
        while(!segments_.empty() && segments_.front().accounted==segments_.front().frames)segments_.pop_front();
    }
    std::uint32_t remaining(unsigned id)const{std::uint64_t remaining=0;for(const auto& b:channels.at(id).queue)remaining+=b.samples.size()/2u-b.cursor;return static_cast<std::uint32_t>(remaining);}
    bool pending()const{for(const auto& c:channels)if(!c.queue.empty())return true;return last_source_end_>device_played_;}
    Diagnostics diagnostics(unsigned id)const{auto d=channels.at(id).diagnostics;d.queued_buffers=static_cast<unsigned>(channels.at(id).queue.size());return d;}
private:
    struct Segment {std::uint64_t first;unsigned frames,accounted,channel,generation;};
    std::deque<Segment> segments_;std::uint64_t device_played_=0,last_source_end_=0,next_mix_frame_=0;
};
}
