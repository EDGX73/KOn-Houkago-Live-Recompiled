#pragma once
#include "atrac_wave.hpp"
#include "native_media.hpp"
#include "psprecomp/guest_memory.hpp"
#include <algorithm>
#include <map>
namespace kon {
class NativeAtrac {
public:
    struct Stream {AtracWave wave;std::uint32_t buffer=0,capacity=0,received=0,consumed=0,position=0,skip=0;std::int32_t loops=0;std::uint32_t origin=0,ring_start=0,prefetched=0,preroll=0;std::uint64_t loop_restarts=0;};
    struct Refill {std::uint32_t address,bytes,offset;};
    struct ResetInfo {std::uint32_t result=0,address=0,bytes=0,minimum=0,offset=0;};
    struct Decoded {std::uint32_t result=0,frames=0,finish=0;std::int32_t remains=0;std::vector<std::int16_t> pcm;};
    std::map<std::uint32_t,Stream> streams;
    NativeAtrac(psprecomp::GuestMemory& memory,NativeMedia& media):memory_(memory),media_(media){}
    std::uint32_t set_data(std::uint32_t buffer,std::uint32_t bytes){
        if(bytes<12)return 0x80630011u;
        if((buffer&3u) || !memory_.contains(buffer,bytes))return 0x80630014u;
        std::vector<std::uint8_t> header(std::min(bytes,65536u));for(unsigned n=0;n<header.size();++n)header[n]=memory_.load8(buffer+n);
        const auto wave=AtracWave::parse(header);
        if(wave.data_offset+std::uint64_t(wave.block_bytes)*((wave.first_sample/2048)+1)>bytes)return 0x80630011u;
        const auto id=media_.bind_atrac3plus(wave.channels,wave.rate,wave.block_bytes);if(id&0x80000000u)return id;
        Stream stream{wave,buffer,bytes,std::min(bytes,wave.file_bytes),wave.data_offset,0,wave.first_sample%2048};
        try{
            // Decode preceding packets into decoder history, discard their PCM.
            for(unsigned n=0;n<wave.first_sample/2048;++n){media_.decode_atrac_packet(id,packet(stream));stream.consumed+=wave.block_bytes;}
        }catch(...){media_.release_atrac(id);throw;}
        streams.emplace(id,stream);return id;
    }
    std::uint32_t release(std::uint32_t id){if(!streams.contains(id))return 0x80630005u;streams.erase(id);return media_.release_atrac(id);}
    Refill refill(std::uint32_t id)const{
        const auto& s=streams.at(id);
        if(s.capacity>=s.wave.file_bytes)return {s.buffer,0,0};
        const bool loop_refill=s.received>=s.wave.file_bytes && s.loops && s.wave.loop_end>=0;
        if(s.received>=s.wave.file_bytes && !loop_refill)return {s.buffer,0,0};
        if(loop_refill && static_cast<unsigned>(s.wave.loop_end)+1u!=s.wave.samples)throw std::runtime_error("ATRAC streamed loop with a tail requires second-buffer implementation");
        const auto used=s.received-s.consumed+s.prefetched,write=(s.ring_start+s.received-s.origin+s.prefetched)%s.capacity;
        const auto offset=loop_refill?seek_offset(s,static_cast<unsigned>(s.wave.loop_start))+s.prefetched:s.received;
        return {s.buffer+write,std::min({s.capacity-used,s.capacity-write,s.wave.file_bytes-offset}),offset};
    }
    std::uint32_t add(std::uint32_t id,std::uint32_t bytes){auto& s=streams.at(id);
        if(s.received>=s.wave.file_bytes && !s.loops)return 0x80630009u;
        if(bytes>refill(id).bytes)return 0x80630018u;
        if(s.received>=s.wave.file_bytes)s.prefetched+=bytes;else s.received+=bytes;return 0;
    }
    std::uint32_t set_loops(std::uint32_t id,std::int32_t count){
        if(!streams.contains(id))return 0x80630005u;auto& s=streams.at(id);
        if(s.wave.loop_start<0)return 0x80630021u;
        if(count && s.capacity<s.wave.file_bytes && static_cast<unsigned>(s.wave.loop_end)+1u!=s.wave.samples)throw std::runtime_error("ATRAC streamed loop with a tail requires second-buffer implementation");
        s.loops=count;if(!count)s.prefetched=0;return 0;
    }
    ResetInfo reset_info(std::uint32_t id,std::uint32_t sample)const{
        if(!streams.contains(id))return {0x80630005u};const auto& s=streams.at(id);
        if(sample>=s.wave.samples)return {0x80630015u};
        if(s.wave.loop_end>=0 && static_cast<unsigned>(s.wave.loop_end)+1!=s.wave.samples && s.capacity<s.wave.file_bytes)return {0x80630012u};
        if(s.capacity>=s.wave.file_bytes)return {0,s.buffer,0,0,0};
        const auto absolute=s.wave.first_sample+sample,frame=absolute/2048;
        const auto preceding=std::min(frame,(absolute%2048<368)?2u:1u);
        const auto offset=s.wave.data_offset+(frame-preceding)*s.wave.block_bytes;
        return {0,s.buffer,std::min(s.capacity/s.wave.block_bytes*s.wave.block_bytes,s.wave.file_bytes-offset),(preceding+1)*s.wave.block_bytes,offset};
    }
    std::uint32_t reset(std::uint32_t id,std::uint32_t sample,std::uint32_t bytes_first,std::uint32_t bytes_second){
        const auto info=reset_info(id,sample);if(info.result)return info.result;
        if(bytes_first<info.minimum || bytes_first>info.bytes)return 0x80630016u;
        if(bytes_second)return 0x80630017u;
        auto& s=streams.at(id);const auto absolute=s.wave.first_sample+sample,frame=absolute/2048;
        const auto preceding=std::min(frame,(absolute%2048<368)?2u:1u);
        const auto offset=s.wave.data_offset+(frame-preceding)*s.wave.block_bytes;
        media_.restart_atrac(id);s.consumed=offset;s.position=sample;s.skip=absolute%2048;s.loops=0;s.prefetched=s.preroll=0;
        if(s.capacity<s.wave.file_bytes){s.origin=offset;s.ring_start=0;s.received=offset+bytes_first;}
        for(unsigned n=0;n<preceding;++n){media_.decode_atrac_packet(id,packet(s));s.consumed+=s.wave.block_bytes;}
        return 0;
    }
    std::uint32_t next_samples(std::uint32_t id)const{const auto& s=streams.at(id);
        auto count=std::min(2048u-s.skip,s.wave.samples-s.position);
        if(s.loops && s.wave.loop_end>=0 && s.position<=static_cast<unsigned>(s.wave.loop_end))count=std::min(count,static_cast<unsigned>(s.wave.loop_end)+1u-s.position);return count;
    }
    std::int32_t remaining(std::uint32_t id)const{const auto& s=streams.at(id);if(s.capacity>=s.wave.file_bytes)return -1;
        return std::max(0,static_cast<int>((s.received-s.consumed+s.prefetched)/s.wave.block_bytes)-static_cast<int>(s.preroll));
    }
    Decoded decode(std::uint32_t id){
        auto& s=streams.at(id);Decoded result;
        if(s.position>=s.wave.samples){result.result=0x80630024u;result.finish=1;return result;}
        if(s.received-s.consumed<(s.preroll+1u)*s.wave.block_bytes){result.result=0x80630023u;return result;}
        while(s.preroll){media_.decode_atrac_packet(id,packet(s));s.consumed+=s.wave.block_bytes;--s.preroll;}
        const auto pcm=media_.decode_atrac_packet(id,packet(s));
        if(pcm.size()!=4096)throw std::runtime_error("ATRAC3+ packet produced unexpected frame length");
        result.frames=next_samples(id);result.pcm.assign(pcm.begin()+s.skip*2,pcm.begin()+(s.skip+result.frames)*2);
        s.consumed+=s.wave.block_bytes;s.position+=result.frames;s.skip=0;
        if(s.loops && s.wave.loop_end>=0 && s.position==static_cast<unsigned>(s.wave.loop_end)+1u){
            const auto sample=static_cast<unsigned>(s.wave.loop_start),offset=seek_offset(s,sample);
            const auto write=(s.ring_start+s.received-s.origin)%s.capacity;
            media_.restart_atrac(id);s.position=sample;s.skip=(s.wave.first_sample+sample)%2048u;s.preroll=seek_history(s,sample);
            if(s.capacity<s.wave.file_bytes){s.origin=offset;s.ring_start=write;s.received=offset+s.prefetched;s.prefetched=0;}
            s.consumed=offset;if(s.loops>0)--s.loops;++s.loop_restarts;
        }
        result.finish=s.position>=s.wave.samples;result.remains=remaining(id);return result;
    }
private:
    psprecomp::GuestMemory& memory_;NativeMedia& media_;
    static unsigned seek_history(const Stream& s,unsigned sample){const auto absolute=s.wave.first_sample+sample;return std::min(absolute/2048u,absolute%2048u<368u?2u:1u);}
    static unsigned seek_offset(const Stream& s,unsigned sample){return s.wave.data_offset+((s.wave.first_sample+sample)/2048u-seek_history(s,sample))*s.wave.block_bytes;}
    std::vector<std::uint8_t> packet(const Stream& s){std::vector<std::uint8_t> data(s.wave.block_bytes);
        for(unsigned n=0;n<data.size();++n)data[n]=memory_.load8(s.buffer+(s.ring_start+s.consumed+n-s.origin)%s.capacity);return data;
    }
};
}
