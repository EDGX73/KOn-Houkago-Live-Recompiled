#include "native_glyphs.hpp"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string_view>
// Button glyph labels per style and, given textures dumped with
// KON_TEXTURE_DUMP (native_glyph_test <out-folder> <dump.bmp>...), the hash
// check and the redrawn texture for every style, for visual review.
using namespace kon;
void check(bool ok,const char* what){if(!ok)throw std::runtime_error(what);}
bool is(std::uint8_t style,glyph_table::Face face,std::string_view text){return glyph_detail::label(style,face)==text;}
std::vector<std::uint32_t> read_dump(const std::filesystem::path& path,unsigned& width,unsigned& height){
    std::ifstream in(path,std::ios::binary);std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(in)),{});
    check(bytes.size()>=54 && bytes[0]=='B' && bytes[1]=='M',"Not a texture dump");
    const auto u32=[&](std::size_t o){return std::uint32_t(bytes[o])|(std::uint32_t(bytes[o+1])<<8)|(std::uint32_t(bytes[o+2])<<16)|(std::uint32_t(bytes[o+3])<<24);};
    width=u32(18);height=std::uint32_t(-std::int32_t(u32(22)));
    std::vector<std::uint32_t> pixels(std::size_t(width)*height);
    check(bytes.size()>=54+pixels.size()*4,"Truncated texture dump");
    for(std::size_t i=0;i<pixels.size();++i){const auto* p=&bytes[54+i*4];pixels[i]=std::uint32_t(p[2])|(std::uint32_t(p[1])<<8)|(std::uint32_t(p[0])<<16)|(std::uint32_t(p[3])<<24);}
    return pixels;
}
int main(int argc,char** argv){try{
    using glyph_table::Face;
    check(is(1,Face::Cross,"B") && is(1,Face::Circle,"A") && is(1,Face::Square,"X") && is(1,Face::Triangle,"Y"),"Xbox labels (A confirms as Circle)");
    check(is(3,Face::Cross,"B") && is(3,Face::Circle,"A") && is(3,Face::Square,"Y") && is(3,Face::Triangle,"X"),"Nintendo labels");
    check(is(0,Face::Cross,"Z") && is(0,Face::Circle,"X") && is(0,Face::Square,"A") && is(0,Face::Triangle,"S"),"Keyboard labels (default keys)");
    check(glyph_detail::label(2,Face::Cross).empty() && glyph_detail::label(2,Face::Start).empty(),"PlayStation keeps the original glyphs");
    check(is(0,Face::Start,"ENTER") && is(0,Face::Select,"BKSP") && is(1,Face::Start,"MENU") && is(1,Face::Select,"VIEW") && is(3,Face::Start,"+") && is(3,Face::Select,"-"),"START/SELECT labels");
    for(const auto& g:glyph_table::glyphs)for(std::uint8_t style=0;style<5;++style){for(const char c:glyph_detail::label(style,g.face))check(glyph_detail::letter(c)!=nullptr,"Every label has letters");}
    unsigned redrawn=0;
    for(int i=2;i<argc;++i){
        unsigned w=0,h=0;auto pixels=read_dump(argv[i],w,h);
        const auto hash=texture_hash(pixels,w,h);const auto name=std::filesystem::path(argv[i]).stem().string();
        char expected[17];std::snprintf(expected,sizeof expected,"%016llx",static_cast<unsigned long long>(hash));
        check(name.rfind(expected,0)==0,"Dump hash differs from texture_hash");
        if(!has_button_glyphs(hash,w,h))continue;
        static const char* names[]{"keyboard","xbox","playstation","nintendo","generic"};
        for(std::uint8_t style=0;style<5;++style){
            auto copy=pixels;const bool changed=apply_button_glyphs(copy,w,h,hash,style);
            check(changed==(style!=2),"Styles other than PlayStation redraw");
            const auto out=std::filesystem::path(argv[1])/(name+"_"+names[style]);
            dump_decoded_texture_to(copy,w,h,out.string()+".bmp");++redrawn;
        }
    }
    std::cout<<"PASS: button and START/SELECT labels for keyboard, Xbox, Nintendo and generic pads; "<<redrawn<<" styled textures written\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<"\n";return 1;}}
