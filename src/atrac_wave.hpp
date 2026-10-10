#pragma once
#include <cstdint>
#include <span>
#include <stdexcept>
namespace kon {
struct AtracWave {
    std::uint32_t channels=0,rate=0,block_bytes=0,file_bytes=0,data_offset=0,data_bytes=0,samples=0,first_sample=0;
    std::int32_t loop_start=-1,loop_end=-1;
    static AtracWave parse(std::span<const std::uint8_t> bytes){
        const auto u32=[&](std::size_t at){if(at+4>bytes.size())throw std::runtime_error("ATRAC RIFF header truncated");return bytes[at]|(std::uint32_t(bytes[at+1])<<8u)|(std::uint32_t(bytes[at+2])<<16u)|(std::uint32_t(bytes[at+3])<<24u);};
        const auto u16=[&](std::size_t at){if(at+2>bytes.size())throw std::runtime_error("ATRAC RIFF header truncated");return std::uint32_t(bytes[at])|(std::uint32_t(bytes[at+1])<<8u);};
        if(u32(0)!=0x46464952u || u32(8)!=0x45564157u)throw std::runtime_error("ATRAC input is not RIFF/WAVE");
        AtracWave result;const auto total=std::uint64_t(u32(4))+8;
        if(total>0x20000000u)throw std::runtime_error("ATRAC RIFF length exceeds supported range");
        result.file_bytes=static_cast<std::uint32_t>(total);bool format=false,fact=false,extended=false;
        std::uint32_t offset=2048,loop_raw_start=0xFFFFFFFFu,loop_raw_end=0xFFFFFFFFu;
        for(std::size_t at=12;at+8<=bytes.size();){const auto tag=u32(at),size=u32(at+4);const auto payload=at+8;
            if(tag==0x61746164u){
                if(!format || !size || payload+std::uint64_t(size)>total)throw std::runtime_error("ATRAC data chunk invalid");
                result.data_offset=static_cast<std::uint32_t>(payload);result.data_bytes=size;result.file_bytes=result.data_offset+size;
                if(size%result.block_bytes)throw std::runtime_error("ATRAC compressed data ends in a partial packet");
                if(extended){if(offset<184)throw std::runtime_error("ATRAC extended sample offset invalid");offset-=184;}
                result.first_sample=offset+368;
                if(!fact)result.samples=size/result.block_bytes*2048-result.first_sample;
                if(!result.samples || std::uint64_t(result.samples)+result.first_sample>std::uint64_t(size/result.block_bytes)*2048)throw std::runtime_error("ATRAC sample length invalid");
                if(loop_raw_start!=0xFFFFFFFFu){
                    const auto adjust=extended?184u:0u;
                    const auto absolute_start=std::uint64_t(loop_raw_start)+368-adjust,absolute_end=std::uint64_t(loop_raw_end)+368-adjust;
                    if(absolute_start<result.first_sample || absolute_end<absolute_start || absolute_end-result.first_sample>=result.samples)throw std::runtime_error("ATRAC loop bounds invalid");
                    result.loop_start=static_cast<std::int32_t>(absolute_start-result.first_sample);result.loop_end=static_cast<std::int32_t>(absolute_end-result.first_sample);
                }
                return result;
            }
            if(payload+std::uint64_t(size)>bytes.size())throw std::runtime_error("ATRAC metadata extends beyond initial buffer");
            if(tag==0x20746D66u){
                if(format || size<52 || u16(payload)!=0xFFFEu || u32(payload+24)!=0xE923AABFu)throw std::runtime_error("ATRAC codec is not supported ATRAC3+");
                result.channels=u16(payload+2);result.rate=u32(payload+4);result.block_bytes=u16(payload+12);
                if((result.channels!=1 && result.channels!=2) || result.rate!=44100 || !result.block_bytes || result.block_bytes>8192)throw std::runtime_error("ATRAC codec parameters unsupported");
                format=true;
            }else if(tag==0x74636166u){
                if(size<4)throw std::runtime_error("ATRAC fact chunk invalid");result.samples=u32(payload);fact=true;
                if(size>=12){offset=u32(payload+8);extended=true;}else if(size>=8)offset=u32(payload+4);
            }else if(tag==0x6C706D73u){
                if(size<36)throw std::runtime_error("ATRAC loop chunk invalid");const auto loops=u32(payload+28);
                if(loops>1)throw std::runtime_error("Multiple ATRAC loops require native implementation");
                if(loops){if(size<60)throw std::runtime_error("ATRAC loop chunk truncated");loop_raw_start=u32(payload+44);loop_raw_end=u32(payload+48);}
            }
            at=payload+std::uint64_t(size)+(size&1u);
        }
        throw std::runtime_error("ATRAC data header absent from initial buffer");
    }
};
}
