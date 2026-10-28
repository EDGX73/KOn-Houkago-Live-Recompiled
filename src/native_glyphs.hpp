#pragma once
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <set>
#include <span>
#include <string>
#include <vector>
#include "native_glyph_table.hpp"
#include "native_button_map.hpp"
// Button glyphs: the original textures that show PSP buttons are recognised
// by the content hash of their decoded pixels and, when another device is in
// use, redrawn for it (Xbox, PlayStation, Nintendo, keyboard).
namespace kon {
// Style shown on screen (InputStyle values; the window/input side sets it).
inline std::atomic<std::uint8_t>& glyph_style(){static std::atomic<std::uint8_t> style{0};return style;}
inline std::uint64_t texture_hash(std::span<const std::uint32_t> pixels,unsigned width,unsigned height){
    std::uint64_t h=1469598103934665603ull^(std::uint64_t(width)<<32)^height;
    for(const auto p:pixels){h^=p;h*=1099511628211ull;}
    return h;
}
// Style the textures are drawn for: KON_GLYPH_STYLE (keyboard, xbox,
// playstation, nintendo, generic) fixes it for tests; otherwise the device in use.
inline std::uint8_t current_glyph_style(){
    static const int forced=[]{
        const char* v=std::getenv("KON_GLYPH_STYLE");if(!v)return -1;
        const std::string s(v);
        return s=="keyboard"?0:s=="xbox"?1:s=="playstation"?2:s=="nintendo"?3:s=="generic"?4:-1;
    }();
    return forced>=0?static_cast<std::uint8_t>(forced):glyph_style().load(std::memory_order_relaxed);
}
inline constexpr std::uint8_t no_glyphs=0xFF; // texture without button glyphs
namespace glyph_detail {
// 5x7 letters, one row per byte (bit 4 = leftmost column).
struct Letter {char c;std::array<std::uint8_t,7> rows;};
inline constexpr Letter letters[]{
    {'A',{0x0E,0x11,0x11,0x1F,0x11,0x11,0x11}},{'B',{0x1E,0x11,0x11,0x1E,0x11,0x11,0x1E}},
    {'X',{0x11,0x11,0x0A,0x04,0x0A,0x11,0x11}},{'Y',{0x11,0x11,0x0A,0x04,0x04,0x04,0x04}},
    {'Z',{0x1F,0x01,0x02,0x04,0x08,0x10,0x1F}},{'S',{0x0F,0x10,0x10,0x0E,0x01,0x01,0x1E}},
    {'1',{0x04,0x0C,0x04,0x04,0x04,0x04,0x0E}},{'2',{0x0E,0x11,0x01,0x02,0x04,0x08,0x1F}},
    {'3',{0x1E,0x01,0x01,0x0E,0x01,0x01,0x1E}},{'4',{0x02,0x06,0x0A,0x12,0x1F,0x02,0x02}},
    {'E',{0x1F,0x10,0x10,0x1E,0x10,0x10,0x1F}},{'N',{0x11,0x19,0x15,0x13,0x11,0x11,0x11}},
    {'T',{0x1F,0x04,0x04,0x04,0x04,0x04,0x04}},{'R',{0x1E,0x11,0x11,0x1E,0x14,0x12,0x11}},
    {'K',{0x11,0x12,0x14,0x18,0x14,0x12,0x11}},{'P',{0x1E,0x11,0x11,0x1E,0x10,0x10,0x10}},
    {'M',{0x11,0x1B,0x15,0x15,0x11,0x11,0x11}},{'U',{0x11,0x11,0x11,0x11,0x11,0x11,0x0E}},
    {'V',{0x11,0x11,0x11,0x11,0x11,0x0A,0x04}},{'I',{0x0E,0x04,0x04,0x04,0x04,0x04,0x0E}},
    {'W',{0x11,0x11,0x11,0x15,0x15,0x15,0x0A}},{'+',{0x00,0x04,0x04,0x1F,0x04,0x04,0x00}},
    {'-',{0x00,0x00,0x00,0x1F,0x00,0x00,0x00}},{'?',{0x0E,0x11,0x01,0x02,0x04,0x00,0x04}},
    {'C',{0x0E,0x11,0x10,0x10,0x10,0x11,0x0E}},{'D',{0x1C,0x12,0x11,0x11,0x11,0x12,0x1C}},
    {'F',{0x1F,0x10,0x10,0x1E,0x10,0x10,0x10}},{'G',{0x0E,0x11,0x10,0x17,0x11,0x11,0x0F}},
    {'H',{0x11,0x11,0x11,0x1F,0x11,0x11,0x11}},{'J',{0x07,0x02,0x02,0x02,0x02,0x12,0x0C}},
    {'L',{0x10,0x10,0x10,0x10,0x10,0x10,0x1F}},{'O',{0x0E,0x11,0x11,0x11,0x11,0x11,0x0E}},
    {'Q',{0x0E,0x11,0x11,0x11,0x15,0x12,0x0D}},{'0',{0x0E,0x11,0x13,0x15,0x19,0x11,0x0E}},
    {'5',{0x1F,0x10,0x1E,0x01,0x01,0x11,0x0E}},{'6',{0x06,0x08,0x10,0x1E,0x11,0x11,0x0E}},
    {'7',{0x1F,0x01,0x02,0x04,0x08,0x08,0x08}},{'8',{0x0E,0x11,0x11,0x0E,0x11,0x11,0x0E}},
    {'9',{0x0E,0x11,0x11,0x0F,0x01,0x02,0x0C}},
};
inline const Letter* letter(char c){for(const auto& l:letters)if(l.c==c)return &l;return nullptr;}
// Label of a PSP button (Circle, Cross, Triangle, Square, Start, Select) for
// a style: the key or button assigned to it (ButtonMap, config/controls.ini);
// empty keeps the original (PlayStation).
inline std::string label(std::uint8_t style,glyph_table::Face face){
    using namespace psp_button;
    static constexpr std::uint32_t psp[]{circle,cross,triangle,square,start,select};
    return ButtonMap::instance().label(static_cast<InputStyle>(style),psp[static_cast<unsigned>(face)]);
}
// Xbox button colors: A green, B red, X blue, Y yellow, anything else white.
inline std::uint32_t xbox_color(const std::string& label){
    if(label=="A")return 0xFF3CC85Cu;if(label=="B")return 0xFF3A3AE8u;if(label=="Y")return 0xFF20D0F5u;if(label=="X")return 0xFFF07A2Eu; // ABGR
    return 0xFFFFFFFFu;
}
inline unsigned channel(std::uint32_t p,unsigned i){return (p>>(i*8))&0xFFu;}
inline unsigned luma(std::uint32_t p){return (channel(p,0)*3+channel(p,1)*6+channel(p,2))/10;}
inline std::uint32_t scaled(std::uint32_t p,unsigned num,unsigned den){
    std::uint32_t out=p&0xFF000000u;for(unsigned i=0;i<3;++i)out|=std::min(255u,channel(p,i)*num/den)<<(i*8);return out;
}
struct Canvas {
    std::span<std::uint32_t> px;unsigned w,h;
    std::uint32_t* at(int x,int y){return x>=0 && y>=0 && unsigned(x)<w && unsigned(y)<h?&px[std::size_t(y)*w+x]:nullptr;}
    // Average opaque color of the texels at distance [r0,r1) from the center,
    // keeping only those whose luma passes the filter.
    template<class Keep> std::uint32_t average(float cx,float cy,float r0,float r1,Keep keep){
        unsigned long long sum[3]{};unsigned n=0;
        for(int y=int(cy-r1)-1;y<=int(cy+r1)+1;++y)for(int x=int(cx-r1)-1;x<=int(cx+r1)+1;++x){
            const float d=std::hypot(x+0.5f-cx,y+0.5f-cy);auto* p=at(x,y);
            if(!p || d<r0 || d>=r1 || (*p>>24)<128 || !keep(luma(*p)))continue;
            for(unsigned i=0;i<3;++i)sum[i]+=channel(*p,i);++n;
        }
        if(!n)return 0xFF000000u;
        return 0xFF000000u|std::uint32_t(sum[0]/n)|(std::uint32_t(sum[1]/n)<<8)|(std::uint32_t(sum[2]/n)<<16);
    }
    void disk(float cx,float cy,float r,std::uint32_t color){
        for(int y=int(cy-r)-1;y<=int(cy+r)+1;++y)for(int x=int(cx-r)-1;x<=int(cx+r)+1;++x)
            if(auto* p=at(x,y);p && std::hypot(x+0.5f-cx,y+0.5f-cy)<r)*p=(*p&0xFF000000u)|(color&0x00FFFFFFu);
    }
    // Gives the colored texels of a box (saturation >= 0.3) the hue of color,
    // keeping their saturation and brightness: recolors a whole note sprite
    // (head, stem and flag) while keeping its shading and outline.
    void hue(int x0,int y0,int x1,int y1,std::uint32_t color){
        const auto hue_of=[](float r,float g,float b){
            const float mx=std::max({r,g,b}),mn=std::min({r,g,b}),d=mx-mn;if(d<=0.f)return 0.f;
            float h=mx==r?(g-b)/d:mx==g?2.f+(b-r)/d:4.f+(r-g)/d;h*=60.f;return h<0.f?h+360.f:h;
        };
        const float target=hue_of(float(channel(color,0)),float(channel(color,1)),float(channel(color,2)));
        for(int y=y0;y<y1;++y)for(int x=x0;x<x1;++x){
            auto* p=at(x,y);if(!p || (*p>>24)==0)continue;
            const float r=float(channel(*p,0)),g=float(channel(*p,1)),b=float(channel(*p,2));
            float v=std::max({r,g,b});const float sat=v>0.f?(v-std::min({r,g,b}))/v:0.f;
            if(sat<0.3f || v<40.f)continue;
            if(target>=40.f && target<=70.f)v=std::min(255.f,v*1.3f+30.f); // yellow only reads as yellow when bright
            const float hh=target/60.f,f=hh-std::floor(hh),pp=v*(1.f-sat),q=v*(1.f-sat*f),t=v*(1.f-sat*(1.f-f));
            float o[3];
            switch(int(hh)%6){case 0:o[0]=v;o[1]=t;o[2]=pp;break;case 1:o[0]=q;o[1]=v;o[2]=pp;break;case 2:o[0]=pp;o[1]=v;o[2]=t;break;
                case 3:o[0]=pp;o[1]=q;o[2]=v;break;case 4:o[0]=t;o[1]=pp;o[2]=v;break;default:o[0]=v;o[1]=pp;o[2]=q;break;}
            *p=(*p&0xFF000000u)|std::uint32_t(o[0]+0.5f)|(std::uint32_t(o[1]+0.5f)<<8)|(std::uint32_t(o[2]+0.5f)<<16);
        }
    }
    void rect(float cx,float cy,float half_w,float half_h,std::uint32_t color){
        for(int y=int(std::floor(cy-half_h));y<int(std::ceil(cy+half_h));++y)for(int x=int(std::floor(cx-half_w));x<int(std::ceil(cx+half_w));++x)
            if(auto* p=at(x,y))*p=(*p&0xFF000000u)|(color&0x00FFFFFFu);
    }
    // Word centered on (cx, cy): 5x7 letters one cell apart, each font cell
    // scale x scale texels, with an optional one-texel outline. `clip` > 0
    // keeps the ink inside that radius (the button ring).
    void text(float cx,float cy,const std::string& word,unsigned scale,std::uint32_t color,std::uint32_t outline,bool outlined,float clip=0.f){
        const auto n=word.size();const float width=float(n*6-1)*scale;
        for(std::size_t i=0;i<n;++i)letter_at(cx-width/2+float(i*6*scale)+2.5f*scale,cy,word[i],scale,color,outline,outlined,clip);
    }
    void letter_at(float cx,float cy,char c,unsigned scale,std::uint32_t color,std::uint32_t outline,bool outlined,float clip=0.f){
        const auto* l=letter(c);if(!l)return;
        const int x0=int(std::floor(cx-2.5f*scale+0.5f)),y0=int(std::floor(cy-3.5f*scale+0.5f));
        const auto ink=[&](int x,int y){
            if(x<0 || y<0 || x>=int(5*scale) || y>=int(7*scale))return false;
            return ((l->rows[y/scale]>>(4-x/scale))&1u)!=0;
        };
        for(int y=-1;y<=int(7*scale);++y)for(int x=-1;x<=int(5*scale);++x){
            auto* p=at(x0+x,y0+y);if(!p)continue;
            if(clip>0.f && std::hypot(float(x0+x)+0.5f-cx,float(y0+y)+0.5f-cy)>clip)continue;
            if(ink(x,y))*p=(*p&0xFF000000u)|(color&0x00FFFFFFu);
            else if(outlined && (ink(x-1,y)||ink(x+1,y)||ink(x,y-1)||ink(x,y+1)))*p=(*p&0xFF000000u)|(outline&0x00FFFFFFu);
        }
    }
};
}
// Redraws the PSP button glyphs listed for this texture (by hash) for the
// style; returns false when the texture has none or the style keeps them.
inline bool apply_button_glyphs(std::span<std::uint32_t> pixels,unsigned width,unsigned height,std::uint64_t hash,std::uint8_t style){
    using namespace glyph_detail;using glyph_table::Kind;
    bool changed=false;Canvas canvas{pixels,width,height};
    for(const auto& g:glyph_table::glyphs){
        if(g.hash!=hash || g.width!=width || g.height!=height)continue;
        const auto c=label(style,g.face);if(c.empty())continue;
        const float cx=g.x+0.5f,cy=g.y+0.5f; // table stores texel indices
        const unsigned big=c.size()>1?1u:2u;
        changed=true;
        const bool xbox=style==1;
        switch(g.kind){
        case Kind::Badge:{ // dark disc with a light symbol: letter in the button color
            // CHECK! is a dot plus a light ring: keep the ring, only fill the dot.
            const bool check=(g.hash==0x84bd7bb4479b4d2eull || g.hash==0x1806865d6c4fad5dull);
            const float fill=check?4.6f:g.radius*0.80f;
            const auto dark=canvas.average(cx,cy,fill*0.55f,fill,[](unsigned l){return l<110;});
            canvas.disk(cx,cy,fill,dark);
            const unsigned scale=c.size()>1?1u:(fill>=8.f?2u:1u);
            canvas.text(cx,cy,c,scale,xbox?xbox_color(c):0xFFFFFFFFu,dark,false,fill);
            break;}
        case Kind::Note:{ // colored note head: white letter outlined in a darker head color
            // Xbox: the whole note sprite (head, stem, flag; 40x52 texels) takes the button color.
            if(xbox)canvas.hue(int(g.x)-19,int(g.y)-36,int(g.x)+21,int(g.y)+16,xbox_color(c));
            const auto head=canvas.average(g.x,g.y,0.f,g.radius,[xbox](unsigned l){return l<(xbox?240u:200u) && l>60;});
            canvas.disk(g.x,g.y,g.radius,head);
            canvas.text(g.x,g.y,c,big,0xFFFFFFFFu,scaled(head,1,2),true);
            break;}
        case Kind::Ring:{ // "PUSH (o) BUTTON!!" ring: keep the outer ring, letter inside
            const auto ring=canvas.average(cx,cy,g.radius*0.55f,g.radius,[](unsigned l){return l>40 && l<230;});
            const auto fill=xbox?xbox_color(c):ring;
            canvas.disk(cx,cy,g.radius*0.78f,fill);
            const unsigned scale=c.size()>1?1u:(g.radius>=10.f?2u:1u);
            canvas.text(cx,cy,c,scale,0xFFFFFFFFu,scaled(fill,1,2),true,g.radius*0.95f);
            break;}
        case Kind::Pill:{ // dark rounded label (START/SELECT): word redrawn inside, radius = half width
            const auto dark=canvas.average(cx,cy,0.f,g.radius,[](unsigned l){return l<90;});
            canvas.rect(cx,cy,g.radius-3.f,5.0f,dark);
            canvas.text(cx,cy,c,1u,0xFFFFFFFFu,dark,false);
            break;}
        }
    }
    return changed;
}
inline bool has_button_glyphs(std::uint64_t hash,unsigned width,unsigned height){
    for(const auto& g:glyph_table::glyphs)if(g.hash==hash && g.width==width && g.height==height)return true;
    return false;
}
// Whether a texture of this size may carry glyphs (hashing is skipped otherwise).
inline bool glyph_candidate(unsigned width,unsigned height){
    for(const auto& g:glyph_table::glyphs)if(g.width==width && g.height==height)return true;
    return false;
}
// Diagnostic (KON_TEXTURE_DUMP=<folder>): every distinct decoded texture once,
// as <hash>_<width>x<height>.bmp, to find the textures that draw buttons.
// 32-bit top-down BMP of decoded texels (ABGR in memory).
inline void dump_decoded_texture_to(std::span<const std::uint32_t> pixels,unsigned width,unsigned height,const std::string& path){
    if(FILE* f=std::fopen(path.c_str(),"wb")){
        const std::uint32_t data=width*height*4u,file=54u+data;
        const std::uint8_t header[54]{'B','M',std::uint8_t(file),std::uint8_t(file>>8),std::uint8_t(file>>16),std::uint8_t(file>>24),0,0,0,0,54,0,0,0,40,0,0,0,
            std::uint8_t(width),std::uint8_t(width>>8),0,0,std::uint8_t(-int(height)),std::uint8_t((-int(height))>>8),0xFF,0xFF,1,0,32,0,0,0,0,0,
            std::uint8_t(data),std::uint8_t(data>>8),std::uint8_t(data>>16),std::uint8_t(data>>24),0x13,0x0B,0,0,0x13,0x0B,0,0,0,0,0,0,0,0,0,0};
        std::fwrite(header,1,54,f);
        std::vector<std::uint8_t> row(std::size_t(width)*4u);
        for(unsigned y=0;y<height;++y){for(unsigned x=0;x<width;++x){const auto p=pixels[std::size_t(y)*width+x];row[x*4]=std::uint8_t(p>>16);row[x*4+1]=std::uint8_t(p>>8);row[x*4+2]=std::uint8_t(p);row[x*4+3]=std::uint8_t(p>>24);}std::fwrite(row.data(),1,row.size(),f);}
        std::fclose(f);
    }
}
inline bool texture_dump_enabled(){static const bool on=std::getenv("KON_TEXTURE_DUMP")!=nullptr;return on;}
inline void dump_decoded_texture(std::span<const std::uint32_t> pixels,unsigned width,unsigned height,std::uint64_t hash){
    static const char* folder=std::getenv("KON_TEXTURE_DUMP");if(!folder || pixels.size()!=std::size_t(width)*height)return;
    static std::mutex mutex;static std::set<std::uint64_t> seen;
    {std::lock_guard lock(mutex);if(!seen.insert(hash).second)return;}
    char name[64];std::snprintf(name,sizeof name,"/%016llx_%ux%u.bmp",static_cast<unsigned long long>(hash),width,height);
    dump_decoded_texture_to(pixels,width,height,std::string(folder)+name);
}
}
