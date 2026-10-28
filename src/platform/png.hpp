#pragma once
// Portable PNG decoder for texture mods on hosts without WIC (platform layer).
// Own implementation (zlib inflate per RFC 1950/1951 and PNG per the W3C spec):
// every color type, bit depths 1-16, palettes and tRNS, the five row filters
// and Adam7 interlacing. Output texels: R in the low byte, alpha in the high byte,
// straight alpha; 16-bit samples keep their high byte. Limits match the WIC path.
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>
namespace kon::platform {
namespace png_detail {
[[noreturn]] inline void fail(const char* what){throw std::runtime_error(std::string("PNG: ")+what);}
class Bits {
public:
    Bits(const std::uint8_t* data,std::size_t size):data_(data),size_(size){}
    std::uint32_t bit(){
        if(!count_){if(pos_>=size_)fail("compressed data ends early");buffer_=data_[pos_++];count_=8;}
        const std::uint32_t b=buffer_&1u;buffer_>>=1;--count_;return b;
    }
    std::uint32_t bits(unsigned n){std::uint32_t v=0;for(unsigned i=0;i<n;++i)v|=bit()<<i;return v;}
    void align(){count_=0;}
    std::uint8_t byte(){if(pos_>=size_)fail("compressed data ends early");return data_[pos_++];}
private:
    const std::uint8_t* data_;std::size_t size_,pos_=0;std::uint32_t buffer_=0;unsigned count_=0;
};
struct Huffman {
    std::array<std::uint16_t,16> counts{};std::array<std::uint16_t,320> symbols{};
    void build(const std::uint8_t* lengths,unsigned n){
        counts.fill(0);for(unsigned i=0;i<n;++i)++counts[lengths[i]];
        counts[0]=0;std::array<std::uint16_t,16> offsets{};
        for(unsigned len=1;len<16;++len)offsets[len]=static_cast<std::uint16_t>(offsets[len-1]+counts[len-1]);
        for(unsigned i=0;i<n;++i)if(lengths[i])symbols[offsets[lengths[i]]++]=static_cast<std::uint16_t>(i);
    }
    unsigned decode(Bits& in) const {
        int code=0,first=0,index=0;
        for(unsigned len=1;len<16;++len){
            code|=static_cast<int>(in.bit());const int count=counts[len];
            if(code-first<count)return symbols[static_cast<std::size_t>(index+code-first)];
            index+=count;first+=count;first<<=1;code<<=1;
        }
        fail("invalid Huffman code");
    }
};
// zlib stream (RFC 1950) to bytes; never more than `limit` bytes.
inline std::vector<std::uint8_t> inflate(const std::vector<std::uint8_t>& z,std::size_t limit){
    if(z.size()<2 || (z[0]&0x0F)!=8 || ((z[0]<<8)|z[1])%31 || (z[1]&0x20))fail("unsupported zlib stream");
    static constexpr std::uint16_t length_base[29]{3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,67,83,99,115,131,163,195,227,258};
    static constexpr std::uint8_t length_extra[29]{0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0};
    static constexpr std::uint16_t distance_base[30]{1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,1025,1537,2049,3073,4097,6145,8193,12289,16385,24577};
    static constexpr std::uint8_t distance_extra[30]{0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13};
    Bits in(z.data()+2,z.size()-2);std::vector<std::uint8_t> out;out.reserve(limit);
    for(bool last=false;!last;){
        last=in.bit();const auto type=in.bits(2);
        if(type==0){
            in.align();const unsigned len=in.byte()|(in.byte()<<8),nlen=in.byte()|(in.byte()<<8);
            if((len^0xFFFFu)!=nlen)fail("stored block length mismatch");
            if(out.size()+len>limit)fail("image data larger than declared");
            for(unsigned i=0;i<len;++i)out.push_back(in.byte());
            continue;
        }
        if(type==3)fail("invalid block type");
        Huffman literal,distance;
        if(type==1){
            std::array<std::uint8_t,288> l{};std::fill(l.begin(),l.begin()+144,8);std::fill(l.begin()+144,l.begin()+256,9);
            std::fill(l.begin()+256,l.begin()+280,7);std::fill(l.begin()+280,l.end(),8);literal.build(l.data(),288);
            std::array<std::uint8_t,30> d{};d.fill(5);distance.build(d.data(),30);
        }else{
            const unsigned hlit=in.bits(5)+257,hdist=in.bits(5)+1,hclen=in.bits(4)+4;
            static constexpr std::uint8_t order[19]{16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15};
            std::array<std::uint8_t,19> code_lengths{};for(unsigned i=0;i<hclen;++i)code_lengths[order[i]]=static_cast<std::uint8_t>(in.bits(3));
            Huffman lengths;lengths.build(code_lengths.data(),19);
            std::array<std::uint8_t,320> l{};
            for(unsigned i=0;i<hlit+hdist;){
                const unsigned symbol=lengths.decode(in);
                if(symbol<16){l[i++]=static_cast<std::uint8_t>(symbol);continue;}
                unsigned repeat=0;std::uint8_t value=0;
                if(symbol==16){if(!i)fail("length repeat without a previous length");value=l[i-1];repeat=3+in.bits(2);}
                else if(symbol==17)repeat=3+in.bits(3);else repeat=11+in.bits(7);
                if(i+repeat>hlit+hdist)fail("code lengths overflow");
                while(repeat--)l[i++]=value;
            }
            if(!l[256])fail("missing end-of-block code");
            literal.build(l.data(),hlit);distance.build(l.data()+hlit,hdist);
        }
        for(;;){
            const unsigned symbol=literal.decode(in);
            if(symbol<256){if(out.size()>=limit)fail("image data larger than declared");out.push_back(static_cast<std::uint8_t>(symbol));continue;}
            if(symbol==256)break;
            if(symbol>285)fail("invalid length symbol");
            const unsigned length=length_base[symbol-257]+in.bits(length_extra[symbol-257]);
            const unsigned dsym=distance.decode(in);if(dsym>29)fail("invalid distance symbol");
            const std::size_t dist=distance_base[dsym]+in.bits(distance_extra[dsym]);
            if(dist>out.size())fail("distance before the start of the data");
            if(out.size()+length>limit)fail("image data larger than declared");
            for(unsigned i=0;i<length;++i)out.push_back(out[out.size()-dist]);
        }
    }
    return out;
}
inline std::uint32_t be32(const std::uint8_t* p){return (std::uint32_t(p[0])<<24)|(std::uint32_t(p[1])<<16)|(std::uint32_t(p[2])<<8)|p[3];}
inline std::uint8_t paeth(int a,int b,int c){const int p=a+b-c,pa=std::abs(p-a),pb=std::abs(p-b),pc=std::abs(p-c);return static_cast<std::uint8_t>(pa<=pb && pa<=pc?a:pb<=pc?b:c);}
}
// Decodes a whole PNG file held in memory; throws std::runtime_error on any
// malformed or unsupported input (no partial images).
inline std::vector<std::uint32_t> decode_png(const std::vector<std::uint8_t>& file,unsigned& width,unsigned& height){
    using namespace png_detail;
    static constexpr std::uint8_t signature[8]{0x89,'P','N','G',0x0D,0x0A,0x1A,0x0A};
    if(file.size()<33 || std::memcmp(file.data(),signature,8))fail("not a PNG file");
    unsigned w=0,h=0,depth=0,color=0,interlace=0;bool header=false;
    std::vector<std::uint8_t> idat;std::vector<std::array<std::uint8_t,4>> palette;
    std::vector<std::uint8_t> trns;
    for(std::size_t at=8;;){
        if(at+12>file.size())fail("missing IEND chunk");
        const auto length=be32(&file[at]);const char* type=reinterpret_cast<const char*>(&file[at+4]);
        if(length>file.size()-at-12)fail("chunk beyond the end of the file");
        const std::uint8_t* data=&file[at+8];
        if(!std::memcmp(type,"IHDR",4)){
            if(length!=13)fail("bad IHDR");
            w=be32(data);h=be32(data+4);depth=data[8];color=data[9];interlace=data[12];
            if(data[10] || data[11] || interlace>1)fail("unsupported compression, filter or interlace method");
            header=true;
        }else if(!std::memcmp(type,"PLTE",4)){
            if(length%3 || length>768)fail("bad palette");
            for(unsigned i=0;i<length;i+=3)palette.push_back({data[i],data[i+1],data[i+2],255});
        }else if(!std::memcmp(type,"tRNS",4))trns.assign(data,data+length);
        else if(!std::memcmp(type,"IDAT",4))idat.insert(idat.end(),data,data+length);
        else if(!std::memcmp(type,"IEND",4))break;
        else if(!(type[0]&0x20))fail("unknown critical chunk");
        at+=12+length;
    }
    if(!header)fail("missing IHDR");
    if(!w || !h || w>4096 || h>4096)fail("image size out of range");
    const unsigned channels=color==0?1:color==2?3:color==3?1:color==4?2:color==6?4:0;
    const bool depth_ok=color==0?(depth==1||depth==2||depth==4||depth==8||depth==16):color==3?(depth==1||depth==2||depth==4||depth==8):(depth==8||depth==16);
    if(!channels || !depth_ok)fail("invalid color type and bit depth");
    if(color==3 && palette.empty())fail("palette image without PLTE");
    for(std::size_t i=0;i<trns.size() && color==3 && i<palette.size();++i)palette[i][3]=trns[i];
    const unsigned bits_per_pixel=channels*depth,bpp=std::max(1u,bits_per_pixel/8);
    // Adam7 passes (x0, y0, dx, dy); a non-interlaced image is one full pass.
    static constexpr unsigned adam7[7][4]{{0,0,8,8},{4,0,8,8},{0,4,4,8},{2,0,4,4},{0,2,2,4},{1,0,2,2},{0,1,1,2}};
    static constexpr unsigned whole[1][4]{{0,0,1,1}};
    const auto* passes=interlace?adam7:whole;const unsigned pass_count=interlace?7:1;
    std::size_t expected=0;
    for(unsigned p=0;p<pass_count;++p){
        const unsigned pw=(w+passes[p][2]-1-passes[p][0])/passes[p][2],ph=(h+passes[p][3]-1-passes[p][1])/passes[p][3];
        if(w>passes[p][0] && h>passes[p][1] && pw && ph)expected+=std::size_t(ph)*(1+(std::size_t(pw)*bits_per_pixel+7)/8);
    }
    const auto raw=inflate(idat,expected);
    if(raw.size()!=expected)fail("image data shorter than declared");
    const auto sample=[&](const std::uint8_t* row,unsigned x,unsigned c)->unsigned{
        if(depth==8)return row[x*channels+c];
        if(depth==16)return (unsigned(row[(x*channels+c)*2])<<8)|row[(x*channels+c)*2+1];
        const unsigned bit=(x*channels+c)*depth;return (row[bit/8]>>(8-depth-bit%8))&((1u<<depth)-1);
    };
    // 16-bit samples keep their high byte (WIC itself rounds some 16-bit formats
    // and truncates others: up to one step of difference there).
    const auto to8=[&](unsigned v)->unsigned{return depth==16?v>>8:depth==8?v:v*255u/((1u<<depth)-1);};
    std::vector<std::uint32_t> pixels(std::size_t(w)*h);
    std::size_t at=0;
    for(unsigned p=0;p<pass_count;++p){
        if(w<=passes[p][0] || h<=passes[p][1])continue;
        const unsigned pw=(w+passes[p][2]-1-passes[p][0])/passes[p][2],ph=(h+passes[p][3]-1-passes[p][1])/passes[p][3];
        const std::size_t stride=(std::size_t(pw)*bits_per_pixel+7)/8;
        std::vector<std::uint8_t> previous(stride,0),current(stride);
        for(unsigned y=0;y<ph;++y){
            const unsigned filter=raw[at++];if(filter>4)fail("invalid row filter");
            std::memcpy(current.data(),&raw[at],stride);at+=stride;
            for(std::size_t i=0;i<stride;++i){
                const int a=i>=bpp?current[i-bpp]:0,b=previous[i],c=i>=bpp?previous[i-bpp]:0;
                int v=current[i];
                switch(filter){case 1:v+=a;break;case 2:v+=b;break;case 3:v+=(a+b)/2;break;case 4:v+=paeth(a,b,c);break;default:break;}
                current[i]=static_cast<std::uint8_t>(v);
            }
            for(unsigned x=0;x<pw;++x){
                unsigned r,g,b,alpha=255;
                if(color==3){const unsigned index=sample(current.data(),x,0);if(index>=palette.size())fail("palette index out of range");
                    r=palette[index][0];g=palette[index][1];b=palette[index][2];alpha=palette[index][3];}
                else if(color==0 || color==4){const unsigned v=sample(current.data(),x,0);r=g=b=to8(v);
                    if(color==4)alpha=to8(sample(current.data(),x,1));
                    else if(trns.size()>=2 && v==((unsigned(trns[0])<<8)|trns[1]))r=g=b=alpha=0;}  // as WIC: a tRNS key is transparent black
                else{const unsigned R=sample(current.data(),x,0),G=sample(current.data(),x,1),B=sample(current.data(),x,2);
                    r=to8(R);g=to8(G);b=to8(B);
                    if(color==6)alpha=to8(sample(current.data(),x,3));
                    else if(trns.size()>=6 && R==((unsigned(trns[0])<<8)|trns[1]) && G==((unsigned(trns[2])<<8)|trns[3]) && B==((unsigned(trns[4])<<8)|trns[5]))alpha=0;}  // as WIC: color kept
                const std::size_t px=passes[p][0]+std::size_t(x)*passes[p][2],py=passes[p][1]+std::size_t(y)*passes[p][3];
                pixels[py*w+px]=r|(g<<8)|(b<<16)|(alpha<<24);
            }
            std::swap(previous,current);
        }
    }
    width=w;height=h;return pixels;
}
}
