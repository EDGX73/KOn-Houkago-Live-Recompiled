#pragma once
#include "ge_queue.hpp"
#include "psprecomp/guest_memory.hpp"
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <functional>
#include <string>
#include <vector>

#ifdef __ANDROID__
// The in-game Video row is drawn with GDI text; Android uses its own
// settings UI in Java. Keep the same interface so the runtime compiles and the
// game behaves as if the row were absent.
namespace kon {
class NativeSettingsFps {
public:
    static constexpr std::array<unsigned,4> kRates{30u,60u,120u,165u};
    static constexpr std::array<unsigned,4> kScales{1u,2u,3u,4u};
    static constexpr unsigned kTexWidth=256,kTexHeight=128;
    static constexpr std::uint32_t kTexBytes=kTexWidth*kTexHeight*4u,kVertexSlots=512;
    static constexpr std::uint32_t kScratchBytes=kTexBytes+kVertexSlots*48u;
    static constexpr unsigned kRateChanged=1u,kScaleChanged=2u,kScreenChanged=4u,kLanguageChanged=8u;
    using Draw=std::function<void(GeState&,unsigned,unsigned)>;
    NativeSettingsFps(psprecomp::GuestMemory&,std::uint32_t,std::uint32_t){}
    unsigned filter(std::uint32_t&){return 0;}
    bool draw(const GeState&,unsigned,unsigned,const Draw&){return false;}
    void frame_end(){}
    bool screen_visible()const{return false;}
    unsigned rate()const{return 30;}
    unsigned scale()const{return 1;}
    bool fullscreen()const{return true;}
    std::string language()const{return "en";}
    void set_rate(unsigned){}
    void set_scale(unsigned){}
    void set_scale_limit(unsigned){}
    void set_fullscreen(bool){}
    void set_language(const std::string&){}
    void set_language_choice(bool){}
};
}
#else

// "Video" row in the game's own Settings screen.  The PSP-only "Install" row
// (copy UMD data to the memory stick) has no purpose on PC, so its drawing is
// rebuilt as a frame-rate and internal-resolution selector inside the
// original screen:
//   * the "Install" label sprite is drawn as "Video";
//   * its ON/OFF value is drawn as three columns: the frame rate
//     (30/60/120/165) and the internal resolution (272p, the original, and
//     its whole multiples up to the screen's height) with the game's own
//     digits, the screen mode (Window/Full, also toggled by F11) and the
//     language (English/Japanese: saved, and the game restarts in it once
//     the player leaves Settings), the column being changed framed in green
//     as in the Sound row;
//   * while the row is selected the help box shows an explanation written
//     with the game's own 8x16 help font, Circle switches column and
//     Left/Right change its value (those buttons never reach the game on
//     this row, so the PSP install is never started).
// Everything is decided from the GE draws of the screen itself (sprite sheet
// layout of anime/sel_option + sprite/sel_option.uvr), so no game code is
// changed.  Port-made pixels live in a small guest block reserved at boot.
namespace kon {
class NativeSettingsFps {
public:
    static constexpr std::array<unsigned,4> kRates{30u,60u,120u,165u};
    static constexpr std::array<unsigned,4> kScales{1u,2u,3u,4u};
    static constexpr unsigned kTexWidth=256,kTexHeight=128;
    static constexpr std::uint32_t kTexBytes=kTexWidth*kTexHeight*4u,kVertexSlots=512;
    using Draw=std::function<void(GeState&,unsigned,unsigned)>;
    NativeSettingsFps(psprecomp::GuestMemory& memory,std::uint32_t scratch,std::uint32_t scratch_size):memory_(memory),scratch_(scratch),size_(scratch_size){}
    static constexpr std::uint32_t kScratchBytes=kTexBytes+kVertexSlots*48u;

