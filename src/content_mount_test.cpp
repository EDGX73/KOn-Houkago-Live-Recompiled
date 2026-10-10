#include "content_mount.hpp"
#include "native_mods.hpp"
#include "native_image_wic.hpp"
#include <fstream>
#include <iostream>
#include <string>
#include <functional>

int main(int argc, char** argv) {
    try {
        if (argc != 2) throw std::runtime_error("Usage: content_mount_test <project root>");
        const auto root = std::filesystem::path(argv[1]);
        const auto require = [](bool value, const char* message) { if (!value) throw std::runtime_error(message); };
        kon::ContentMount mount(root, "en");
        const auto english = mount.resolve("disc0:/PSP_GAME/USRDIR/std.cpk");
        const auto sound = mount.resolve("disc0:/PSP_GAME/USRDIR/snd.cpk");
        mount.select("ja");
        require(mount.resolve("umd0:/PSP_GAME/USRDIR/std.cpk") != english, "Language archive must change");
        require(mount.resolve("disc0:/PSP_GAME/USRDIR/snd.cpk") == sound, "Sound must remain shared");
        const auto exe_jp = mount.resolve("disc0:/PSP_GAME/SYSDIR/EBOOT.BIN");
        mount.select("en");
        require(std::filesystem::file_size(exe_jp) == std::filesystem::file_size(mount.resolve("disc0:/PSP_GAME/SYSDIR/EBOOT.BIN")), "Executable sizes differ");
        for (const auto path : {"disc0:/../secret", "ms0:/save", "disc0:/PSP_GAME/C:/file"}) {
            bool rejected = false;
            try { (void)mount.resolve(path); } catch (const std::exception&) { rejected = true; }
            require(rejected, "Invalid disc path was accepted");
        }
        const auto settings = root / "work/tests/language.ini";
        mount.save_setting(settings);
        require(kon::ContentMount::read_setting(settings) == "en", "English setting did not persist");
        mount.select("ja");
        mount.save_setting(settings);
        require(kon::ContentMount::read_setting(settings) == "ja", "Japanese setting did not persist");
        bool missing_spanish = false;
        try { mount.select("es"); } catch (const std::exception&) { missing_spanish = true; }
        require(missing_spanish, "Spanish pack unexpectedly exists");
        require(mount.language() == "ja", "Failed locale selection changed active locale");
        // Synthetic archives in an isolated root exercise adding languages
        // without shipping or altering original commercial resources.
        const auto fixture=root/"work/tests/locale_extension";
        const auto fake_archive=[&](const std::string& folder,const std::string& file){
            const auto path=fixture/folder/"PSP_GAME/USRDIR"/file;
            std::filesystem::create_directories(path.parent_path());std::ofstream out(path,std::ios::binary);out<<"synthetic test resource";
        };
        fake_archive("analysis/extracted/jp","std.cpk");fake_archive("analysis/extracted/jp","snd.cpk");
        fake_archive("analysis/extracted/en","std.cpk");fake_archive("assets/locales/es","std.cpk");
        fake_archive("assets/locales/pt-br","std.cpk");
        kon::ContentMount extensible(fixture,"es");
        require(extensible.resolve("disc0:/PSP_GAME/USRDIR/std.cpk")==std::filesystem::weakly_canonical(fixture/"assets/locales/es/PSP_GAME/USRDIR/std.cpk"),"Spanish pack not selected");
        require(extensible.resolve("disc0:/PSP_GAME/USRDIR/snd.cpk")==std::filesystem::weakly_canonical(fixture/"analysis/extracted/jp/PSP_GAME/USRDIR/snd.cpk"),"Custom language duplicated shared sound");
        extensible.select("pt-br");extensible.save_setting(fixture/"settings.ini");
        require(kon::ContentMount::read_setting(fixture/"settings.ini")=="pt-br","Added language setting did not persist");
        require(extensible.available_languages()==std::vector<std::string>{"en","es","ja","pt-br"},"Language discovery omitted added packs");
        for(const auto id:{"../es","ES","es/other","-es","es-",""}){
            bool rejected=false;try{extensible.select(id);}catch(const std::exception&){rejected=true;}
            require(rejected && extensible.language()=="pt-br","Invalid language changed active pack");
        }
        // Mods by folders: name order is priority (the later wins), '-' disables.
        fake_archive("mods/10-first/disc","std.cpk");fake_archive("mods/20-second/disc","std.cpk");fake_archive("mods/-off/disc","snd.cpk");
        { const auto texture=fixture/"mods/10-first/textures/00112233445566ff_2x1.bmp";std::filesystem::create_directories(texture.parent_path());
          std::ofstream out(texture,std::ios::binary);const unsigned char bmp[62]{'B','M',62,0,0,0,0,0,0,0,54,0,0,0,40,0,0,0,2,0,0,0,0xFF,0xFF,0xFF,0xFF,1,0,32,0,0,0,0,0,8,0,0,0,0x13,0x0B,0,0,0x13,0x0B,0,0,0,0,0,0,0,0,0,0,0x30,0x20,0x10,0x80,0x01,0x02,0x03,0xFF};
          out.write(reinterpret_cast<const char*>(bmp),sizeof bmp); }
        const auto mods=kon::list_mods(fixture);
        require(mods.size()==3 && mods[0].name=="-off" && !mods[0].enabled && mods[1].name=="10-first" && mods[2].name=="20-second" && mods[2].enabled,"Mod folders, order or disabling");
        std::vector<std::filesystem::path> disc;for(const auto& mod:mods)if(mod.enabled)disc.push_back(mod.path/"disc");
        extensible.set_disc_overrides(disc);
        require(extensible.resolve("disc0:/PSP_GAME/USRDIR/std.cpk")==std::filesystem::weakly_canonical(fixture/"mods/20-second/disc/PSP_GAME/USRDIR/std.cpk"),"Highest-priority mod must win");
        require(extensible.resolve("disc0:/PSP_GAME/USRDIR/snd.cpk")==std::filesystem::weakly_canonical(fixture/"analysis/extracted/jp/PSP_GAME/USRDIR/snd.cpk"),"Disabled mod was used");
        auto& textures=kon::TextureMods::instance();
        require(textures.load(mods,&kon::decode_image_file)==1 && textures.active(),"Texture mod index");
        const auto replaced=textures.find(0x00112233445566ffull,2,1);
        require(replaced && (*replaced)[0]==0x80302010u && (*replaced)[1]==0xFF010203u,"Texture mod pixels (BMP with alpha)");
        require(!textures.find(0x00112233445566ffull,4,4) && !textures.find(1,2,1),"Texture mod size or hash mismatch must not replace");
        std::cout << "PASS: actual JP/EN content mounts, shared sound, persistence, missing locale, path checks and resource mods\n";
        return 0;
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
