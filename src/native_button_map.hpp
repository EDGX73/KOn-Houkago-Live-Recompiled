#pragma once
#include "native_gamepad_reports.hpp"
#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
// Button remapping (config/controls.ini), one section per device type:
//   [keyboard]     circle=X  cross=Z  ...   key names: A-Z, 0-9, ENTER, BACKSPACE,
//                                           SPACE, TAB, SHIFT, CTRL, UP, DOWN, LEFT, RIGHT
//   [xbox]         circle=A  cross=B  ...   A B X Y LB RB MENU VIEW
//   [playstation]  circle=CIRCLE ...        CROSS CIRCLE SQUARE TRIANGLE L1 R1 OPTIONS SHARE
//   [nintendo]     circle=A  cross=B  ...   A B X Y L R PLUS MINUS
//   [generic]      circle=3  cross=2  ...   button numbers 1-16
// PSP buttons: cross circle square triangle l r start select. Unlisted
// buttons keep their defaults; a button may be given to several actions.
// Controllers report buttons by position (native_gamepad_reports.hpp); this
// map routes each physical button to PSP buttons and names the button that
// performs a PSP action, so the on-screen glyphs show the real assignment.
namespace kon {
class ButtonMap {
public:
    struct Physical {const char* name;std::uint32_t position;const char* label;};
    static ButtonMap& instance(){static ButtonMap map;return map;}
    ButtonMap(){reset();}
    void reset(){
        keys_.clear();
        for(const auto& [code,bit]:default_keys())keys_.push_back({code,bit});
        for(auto& route:routes_)for(unsigned i=0;i<16;++i)route[i]=1u<<i;
        generic_=GenericMapping{};
    }
    // Keyboard: virtual-key code -> PSP button.
    const std::vector<std::pair<unsigned,std::uint32_t>>& keys()const{return keys_;}
    // Physical buttons of a controller, by their default PSP position, to the PSP buttons they press.
    std::uint32_t route(InputStyle style,std::uint32_t positional)const{
        if(style==InputStyle::Keyboard || style==InputStyle::Generic)return positional;
        const auto& r=routes_[static_cast<unsigned>(style)];std::uint32_t out=positional&~0xFFFFu;
        for(unsigned i=0;i<16;++i)if(positional&(1u<<i))out|=r[i];
        return out;
    }
    const GenericMapping& generic()const{return generic_;}
    // Name shown for the button that performs a PSP action (nullptr: keep the game's glyph).
    std::string label(InputStyle style,std::uint32_t psp)const{
        switch(style){
        case InputStyle::Keyboard:
            for(const auto& [code,bit]:keys_)if(bit&psp)return key_label(code);
            return "?";
        case InputStyle::Generic:
            for(unsigned i=0;i<generic_.buttons.size();++i)if(generic_.buttons[i]&psp)return std::to_string(i+1);
            return "?";
        case InputStyle::PlayStation:return {};
        default:{
            const auto& r=routes_[static_cast<unsigned>(style)];
            for(const auto& p:physical(style))if(r[bit_index(p.position)]&psp)return p.label;
            return "?";}
        }
    }
    // Reads controls.ini; returns the problems found (unknown names), empty when clean.
    std::vector<std::string> load(const std::filesystem::path& file){
        std::vector<std::string> problems;std::ifstream in(file);if(!in)return problems;
        std::string line,section;
        std::vector<std::pair<std::uint32_t,std::string>> entries;
        const auto flush=[&](){
            if(section.empty())return;
            InputStyle style;
            if(!style_named(section,style)){if(!entries.empty())problems.push_back("unknown section ["+section+"]");entries.clear();return;}
            for(const auto& [psp,name]:entries){
                if(style==InputStyle::Keyboard){
                    const auto code=key_code(name);if(!code){problems.push_back("unknown key "+name);continue;}
                    std::erase_if(keys_,[&](const auto& k){return k.second==psp;});keys_.push_back({code,psp});
                }else if(style==InputStyle::Generic){
                    unsigned number=0;try{number=unsigned(std::stoul(name));}catch(...){}
                    if(number<1 || number>generic_.buttons.size()){problems.push_back("unknown generic button "+name);continue;}
                    for(auto& b:generic_.buttons)b&=~psp;generic_.buttons[number-1]|=psp;
                }else{
                    const auto* p=physical_named(style,name);if(!p){problems.push_back("unknown "+section+" button "+name);continue;}
                    auto& r=routes_[static_cast<unsigned>(style)];
                    for(auto& bits:r)bits&=~psp;r[bit_index(p->position)]|=psp;
                }
            }
            entries.clear();
        };
        while(std::getline(in,line)){
            if(const auto c=line.find_first_of("#;");c!=std::string::npos)line.erase(c);
            line=trim(line);if(line.empty())continue;
            if(line.front()=='[' && line.back()==']'){flush();section=lower(trim(line.substr(1,line.size()-2)));continue;}
            const auto eq=line.find('=');if(eq==std::string::npos){problems.push_back("unreadable line: "+line);continue;}
            const auto action=lower(trim(line.substr(0,eq)));const auto name=upper(trim(line.substr(eq+1)));
            const auto psp=psp_named(action);if(!psp){problems.push_back("unknown PSP button "+action);continue;}
            entries.push_back({psp,name});
        }
        flush();return problems;
    }
    // A commented template with the defaults, written when the file is missing.
    static std::string template_text(){
        return "# Reasignacion de botones del port (se lee al iniciar el juego).\n"
               "# Botones de PSP: cross circle square triangle l r start select\n"
               "# Quita el # de una linea para cambiarla. Ejemplos con los valores por defecto:\n"
               "[keyboard]\n#cross=Z\n#circle=X\n#square=A\n#triangle=S\n#l=Q\n#r=W\n#start=ENTER\n#select=BACKSPACE\n"
               "[xbox]\n#cross=B\n#circle=A\n#square=X\n#triangle=Y\n#l=LB\n#r=RB\n#start=MENU\n#select=VIEW\n"
               "[playstation]\n#cross=CROSS\n#circle=CIRCLE\n#square=SQUARE\n#triangle=TRIANGLE\n#l=L1\n#r=R1\n#start=OPTIONS\n#select=SHARE\n"
               "[nintendo]\n#cross=B\n#circle=A\n#square=Y\n#triangle=X\n#l=L\n#r=R\n#start=PLUS\n#select=MINUS\n"
               "[generic]\n#cross=2\n#circle=3\n#square=1\n#triangle=4\n#l=5\n#r=6\n#start=10\n#select=9\n";
    }
    static constexpr std::array<std::pair<unsigned,std::uint32_t>,12> default_keys(){
        return {{{8,1},{13,8},{38,0x10},{39,0x20},{40,0x40},{37,0x80},{'Q',0x100},{'W',0x200},{'S',0x1000},{'X',0x2000},{'Z',0x4000},{'A',0x8000}}};
    }
    static unsigned key_code(const std::string& name){
        if(name.size()==1 && ((name[0]>='A' && name[0]<='Z') || (name[0]>='0' && name[0]<='9')))return unsigned(name[0]);
        for(const auto& k:named_keys())if(name==k.name)return k.code;
        return 0;
    }
    static std::string key_label(unsigned code){
        if((code>='A' && code<='Z') || (code>='0' && code<='9'))return std::string(1,char(code));
        for(const auto& k:named_keys())if(code==k.code)return k.label;
        return "?";
    }
private:
    struct NamedKey {const char* name;unsigned code;const char* label;};
    static constexpr std::array<NamedKey,12> named_keys(){
        return {{{"ENTER",13,"ENTER"},{"BACKSPACE",8,"BKSP"},{"SPACE",32,"SPACE"},{"TAB",9,"TAB"},{"SHIFT",16,"SHIFT"},{"CTRL",17,"CTRL"},
                 {"UP",38,"UP"},{"DOWN",40,"DOWN"},{"LEFT",37,"LEFT"},{"RIGHT",39,"RIGHT"},{"BKSP",8,"BKSP"},{"RETURN",13,"ENTER"}}};
    }
    static std::span<const Physical> physical(InputStyle style){
        using namespace psp_button;
        static constexpr Physical xbox[]{{"A",circle,"A"},{"B",cross,"B"},{"X",square,"X"},{"Y",triangle,"Y"},{"LB",l,"LB"},{"RB",r,"RB"},{"MENU",start,"MENU"},{"VIEW",select,"VIEW"},{"START",start,"MENU"},{"BACK",select,"VIEW"}};
        static constexpr Physical playstation[]{{"CROSS",cross,""},{"CIRCLE",circle,""},{"SQUARE",square,""},{"TRIANGLE",triangle,""},{"L1",l,""},{"R1",r,""},{"OPTIONS",start,""},{"SHARE",select,""},{"CREATE",select,""}};
        static constexpr Physical nintendo[]{{"A",circle,"A"},{"B",cross,"B"},{"X",triangle,"X"},{"Y",square,"Y"},{"L",l,"L"},{"R",r,"R"},{"PLUS",start,"+"},{"MINUS",select,"-"}};
        switch(style){case InputStyle::Xbox:return xbox;case InputStyle::PlayStation:return playstation;case InputStyle::Nintendo:return nintendo;default:return {};}
    }
    static const Physical* physical_named(InputStyle style,const std::string& name){
        for(const auto& p:physical(style))if(name==p.name)return &p;
        return nullptr;
    }
    static unsigned bit_index(std::uint32_t bit){unsigned i=0;while(i<16 && !(bit&(1u<<i)))++i;return i;}
    static std::uint32_t psp_named(const std::string& action){
        using namespace psp_button;
        static constexpr std::pair<std::string_view,std::uint32_t> names[]{{"cross",cross},{"circle",circle},{"square",square},{"triangle",triangle},{"l",l},{"r",r},{"start",start},{"select",select}};
        for(const auto& [n,bit]:names)if(action==n)return bit;
        return 0;
    }
    static bool style_named(const std::string& section,InputStyle& style){
        static constexpr std::pair<std::string_view,InputStyle> names[]{{"keyboard",InputStyle::Keyboard},{"xbox",InputStyle::Xbox},{"playstation",InputStyle::PlayStation},{"nintendo",InputStyle::Nintendo},{"generic",InputStyle::Generic}};
        for(const auto& [n,s]:names)if(section==n){style=s;return true;}
        return false;
    }
    static std::string trim(const std::string& s){const auto b=s.find_first_not_of(" \t\r");if(b==std::string::npos)return {};return s.substr(b,s.find_last_not_of(" \t\r")-b+1);}
    static std::string lower(std::string s){for(auto& c:s)c=char(std::tolower(static_cast<unsigned char>(c)));return s;}
    static std::string upper(std::string s){for(auto& c:s)c=char(std::toupper(static_cast<unsigned char>(c)));return s;}
    std::vector<std::pair<unsigned,std::uint32_t>> keys_;
    std::array<std::array<std::uint32_t,16>,5> routes_{};
    GenericMapping generic_;
};
}