    // Called for every GE draw before it is rendered.  Returns true when the
    // draw was handled here (replaced or dropped).
    bool draw(GeState& state,unsigned kind,unsigned count,const Draw& render){
        const auto& r=state.registers;
        if(kind!=4 || count!=4 || !(r[0x1E]&1u) || (r[0x12]&0xFFFFFFu)!=0x0080011Au)return false;
        const unsigned w=1u<<(r[0xB8]&15u),h=1u<<((r[0xB8]>>8u)&15u),fmt=r[0xC3]&15u;
        if(fmt!=5)return false;
        Quad q;if(!read_quad(state.vertex_address,q))return false;
        const auto texture=(r[0xA0]&0xFFFFF0u)|((r[0xA8]<<8u)&0x0F000000u);
        if(w==256 && h==256){
            if(q.u0==138 && q.v0==2 && q.u1==246 && q.v1==18){ // "Install" label cell
                sheet_=texture;sheet_state_=state;label_y_=q.y0;label_x_=q.x0;label_seen_=true;
                if(!prepare())return false;
                draw_own(state,q,0,0,108,16,render);
                // Values: always drawn here, around where the game puts ON/OFF.
                const auto value=[&](int centre,Text text,unsigned row){
                    Quad v{0,0,0,0,static_cast<std::int16_t>(centre-static_cast<int>(text.width)/2),static_cast<std::int16_t>(q.y0+1),0,0,q.color,q.z};
                    v.x1=static_cast<std::int16_t>(v.x0+text.width);v.y1=static_cast<std::int16_t>(v.y0+16);
                    draw_own(state,v,text.u,row,text.width,16,render);
                };
                value(kRateCentre,value_rect(rate_index_),16);value(kScaleCentre,scale_rect(scale_index_),32);value(kScreenCentre,screen_rect(fullscreen_),96);
                value(kLanguageCentre,language_rect(japanese_),112);
                if(selected_){ // frame of the column being changed
                    const int centre=column_==0?kRateCentre:column_==1?kScaleCentre:column_==2?kScreenCentre:kLanguageCentre;
                    Quad f{0,0,0,0,static_cast<std::int16_t>(centre-32),static_cast<std::int16_t>(q.y0-1),static_cast<std::int16_t>(centre+32),static_cast<std::int16_t>(q.y0+19),q.color,q.z};
                    draw_own(state,f,0,64,64,20,render);
                }
                return true;
            }
            if(texture==sheet_ && label_seen_ && q.y0>=label_y_-4 && q.y0<=label_y_+4){
                if(((q.u0==34 && q.u1==66) || (q.u0==2 && q.u1==34)) && q.v0==34 && q.v1==50)return true; // ON/OFF: replaced
                if(q.u0==66 && q.v0==214 && q.u1==110 && q.v1==238)return true;                          // value frame: drawn per column
                if(q.u0==66 && q.v0==190 && q.u1==210 && q.v1==214)selected_now_=true;                 // row highlight
            }
            return false;
        }
        // Help box punctuation drawn from the 16x16 text atlas: hidden while the
        // row is selected (the replacement text only uses the 8x16 help font).
        if(w==512 && h==512 && selected_ && in_help_box(q))return true;
        // Help box text (8x16 cells of the help font) while the row is selected:
        // the first cell drawn this frame draws the whole replacement text with
        // the same state; every original cell of the box is then left out.
        if(w==256 && h==128 && selected_ && in_help_box(q)){
            if(!help_drawn_){
                help_drawn_=true;
                for(int row=0;row<3;++row)for(int col=0;col<46;++col){
                    const auto glyph=help_glyph(row,col);if(!glyph)continue;
                    const int start=help_center_-static_cast<int>(help_line(row).size())*4; // centred in the box
                    Quad g=q;g.x0=static_cast<std::int16_t>(start+col*8);g.x1=static_cast<std::int16_t>(g.x0+8);
                    g.y0=static_cast<std::int16_t>(help_top_+row*16);g.y1=static_cast<std::int16_t>(g.y0+16);
                    g.u0=static_cast<std::uint16_t>((glyph%32u)*8u);g.v0=static_cast<std::uint16_t>((glyph/32u)*16u);g.u1=g.u0+8u;g.v1=g.v0+16u;
                    const auto at=next_vertices();write_quad(at,g);
                    GeState copy=state;copy.vertex_address=at;render(copy,kind,count);
                }
            }
            return true;
        }
        return false;
    }
    // Once per logic frame (the game's vblank wait).
    void frame_end(){selected_=label_seen_ && selected_now_;selected_now_=false;label_seen_=false;help_drawn_=false;}
    bool row_selected()const{return selected_;}
    unsigned rate()const{return kRates[rate_index_];}
    void set_rate(unsigned fps){for(unsigned n=0;n<kRates.size();++n)if(kRates[n]==fps)rate_index_=n;}
    unsigned scale()const{return kScales[scale_index_];}
    void set_scale(unsigned scale){for(unsigned n=0;n<scale_count_;++n)if(kScales[n]==scale)scale_index_=n;}
    // Highest scale offered (1 when internal resolution is not available).
    void set_scale_limit(unsigned limit){scale_count_=std::clamp(limit,1u,static_cast<unsigned>(kScales.size()));scale_index_=std::min(scale_index_,scale_count_-1u);}
    bool fullscreen()const{return fullscreen_;}
    void set_fullscreen(bool on){fullscreen_=on;}
    const char* language()const{return japanese_?"ja":"en";}
    void set_language(const std::string& id){japanese_=id=="ja";}
    // Whether both languages are installed (one disc only: the column stays on it).
    void set_language_choice(bool both){language_choice_=both;}
    // The Settings screen drew its Video row this frame (call before frame_end).
    bool screen_visible()const{return label_seen_;}
    static constexpr unsigned kRateChanged=1,kScaleChanged=2,kScreenChanged=4,kLanguageChanged=8;
    // Controller filter: on the Video row Circle switches column, Left/Right
    // change its value (on press); Left/Right/Circle are hidden from the
    // game.  Returns which value changed (kRateChanged/kScaleChanged).
    unsigned filter(std::uint32_t& buttons){
        constexpr std::uint32_t left=0x80u,right=0x20u,circle=0x2000u;
        unsigned changed=0;
        if(selected_){
            const auto pressed=buttons&~previous_;
            if(pressed&circle)column_=(column_+1u)%4u;
            const int step=(pressed&right)?1:(pressed&left)?-1:0;
            if(step && column_==3 && language_choice_){japanese_=!japanese_;changed=kLanguageChanged;}
            if(step && column_==2){fullscreen_=!fullscreen_;changed=kScreenChanged;}
            if(step && !column_){rate_index_=static_cast<unsigned>((rate_index_+kRates.size()+step)%kRates.size());changed=kRateChanged;}
            if(step && column_==1 && scale_count_>1){scale_index_=(scale_index_+scale_count_+step)%scale_count_;changed=kScaleChanged;}
            previous_=buttons;buttons&=~(left|right|circle);
        }else previous_=buttons;
        return changed;
    }
private:
    static constexpr int help_top_=189,help_center_=239,kRateCentre=222,kScaleCentre=286,kScreenCentre=354,kLanguageCentre=434;
    struct Quad{std::uint16_t u0,v0,u1,v1;std::int16_t x0,y0,x1,y1;std::uint16_t color,z;};
    // Inside the help box (both the EN and JP texts, whatever font they use).
    static bool in_help_box(const Quad& q){return q.x0>=48 && q.x1<=432 && q.y0>=help_top_-4 && q.y1<=help_top_+52;}
    struct Text{unsigned u,width;};
    psprecomp::GuestMemory& memory_;std::uint32_t scratch_,size_;
    std::uint32_t sheet_=0,previous_=0;GeState sheet_state_{};
    int label_y_=0,label_x_=0;bool label_seen_=false,selected_now_=false,selected_=false,prepared_=false,help_drawn_=false;
    unsigned rate_index_=0,scale_index_=0,scale_count_=1,column_=0,vertex_slot_=0;bool fullscreen_=false,japanese_=false,language_choice_=true;std::array<unsigned,2> screen_widths_{},language_widths_{};std::array<unsigned,4> scale_widths_{};
    bool read_quad(std::uint32_t at,Quad& q) const{
        if(!memory_.contains(at,48))return false;
        const auto u=[&](unsigned n,unsigned o){return memory_.load16(at+n*12u+o);};
        const auto s=[&](unsigned n,unsigned o){return static_cast<std::int16_t>(memory_.load16(at+n*12u+o));};
        // triangle strip of one sprite: (x0,y0) (x0,y1) (x1,y0) (x1,y1)
        if(s(0,6)!=s(1,6) || s(2,6)!=s(3,6) || s(0,8)!=s(2,8) || s(1,8)!=s(3,8))return false;
        q={u(0,0),u(0,2),u(3,0),u(3,2),s(0,6),s(0,8),s(3,6),s(3,8),u(0,4),u(0,10)};return true;
    }
    void write_quad(std::uint32_t at,const Quad& q){
        const std::array<std::array<std::int32_t,4>,4> corners{{{q.u0,q.v0,q.x0,q.y0},{q.u0,q.v1,q.x0,q.y1},{q.u1,q.v0,q.x1,q.y0},{q.u1,q.v1,q.x1,q.y1}}};
        for(unsigned n=0;n<4;++n){const auto v=at+n*12u;
            memory_.store16(v,static_cast<std::uint16_t>(corners[n][0]));memory_.store16(v+2,static_cast<std::uint16_t>(corners[n][1]));memory_.store16(v+4,q.color);
            memory_.store16(v+6,static_cast<std::uint16_t>(corners[n][2]));memory_.store16(v+8,static_cast<std::uint16_t>(corners[n][3]));memory_.store16(v+10,q.z);}
    }
    std::uint32_t next_vertices(){const auto at=scratch_+kTexBytes+vertex_slot_*48u;vertex_slot_=(vertex_slot_+1u)%kVertexSlots;return at;}
    // Draws a region of the port texture (RGBA8888, linear) over the quad.
    void draw_own(const GeState& source,const Quad& where,unsigned u,unsigned v,unsigned width,unsigned height,const Draw& render){
        GeState copy=source;auto& r=copy.registers;
        r[0xA0]=(r[0xA0]&0xFF000000u)|(scratch_&0xFFFFF0u);
        r[0xA8]=(r[0xA8]&0xFF000000u)|((scratch_>>8u)&0x0F0000u)|kTexWidth;
        r[0xB8]=(r[0xB8]&0xFF000000u)|8u|(7u<<8u); // 256 x 128
        r[0xC3]=(r[0xC3]&0xFFFFFFF0u)|3u;          // ABGR8888
        r[0xC2]=r[0xC2]&~1u;                         // not swizzled
        Quad q=where;q.u0=static_cast<std::uint16_t>(u);q.v0=static_cast<std::uint16_t>(v);q.u1=static_cast<std::uint16_t>(u+width);q.v1=static_cast<std::uint16_t>(v+height);
        const auto at=next_vertices();write_quad(at,q);copy.vertex_address=at;render(copy,4,4);
    }
    static Text value_rect(unsigned index){
        static constexpr std::array<Text,4> rects{{{0,24},{40,24},{80,36},{128,36}}};return rects[index];
    }
    // Resolution values: row 32, 64 texels per scale.
    Text scale_rect(unsigned index)const{return {index*64u,std::max(1u,scale_widths_[index])};}
    // Screen mode words: row 96, "Window" at 0 and "Full" at 64.
    Text screen_rect(bool full)const{return {full?64u:0u,std::max(1u,screen_widths_[full?1:0])};}
    // Language names: row 112, "English" at 0 and the Japanese name at 64.
    Text language_rect(bool japanese)const{return {japanese?64u:0u,std::max(1u,language_widths_[japanese?1:0])};}
    // Help text: only characters of the game's 8x16 help font (no . , :).
    std::string help_line(int row)const{
        static const char* rate[3]={
            "Frame rate - 30 is the original - 60/120/165",
            "are computed for real (no interpolation)",
            "Left/Right change it - Circle next column"};
        static const char* scale[3]={
            "Internal resolution - 272p is the original",
            "higher draws the 3D sharper (needs more GPU)",
            "Left/Right change it - Circle next column"};
        static const char* screen[3]={
            "Screen mode - window or full screen",
            "F11 (or Alt and Enter) also switches it",
            "Left/Right change it - Circle next column"};
        static const char* language[3]={
            "Language - English or Japanese",
            "the game restarts in it after Settings",
            "Left/Right change it - Circle next column"};
        static const char* language_one[3]={
            "Language - install the other disc to change it",
            "with one disc the language is fixed",
            "the game restarts after Settings"};
        return row>=0 && row<3?(column_==0?rate:column_==1?scale:column_==2?screen:(language_choice_?language:language_one))[row]:"";
    }
    unsigned help_glyph(int row,int col)const{
        if(row<0 || row>2 || col<0)return 0;const std::string line=help_line(row);
        if(col>=static_cast<int>(line.size()))return 0;const char c=line[col];
        if(c>='0' && c<='9')return 1u+(c-'0');
        if(c>='A' && c<='U')return 11u+(c-'A');
        if(c>='V' && c<='Z')return 32u+(c-'V');
        if(c>='a' && c<='z')return 37u+(c-'a');
        switch(c){case '-':return 63u;case '\'':return 68u;case '(':return 71u;case ')':return 72u;case '/':return 73u;case '&':return 74u;case '!':return 75u;case '?':return 76u;default:return 0u;}
    }
    // Sheet texel (CLUT8, PSP swizzled) through the palette loaded for it.
    std::uint32_t sheet_texel(unsigned x,unsigned y) const{
        const unsigned bx=x/16u,by=y/8u,blocks=256u/16u;
        const auto offset=((by*blocks+bx)*8u+(y%8u))*16u+(x%16u);
        const auto index=memory_.load8(sheet_+offset);
        std::uint32_t c=0;std::memcpy(&c,sheet_state_.clut.data()+index*4u,4);return c;
    }
    bool prepare(){
        if(prepared_)return true;
        if(size_<kScratchBytes || !memory_.contains(scratch_,kScratchBytes) || !memory_.contains(sheet_,256u*256u))return false;
        std::vector<std::uint32_t> pixels(kTexWidth*kTexHeight,0u);
        render_label(pixels);
        // Values from the game's own digits (sheet row 50..66, 12 px pitch from u=90).
        const auto put_digits=[&](unsigned x,const char* digits){
            for(;*digits;++digits,x+=12u){const unsigned du=90u+12u*static_cast<unsigned>(*digits-'0');
                for(unsigned y=0;y<16;++y)for(unsigned dx=0;dx<12;++dx)pixels[(16u+y)*kTexWidth+x+dx]=sheet_texel(du+dx,50u+y)&0xFF000000u;}
        };
        put_digits(0,"30");put_digits(40,"60");put_digits(80,"120");put_digits(128,"165");
        // Resolutions: the game's digits and a "p" in their style.
        const auto put_row32=[&](unsigned x,const char* digits){
            for(;*digits;++digits,x+=12u){const unsigned du=90u+12u*static_cast<unsigned>(*digits-'0');
                for(unsigned y=0;y<16;++y)for(unsigned dx=0;dx<12;++dx)pixels[(32u+y)*kTexWidth+x+dx]=sheet_texel(du+dx,50u+y)&0xFF000000u;}
            return x;
        };
        for(unsigned n=0;n<kScales.size();++n){const auto digits=std::to_string(272u*kScales[n]);const auto end=put_row32(n*64u,digits.c_str());scale_widths_[n]=end-n*64u+put_text(pixels,end,32u,L"p");}
        screen_widths_={put_text(pixels,0,96u,L"Window"),put_text(pixels,64,96u,L"Full")};
        language_widths_={put_text(pixels,0,112u,L"English"),put_text(pixels,64,112u,L"\u65E5\u672C\u8A9E",L"Meiryo")};
        // Column frame: 2-texel green outline with rounded corners, 64x20.
        for(unsigned y=0;y<20;++y)for(unsigned x=0;x<64;++x){
            const bool edge=x<2 || x>=62 || y<2 || y>=18,corner=(x<1 || x>=63) && (y<1 || y>=19);
            if(edge && !corner)pixels[(64u+y)*kTexWidth+x]=0xFF3CDC8Cu;}
        for(std::size_t n=0;n<pixels.size();++n)memory_.store32(scratch_+static_cast<std::uint32_t>(n*4u),pixels[n]);
        prepared_=true;return true;
    }
    // Black text (alpha only, like the game's digits) at (x,y), 16 rows tall,
    // in the label's rounded font; returns its width (at most 64).
    static unsigned put_text(std::vector<std::uint32_t>& pixels,unsigned x,unsigned y,const wchar_t* text,const wchar_t* face=L"Arial Rounded MT Bold"){
        constexpr int box=64;
        BITMAPINFO info{};info.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);info.bmiHeader.biWidth=box;info.bmiHeader.biHeight=-16;info.bmiHeader.biPlanes=1;info.bmiHeader.biBitCount=32;info.bmiHeader.biCompression=BI_RGB;
        HDC dc=CreateCompatibleDC(nullptr);void* bits=nullptr;HBITMAP bitmap=CreateDIBSection(dc,&info,DIB_RGB_COLORS,&bits,nullptr,0);
        if(!dc || !bitmap || !bits){if(bitmap)DeleteObject(bitmap);if(dc)DeleteDC(dc);return 0;}
        const auto old=SelectObject(dc,bitmap);std::memset(bits,0,box*16*4);
        HFONT font=CreateFontW(-16,0,0,0,FW_HEAVY,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,NONANTIALIASED_QUALITY,DEFAULT_PITCH,face);
        const auto oldfont=SelectObject(dc,font);SetBkMode(dc,TRANSPARENT);SetTextColor(dc,RGB(0,255,0));
        const int length=static_cast<int>(std::wcslen(text));SIZE size{};GetTextExtentPoint32W(dc,text,length,&size);
        TextOutW(dc,0,-2,text,length);GdiFlush();
        const unsigned width=std::min<unsigned>(box,static_cast<unsigned>(size.cx));
        const auto* src=static_cast<const std::uint32_t*>(bits);
        for(unsigned row=0;row<16;++row)for(unsigned col=0;col<width;++col)if((src[row*box+col]&0x0000FF00u) && x+col<kTexWidth)pixels[(y+row)*kTexWidth+x+col]=0xFF000000u;
        SelectObject(dc,oldfont);if(font)DeleteObject(font);SelectObject(dc,old);DeleteObject(bitmap);DeleteDC(dc);
        return width;
    }
    // "Video" in the label style (white, dark outline) inside a 108x16 cell.
    static void render_label(std::vector<std::uint32_t>& pixels){
        BITMAPINFO info{};info.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);info.bmiHeader.biWidth=108;info.bmiHeader.biHeight=-16;info.bmiHeader.biPlanes=1;info.bmiHeader.biBitCount=32;info.bmiHeader.biCompression=BI_RGB;
        HDC dc=CreateCompatibleDC(nullptr);void* bits=nullptr;HBITMAP bitmap=CreateDIBSection(dc,&info,DIB_RGB_COLORS,&bits,nullptr,0);
        if(!dc || !bitmap || !bits){if(bitmap)DeleteObject(bitmap);if(dc)DeleteDC(dc);return;}
        const auto old=SelectObject(dc,bitmap);std::memset(bits,0,108*16*4);
        HFONT font=CreateFontW(-15,0,0,0,FW_HEAVY,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,NONANTIALIASED_QUALITY,DEFAULT_PITCH,L"Arial Rounded MT Bold");
        const auto oldfont=SelectObject(dc,font);SetBkMode(dc,TRANSPARENT);
        RECT box{0,0,108,16};
        // outline: mark pixels with red, fill with green, then build RGBA
        SetTextColor(dc,RGB(255,0,0));
        for(int dy=-1;dy<=1;++dy)for(int dx=-1;dx<=1;++dx){RECT b{box.left+dx,box.top+dy,box.right+dx,box.bottom+dy};DrawTextW(dc,L"Video",5,&b,DT_CENTER|DT_VCENTER|DT_SINGLELINE);}
        SetTextColor(dc,RGB(0,255,0));DrawTextW(dc,L"Video",5,&box,DT_CENTER|DT_VCENTER|DT_SINGLELINE);GdiFlush();
        const auto* src=static_cast<const std::uint32_t*>(bits);
        for(unsigned y=0;y<16;++y)for(unsigned x=0;x<108;++x){const auto p=src[y*108+x];
            std::uint32_t out=0;if(p&0x0000FF00u)out=0xFFFFFFFFu;else if(p&0x00FF0000u)out=0xFF505050u; // ABGR: white fill, dark grey outline
            pixels[y*kTexWidth+x]=out;}
        SelectObject(dc,oldfont);if(font)DeleteObject(font);SelectObject(dc,old);DeleteObject(bitmap);DeleteDC(dc);
    }
};
}
#endif // __ANDROID__
