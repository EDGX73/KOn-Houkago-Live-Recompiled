// K-On Houkago Live Recompiled.exe: the only program the player opens.
//  - First time: asks for the player's ISO and installs it (native_install.hpp).
//  - Builds the game from it on this PC when needed (native_build.hpp): the
//    first time, and after an update of the package.
//  - Offers new releases from the official GitHub (native_update.hpp).
//  - Starts the game (bin/), with everything kept in this folder.
// A console window appears only while installing or building.
#include "kon_version.h"
#include "native_install.hpp"
#include "native_build.hpp"
#include "native_update.hpp"
#include <cstdio>
#include <future>
#include <iostream>
#include <string>
#include <vector>

namespace {
bool console_open=false;
void open_console(){
    if(console_open)return;
    console_open=true;AllocConsole();SetConsoleOutputCP(CP_UTF8);SetConsoleTitleW(L"K-On! Houkago Live!! Recompiled " KON_VERSION_WIDE);
    FILE* f=nullptr;freopen_s(&f,"CONOUT$","w",stdout);freopen_s(&f,"CONOUT$","w",stderr);freopen_s(&f,"CONIN$","r",stdin);
    std::cout.clear();std::cerr.clear();std::cin.clear();
    std::cout<<"K-On! Houkago Live!! Recompiled " KON_VERSION_STRING "\n\n";
}
void wait_key(){std::cout<<"\nPulsa Intro para cerrar."<<std::flush;std::string line;std::getline(std::cin,line);}
std::string setting(const std::filesystem::path& file,const std::string& key){
    std::ifstream in(file);std::string line,value;
    while(std::getline(in,line))if(line.rfind(key+"=",0)==0)value=line.substr(key.size()+1);
    while(!value.empty() && (value.back()=='\r' || value.back()==' '))value.pop_back();
    return value;
}
// Offers a newer release from GitHub; true when it was installed and the new
// launcher started instead. Never delays the game more than 4 s.
bool offer_update(const std::filesystem::path& root,int argc,wchar_t** argv){
    if(setting(root/"config"/"settings.ini","updates")=="0")return false;
    auto check=std::async(std::launch::async,[]{try{return kon::latest_release();}catch(const std::exception&){return std::optional<kon::ReleaseInfo>{};}});
    if(check.wait_for(std::chrono::seconds(4))!=std::future_status::ready){new auto(std::move(check));return false;}  // left running: the process ends soon
    const auto release=check.get();if(!release)return false;
    const auto latest=kon::Version::parse(release->tag),current=kon::Version::parse(KON_VERSION_STRING);
    if(!latest || !current || !(*current<*latest))return false;
    const auto skipped=root/"config"/"actualizacion_omitida.txt";
    if(setting(skipped,"version")==release->tag)return false;
    const std::wstring text=L"Hay una versión nueva: "+kon::widen(release->tag)+L" (tienes la " KON_VERSION_WIDE L").\n\n¿Actualizar ahora? Tus partidas y ajustes se conservan.\n\n(Si dices que no, no volverá a preguntar hasta la siguiente versión.)";
    if(!_wgetenv(L"KON_UPDATE_YES") && MessageBoxW(nullptr,text.c_str(),L"K-On! Houkago Live!! Recompiled",MB_YESNO|MB_ICONINFORMATION)!=IDYES){  // KON_UPDATE_YES: tests
        std::filesystem::create_directories(skipped.parent_path());std::ofstream(skipped)<<"version="<<release->tag<<"\n";return false;
    }
    open_console();
    kon::install_update(root,*release);
    // The new launcher rebuilds the game and starts it.
    wchar_t module[MAX_PATH]{};GetModuleFileNameW(nullptr,module,MAX_PATH);
    std::wstring command=kon::quoted(module);
    for(int i=1;i<argc;++i)command+=L" "+kon::quoted(argv[i]);
    STARTUPINFOW si{};si.cb=sizeof si;PROCESS_INFORMATION pi{};
    if(!CreateProcessW(nullptr,command.data(),nullptr,nullptr,FALSE,0,nullptr,root.wstring().c_str(),&si,&pi))throw std::runtime_error("Actualizado: vuelve a abrir el juego");
    CloseHandle(pi.hThread);CloseHandle(pi.hProcess);
    return true;
}
}

int wmain(int argc,wchar_t** argv){
    wchar_t module[MAX_PATH]{};GetModuleFileNameW(nullptr,module,MAX_PATH);
    const auto root=std::filesystem::path(module).parent_path();
    std::vector<std::filesystem::path> isos;std::vector<std::wstring> game_args;bool rebuild=false,launch=true;
    for(int i=1;i<argc;++i){
        const std::wstring a=argv[i];
        if(a==L"--rebuild")rebuild=true;
        else if(a==L"--no-launch")launch=false;  // install and build only (tests)
        else if(a==L"ja" || a==L"en")game_args.push_back(L"--language "+a);
        else if(std::filesystem::is_regular_file(a))isos.emplace_back(a);  // an ISO dropped on the program
    }
    try{
        {auto old=std::filesystem::path(module);old+=L".old";std::error_code ignore;std::filesystem::remove(old,ignore);}  // left by an update
        kon::GameBuild build{root};
        if(launch && offer_update(root,argc,argv))return 0;
        if(!std::filesystem::is_regular_file(build.elf()) || !isos.empty()){
            open_console();
            std::cout<<"Instalación: solo necesitas tu ISO de K-On! Houkago Live!! (japonesa o la traducción inglesa).\n";
            if(kon::install_from_isos(root,isos)){wait_key();return 1;}
        }
        if(rebuild)std::filesystem::remove(build.cache()/"built_with.txt");
        if(!build.up_to_date()){
            open_console();
            std::cout<<"\nPreparando el juego para tu PC. Solo pasa la primera vez y tras una actualización.\n"<<std::flush;
            const auto start=GetTickCount64();
            build.build();
            std::cout<<"\nJuego listo en "<<(GetTickCount64()-start+500)/1000<<" s.\n"<<std::flush;
        }
        if(!launch)return 0;
        std::wstring command=kon::quoted(build.game())+L" --root "+kon::quoted(root)+L" --window --hold-on-stop 300000";
        for(const auto& a:game_args)command+=L" "+a;
        // Extra game options for tests (for example --mute-audio).
        if(const wchar_t* extra=_wgetenv(L"KON_GAME_EXTRA_ARGS"))command+=std::wstring(L" ")+extra;
        STARTUPINFOW si{};si.cb=sizeof si;PROCESS_INFORMATION pi{};
        if(!CreateProcessW(nullptr,command.data(),nullptr,nullptr,FALSE,CREATE_NO_WINDOW,nullptr,root.wstring().c_str(),&si,&pi))
            throw std::runtime_error("No se pudo abrir el juego");
        CloseHandle(pi.hThread);CloseHandle(pi.hProcess);
        return 0;
    }catch(const std::exception& e){
        open_console();
        std::cout<<"\nERROR: "<<e.what()<<"\n";wait_key();return 1;
    }
}
