#include "native_gamepad_reports.hpp"
#include "native_button_map.hpp"
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <vector>
// Report layouts built by hand from the controllers' documented formats; no
// physical controller is involved (hardware behaviour stays to be checked).
using namespace kon;using namespace kon::psp_button;
void check(bool ok,const char* what){if(!ok)throw std::runtime_error(what);}
int main(){try{
    check(known_pad(0x054C,0x09CC)==KnownPad::DualShock4 && known_pad(0x054C,0x0CE6)==KnownPad::DualSense && known_pad(0x057E,0x2009)==KnownPad::SwitchPro && known_pad(0x046D,0xC216)==KnownPad::None,"Controller identification");
    check(style_of(KnownPad::DualSense)==InputStyle::PlayStation && style_of(KnownPad::SwitchPro)==InputStyle::Nintendo && style_of(KnownPad::None)==InputStyle::Generic,"Controller styles");
    // DualShock 4 USB: Cross + hat right, Options, left stick fully left.
    std::vector<std::uint8_t> ds4(64,0);ds4[0]=0x01;ds4[1]=0;ds4[2]=128;ds4[5]=0x20|2;ds4[6]=0x20;
    auto s=parse_dualshock4(ds4);check(s.valid && s.buttons==(cross|right|start) && s.x==0 && s.y==128,"DualShock 4 USB report");
    // DualShock 4 Bluetooth extended report 0x11: Circle + L1, hat centered.
    std::vector<std::uint8_t> ds4bt(78,0);ds4bt[0]=0x11;ds4bt[3]=128;ds4bt[4]=255;ds4bt[7]=0x40|8;ds4bt[8]=0x01;
    s=parse_dualshock4(ds4bt);check(s.valid && s.buttons==(circle|l) && s.y==255,"DualShock 4 Bluetooth report");
    // DualSense USB: Triangle + Square, hat up-left, R1, Create (select).
    std::vector<std::uint8_t> ds5(64,0);ds5[0]=0x01;ds5[1]=ds5[2]=128;ds5[8]=0x80|0x10|7;ds5[9]=0x02|0x10;
    s=parse_dualsense(ds5);check(s.valid && s.buttons==(triangle|square|up|left|r|psp_button::select),"DualSense USB report");  // qualified: POSIX also has ::select
    // DualSense Bluetooth 0x31: Cross only.
    std::vector<std::uint8_t> ds5bt(78,0);ds5bt[0]=0x31;ds5bt[2]=ds5bt[3]=128;ds5bt[9]=0x20|8;
    s=parse_dualsense(ds5bt);check(s.valid && s.buttons==cross,"DualSense Bluetooth report");
    // Switch Pro full report: A (right=Circle), B (bottom=Cross), plus, d-pad left, L.
    std::vector<std::uint8_t> pro(64,0);pro[0]=0x30;pro[3]=0x08|0x04;pro[4]=0x02;pro[5]=0x08|0x40;pro[6]=0x00;pro[7]=0x08;pro[8]=0x80;
    s=parse_switch_pro(pro);check(s.valid && s.buttons==(circle|cross|start|left|l),"Switch Pro full report");
    // Switch Pro simple report 0x3F: Y (left=Square), X (top=Triangle), hat down.
    std::vector<std::uint8_t> simple(12,0);simple[0]=0x3F;simple[1]=0x04|0x08;simple[3]=4;simple[5]=0x80;simple[7]=0x80;
    s=parse_switch_pro(simple);check(s.valid && s.buttons==(square|triangle|down),"Switch Pro simple report");
    // Generic DirectInput pad: buttons 2 and 10, hat right, stick at the ends.
    const std::vector<unsigned> pressed{2,10};
    s=generic_pad(pressed,2,0,1023,0,1023,0,1023);check(s.valid && s.buttons==(cross|start|right) && s.x==0 && s.y==255,"Generic HID gamepad");
    check(hat_buttons(8)==0 && hat_buttons(15)==0,"Centered hat");
    // Remapping (config/controls.ini): defaults, then a file moving buttons around.
    ButtonMap map;
    check(map.route(InputStyle::Xbox,circle|l)==(circle|l) && map.label(InputStyle::Xbox,circle)=="A" && map.label(InputStyle::Keyboard,cross)=="Z" && map.label(InputStyle::Keyboard,start)=="ENTER" && map.label(InputStyle::Generic,square)=="1" && map.label(InputStyle::PlayStation,circle).empty(),"Default button map");
    {std::ofstream ini("controls_test.ini");ini<<"# test\n[keyboard]\ncircle=enter\ncross = K\n[xbox]\ncircle=B\ncross=A ; swap\nl=RB\n[nintendo]\nstart=MINUS\n[generic]\ncross=7\n[xbox]\nbogus=A\nsquare=Q\n";}
    const auto problems=map.load("controls_test.ini");
    check(problems.size()==2,"Unknown names are reported");
    check(map.label(InputStyle::Keyboard,circle)=="ENTER" && map.label(InputStyle::Keyboard,cross)=="K","Keyboard remap and labels");
    bool enter_circle=false,x_gone=true;for(const auto& [code,bit]:map.keys()){if(code==13 && bit==circle)enter_circle=true;if(code=='X' && bit==circle)x_gone=false;}
    check(enter_circle && x_gone,"Keyboard remap replaces the default key");
    // Xbox: physical B (positional Cross) now presses Circle, A presses Cross, RB presses L.
    check(map.route(InputStyle::Xbox,cross)==circle && map.route(InputStyle::Xbox,circle)==cross && (map.route(InputStyle::Xbox,r)&l) && map.label(InputStyle::Xbox,circle)=="B" && map.label(InputStyle::Xbox,l)=="RB","Xbox remap and labels");
    check(map.route(InputStyle::Nintendo,psp_button::select)==(psp_button::select|start) && map.route(InputStyle::Nintendo,start)==0 && map.label(InputStyle::Nintendo,start)=="-","Nintendo remap");
    check((map.generic().buttons[6]&cross) && map.generic().buttons[1]==0 && map.label(InputStyle::Generic,cross)=="7","Generic remap");
    check(!ButtonMap::template_text().empty() && map.route(InputStyle::Xbox,up)==up,"Template and untouched d-pad");
    std::cout<<"PASS: DualShock 4 (USB, Bluetooth), DualSense (USB, Bluetooth), Switch Pro (full, simple) and generic HID report parsing, positional PSP mapping, styles and controls.ini remapping; no physical controller tested\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<"\n";return 1;}}
