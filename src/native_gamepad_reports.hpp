#pragma once
#include <array>
#include <cstdint>
#include <span>
// Report parsers for game controllers read through Raw Input (HID), kept free
// of Windows headers so they are testable with recorded report layouts.
// Buttons are mapped by position onto the PSP pad: the bottom face button is
// Cross, the right one Circle, the left one Square and the top one Triangle
// (Nintendo B/A/Y/X, PlayStation as printed). Xbox pads (XInput) swap the
// bottom pair so A confirms like the game's Circle and B goes back.
namespace kon {
enum class InputStyle : std::uint8_t {Keyboard,Xbox,PlayStation,Nintendo,Generic};
inline const char* input_style_name(InputStyle s){
    switch(s){case InputStyle::Keyboard:return "keyboard";case InputStyle::Xbox:return "xbox";case InputStyle::PlayStation:return "playstation";case InputStyle::Nintendo:return "nintendo";default:return "generic";}
}
namespace psp_button {
inline constexpr std::uint32_t select=0x1,start=0x8,up=0x10,right=0x20,down=0x40,left=0x80,l=0x100,r=0x200,triangle=0x1000,circle=0x2000,cross=0x4000,square=0x8000;
}
struct PadState {std::uint32_t buttons=0;std::uint8_t x=128,y=128;bool valid=false;};
// Hat switch value 0..7 clockwise from up; anything else is centered.
inline std::uint32_t hat_buttons(unsigned hat){
    using namespace psp_button;
    static constexpr std::array<std::uint32_t,8> map{up,up|right,right,down|right,down,down|left,left,up|left};
    return hat<8?map[hat]:0u;
}
enum class KnownPad : std::uint8_t {None,DualShock4,DualSense,SwitchPro};
inline KnownPad known_pad(std::uint16_t vendor,std::uint16_t product){
    if(vendor==0x054C){
        if(product==0x05C4 || product==0x09CC || product==0x0BA0)return KnownPad::DualShock4;
        if(product==0x0CE6 || product==0x0DF2)return KnownPad::DualSense;
    }
    if(vendor==0x057E && (product==0x2009 || product==0x2006 || product==0x2007 || product==0x2017))return KnownPad::SwitchPro;
    return KnownPad::None;
}
inline InputStyle style_of(KnownPad pad){
    switch(pad){case KnownPad::DualShock4:case KnownPad::DualSense:return InputStyle::PlayStation;case KnownPad::SwitchPro:return InputStyle::Nintendo;default:return InputStyle::Generic;}
}
// Sony face/shoulder bytes shared by DS4 and DualSense: hat in the low
// nibble, Square/Cross/Circle/Triangle in bits 4-7; L1 R1 L2 R2 Share Options.
inline PadState sony_buttons(std::uint8_t lx,std::uint8_t ly,std::uint8_t face,std::uint8_t shoulders){
    using namespace psp_button;PadState s;s.valid=true;s.x=lx;s.y=ly;
    s.buttons=hat_buttons(face&15u);
    if(face&0x10)s.buttons|=square;if(face&0x20)s.buttons|=cross;if(face&0x40)s.buttons|=circle;if(face&0x80)s.buttons|=triangle;
    if(shoulders&0x01)s.buttons|=l;if(shoulders&0x02)s.buttons|=r;if(shoulders&0x10)s.buttons|=select;if(shoulders&0x20)s.buttons|=start;
    return s;
}
// DualShock 4: USB/basic Bluetooth report 0x01 (sticks at 1..4, buttons at
// 5..6) and extended Bluetooth report 0x11 (the same two bytes later).
inline PadState parse_dualshock4(std::span<const std::uint8_t> r){
    if(r.size()>=10 && r[0]==0x01)return sony_buttons(r[1],r[2],r[5],r[6]);
    if(r.size()>=12 && r[0]==0x11)return sony_buttons(r[3],r[4],r[7],r[8]);
    return {};
}
// DualSense: USB report 0x01 (sticks 1..4, triggers 5..6, buttons 8..9),
// Bluetooth extended report 0x31 (one byte later), basic Bluetooth report 0x01
// (DualShock 4 layout, shorter than the USB one).
inline PadState parse_dualsense(std::span<const std::uint8_t> r){
    if(r.size()>=11 && r[0]==0x01 && r.size()>=32)return sony_buttons(r[1],r[2],r[8],r[9]);
    if(r.size()>=10 && r[0]==0x01)return sony_buttons(r[1],r[2],r[5],r[6]);
    if(r.size()>=12 && r[0]==0x31)return sony_buttons(r[2],r[3],r[9],r[10]);
    return {};
}
// Nintendo Switch Pro Controller: full report 0x30 (buttons at 3..5, 12-bit
// sticks from 6) and the simple Bluetooth HID report 0x3F (buttons at 1..2,
// hat at 3, 16-bit sticks from 4).
inline PadState parse_switch_pro(std::span<const std::uint8_t> d){
    using namespace psp_button;PadState s;
    if(d.size()>=12 && d[0]==0x30){
        s.valid=true;const auto right_buttons=d[3],shared=d[4],left_buttons=d[5];
        if(right_buttons&0x01)s.buttons|=square;   // Y (left)
        if(right_buttons&0x02)s.buttons|=triangle; // X (top)
        if(right_buttons&0x04)s.buttons|=cross;    // B (bottom)
        if(right_buttons&0x08)s.buttons|=circle;   // A (right)
        if(right_buttons&0x40)s.buttons|=psp_button::r;
        if(shared&0x01)s.buttons|=select;if(shared&0x02)s.buttons|=start;
        if(left_buttons&0x01)s.buttons|=down;if(left_buttons&0x02)s.buttons|=up;if(left_buttons&0x04)s.buttons|=right;if(left_buttons&0x08)s.buttons|=left;
        if(left_buttons&0x40)s.buttons|=psp_button::l;
        const unsigned lx=d[6]|((d[7]&15u)<<8),ly=(d[7]>>4)|(d[8]<<4);
        s.x=static_cast<std::uint8_t>(lx>>4);s.y=static_cast<std::uint8_t>(255u-(ly>>4));
        return s;
    }
    if(d.size()>=12 && d[0]==0x3F){
        s.valid=true;const auto b1=d[1],b2=d[2];
        if(b1&0x01)s.buttons|=cross;if(b1&0x02)s.buttons|=circle;if(b1&0x04)s.buttons|=square;if(b1&0x08)s.buttons|=triangle;
        if(b1&0x10)s.buttons|=psp_button::l;if(b1&0x20)s.buttons|=psp_button::r;
        if(b2&0x01)s.buttons|=select;if(b2&0x02)s.buttons|=start;
        s.buttons|=hat_buttons(d[3]);
        const unsigned lx=d[4]|(d[5]<<8),ly=d[6]|(d[7]<<8);
        s.x=static_cast<std::uint8_t>(lx>>8);s.y=static_cast<std::uint8_t>(ly>>8);
        return s;
    }
    return s;
}
// Generic HID gamepads (DirectInput style): button usages are 1-based. The
// default follows the common DirectInput numbering (1 left, 2 bottom,
// 3 right, 4 top, 5 L1, 6 R1, 9 select, 10 start); per-device files can remap.
struct GenericMapping {std::array<std::uint32_t,16> buttons{psp_button::square,psp_button::cross,psp_button::circle,psp_button::triangle,psp_button::l,psp_button::r,psp_button::l,psp_button::r,psp_button::select,psp_button::start,0,0,0,0,0,0};};
inline PadState generic_pad(std::span<const unsigned> pressed_usages,unsigned hat,int x,int y,int x_min,int x_max,int y_min,int y_max,const GenericMapping& m={}){
    PadState s;s.valid=true;
    for(const auto usage:pressed_usages)if(usage>=1 && usage<=m.buttons.size())s.buttons|=m.buttons[usage-1];
    s.buttons|=hat_buttons(hat);
    const auto scale=[](int v,int lo,int hi){if(hi<=lo)return std::uint8_t(128);const long long t=(static_cast<long long>(v)-lo)*255/(static_cast<long long>(hi)-lo);return static_cast<std::uint8_t>(t<0?0:t>255?255:t);};
    s.x=scale(x,x_min,x_max);s.y=scale(y,y_min,y_max);
    return s;
}
}
