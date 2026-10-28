// The port's PNG decoder (platform/png.hpp), used for texture mods on hosts
// without WIC: synthetic images with every color type, bit depth, row filter,
// palette/tRNS and Adam7 (stored zlib blocks), plus the project's icon (dynamic
// Huffman). Checked against the expected texels and, on Windows, against WIC.
#include "platform/png.hpp"
#include "platform/image.hpp"
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>
namespace {
void check(bool ok,const std::string& what){if(!ok)throw std::runtime_error(what);}
std::uint32_t crc32(const std::uint8_t* p,std::size_t n,std::uint32_t crc=0xFFFFFFFFu){
    for(std::size_t i=0;i<n;++i){crc^=p[i];for(int k=0;k<8;++k)crc=(crc>>1)^(crc&1u?0xEDB88320u:0u);}return crc;
}
void be32(std::vector<std::uint8_t>& out,std::uint32_t v){for(int s=24;s>=0;s-=8)out.push_back(static_cast<std::uint8_t>(v>>s));}
void chunk(std::vector<std::uint8_t>& out,const char* type,const std::vector<std::uint8_t>& data){
    be32(out,static_cast<std::uint32_t>(data.size()));const auto start=out.size();
    out.insert(out.end(),type,type+4);out.insert(out.end(),data.begin(),data.end());
    be32(out,crc32(&out[start],out.size()-start)^0xFFFFFFFFu);
}
std::vector<std::uint8_t> zlib_stored(const std::vector<std::uint8_t>& raw){
    std::vector<std::uint8_t> z{0x78,0x01};std::size_t at=0;
    do{const auto n=std::min<std::size_t>(raw.size()-at,65535);const bool last=at+n==raw.size();
        z.push_back(last?1:0);z.push_back(n&0xFF);z.push_back(n>>8);z.push_back(~n&0xFF);z.push_back((~n>>8)&0xFF);
        z.insert(z.end(),raw.begin()+at,raw.begin()+at+n);at+=n;}while(at<raw.size());
    std::uint32_t a=1,b=0;for(auto c:raw){a=(a+c)%65521;b=(b+a)%65521;}be32(z,(b<<16)|a);return z;
}
struct Case {unsigned w,h,depth,color;bool interlace,trns;};
unsigned channels(unsigned color){return color==0?1:color==2?3:color==3?1:color==4?2:4;}
unsigned sample_value(const Case& c,unsigned x,unsigned y,unsigned ch){
    const unsigned limit=c.color==3?std::min(6u,(1u<<c.depth)-1u):(1u<<c.depth)-1u;  // 7 palette entries
    return (x*37u+y*91u+ch*53u+x*y)%(limit+1u);
}
// PNG bytes and expected texels for a case.
std::vector<std::uint8_t> build(const Case& c,std::vector<std::uint32_t>& expected){
    const unsigned n=channels(c.color);
    std::vector<std::array<std::uint8_t,4>> palette;for(unsigned i=0;i<7;++i)palette.push_back({std::uint8_t(i*40),std::uint8_t(255-i*30),std::uint8_t(i*i*5),std::uint8_t(c.trns?i*36:255)});
    const unsigned trns_gray=sample_value(c,1,0,0),trns_rgb[3]{sample_value(c,2,1,0),sample_value(c,2,1,1),sample_value(c,2,1,2)};
    const auto to8=[&](unsigned v){return c.depth==16?v>>8:c.depth==8?v:v*255u/((1u<<c.depth)-1u);};
    expected.assign(std::size_t(c.w)*c.h,0);
    for(unsigned y=0;y<c.h;++y)for(unsigned x=0;x<c.w;++x){
        unsigned r,g,b,a=255;
        if(c.color==3){const auto& p=palette[sample_value(c,x,y,0)];r=p[0];g=p[1];b=p[2];a=p[3];}
        else if(c.color==0 || c.color==4){const auto v=sample_value(c,x,y,0);r=g=b=to8(v);if(c.color==4)a=to8(sample_value(c,x,y,1));else if(c.trns && v==trns_gray)r=g=b=a=0;}
        else{const unsigned R=sample_value(c,x,y,0),G=sample_value(c,x,y,1),B=sample_value(c,x,y,2);r=to8(R);g=to8(G);b=to8(B);
            if(c.color==6)a=to8(sample_value(c,x,y,3));else if(c.trns && R==trns_rgb[0] && G==trns_rgb[1] && B==trns_rgb[2])a=0;}
        expected[std::size_t(y)*c.w+x]=r|(g<<8)|(b<<16)|(a<<24);
    }
    static constexpr unsigned adam7[7][4]{{0,0,8,8},{4,0,8,8},{0,4,4,8},{2,0,4,4},{0,2,2,4},{1,0,2,2},{0,1,1,2}};
    static constexpr unsigned whole[1][4]{{0,0,1,1}};
    const auto* passes=c.interlace?adam7:whole;const unsigned count=c.interlace?7:1;
    const unsigned bits=n*c.depth,bpp=std::max(1u,bits/8);
    std::vector<std::uint8_t> raw;unsigned row_number=0;
    for(unsigned p=0;p<count;++p){
        if(c.w<=passes[p][0] || c.h<=passes[p][1])continue;
        const unsigned pw=(c.w+passes[p][2]-1-passes[p][0])/passes[p][2],ph=(c.h+passes[p][3]-1-passes[p][1])/passes[p][3];
        const std::size_t stride=(std::size_t(pw)*bits+7)/8;std::vector<std::uint8_t> previous(stride,0);
        for(unsigned y=0;y<ph;++y,++row_number){
            std::vector<std::uint8_t> line(stride,0);
            for(unsigned x=0;x<pw;++x)for(unsigned ch=0;ch<n;++ch){
                const unsigned v=sample_value(c,passes[p][0]+x*passes[p][2],passes[p][1]+y*passes[p][3],ch);
                if(c.depth==16){line[(x*n+ch)*2]=v>>8;line[(x*n+ch)*2+1]=v&0xFF;}
                else if(c.depth==8)line[x*n+ch]=static_cast<std::uint8_t>(v);
                else{const unsigned bit=(x*n+ch)*c.depth;line[bit/8]|=static_cast<std::uint8_t>(v<<(8-c.depth-bit%8));}
            }
            const unsigned filter=row_number%5;raw.push_back(static_cast<std::uint8_t>(filter));
            for(std::size_t i=0;i<stride;++i){
                const int a=i>=bpp?line[i-bpp]:0,b=previous[i],cc=i>=bpp?previous[i-bpp]:0;int pred=0;
                if(filter==1)pred=a;else if(filter==2)pred=b;else if(filter==3)pred=(a+b)/2;else if(filter==4)pred=kon::platform::png_detail::paeth(a,b,cc);
                raw.push_back(static_cast<std::uint8_t>(line[i]-pred));
            }
            previous=line;
        }
    }
    std::vector<std::uint8_t> png{0x89,'P','N','G',0x0D,0x0A,0x1A,0x0A},ihdr;
    be32(ihdr,c.w);be32(ihdr,c.h);ihdr.insert(ihdr.end(),{std::uint8_t(c.depth),std::uint8_t(c.color),0,0,std::uint8_t(c.interlace)});
    chunk(png,"IHDR",ihdr);
    if(c.color==3){std::vector<std::uint8_t> plte,t;for(auto& e:palette){plte.insert(plte.end(),e.begin(),e.begin()+3);t.push_back(e[3]);}chunk(png,"PLTE",plte);if(c.trns)chunk(png,"tRNS",t);}
    else if(c.trns && c.color==0){std::vector<std::uint8_t> t;t.push_back(trns_gray>>8);t.push_back(trns_gray&0xFF);chunk(png,"tRNS",t);}
    else if(c.trns && c.color==2){std::vector<std::uint8_t> t;for(auto v:trns_rgb){t.push_back(v>>8);t.push_back(v&0xFF);}chunk(png,"tRNS",t);}
    chunk(png,"tEXt",{'C','o','m','m','e','n','t',0,'t','e','s','t'});  // ancillary chunks are skipped
    chunk(png,"IDAT",zlib_stored(raw));chunk(png,"IEND",{});
    return png;
}
}
int main(int argc,char** argv){
    try{
        const std::filesystem::path root=argc>1?argv[1]:".";
        const std::filesystem::path temp=std::filesystem::temp_directory_path()/"kon_native_image_test.png";
        unsigned cases=0,wic=0,wic16=0;
        for(const unsigned color:{0u,2u,3u,4u,6u})for(const unsigned depth:{1u,2u,4u,8u,16u}){
            const bool valid=color==0 || (color==3 && depth<=8) || ((color==2 || color==4 || color==6) && depth>=8);
            if(!valid)continue;
            for(const bool interlace:{false,true})for(const bool trns:{false,true}){
                if(trns && (color==4 || color==6))continue;
                const Case c{interlace?13u:17u,interlace?11u:9u,depth,color,interlace,trns};
                std::vector<std::uint32_t> expected;const auto png=build(c,expected);
                unsigned w=0,h=0;const auto pixels=kon::platform::decode_png(png,w,h);
                const auto label="color "+std::to_string(color)+" depth "+std::to_string(depth)+(interlace?" Adam7":"")+(trns?" tRNS":"");
                check(w==c.w && h==c.h && pixels==expected,"own decoder, "+label);
#ifdef _WIN32
                // WIC rescales samples of depth 16 (and low gray depths) its own way;
                // where both are exact the texels must agree bit for bit.
                {std::ofstream(temp,std::ios::binary).write(reinterpret_cast<const char*>(png.data()),static_cast<std::streamsize>(png.size()));}
                const auto system=kon::platform::decode_image_with_system(temp,w,h);
                if(depth!=16){
                    for(std::size_t i=0;i<pixels.size() && system!=pixels;++i)if(system[i]!=pixels[i]){std::cout<<"difference ("<<label<<") at "<<i<<": own "<<std::hex<<pixels[i]<<" WIC "<<system[i]<<std::dec<<"\n";break;}
                    check(system==pixels,"WIC agreement, "+label);++wic;}
                else{  // 16-bit: WIC rounds some formats and truncates others; at most one step apart
                    for(std::size_t i=0;i<pixels.size();++i)for(unsigned s=0;s<32;s+=8){
                        const int own=(pixels[i]>>s)&0xFF,other=(system[i]>>s)&0xFF;check(own-other<=1 && other-own<=1,"WIC within one step, "+label);}
                    ++wic16;}
#endif
                ++cases;
            }
        }
        // A real compressed PNG (dynamic Huffman, adaptive filters): the project's icon.
        const auto icon=root/"assets/branding/icon_1024.png";
        std::ifstream in(icon,std::ios::binary);check(static_cast<bool>(in),"cannot open "+icon.string());
        const std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(in)),std::istreambuf_iterator<char>());
        unsigned w=0,h=0;const auto pixels=kon::platform::decode_png(bytes,w,h);
        check(w==1024 && h==1024,"icon size");
#ifdef _WIN32
        const auto system=kon::platform::decode_image_with_system(icon,w,h);check(system==pixels,"WIC agreement on the icon");++wic;
#endif
        bool rejected=false;try{auto broken=bytes;broken.resize(bytes.size()/2);kon::platform::decode_png(broken,w,h);}catch(const std::runtime_error&){rejected=true;}
        check(rejected,"truncated PNG rejected");
        std::filesystem::remove(temp);
        std::cout<<"PASS: own PNG decoder, "<<cases<<" synthetic cases (all color types, depths, filters, palette/tRNS, Adam7) and the icon; "
                 <<wic<<" bit-exact comparisons with WIC, "<<wic16<<" 16-bit ones within one step\n";
        return 0;
    }catch(const std::exception& e){std::cerr<<"FAIL: "<<e.what()<<"\n";return 1;}
}
