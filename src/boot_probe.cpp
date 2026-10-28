#include "content_mount.hpp"
#include "guest_arena.hpp"
#include "thread_abi.hpp"
#include "host_clock.hpp"
#include "host_calendar.hpp"
#include "kernel_mutex.hpp"
#include "kernel_lwmutex.hpp"
#include "kernel_signals.hpp"
#include "native_callback.hpp"
#include "host_power.hpp"
#include "native_files.hpp"
#include "native_savedata.hpp"
#include "ge_queue.hpp"
#include "native_display.hpp"
#include "native_sampling_profiler.hpp"
#include "native_renderer.hpp"
#ifdef KON_HAS_NATIVE_VULKAN
#include "native_vulkan_clear.hpp"
#endif
#include "native_draw_capture.hpp"
#include "native_media.hpp"
#include "native_atrac.hpp"
#include "native_audio.hpp"
#include "native_wait.hpp"
#include "native_network.hpp"
#include "native_controls.hpp"
#include "native_logic_rate.hpp"
#include "native_settings_fps.hpp"
#include "native_port_settings.hpp"
#include "host_headphones.hpp"
#include "control_replay.hpp"
#include "cpk_index.hpp"
#include "native_activity.hpp"
#include "native_discord.hpp"
#include "native_online.hpp"
#ifdef _WIN32
#include "native_install.hpp"
#endif
#ifdef __ANDROID__
#include "../android/game_module_abi.hpp"
#endif
#include <array>
#include <unordered_map>
#include <bit>
#include <cmath>
#include <span>
#include <set>
#include "psprecomp/common.hpp"
#include "psprecomp/elf32.hpp"
#include "psprecomp/runtime.hpp"
#include "psprecomp/sha256.hpp"
#include <algorithm>
#include <sstream>
#include <cstdint>
#include <chrono>
#include <thread>
#include <limits>
#include <iostream>
#include "native_mods.hpp"
#include "native_image_wic.hpp"
#include "native_adhoc.hpp"
#ifdef _WIN32
#include <shellapi.h>
#pragma comment(lib,"shell32.lib")
#endif
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

#ifdef _WIN32
inline unsigned long kon_current_thread_id(){return GetCurrentThreadId();}
#else
#include <unistd.h>
inline unsigned long kon_current_thread_id(){return static_cast<unsigned long>(gettid());}
#endif

// This pinned runtime defines the import-boundary hook in runtime.cpp; its
// public header currently declares the other boundary hooks but omits this one.
namespace psprecomp {
void set_runtime_post_import_hook(void (*hook)(Runtime&,AllegrexContext&)) noexcept;
}
namespace {
constexpr std::uint32_t kReturnPc=0x08000100u;
constexpr std::uint32_t kSettingsFpsScratch=0x08400000u; // kernel RAM, outside the game's partition
constexpr const char* kElfHash="eda890135ea4aee1a8d0a82b2f00a7159060c58498a6705df7518b36e46b85b1";
enum class WaitKind {None,Event,Semaphore,Mutex,Delay,ThreadEnd,GeList,GeDraw,Audio,LwMutex,Controller,Codec,Sleep,Network};
struct Thread {
    std::uint32_t uid,entry,priority,stack_base,stack_size,attributes;
    std::string name;
    bool ready=false,finished=false,wait_callbacks=false,callback_paused=false,started=false;
    bool interrupts=true; // CPU interrupt state of this thread's context (sceKernelCpuSuspendIntr)
    WaitKind wait=WaitKind::None;
    std::uint32_t wait_id=0,wait_bits=0,wait_mode=0,out_pointer=0,timeout_pointer=0,exit_status=0;
    std::uint64_t order=0,deadline=0;
    psprecomp::AllegrexContext context{};
    std::uint32_t stack_memory_uid=0;
    std::uint32_t initial_priority=0,preemptions=0,releases=0;
    std::uint32_t wakeup_count=0;
    std::uint64_t running_us=0;
};
struct DiagnosticCalls : std::vector<std::string> {
    std::ostream* stream=nullptr;
    bool trace_all=false;
    static constexpr std::array<std::string_view,15> sampled_prefixes{
        "Schedule uid=", "Wait uid=", "AudioOutput channel=", "AtracService nid=",
        "CtrlReadBufferPositive ", "GeListFinished ", "GeFinishCallback ",
        "DisplaySetFrameBuffer ", "GeListArgs ", "GeEnqueue ",
        "DcacheWritebackRange ", "DcacheWritebackInvalidateRange ",
        "ReferSemaStatus ", "CtrlPeekBufferPositive ", "DisplayWaitVblankStartMultiCB "};
    std::array<std::uint64_t,sampled_prefixes.size()> seen{},omitted{};
    void push_back(std::string message){
        if(!stream){std::vector<std::string>::push_back(std::move(message));return;}
        // Bound repetitive disk diagnostics; preserve errors and the first
        // 64 observations, then emit powers of two with exact aggregate counts.
        for(unsigned n=0;n<sampled_prefixes.size();++n)if(message.starts_with(sampled_prefixes[n])){
            const auto count=++seen[n];
            const bool error=message.find("result=0x8")!=std::string::npos;
            if(!trace_all && !error && count>64 && (count&(count-1u))){++omitted[n];return;}
            break;
        }
        *stream<<"HLE: "<<message<<'\n';
        recent_[recent_next_++%recent_.size()]=std::move(message);
    }
    // Last calls, oldest first (streamed runs keep only these in memory).
    std::vector<std::string> recent(std::size_t count) const{
        std::vector<std::string> out;
        if(!stream){for(auto n=size()>count?size()-count:0;n<size();++n)out.push_back((*this)[n]);return out;}
        const auto have=std::min<std::size_t>({count,recent_next_,recent_.size()});
        for(auto n=recent_next_-have;n<recent_next_;++n)out.push_back(recent_[n%recent_.size()]);
        return out;
    }
    std::array<std::string,160> recent_;std::size_t recent_next_=0;
    void summarize(std::ostream& output) const {
        for(unsigned n=0;n<seen.size();++n)if(seen[n])output<<"native_hle_diagnostics prefix=\""<<sampled_prefixes[n]<<"\" observed="<<seen[n]<<" omitted="<<omitted[n]<<'\n';
    }
};
struct DiagnosticRedirect {
    std::streambuf* previous=nullptr;
    ~DiagnosticRedirect(){if(previous)std::cout.rdbuf(previous);}
};
void watch_write_summary(std::ostream& out);
}
extern std::uint64_t kon_logic_frame_index;void kon_counter_summary(std::ostream& out);void kon_step_summary(std::ostream& out);extern bool kon_step_active;extern double kon_step_nominal;extern std::int32_t kon_step_thread;extern std::uint64_t kon_step_frames;
namespace {
// Deliberately bounded bootstrap services. Anything outside implemented
// semantics stops with evidence instead of returning a fabricated success.
class BootstrapServices {
public:
    // Private driver object, separate from the guest-created UID sequence.
    // PSP firmware audio waits on one SceAudio event flag (per-channel bits).
    static constexpr std::uint32_t audio_event_uid=0x7F000001u;
    std::map<std::uint32_t,Thread> threads;
    std::uint32_t sdk=0,compiler=0;
    bool module_returned=false;
    std::uint32_t module_status=0;
    DiagnosticCalls calls;
    bool stop_after_visible_frame=false;
    bool trace_render=false;
    std::uint64_t runtime_limit_us=0;
    // --stop-on-result[=ms]: stop the runtime this long after the results screen
    // is loaded (native_activity.hpp on_results). --hold-on-stop keeps it visible.
    std::uint64_t stop_on_result_us=0,result_deadline_us=0;
    void result_screen_loaded(){
        if(!stop_on_result_us || result_deadline_us)return;
        result_deadline_us=clock_.system_microseconds()+stop_on_result_us;
        std::cout<<"native_result_screen=1 activity=\""<<activity_.presence().details<<"\" stop_in_ms="<<stop_on_result_us/1000u<<'\n'<<std::flush;
    }
    kon::ControlReplay control_replay;
    kon::ControlReplay audio_anchored_replay;
    unsigned input_anchor_channel=3;
    std::optional<std::uint64_t> audio_input_anchor;
    // Diagnostic (KON_CHART_AUTOPLAY=<file of "tick buttons">, KON_CHART_CLOCK=<guest
    // address of a 60 Hz game counter>, KON_CHART_OFFSET=<ticks>): presses each note
    // while the game's own counter, counted from the song's first audio, is within
    // three ticks of it. Tests which clock the game judges notes by.
    struct ChartAutoplay {std::vector<std::pair<std::int64_t,std::uint32_t>> notes;std::uint32_t clock=0;std::int64_t offset=0;std::optional<std::uint32_t> start;bool loaded=false;} chart_autoplay_;
    std::uint32_t chart_autoplay_buttons(){
        auto& a=chart_autoplay_;
        if(!a.loaded){a.loaded=true;if(const char* f=std::getenv("KON_CHART_AUTOPLAY")){std::ifstream in(f);std::int64_t tick;std::uint32_t b;while(in>>tick>>b)a.notes.push_back({tick,b});
            if(const char* c=std::getenv("KON_CHART_CLOCK"))a.clock=std::uint32_t(std::stoul(c,nullptr,0));if(const char* o=std::getenv("KON_CHART_OFFSET"))a.offset=std::stoll(o);}}
        if(a.notes.empty() || !a.clock || !audio_input_anchor || !rt.memory().contains(a.clock,4))return 0;
        const auto now=rt.memory().load32(a.clock);if(!a.start){a.start=now;calls.push_back("ChartAutoplayStart counter="+std::to_string(now));}
        const std::int64_t song=std::int64_t(now-*a.start)-a.offset;std::uint32_t buttons=0;
        for(const auto& [tick,b]:a.notes)if(song>=tick && song<tick+3)buttons|=b;
        return buttons;
    }
    std::filesystem::path progress_capture;
    std::uint64_t capture_interval_us=0;
    void inspect_display(const std::filesystem::path& capture,unsigned hold_ms){
        std::cout<<"native_elapsed_ms="<<clock_.system_microseconds()/1000u<<"\ninterrupt_suspend_calls="<<interrupt_suspend_calls_<<"\ninterrupt_resume_calls="<<interrupt_resume_calls_<<"\n";
        calls.summarize(std::cout);
        watch_write_summary(std::cout);kon_counter_summary(std::cout);kon_step_summary(std::cout);
        std::cout<<"native_masked_context_switches="<<masked_context_switches_<<'\n';
        for(const auto& transition:interrupt_transitions_)std::cout<<"native_interrupt_transition "<<transition<<'\n';
        account_running();for(const auto& [uid,t]:threads)std::cout<<"native_task uid="<<uid<<" name="<<t.name<<" priority="<<t.priority<<" ready="<<t.ready<<" finished="<<t.finished<<" wait="<<static_cast<unsigned>(t.wait)<<" wait_id="<<t.wait_id<<" pc="<<psprecomp::hex32(t.context.pc)<<" ra="<<psprecomp::hex32(t.context.gpr[31])<<" runtime_us="<<t.running_us<<'\n';
        for(const auto& [uid,m]:mutexes_.objects)if(m.count){const auto s=mutex_lock_site_[uid];std::cout<<"native_mutex uid="<<uid<<" name="<<m.name<<" count="<<m.count<<" owner="<<m.owner<<" locked_by="<<s.thread<<" ra="<<psprecomp::hex32(s.ra)<<" pc="<<psprecomp::hex32(s.pc)<<" at_us="<<s.us<<" callback_depth="<<s.callback_depth<<" interrupts="<<s.interrupts<<'\n';}
        display_.update(clock_.system_microseconds());
        if(!capture.empty()){display_.save_bitmap(capture,true);std::cout<<"frame_capture="<<capture.string()<<"\n";}
        if(hold_ms){std::cout<<"diagnostic_hold_ms="<<hold_ms<<" (guest execution is stopped)\n"<<std::flush;
            const auto end=clock_.system_microseconds()+static_cast<std::uint64_t>(hold_ms)*1000u;
            while(!display_.closed() && clock_.system_microseconds()<end){display_.update(clock_.system_microseconds());std::this_thread::sleep_for(std::chrono::milliseconds(16));}
        }
    }
    explicit BootstrapServices(psprecomp::Runtime& runtime,std::uint32_t gp,std::uint32_t arena_begin,const psprecomp::Elf32Image& image,const kon::ContentMount& content,bool window=false,const std::filesystem::path& storage={},bool persistent_texture_cache=false,unsigned raster_threads=4):rt(runtime),gp_(gp),arena_(arena_begin,0x09FF0000u),lw_(runtime.memory()),image_(image),content_(content),files_(content,storage),ge_(runtime.memory()),display_(runtime.memory(),window),controls_([this](std::uint64_t sampled_at){
        kon::HostPad pad;
        if(!control_replay.events.empty()){
            pad=control_replay.at(sampled_at);
            if(audio_input_anchor)pad.buttons|=audio_anchored_replay.at_relative(sampled_at,*audio_input_anchor).buttons;
            pad.buttons|=chart_autoplay_buttons();
        }else{pad=kon::read_host_pad(display_.focused());pad.buttons|=display_.peek_key_presses();}
        // Settings screen FPS row: Left/Right choose the rate (not seen by the game).
        const auto unfiltered=pad.buttons;
        const auto changed=settings_fps_?settings_fps_->filter(pad.buttons):0u;
        // Keyboard presses stay latched until the game reads them; those the
        // Video row keeps from the game are delivered here, so the next press counts.
        if(control_replay.events.empty())display_.acknowledge_key_presses(unfiltered&~pad.buttons);
        if(changed){
            if(changed&kon::NativeSettingsFps::kRateChanged)apply_fps_choice();
            if(changed&kon::NativeSettingsFps::kScaleChanged)apply_scale_choice();
            if(changed&kon::NativeSettingsFps::kScreenChanged)apply_screen_choice();
            if(changed&kon::NativeSettingsFps::kLanguageChanged)apply_language_choice();}
        display_.set_input_time(kon::NativeWindow::host_us());
        return pad;
    }),renderer_(runtime.memory(),raster_threads,true,true,persistent_texture_cache),media_(content.root()) {
        // The Settings FPS row draws its own sprites from kernel RAM, which the game never maps: its
        // memory layout and allocation UIDs stay exactly as on the PSP.
        settings_fps_=std::make_unique<kon::NativeSettingsFps>(rt.memory(),kSettingsFpsScratch,kon::NativeSettingsFps::kScratchBytes);
    }
    std::unique_ptr<kon::NativeSettingsFps> settings_fps_;
    std::filesystem::path port_settings_path;
    std::filesystem::path port_settings_file()const{return port_settings_path.empty()?content_.root()/"config"/"settings.ini":port_settings_path;}
    void apply_fps_choice(){
        const auto fps=settings_fps_->rate();logic_rate.set_target(fps);
        const bool saved=kon::write_port_setting(port_settings_file(),"logic_fps",std::to_string(fps));
        calls.push_back("SettingsFps rate="+std::to_string(fps)+" saved="+std::to_string(saved));
    }
    // Diagnostic (KON_SCALE_SCHEDULE="ms:scale,ms:scale,..."): requests
    // internal scales at those run times, as the Settings row does.
    void scale_schedule_step(){
        static const auto schedule=[]{std::vector<std::pair<std::uint64_t,unsigned>> list;
            if(const char* text=std::getenv("KON_SCALE_SCHEDULE")){std::string item;std::istringstream in(text);
                while(std::getline(in,item,',')){const auto colon=item.find(':');if(colon!=std::string::npos)list.emplace_back(std::stoull(item.substr(0,colon))*1000u,unsigned(std::stoul(item.substr(colon+1))));}}
            return list;}();
        if(scale_schedule_next_>=schedule.size() || clock_.system_microseconds()<schedule[scale_schedule_next_].first)return;
        const auto scale=schedule[scale_schedule_next_++].second;renderer_.request_internal_scale(scale);
        std::cout<<"scale_schedule ms="<<clock_.system_microseconds()/1000u<<" scale="<<scale<<'\n';
    }
    std::size_t scale_schedule_next_=0;
    // Screen mode chosen in Settings (the window applies it) or by F11 (the
    // Settings row then follows the window); saved for the next start.
    void apply_screen_choice(){
        const bool full=settings_fps_->fullscreen();display_.set_fullscreen(full);
        const bool saved=kon::write_port_setting(port_settings_file(),"fullscreen",full?"1":"0");
        calls.push_back("SettingsScreen fullscreen="+std::to_string(full)+" saved="+std::to_string(saved));
    }
    bool window_fullscreen_=false;
    // Ad hoc multiplayer state (handlers registered by the game, pending
    // network waits). KON_ADHOC_LOCAL=1 keeps it on this PC (127.0.0.1).
    std::unique_ptr<kon::AdhocNet> adhoc_;std::uint32_t net_pool_=0,next_adhoc_handler_=1;bool adhoc_initialized_=false,adhocctl_initialized_=false;
    std::map<std::uint32_t,std::pair<std::uint32_t,std::uint32_t>> adhoc_handlers_; // id -> handler, argument
    std::map<std::uint32_t,std::function<std::optional<std::uint32_t>()>> net_waits_;  // thread -> retry
    std::map<std::string,unsigned> net_log_counts_;
    bool net_log_budget(const std::string& what){return ++net_log_counts_[what]<=40;} // first calls of each kind in the log
    std::mutex adhoc_create_mutex_; // the F8 window may create it from the presentation thread
    // Online room (F8 window, native_online.hpp): hosting starts the relay
    // here and asks the router (UPnP, in the background) to forward its port;
    // the codes give the friend this PC's public, IPv6 or LAN address.
    static constexpr std::uint16_t kOnlinePort=41700;
    std::mutex online_mutex_;std::unique_ptr<kon::UpnpMapping> upnp_;std::thread upnp_thread_;bool upnp_busy_=false,online_hosting_=false;
    std::string online_joined_,online_note_;
#ifdef _WIN32
    static std::wstring wide(const std::string& s){std::wstring w(s.size(),L'\0');w.resize(std::size_t(MultiByteToWideChar(CP_UTF8,0,s.data(),int(s.size()),w.data(),int(w.size()))));return w;}
    static std::string narrow(const std::wstring& w){std::string s(w.size()*3,'\0');s.resize(std::size_t(WideCharToMultiByte(CP_UTF8,0,w.data(),int(w.size()),s.data(),int(s.size()),nullptr,nullptr)));return s;}
#else
    static std::wstring wide(const std::string& s){
        std::wstring out;out.reserve(s.size());
        for(std::size_t i=0;i<s.size();){
            const auto c=static_cast<unsigned char>(s[i++]);std::uint32_t v=c;
            if(c>=0x80){
                unsigned extra=c<0xE0?1:c<0xF0?2:3;v=c&(0x3F>>extra);
                if(i+extra>s.size()){out+=L'?';break;}
                for(unsigned n=0;n<extra;++n)v=(v<<6)|(static_cast<unsigned char>(s[i++])&0x3F);
            }
            out+=static_cast<wchar_t>(v);
        }
        return out;
    }
    static std::string narrow(const std::wstring& w){
        std::string out;out.reserve(w.size());
        for(const wchar_t c:w){
            auto v=static_cast<std::uint32_t>(c);
            if(v<0x80)out+=char(v);
            else if(v<0x800){out+=char(0xC0|(v>>6));out+=char(0x80|(v&0x3F));}
            else if(v<0x10000){out+=char(0xE0|(v>>12));out+=char(0x80|((v>>6)&0x3F));out+=char(0x80|(v&0x3F));}
            else{out+=char(0xF0|(v>>18));out+=char(0x80|((v>>12)&0x3F));out+=char(0x80|((v>>6)&0x3F));out+=char(0x80|(v&0x3F));}
        }
        return out;
    }
#endif
    struct RoomCodes {std::string internet,ipv6,lan;std::vector<std::pair<std::string,std::string>> vpn;};
    RoomCodes room_codes(){
        RoomCodes codes;const auto local=kon::local_addresses();
        std::lock_guard lock(online_mutex_);
        if(upnp_ && upnp_->mapped && !upnp_->external_ip.empty() && !kon::private_ipv4(upnp_->external_ip))codes.internet=kon::room_code(upnp_->external_ip,kOnlinePort);
        if(!local.ipv6_global.empty())codes.ipv6=kon::room_code(local.ipv6_global.front(),kOnlinePort);
        for(const auto& a:local.ipv4)if(kon::private_ipv4(a)){codes.lan=kon::room_code(a,kOnlinePort);break;}
        for(const auto& [name,a]:local.vpn)codes.vpn.push_back({name,kon::room_code(a,kOnlinePort)});
        return codes;
    }
    std::wstring online_action(kon::NativeWindow::OnlineAction action,const std::wstring& text){
        using A=kon::NativeWindow::OnlineAction;auto& net=adhoc();
        if(action==A::Host){
            if(!net.online_host(kOnlinePort)){
                std::lock_guard lock(online_mutex_);
                online_note_="No se pudo crear la sala: el puerto TCP 41700 puede estar ocupado por otra instancia del juego, o no hay red. CiÃ©rrala o comprueba la conexiÃ³n y vuelve a intentarlo.";
                return {};
            }
            std::lock_guard lock(online_mutex_);online_hosting_=true;online_joined_.clear();online_note_.clear();
            if(!upnp_busy_ && !(upnp_ && upnp_->mapped) && !std::getenv("KON_ONLINE_NO_UPNP")){
                upnp_busy_=true;if(upnp_thread_.joinable())upnp_thread_.join();
                upnp_thread_=std::thread([this]{auto m=std::make_unique<kon::UpnpMapping>(kon::UpnpMapping::open(kOnlinePort));
                    std::lock_guard l(online_mutex_);calls.push_back("OnlineUpnp mapped="+std::to_string(m->mapped)+" external="+m->external_ip+" error="+m->error);upnp_=std::move(m);upnp_busy_=false;});
            }
            calls.push_back("OnlineHost port="+std::to_string(kOnlinePort));return {};
        }
        if(action==A::Join){
            if(narrow(text).empty()){std::lock_guard lock(online_mutex_);online_note_="Escribe el cÃ³digo de la sala de tu amigo.";return {};}
            const auto room=kon::parse_room_code(narrow(text));
            if(!room){std::lock_guard lock(online_mutex_);online_note_="Ese cÃ³digo no es vÃ¡lido: debe tener 10 o 29 letras y nÃºmeros (o direcciÃ³n:puerto).";return {};}
            {std::lock_guard lock(online_mutex_);online_note_.clear();}
            net.online_join(room->host,room->port);std::lock_guard lock(online_mutex_);online_hosting_=false;online_joined_=narrow(text);
            calls.push_back("OnlineJoin host="+room->host+" port="+std::to_string(room->port));return {};
        }
        if(action==A::Leave){net.online_leave();std::lock_guard lock(online_mutex_);online_hosting_=false;online_joined_.clear();online_note_.clear();if(upnp_)upnp_->close();calls.push_back("OnlineLeave");return {};}
        if(action==A::Code){
            {std::lock_guard lock(online_mutex_);
             if(!online_hosting_){online_note_="Crea una sala antes de copiar el cÃ³digo.";return {};}
             online_note_.clear();}
            const auto codes=room_codes();
            return wide(!codes.internet.empty()?codes.internet:!codes.vpn.empty()?codes.vpn.front().second:!codes.ipv6.empty()?codes.ipv6:codes.lan);
        }
        const auto codes=room_codes();
        // Status text.
        const auto state=net.online();std::string out;bool hosting,busy;std::string joined;kon::UpnpMapping upnp_view;
        {std::lock_guard lock(online_mutex_);hosting=online_hosting_;busy=upnp_busy_;joined=online_joined_;if(upnp_){upnp_view.mapped=upnp_->mapped;upnp_view.external_ip=upnp_->external_ip;upnp_view.error=upnp_->error;}}
        if(hosting && state.hosting){
            out="Sala creada (puerto TCP "+std::to_string(kOnlinePort)+"). Jugadores: "+std::to_string(state.players)+".\r\n";
            if(busy)out+="Pidiendo al router que abra el puerto (UPnP)...\r\n";
            if(!codes.internet.empty())out+="CÃ³digo para tu amigo (Internet): "+codes.internet+"\r\n";
            else if(!busy){
                const bool cgnat=upnp_view.mapped && kon::private_ipv4(upnp_view.external_ip);
                if(cgnat)out+="Tu conexiÃ³n no tiene IP pÃºblica propia (CG-NAT): desde fuera no se puede entrar directamente a tu sala.\r\n";
                else{out+="El router no abriÃ³ el puerto";if(!upnp_view.error.empty())out+=" ("+upnp_view.error+")";
                    out+=". Se puede abrir a mano el puerto TCP "+std::to_string(kOnlinePort)+" en el router.\r\n";}
            }
            for(const auto& [name,code]:codes.vpn)out+="CÃ³digo por "+name+" (tu amigo conectado a tu red de "+name+"): "+code+"\r\n";
            if(!codes.ipv6.empty())out+="CÃ³digo IPv6: "+codes.ipv6+"\r\n";
            if(!codes.lan.empty())out+="CÃ³digo solo para tu misma casa (wifi o cable): "+codes.lan+"\r\n";
            if(codes.internet.empty() && codes.vpn.empty() && !busy)out+="Para jugar con alguien de fuera: que cree la sala tu amigo (si su conexiÃ³n no tiene CG-NAT), o usad los dos Radmin VPN (gratis) y volved a crear la sala.\r\n";
            out+="Â«Copiar cÃ³digoÂ» copia el primero. Windows puede pedir permiso al Cortafuegos: acÃ©ptalo.";
        }else if(!joined.empty()){
            if(state.linked)out="Conectado a la sala. Jugadores: "+std::to_string(state.players)+".\r\nEntra en ã¤ã†ã—ã‚“! (Local) y Ãºnete a la banda de tu amigo.";
            else out="Conectando a la sala "+joined+"...\r\n"+(state.error.empty()?std::string{}:"Ãšltimo intento: "+state.error+".\r\nComprueba el cÃ³digo y que tu amigo tenga la sala abierta.");
        }else{
            out="No estÃ¡s en ninguna sala.";
            std::lock_guard lock(online_mutex_);if(!online_note_.empty())out+="\r\n"+online_note_;
        }
        return wide(out);
    }
    void attach_online(){display_.set_online_handler([this](kon::NativeWindow::OnlineAction a,const std::wstring& t){return online_action(a,t);});}
    kon::AdhocNet& adhoc(){
        std::lock_guard create(adhoc_create_mutex_);
        if(!adhoc_){kon::AdhocNet::Options options;options.lan=!std::getenv("KON_ADHOC_LOCAL");options.relay_only=std::getenv("KON_ADHOC_RELAY_ONLY")!=nullptr;
            const char* name=std::getenv("COMPUTERNAME");adhoc_=std::make_unique<kon::AdhocNet>(name?name:"PC",options);
            std::cout<<"adhoc "<<adhoc_->describe()<<'\n';}
        return *adhoc_;
    }
    // A network call: done at once when it can be, else WOULD_BLOCK (flag) or
    // a wait until it can (timeout in microseconds, 0 = none; TIMEOUT after).
    void net_call(psprecomp::AllegrexContext& c,std::uint32_t timeout,bool nonblocking,std::function<std::optional<std::uint32_t>()> attempt){
        if(const auto result=attempt()){c.set_gpr(2,*result);return;}
        if(nonblocking){c.set_gpr(2,0x80410709u);if(net_log_budget("WouldBlock"))calls.push_back("NetAdhoc would_block ra="+psprecomp::hex32(c.gpr[31]));return;}
        const auto uid=static_cast<std::uint32_t>(psprecomp::runtime_thread_uid());
        block_wait(c,WaitKind::Network,0,0,0,0,0,timeout);
        if(!rt.stopped())net_waits_[uid]=std::move(attempt);
    }
    // Adhocctl events reach the game's handlers at callback points of the
    // thread that is running (as the PSP's adhocctl thread would call them).
    void deliver_adhoc_events(psprecomp::AllegrexContext& c){
        if(!adhoc_ || adhoc_handlers_.empty())return;
        const auto owner=psprecomp::runtime_thread_uid();const auto thread=threads.find(static_cast<std::uint32_t>(owner));if(thread==threads.end())return;
        for(const auto& [event,error]:adhoc_->take_events()){
            const auto handlers=adhoc_handlers_;
            for(const auto& [id,h]:handlers){
                ++callback_depth_;const auto outcome=kon::run_native_callback(rt,c,h.first,std::uint32_t(event),std::uint32_t(error),h.second,thread->second.stack_base);--callback_depth_;
                if(!outcome.completed)return;
                calls.push_back("NetAdhocctlEvent event="+std::to_string(int(event))+" handler="+std::to_string(id));
            }
        }
    }
    // Language chosen in Settings: saved at once; the game restarts in it after
    // the player leaves the Settings screen (the game has saved its own options
    // by then). Choosing the running language again cancels the restart.
    std::string running_language_;bool language_restart_pending_=false,restart_requested_=false;std::uint64_t settings_left_us_=0;
    void apply_language_choice(){
        const std::string id=settings_fps_->language();
        const bool saved=kon::write_port_setting(port_settings_file(),"language",id);
        language_restart_pending_=id!=running_language_;settings_left_us_=0;
        calls.push_back("SettingsLanguage language="+id+" saved="+std::to_string(saved)+" restart="+std::to_string(language_restart_pending_));
    }
    // Once per logic frame, before the Settings row forgets this frame.
    void language_restart_step(){
        if(!language_restart_pending_ || !settings_fps_)return;
        const auto now=clock_.system_microseconds();
        if(settings_fps_->screen_visible()){settings_left_us_=0;return;}
        if(!settings_left_us_){settings_left_us_=now;return;}
        if(now-settings_left_us_>=1000000u){language_restart_pending_=false;restart_requested_=true;std::cout<<"language_restart="<<settings_fps_->language()<<'\n';rt.stop(kRestartReason);}
    }
    static constexpr const char* kRestartReason="Language changed: restarting";
    void init_screen_mode(bool full){display_.set_fullscreen(full);window_fullscreen_=full;if(settings_fps_)settings_fps_->set_fullscreen(full);}
    void follow_window_screen_mode(){
        const bool full=display_.fullscreen();if(full==window_fullscreen_)return;window_fullscreen_=full;
        if(settings_fps_ && settings_fps_->fullscreen()!=full){settings_fps_->set_fullscreen(full);kon::write_port_setting(port_settings_file(),"fullscreen",full?"1":"0");}
    }
    // Internal resolution chosen in Settings: applied by the render thread
    // between batches, saved for the next start.
    void apply_scale_choice(){
        const auto scale=settings_fps_->scale();renderer_.request_internal_scale(scale);
        const bool saved=kon::write_port_setting(port_settings_file(),"internal_scale",std::to_string(scale));
        calls.push_back("SettingsScale scale="+std::to_string(scale)+" saved="+std::to_string(saved));
    }
    unsigned verified_render_frames=0,rejected_render_frames=0,requested_render_frames=0;
    std::uint64_t verify_render_after_us=0;
    std::array<std::uint64_t,4> vblank_waits_{}; // WaitVblankStartCB, then MultiCB with 1, 2, 3+ cycles
    std::array<std::uint64_t,4> time_calls_{}; // GetSystemTimeLow, GetSystemTimeWide, RtcGetCurrentTick, AtracGetNextSample
    unsigned logic_vblank_cycles=0;
    std::uint32_t screenshot_status_=0,screenshot_params_=0;
    std::uint32_t osk_status_=0,osk_params_=0;
    std::map<std::uint32_t,std::uint64_t> vblank_given_;std::uint64_t masked_blocks_=0;
    // Last successful lock of each kernel mutex (diagnostics for freezes).
    struct LockSite {std::int32_t thread=0;std::uint32_t ra=0,pc=0;std::uint64_t us=0;unsigned callback_depth=0;bool interrupts=true;};
    std::map<std::uint32_t,LockSite> mutex_lock_site_;
    // Reads inside std.cpk, by member (Discord presence: current song or mode).
    std::optional<kon::CpkIndex> std_index_;bool std_index_tried_=false;std::string last_member_;
    void note_disc_read(const std::filesystem::path& path,std::int64_t offset){
        if(offset<0 || path.filename()!="std.cpk")return;
        if(!std_index_tried_){std_index_tried_=true;std_index_=kon::CpkIndex::load(path);calls.push_back("CpkIndex std members="+std::to_string(std_index_?std_index_->size():0));}
        if(!std_index_)return;
        const auto* member=std_index_->at(std::uint64_t(offset));if(!member || member->name==last_member_)return;
        last_member_=member->name;activity_.on_member(member->name);
        if(trace_members_)calls.push_back("CpkMember "+member->name);
    }
    bool trace_members_=std::getenv("KON_TRACE_MEMBERS")!=nullptr;
    kon::GameActivity activity_;
    void note_mutex_lock(std::uint32_t id,const psprecomp::AllegrexContext& c){mutex_lock_site_[id]={psprecomp::runtime_thread_uid(),c.gpr[31],c.pc,clock_.system_microseconds(),callback_depth_,interrupts_enabled_};}std::uint64_t vblank_caught_up_=0;
    // Played from the launcher: a game that keeps running but shows no new
    // frame for 10 s (black window, FPS 0) leaves a report, since no stop does.
    std::filesystem::path freeze_report_path;std::uint64_t freeze_window_frames_=0,freeze_game_frames_=0;unsigned freeze_checks_=0;bool freeze_reported_=false;
    void check_freeze(){
        if(freeze_report_path.empty() || display_.minimized())return;
        const auto window_frames=display_.window_frames(),game_frames=display_.submitted_frames();
        if(window_frames!=freeze_window_frames_){freeze_window_frames_=window_frames;freeze_game_frames_=game_frames;freeze_checks_=0;freeze_reported_=false;return;}
        if(++freeze_checks_<2 || freeze_reported_)return;
        freeze_reported_=true;
        std::ofstream report(freeze_report_path);
        const auto now=std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());char when[32]{};std::strftime(when,sizeof(when),"%Y-%m-%d %H:%M:%S",std::localtime(&now));
        report<<"fecha="<<when<<"\nmotivo=posible congelacion: ninguna imagen nueva en la ventana durante 10 s (el juego sigue en marcha)\n"
              <<"tiempo_juego_ms="<<clock_.system_microseconds()/1000u<<"\nimagenes_ventana="<<window_frames<<"\nimagenes_juego="<<game_frames<<" (antes "<<freeze_game_frames_<<")\n"
              <<"pantalla="<<psprecomp::hex32(display_.buffer(0).address)<<" pendiente="<<psprecomp::hex32(display_.buffer(1).address)<<"\nhires_presents="<<display_.hires_presents()<<" hires_bad_presents="<<display_.hires_bad_presents()<<" hires_fallback_tiles="<<display_.hires_fallback_tiles()<<"\n"
              <<"hilo_activo="<<psprecomp::runtime_thread_uid()<<" PC="<<psprecomp::hex32(rt.cpu().pc)<<" RA="<<psprecomp::hex32(rt.cpu().gpr[31])<<"\n";
        for(const auto& [uid,t]:threads)report<<"hilo uid="<<uid<<" nombre="<<t.name<<" listo="<<t.ready<<" terminado="<<t.finished<<" espera="<<static_cast<int>(t.wait)<<" id="<<psprecomp::hex32(t.wait_id)<<" pc="<<psprecomp::hex32(t.context.pc)<<" ra="<<psprecomp::hex32(t.context.gpr[31])<<"\n";
        for(const auto& [uid,m]:mutexes_.objects)if(m.count){const auto s=mutex_lock_site_[uid];report<<"mutex uid="<<uid<<" nombre="<<m.name<<" cuenta="<<m.count<<" dueno="<<m.owner<<" bloqueado_por="<<s.thread<<" ra="<<psprecomp::hex32(s.ra)<<" en_us="<<s.us<<" callback="<<s.callback_depth<<" interrupciones="<<s.interrupts<<"\n";}
        for(const auto& call:calls.recent(150))report<<"HLE: "<<call<<"\n";
        std::cout<<"freeze_report="<<freeze_report_path.string()<<"\n";
    }
    kon::NativeLogicRate logic_rate;
    // Diagnostic (--ram-snapshots dir start_ms count): user RAM at consecutive
    // logic frames, used offline to find values that advance once per frame.
    std::string ram_snapshot_dir;std::uint64_t ram_snapshot_start_us=0,ram_snapshot_interval_us=0;unsigned ram_snapshot_count=0,ram_snapshots_taken=0;
    void ram_snapshot(std::uint64_t now){
        if(ram_snapshots_taken>=ram_snapshot_count || now<ram_snapshot_start_us+ram_snapshots_taken*ram_snapshot_interval_us)return;
        std::vector<std::uint8_t> ram(0x01800000u);rt.memory().copy_out(0x08800000u,ram);
        char name[32];std::snprintf(name,sizeof name,"/frame_%03u.bin",ram_snapshots_taken++);
        std::ofstream(ram_snapshot_dir+name,std::ios::binary).write(reinterpret_cast<const char*>(ram.data()),std::streamsize(ram.size()));
        std::cout<<"native_ram_snapshot index="<<ram_snapshots_taken-1<<" elapsed_us="<<now<<'\n';
    } // diagnostic override, 0 keeps the game value
    // Diagnostic (--ram-variation file start_ms duration_ms): per user-RAM word,
    // how often and how much it changed between consecutive logic frames in the
    // window. Comparing a 30 FPS run with a PC-rate run of the same scene finds
    // values that still advance per frame (tools/compare_ram_variation.py).
    std::string ram_variation_file;std::uint64_t ram_variation_start_us=0,ram_variation_end_us=0,ram_variation_first_us=0,ram_variation_last_us=0,ram_variation_frames=0;bool ram_variation_done=false;
    std::vector<std::uint32_t> ram_variation_prev,ram_variation_now,ram_variation_changes;std::vector<double> ram_variation_int,ram_variation_float;
    void ram_variation(std::uint64_t now){
        if(ram_variation_done || now<ram_variation_start_us)return;
        constexpr std::size_t words=0x01800000u/4u;
        if(ram_variation_prev.empty()){ram_variation_prev.resize(words);ram_variation_now.resize(words);ram_variation_changes.assign(words,0u);ram_variation_int.assign(words,0.0);ram_variation_float.assign(words,0.0);}
        rt.memory().copy_out(0x08800000u,std::span<std::uint8_t>(reinterpret_cast<std::uint8_t*>(ram_variation_now.data()),words*4u));
        if(ram_variation_first_us){
            for(std::size_t i=0;i<words;++i){
                const auto a=ram_variation_prev[i],b=ram_variation_now[i];if(a==b)continue;
                ++ram_variation_changes[i];ram_variation_int[i]+=std::fabs(double(std::int32_t(b-a)));
                const auto fa=std::bit_cast<float>(a),fb=std::bit_cast<float>(b);
                if(std::isfinite(fa) && std::isfinite(fb) && std::fabs(fa)<1e7f && std::fabs(fb)<1e7f)ram_variation_float[i]+=std::fabs(double(fb)-double(fa));
            }
            ++ram_variation_frames;
        }else ram_variation_first_us=now;
        ram_variation_last_us=now;std::swap(ram_variation_prev,ram_variation_now);
        if(now>=ram_variation_end_us){
            ram_variation_done=true;std::ofstream out(ram_variation_file,std::ios::binary);
            const std::uint64_t header[3]{ram_variation_frames,ram_variation_last_us-ram_variation_first_us,words};out.write(reinterpret_cast<const char*>(header),sizeof header);
            out.write(reinterpret_cast<const char*>(ram_variation_changes.data()),std::streamsize(words*4u));
            out.write(reinterpret_cast<const char*>(ram_variation_int.data()),std::streamsize(words*8u));
            out.write(reinterpret_cast<const char*>(ram_variation_float.data()),std::streamsize(words*8u));
            std::cout<<"native_ram_variation frames="<<ram_variation_frames<<" duration_us="<<header[1]<<" file="<<ram_variation_file<<'\n';
        }
    }
    // Diagnostic (--speed-audit start_ms measure_ms): with alternating short
    // and long frames (NativeLogicRate::set_jitter), every user-RAM word that
    // changes as much on short frames as on long ones is still advanced per
    // logic frame rather than by game time. Those words are then watched
    // (PSPRECOMP_WATCH_WRITE must be set) so the exit summary names the
    // generated code that writes them.
    std::uint64_t speed_audit_start_us=0,speed_audit_end_us=0;bool speed_audit_done=false;float speed_audit_ticks=0.0f;
    std::vector<std::uint32_t> speed_audit_prev,speed_audit_now;std::vector<float> speed_audit_sum[2][2];std::vector<std::uint16_t> speed_audit_changes[2],speed_audit_same;std::vector<std::uint32_t> speed_audit_first;std::uint32_t speed_audit_frames[2]{};double speed_audit_tick_sum[2]{};
    void speed_audit(std::uint64_t now){
        if(speed_audit_done || now<speed_audit_start_us)return;
        kon_step_active=true;kon_step_thread=psprecomp::runtime_thread_uid();kon_step_nominal=1000000.0/double(logic_rate.target())/kon::NativeLogicRate::kTickUs;++kon_step_frames;
        constexpr std::size_t words=0x01800000u/4u;
        const float ticks=speed_audit_ticks;speed_audit_ticks=kon_logic_frame_ticks_f; // the step the frame just finished ran with
        if(speed_audit_prev.empty()){
            speed_audit_prev.resize(words);speed_audit_now.resize(words);
            for(auto& kind:speed_audit_sum)for(auto& s:kind)s.assign(words,0.0f);
            for(auto& c:speed_audit_changes)c.assign(words,0u);
            speed_audit_same.assign(words,0u);speed_audit_first.assign(words,0u);
            rt.memory().copy_out(0x08800000u,std::span<std::uint8_t>(reinterpret_cast<std::uint8_t*>(speed_audit_prev.data()),words*4u));return;
        }
        rt.memory().copy_out(0x08800000u,std::span<std::uint8_t>(reinterpret_cast<std::uint8_t*>(speed_audit_now.data()),words*4u));
        const double expected=1000000.0/double(logic_rate.target())/kon::NativeLogicRate::kTickUs;
        const int f=ticks>expected?1:0;++speed_audit_frames[f];speed_audit_tick_sum[f]+=ticks;
        for(std::size_t i=0;i<words;++i){
            const auto a=speed_audit_prev[i],b=speed_audit_now[i];if(a==b)continue;
            if(speed_audit_changes[f][i]<0xFFFFu)++speed_audit_changes[f][i];
            // Same step every frame (timers, constant speeds, phases).
            const auto delta=b-a;auto& first=speed_audit_first[i];
            if(!first)first=delta;
            else if(delta==first && speed_audit_same[i]<0xFFFFu)++speed_audit_same[i];
            speed_audit_sum[0][f][i]+=float(std::fabs(double(std::int32_t(b-a))));
            const auto fa=std::bit_cast<float>(a),fb=std::bit_cast<float>(b);
            if(std::isfinite(fa) && std::isfinite(fb) && std::fabs(fa)<1e6f && std::fabs(fb)<1e6f)speed_audit_sum[1][f][i]+=std::fabs(fb-fa);
        }
        std::swap(speed_audit_prev,speed_audit_now);
        if(now<speed_audit_end_us)return;
        // KON_SPEED_AUDIT_SHARE=n: words that change on at least 1/n of the frames (default 2).
        static const unsigned speed_audit_share=std::getenv("KON_SPEED_AUDIT_SHARE")?std::max(1,std::atoi(std::getenv("KON_SPEED_AUDIT_SHARE"))):2;
        speed_audit_done=true;kon_step_active=false;logic_rate.set_jitter(false);logic_rate.set_fixed_step(false);
        const double tick_ratio=(speed_audit_tick_sum[1]/std::max(1u,speed_audit_frames[1]))/(speed_audit_tick_sum[0]/std::max(1u,speed_audit_frames[0]));
        std::vector<std::uint32_t> flagged;
        for(std::size_t i=0;i<words;++i){
            if(speed_audit_changes[0][i]*speed_audit_share<speed_audit_frames[0] || speed_audit_changes[1][i]*speed_audit_share<speed_audit_frames[1])continue;
            const auto changed=std::uint32_t(speed_audit_changes[0][i])+speed_audit_changes[1][i];
            const bool steady_float=speed_audit_sum[1][0][i]>0.0f && std::fabs(speed_audit_sum[1][0][i]/std::max<std::uint16_t>(1,speed_audit_changes[0][i])-speed_audit_sum[1][1][i]/std::max<std::uint16_t>(1,speed_audit_changes[1][i]))<=1e-3f*speed_audit_sum[1][0][i]/std::max<std::uint16_t>(1,speed_audit_changes[0][i]);
            if(std::uint32_t(speed_audit_same[i])*10u<changed*7u && !steady_float)continue;
            const bool as_float=speed_audit_sum[1][0][i]>0.0f && speed_audit_sum[1][1][i]>0.0f && speed_audit_sum[1][0][i]<speed_audit_sum[0][0][i]*1e-3f;
            const auto& s=speed_audit_sum[as_float?1:0];
            const double per_short=s[0][i]/speed_audit_frames[0],per_long=s[1][i]/speed_audit_frames[1];
            if(per_short<=0.0)continue;
            const double ratio=per_long/per_short;
            if(ratio<0.8 || ratio>1.25)continue;
            const auto address=0x08800000u+std::uint32_t(i*4u);flagged.push_back(address);
            if(flagged.size()<=400)std::cout<<"native_speed_audit_word address="<<psprecomp::hex32(address)<<" kind="<<(as_float?"float":"int")<<" per_frame="<<per_short<<" long_over_short="<<ratio<<" value="<<(as_float?double(std::bit_cast<float>(speed_audit_prev[i])):double(std::int32_t(speed_audit_prev[i])))<<'\n';
        }
        std::cout<<"native_speed_audit frames="<<speed_audit_frames[0]<<"+"<<speed_audit_frames[1]<<" tick_ratio="<<tick_ratio<<" per_frame_words="<<flagged.size()<<'\n';
        // Plausible floats (|x| 0.01..10000): also watch every other word holding
        // the same bits, where the value usually comes from (e.g. an actor's position).
        {
            std::set<std::uint32_t> values;
            for(const auto address:flagged){const auto bits=speed_audit_prev[(address-0x08800000u)/4u];const auto v=std::fabs(std::bit_cast<float>(bits));if(v>0.01f && v<10000.0f)values.insert(bits);}
            std::set<std::uint32_t> known(flagged.begin(),flagged.end());unsigned twins=0;
            for(std::size_t i=0;i<words && twins<2000;++i)if(values.count(speed_audit_prev[i])){const auto address=0x08800000u+std::uint32_t(i*4u);if(known.insert(address).second){flagged.push_back(address);++twins;std::cout<<"native_speed_audit_twin address="<<psprecomp::hex32(address)<<" value="<<std::bit_cast<float>(speed_audit_prev[i])<<'\n';}}
        }
        if(flagged.size()>6000)flagged.resize(6000);
        psprecomp::set_write_watch_words(std::move(flagged));
        for(auto& kind:speed_audit_sum)for(auto& s:kind)std::vector<float>().swap(s);
    }
    std::filesystem::path frame_dump; // optional offline replay snapshots of verified frames
    // Diagnostic frame timeline: (elapsed_us, event, argument) inside a window.
    // 1 enqueue 2 stall 3 list-sync 4 draw-sync 5 vblank-multi 6 ge-begin 7 ge-end(draws) 8 set-framebuffer 9 schedule(uid) 10 idle-wait(us)
    std::atomic<bool> in_ge{false}; // read by the diagnostic sampling profiler
    bool profile_ge_only=false;
    std::vector<std::array<std::uint64_t,3>> frame_trace;std::uint64_t trace_begin_us=0,trace_end_us=0;
    void trace(unsigned event,std::uint64_t argument){if(trace_end_us){const auto now=clock_.system_microseconds();if(now>=trace_begin_us && now<trace_end_us)frame_trace.push_back({now,event,argument});}}
    std::unique_ptr<kon::NativeFrameStream> frame_stream;
    kon::NativeGpuRaster* frame_verification_gpu=nullptr;
    void enable_frame_verification(unsigned count,kon::NativeGpuRaster* backend){requested_render_frames=count;frame_verification_gpu=backend;}
    void finish_render_frame(unsigned address,unsigned stride,unsigned format){
        if(resident_gpu_ownership && frame_stream)complete_gpu_batch();
        if(!frame_stream)return;const auto frame=frame_stream->finish(address,stride,format,clock_.system_microseconds());if(!frame)return;
        if(!frame->complete){++rejected_render_frames;std::cout<<"native_frame_rejected reason="<<frame->rejection<<"\n";return;}
        const auto output=kon::replay_native_frame(*frame,*frame_verification_gpu);
        const auto offset=address%psprecomp::GuestMemory::kVramSize,bytes=format==3?4u:2u;
        const auto size=std::size_t(stride)*272u*bytes;const auto current=rt.memory().vram_bytes();
        if(offset+size>output.size() || !std::equal(output.begin()+offset,output.begin()+offset+size,current.begin()+offset)){
            ++rejected_render_frames;std::cout<<"native_frame_rejected reason=private_replay_differs_from_original_framebuffer\n";return;
        }
        if(!frame_dump.empty()){const auto path=frame_dump.parent_path()/(frame_dump.filename().string()+"_"+std::to_string(verified_render_frames)+".konframe");kon::save_native_frame(path,*frame,current);std::cout<<"native_frame_snapshot="<<path.string()<<"\n";}
        ++verified_render_frames;std::cout<<"native_frame_replay_verified elapsed_ms="<<clock_.system_microseconds()/1000<<" commands="<<frame->commands.size()<<" address="<<psprecomp::hex32(address)<<" private_original_code_calls=0\n";
        if(verified_render_frames>=requested_render_frames){renderer_.set_frame_stream(nullptr);frame_stream.reset();}
    }
    ~BootstrapServices(){psprecomp::set_native_vram_access_hook(nullptr,nullptr,nullptr);if(upnp_thread_.joinable())upnp_thread_.join();}
    // Native id of the asynchronous render thread (diagnostic profiling).
    unsigned long render_thread_id(){if(!renderer_.async())return kon_current_thread_id();unsigned long id=0;renderer_.enqueue([&id]{id=kon_current_thread_id();});renderer_.drain();return id;}
    void enable_resident_gpu_ownership(){
        renderer_.set_draw_queue(std::getenv("KON_NO_DRAW_QUEUE")==nullptr); // diagnostic A/B
        // Asynchronous rendering: GE work and frame presentation run on the
        // renderer's thread while original code continues (KON_NO_ASYNC_RENDER: off).
        if(std::getenv("KON_NO_DRAW_QUEUE")==nullptr && std::getenv("KON_NO_ASYNC_RENDER")==nullptr){
            renderer_.set_async(true);
            display_.set_present_hook([this](const kon::NativeDisplay::Buffer& buffer,bool new_frame){
                renderer_.enqueue([this,buffer,new_frame]{
                    const auto bytes=std::size_t(buffer.stride)*272u*(buffer.format==3?4u:2u);
                    renderer_.present_when_ready(buffer.address,bytes,[this,buffer,new_frame]{display_.present_buffer(buffer,new_frame);});
                },true);
            });
        }
        psprecomp::set_native_vram_access_hook(&rt.memory(),[](void* owner,std::uint32_t address,std::size_t length) noexcept {
            auto& self=*static_cast<BootstrapServices*>(owner);++self.vram_hook_calls_;if(!self.renderer_.pending_vram_access(address,length))return true;++self.vram_hook_collects_;
            try{const auto before=self.renderer_.gpu_collect_wait_us;self.trace(11,address);self.complete_gpu_batch();self.hook_collect_wait_us+=self.renderer_.gpu_collect_wait_us-before;return true;}catch(const std::exception& error){self.rt.stop(error.what());return false;}
        },this);
    }
    void submit_gpu_batch(){const auto start=std::chrono::steady_clock::now();renderer_.submit_gpu_batch();gpu_batch_time_us+=std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now()-start).count();renderer_.begin_gpu_batch();}
    void complete_gpu_batch(){const auto start=std::chrono::steady_clock::now();renderer_.end_gpu_batch();gpu_batch_time_us+=std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now()-start).count();renderer_.begin_gpu_batch();}
    // Kernel memory services write guest memory directly, outside the VRAM
    // access hook of compiled code: VRAM ranges first wait for the renderer
    // (queued draws, batches and presentations reading them), as the hook does.
    std::uint64_t vram_service_syncs_=0;
    bool sync_vram(std::uint32_t address,std::size_t length){
        const auto c=address&0x1FFFFFFFu;if(c<0x04000000u || c>=0x04800000u)return true;
        ++vram_service_syncs_;
        if(!renderer_.pending_vram_access(address,length))return true;
        try{complete_gpu_batch();return true;}catch(const std::exception& error){rt.stop(error.what());return false;}
    }
    void set_gpu_raster(kon::NativeGpuRaster* backend){
        renderer_.set_gpu_raster(backend);gpu_raster_backend_=backend;
        // Internal resolution: presentations show the enlarged buffer when one exists.
        display_.set_hires_source([backend](std::uint32_t address){return backend->hires_view(address);});
    }
    kon::NativeGpuRaster* gpu_raster_backend_=nullptr;
    void set_sync_show(bool sync){renderer_.set_sync_show(sync);}
    void set_gpu_clear(kon::NativeGpuClear* backend){renderer_.set_gpu_clear(backend);}
    void set_gpu_triangles(kon::NativeGpuTriangles* backend){renderer_.set_gpu_triangles(backend);}
    std::filesystem::path projected_draw_capture,quad_draw_capture;
    bool quad_capture_written_=false;
    std::uint64_t projected_capture_after_us=0;unsigned projected_capture_min_vertices=64;
    bool profile_render=false,profile_frames=false,gpu_batch=false,resident_gpu_ownership=false;
    bool clear_milestone_logged_=false;
    std::uint64_t gpu_batch_time_us=0;
    void write_frame_profile(const std::filesystem::path& path){
        std::ofstream out(path);out<<"{\"sample_interval_us\":100000,\"audio_anchor_us\":";
        if(audio_input_anchor)out<<*audio_input_anchor;else out<<"null";
        out<<",\"samples\":[";bool comma=false;
        for(const auto& sample:frame_samples_){if(comma)out<<',';comma=true;out<<'['<<sample[0]<<','<<sample[1]<<','<<sample[2]<<','<<sample[3]<<','<<sample[4]<<','<<sample[5]<<']';}
        out<<"],\"columns\":[\"elapsed_us\",\"submitted_frames\",\"render_2d_us\",\"render_3d_us\",\"main_thread_cycles\",\"gpu_collect_wait_us\"],\"full_song_verified\":false}\n";
        if(!out)throw std::runtime_error("Cannot save frame profile");
    }
    void write_render_profile(const std::filesystem::path& path){
        std::ofstream out(path);out<<"{\"draws\":[";bool comma=false;
        for(const auto& [key,value]:render_profile_){if(comma)out<<',';comma=true;out<<"{\"key\":[";for(unsigned n=0;n<key.size();++n){if(n)out<<',';out<<key[n];}out<<"],\"calls\":"<<value.calls<<",\"total_us\":"<<value.total_us<<",\"max_us\":"<<value.max_us<<",\"covered\":"<<value.covered<<",\"drawn\":"<<value.drawn<<'}';}
        out<<"],\"unrecorded_draws\":"<<render_profile_overflow_<<",\"gpu_triangle_primitives\":"<<renderer_.gpu_triangle_primitives<<",\"gpu_triangle_pixels\":"<<renderer_.gpu_triangle_pixels<<",\"diagnostic_only\":true}\n";
        if(!out)throw std::runtime_error("Cannot save renderer profile");
    }
    void install() {
        calls.push_back("SchedulerTimer high_resolution="+std::to_string(scheduler_wait_.high_resolution()));
        if(signals_.create_event(audio_event_uid,"SceAudio",0x201u,0))throw std::runtime_error("Cannot create native audio completion event");
        const auto info=image_.find_module_info(rt.memory());
        for(const auto& item:image_.scan_imports(rt.memory(),*info))import_pcs_.insert(item.stub_address);
        rt.nids().add("sceAudio",0x5EC81C55u,"sceAudioChReserve");
        rt.register_hle("sceAudio",0x5EC81C55u,[this](auto&,auto& c){
            const auto result=audio_.reserve(static_cast<std::int32_t>(c.gpr[4]),c.gpr[5],c.gpr[6]);
            calls.push_back("AudioReserve frames="+std::to_string(c.gpr[5])+" format="+std::to_string(c.gpr[6])+" result="+psprecomp::hex32(result));c.set_gpr(2,result);
        });
        rt.register_hle("sceAudio",0x6FC46853u,[this](auto&,auto& c){
            for(const auto& [id,t]:threads)if(t.wait==WaitKind::Audio && t.wait_id==c.gpr[4]){c.set_gpr(2,kon::NativeAudio::busy);return;}
            c.set_gpr(2,audio_.release(c.gpr[4]));
        });
        rt.register_hle("sceAudio",0xCB2E439Eu,[this](auto&,auto& c){
            for(const auto& [id,t]:threads)if(t.wait==WaitKind::Audio && t.wait_id==c.gpr[4]){c.set_gpr(2,kon::NativeAudio::busy);return;}
            c.set_gpr(2,audio_.set_frames(c.gpr[4],c.gpr[5]));
        });
        rt.register_hle("sceAudio",0x95FD0C2Du,[this](auto&,auto& c){c.set_gpr(2,audio_.set_format(c.gpr[4],c.gpr[5]));});
        rt.register_hle("sceAudio",0xB7E1D8E7u,[this](auto&,auto& c){c.set_gpr(2,audio_.set_volume(c.gpr[4],static_cast<std::int32_t>(c.gpr[5]),static_cast<std::int32_t>(c.gpr[6])));});
        rt.register_hle("sceAudio",0xB011922Fu,[this](auto&,auto& c){c.set_gpr(2,audio_.rest_frames(c.gpr[4]));});
        for(const auto nid:{0x136CAF51u,0x13F592BCu,0xE2D56B2Du})rt.register_hle("sceAudio",nid,[this,nid](auto&,auto& c){
            const bool panned=nid!=0x136CAF51u,blocking=nid!=0xE2D56B2Du;
            const auto channel=c.gpr[4],pointer=c.gpr[panned?7:6];
            const auto frames=audio_.sample_count(channel),channels=audio_.channel_format(channel)==0x10?1u:2u;
            if(!frames){c.set_gpr(2,channel>=8?kon::NativeAudio::invalid_channel:kon::NativeAudio::not_reserved);return;}
            if(!pointer){rt.stop("Audio null-buffer drain requires native wait adaptation");return;}
            if(callback_depth_ && blocking){rt.stop("Blocking audio output inside native callback requires asynchronous callback frames");return;}
            if(!rt.memory().contains(pointer,static_cast<std::size_t>(frames)*channels*2u)){c.set_gpr(2,0x80000103u);return;}
            std::vector<std::int16_t> pcm(static_cast<std::size_t>(frames)*channels);
            for(std::size_t n=0;n<pcm.size();n++)pcm[n]=std::bit_cast<std::int16_t>(rt.memory().load16(pointer+static_cast<std::uint32_t>(n*2u)));
            const auto result=audio_.output(channel,static_cast<std::int32_t>(c.gpr[5]),static_cast<std::int32_t>(c.gpr[panned?6:5]),pcm,blocking);
            if(!audio_anchored_replay.events.empty() && channel==input_anchor_channel && !audio_input_anchor && result.value==frames){
                audio_input_anchor=clock_.system_microseconds();
                calls.push_back("SyntheticAudioInputAnchor channel="+std::to_string(channel)+" elapsed_us="+std::to_string(*audio_input_anchor)+" source_frames="+std::to_string(frames));
            }
            const auto nonzero=std::count_if(pcm.begin(),pcm.end(),[](auto sample){return sample!=0;});
            calls.push_back("AudioOutput channel="+std::to_string(channel)+" result="+psprecomp::hex32(result.value)+" wait_token="+std::to_string(result.wait_token)+" source_nonzero_samples="+std::to_string(nonzero));
            if(result.wait_token){block_wait(c,WaitKind::Audio,channel,result.wait_token,result.value,0,0);return;}
            c.set_gpr(2,blocking || (result.value&0x80000000u)?result.value:0u);
        });
        rt.nids().add("sceUtility",0x8874DBE0u,"sceUtilitySavedataGetStatus");
        rt.register_hle("sceUtility",0x8874DBE0u,[this](auto&,auto& c){
            const auto status=savedata_.get_status(clock_.system_microseconds());
            calls.push_back("SavedataGetStatus result="+psprecomp::hex32(status));c.set_gpr(2,status);
        });
        rt.register_hle("sceUtility",0x50C4CD57u,[this](auto&,auto& c){
            try{
            const auto result=savedata_.init(c.gpr[4],clock_.system_microseconds());
            calls.push_back("SavedataInitStart mode="+std::to_string(savedata_.mode())+" path="+savedata_.path().string()+" native_path="+savedata_.native_path().string()+" data_size="+std::to_string(rt.memory().load32(c.gpr[4]+124u))+" result="+psprecomp::hex32(result));c.set_gpr(2,result);
            }catch(const std::exception& error){rt.stop(error.what());}
        });
        rt.register_hle("sceUtility",0xD4B95FFBu,[this](auto&,auto& c){
            try{
            const auto result=savedata_.update(clock_.system_microseconds());
            calls.push_back("SavedataUpdate result="+psprecomp::hex32(result)+" request_result="+psprecomp::hex32(savedata_.result()));c.set_gpr(2,result);
            }catch(const std::exception& error){rt.stop(error.what());}
        });
        rt.register_hle("sceUtility",0x9790B33Cu,[this](auto&,auto& c){
            const auto result=savedata_.shutdown(clock_.system_microseconds());calls.push_back("SavedataShutdownStart result="+psprecomp::hex32(result));c.set_gpr(2,result);
        });
        // Screenshot utility (START in the club room): the system saves the
        // current screen to the memory stick (PICTURE folder) and finishes.
        // Status: 0 none, 1 initializing, 2 running, 3 finished, 4 shutting down.
        rt.nids().add("sceUtility",0x0251B134u,"sceUtilityScreenshotInitStart");
        rt.register_hle("sceUtility",0x0251B134u,[this](auto&,auto& c){
            if(screenshot_status_){c.set_gpr(2,0x80110001u);return;}
            if(!rt.memory().contains(c.gpr[4],0x30)){c.set_gpr(2,0x80110002u);return;}
            screenshot_params_=c.gpr[4];screenshot_status_=1;calls.push_back("ScreenshotInitStart params="+psprecomp::hex32(c.gpr[4]));c.set_gpr(2,0);
        });
        rt.nids().add("sceUtility",0xD81957B7u,"sceUtilityScreenshotGetStatus");
        rt.register_hle("sceUtility",0xD81957B7u,[this](auto&,auto& c){
            const auto status=screenshot_status_;
            if(status==1)screenshot_status_=2;else if(status==4)screenshot_status_=0;
            c.set_gpr(2,status);
        });
        rt.nids().add("sceUtility",0xAB083EA9u,"sceUtilityScreenshotUpdate");
        rt.register_hle("sceUtility",0xAB083EA9u,[this](auto&,auto& c){
            if(screenshot_status_!=2){c.set_gpr(2,0);return;}
            std::uint32_t result=0;
            try{
                const auto folder=files_.storage()/"PICTURE"/"ULJM05709";std::filesystem::create_directories(folder);
                unsigned n=1;while(std::filesystem::exists(folder/("SCRN"+std::string(n<10?"000":n<100?"00":n<1000?"0":"")+std::to_string(n)+".BMP")) && n<9999)++n;
                const auto file=folder/("SCRN"+std::string(n<10?"000":n<100?"00":n<1000?"0":"")+std::to_string(n)+".BMP");
                display_.save_bitmap(file);calls.push_back("ScreenshotUpdate saved="+file.string());
            }catch(const std::exception& error){result=0x80110301u;calls.push_back(std::string("ScreenshotUpdate failed: ")+error.what());}
            rt.memory().store32(screenshot_params_+28u,result); // pspUtilityDialogCommon.result
            screenshot_status_=3;c.set_gpr(2,0);
        });
        rt.nids().add("sceUtility",0xF9E0008Cu,"sceUtilityScreenshotShutdownStart");
        rt.register_hle("sceUtility",0xF9E0008Cu,[this](auto&,auto& c){
            if(screenshot_status_!=3){c.set_gpr(2,0x80110001u);return;}
            screenshot_status_=4;c.set_gpr(2,0);
        });
        // On-screen keyboard (sceUtilityOsk, START in the story): the text is
        // typed in a host text box; status 0 none, 1 initializing, 2 visible,
        // 3 finished, 4 shutting down. Only the first field is edited (the PSP
        // dialog edits one at a time and the game asks for one).
        rt.nids().add("sceUtility",0xF6269B82u,"sceUtilityOskInitStart");
        rt.register_hle("sceUtility",0xF6269B82u,[this](auto&,auto& c){
            if(osk_status_){c.set_gpr(2,0x80110001u);return;}
            auto& m=rt.memory();const auto params=c.gpr[4];
            if(!m.contains(params,0x40)){c.set_gpr(2,0x80110002u);return;}
            const auto count=m.load32(params+0x30u),field=m.load32(params+0x34u);
            if(!count || !m.contains(field,0x34)){c.set_gpr(2,0x80110002u);return;}
            const auto read=[&](std::uint32_t address){std::wstring text;if(address)for(unsigned n=0;n<512 && m.contains(address+n*2u,2);++n){const auto ch=m.load16(address+n*2u);if(!ch)break;text.push_back(wchar_t(ch));}return text;};
            const auto length=m.load32(field+36u),limit=m.load32(field+48u);
            const unsigned max_chars=length?((limit==0 || limit>length-1u)?length-1u:limit):0u;
            osk_params_=params;osk_status_=1;
            display_.begin_text_input(read(m.load32(field+28u)),read(m.load32(field+32u)),max_chars);
            calls.push_back("OskInitStart params="+psprecomp::hex32(params)+" fields="+std::to_string(count)+" input_type="+psprecomp::hex32(m.load32(field+16u))+" max_chars="+std::to_string(max_chars));c.set_gpr(2,0);
        });
        rt.nids().add("sceUtility",0xF3F76017u,"sceUtilityOskGetStatus");
        rt.register_hle("sceUtility",0xF3F76017u,[this](auto&,auto& c){
            const auto status=osk_status_;
            if(status==1)osk_status_=2;else if(status==4)osk_status_=0;
            c.set_gpr(2,status);
        });
        rt.nids().add("sceUtility",0x4B85C861u,"sceUtilityOskUpdate");
        rt.register_hle("sceUtility",0x4B85C861u,[this](auto&,auto& c){
            c.set_gpr(2,0);if(osk_status_!=2)return;
            const auto result=display_.text_input_result();if(!result)return;
            auto& m=rt.memory();const auto field=m.load32(osk_params_+0x34u);
            const auto length=m.load32(field+36u),out=m.load32(field+40u),limit=m.load32(field+48u);
            const unsigned max_chars=length?((limit==0 || limit>length-1u)?length-1u:limit):0u;
            const auto chars=std::min<std::size_t>(result->text.size(),max_chars);
            if(out && m.contains(out,(chars+1u)*2u)){for(std::size_t n=0;n<chars;++n)m.store16(out+std::uint32_t(n*2u),std::uint16_t(result->text[n]));m.store16(out+std::uint32_t(chars*2u),0);}
            m.store32(field+44u,result->accepted?2u:1u);       // field result: changed / cancelled
            m.store32(osk_params_+28u,result->accepted?0u:1u); // dialog result: ok / cancel
            calls.push_back("OskUpdate finished accepted="+std::to_string(result->accepted)+" chars="+std::to_string(chars));
            osk_status_=3;
        });
        rt.nids().add("sceUtility",0x3DFAEBA9u,"sceUtilityOskShutdownStart");
        rt.register_hle("sceUtility",0x3DFAEBA9u,[this](auto&,auto& c){
            if(osk_status_!=3){c.set_gpr(2,0x80110001u);return;}
            osk_status_=4;c.set_gpr(2,0);
        });
        // 0xD7763699 is sceWlanGetSwitchState (no arguments; 1 = WLAN switch on),
        // polled every frame. It was taken for sceWlanGetEtherAddr: it returned
        // 0 ("switch OFF" in the Local menu) and wrote six bytes wherever a0
        // happened to point. On PC the switch is always on (play over the LAN).
        rt.nids().add("sceWlanDrv",0xD7763699u,"sceWlanGetSwitchState");
        rt.register_hle("sceWlanDrv",0xD7763699u,[this](auto&,auto& c){
            static bool logged=false;c.set_gpr(2,1);
            if(!logged)calls.push_back("WlanGetSwitchState on");logged=true;
        });
        // ---- Ad hoc multiplayer (sceNet, sceNetAdhocctl, sceNetAdhoc) over the
        // PC's network (native_adhoc.hpp). Blocking calls wait in the
        // scheduler (WaitKind::Network) and keep the PSP's timeouts.
        rt.nids().add("sceNet",0x39AF39A6u,"sceNetInit");
        rt.register_hle("sceNet",0x39AF39A6u,[this](auto&,auto& c){net_pool_=c.gpr[4];calls.push_back("NetInit pool="+std::to_string(c.gpr[4]));c.set_gpr(2,0);});
        rt.nids().add("sceNet",0x281928A9u,"sceNetTerm");
        rt.register_hle("sceNet",0x281928A9u,[this](auto&,auto& c){calls.push_back("NetTerm");c.set_gpr(2,0);});
        rt.nids().add("sceNet",0x0BF0A3AEu,"sceNetGetLocalEtherAddr");
        rt.register_hle("sceNet",0x0BF0A3AEu,[this](auto&,auto& c){
            if(!rt.memory().contains(c.gpr[4],6)){c.set_gpr(2,0x80410103u);return;}
            const auto& m=adhoc().mac();for(unsigned n=0;n<6;++n)rt.memory().store8(c.gpr[4]+n,m[n]);c.set_gpr(2,0);
            if(net_log_budget("GetLocalEtherAddr"))calls.push_back("NetGetLocalEtherAddr ra="+psprecomp::hex32(c.gpr[31]));
        });
        rt.nids().add("sceNet",0xCC393E48u,"sceNetGetMallocStat");
        rt.register_hle("sceNet",0xCC393E48u,[this](auto&,auto& c){
            if(!rt.memory().contains(c.gpr[4],12)){c.set_gpr(2,0x80410103u);return;}
            if(net_log_budget("GetMallocStat"))calls.push_back("NetGetMallocStat ra="+psprecomp::hex32(c.gpr[31]));
            const auto pool=net_pool_?net_pool_:0x20000u;rt.memory().store32(c.gpr[4],pool);rt.memory().store32(c.gpr[4]+4,pool/8u);rt.memory().store32(c.gpr[4]+8,pool-pool/8u);c.set_gpr(2,0);
        });
        rt.nids().add("sceNetAdhoc",0xE1D621D7u,"sceNetAdhocInit");
        rt.register_hle("sceNetAdhoc",0xE1D621D7u,[this](auto&,auto& c){
            if(adhoc_initialized_){c.set_gpr(2,0x80410713u);return;}adhoc_initialized_=true;
            calls.push_back("NetAdhocInit "+adhoc().describe());c.set_gpr(2,0);
        });
        rt.nids().add("sceNetAdhoc",0xA62C6F57u,"sceNetAdhocTerm");
        rt.register_hle("sceNetAdhoc",0xA62C6F57u,[this](auto&,auto& c){adhoc_initialized_=false;calls.push_back("NetAdhocTerm");c.set_gpr(2,0);});
        rt.nids().add("sceNetAdhocctl",0xE26F226Eu,"sceNetAdhocctlInit");
        rt.register_hle("sceNetAdhocctl",0xE26F226Eu,[this](auto&,auto& c){
            if(adhocctl_initialized_){c.set_gpr(2,0x80410B07u);return;}adhocctl_initialized_=true;
            std::string product;if(c.gpr[6] && rt.memory().contains(c.gpr[6],13))for(unsigned n=0;n<9;++n){const char ch=char(rt.memory().load8(c.gpr[6]+4+n));if(!ch)break;product.push_back(ch);}
            adhoc().set_product(product);calls.push_back("NetAdhocctlInit product="+product);c.set_gpr(2,0);
        });
        rt.nids().add("sceNetAdhocctl",0x9D689E13u,"sceNetAdhocctlTerm");
        rt.register_hle("sceNetAdhocctl",0x9D689E13u,[this](auto&,auto& c){
            if(adhoc_)adhoc_->disconnect();adhocctl_initialized_=false;adhoc_handlers_.clear();if(adhoc_)adhoc_->take_events();calls.push_back("NetAdhocctlTerm");c.set_gpr(2,0);
        });
        rt.nids().add("sceNetAdhocctl",0x20B317A0u,"sceNetAdhocctlAddHandler");
        rt.register_hle("sceNetAdhocctl",0x20B317A0u,[this](auto&,auto& c){
            if(adhoc_handlers_.size()>=4){c.set_gpr(2,0x80410B12u);return;}
            const auto id=next_adhoc_handler_++;adhoc_handlers_[id]={c.gpr[4],c.gpr[5]};
            calls.push_back("NetAdhocctlAddHandler id="+std::to_string(id)+" handler="+psprecomp::hex32(c.gpr[4]));c.set_gpr(2,id);
        });
        rt.nids().add("sceNetAdhocctl",0x6402490Bu,"sceNetAdhocctlDelHandler");
        rt.register_hle("sceNetAdhocctl",0x6402490Bu,[this](auto&,auto& c){adhoc_handlers_.erase(c.gpr[4]);calls.push_back("NetAdhocctlDelHandler id="+std::to_string(c.gpr[4]));c.set_gpr(2,0);});
        rt.nids().add("sceNetAdhocctl",0x08FFF7A0u,"sceNetAdhocctlScan");
        rt.register_hle("sceNetAdhocctl",0x08FFF7A0u,[this](auto&,auto& c){adhoc().scan();calls.push_back("NetAdhocctlScan");c.set_gpr(2,0);});
        rt.nids().add("sceNetAdhocctl",0x81AEE1BEu,"sceNetAdhocctlGetScanInfo");
        rt.register_hle("sceNetAdhocctl",0x81AEE1BEu,[this](auto&,auto& c){
            const auto size_ptr=c.gpr[4],buffer=c.gpr[5];if(!rt.memory().contains(size_ptr,4)){c.set_gpr(2,0x80410B04u);return;}
            const auto groups=adhoc().groups();constexpr std::uint32_t entry=28;
            if(!buffer){rt.memory().store32(size_ptr,std::uint32_t(groups.size())*entry);c.set_gpr(2,0);return;}
            const auto room=rt.memory().load32(size_ptr)/entry;const auto count=std::min<std::uint32_t>(room,std::uint32_t(groups.size()));
            if(count && !rt.memory().contains(buffer,count*entry)){c.set_gpr(2,0x80410B04u);return;}
            for(std::uint32_t n=0;n<count;++n){const auto at=buffer+n*entry;
                rt.memory().store32(at,n+1<count?at+entry:0u);rt.memory().store32(at+4,1u); // next, channel
                for(unsigned k=0;k<8;++k)rt.memory().store8(at+8+k,std::uint8_t(groups[n].name[k]));
                for(unsigned k=0;k<6;++k)rt.memory().store8(at+16+k,groups[n].host[k]);
                rt.memory().store16(at+22,0);rt.memory().store32(at+24,1u);} // padding, mode
            rt.memory().store32(size_ptr,count*entry);calls.push_back("NetAdhocctlGetScanInfo groups="+std::to_string(count));c.set_gpr(2,0);
        });
        const auto group_name=[this](std::uint32_t address){std::array<char,8> name{};if(address && rt.memory().contains(address,8))for(unsigned n=0;n<8;++n)name[n]=char(rt.memory().load8(address+n));return name;};
        const auto shown=[](const std::array<char,8>& name){return std::string(name.data(),strnlen(name.data(),8));};
        rt.nids().add("sceNetAdhocctl",0xEC0635C1u,"sceNetAdhocctlCreate");
        rt.register_hle("sceNetAdhocctl",0xEC0635C1u,[this,group_name,shown](auto&,auto& c){const auto name=group_name(c.gpr[4]);adhoc().create(name);calls.push_back("NetAdhocctlCreate group="+shown(name));c.set_gpr(2,0);});
        rt.nids().add("sceNetAdhocctl",0x0AD043EDu,"sceNetAdhocctlConnect");
        rt.register_hle("sceNetAdhocctl",0x0AD043EDu,[this,group_name,shown](auto&,auto& c){
            // Join the group if a host is known, otherwise host it.
            const auto name=group_name(c.gpr[4]);const bool hosted=adhoc().hosted(name);
            if(hosted)adhoc().join(name);else adhoc().create(name);calls.push_back("NetAdhocctlConnect group="+shown(name)+(hosted?" joined":" created"));c.set_gpr(2,0);
        });
        rt.nids().add("sceNetAdhocctl",0x5E7F79C9u,"sceNetAdhocctlJoin");
        rt.register_hle("sceNetAdhocctl",0x5E7F79C9u,[this,group_name,shown](auto&,auto& c){
            if(!c.gpr[4] || !rt.memory().contains(c.gpr[4],28)){c.set_gpr(2,0x80410B04u);return;}
            const auto name=group_name(c.gpr[4]+8);adhoc().join(name);calls.push_back("NetAdhocctlJoin group="+shown(name));c.set_gpr(2,0);
        });
        rt.nids().add("sceNetAdhocctl",0x34401D65u,"sceNetAdhocctlDisconnect");
        rt.register_hle("sceNetAdhocctl",0x34401D65u,[this](auto&,auto& c){adhoc().disconnect();calls.push_back("NetAdhocctlDisconnect");c.set_gpr(2,0);});
        rt.nids().add("sceNetAdhocctl",0xE162CB14u,"sceNetAdhocctlGetPeerList");
        rt.register_hle("sceNetAdhocctl",0xE162CB14u,[this](auto&,auto& c){
            const auto size_ptr=c.gpr[4],buffer=c.gpr[5];if(!rt.memory().contains(size_ptr,4)){c.set_gpr(2,0x80410B04u);return;}
            const auto peers=adhoc().peers();constexpr std::uint32_t entry=152;
            if(net_log_budget("GetPeerList"))calls.push_back("NetAdhocctlGetPeerList peers="+std::to_string(peers.size())+" buffer="+psprecomp::hex32(buffer)+" room="+std::to_string(rt.memory().load32(size_ptr)));
            if(!buffer){rt.memory().store32(size_ptr,std::uint32_t(peers.size())*entry);c.set_gpr(2,0);return;}
            const auto room=rt.memory().load32(size_ptr)/entry;const auto count=std::min<std::uint32_t>(room,std::uint32_t(peers.size()));
            if(count && !rt.memory().contains(buffer,count*entry)){c.set_gpr(2,0x80410B04u);return;}
            const auto now=clock_.system_microseconds();
            for(std::uint32_t n=0;n<count;++n){const auto at=buffer+n*entry;
                rt.memory().store32(at,n+1<count?at+entry:0u);
                for(unsigned k=0;k<128;++k)rt.memory().store8(at+4+k,k<peers[n].nickname.size()?std::uint8_t(peers[n].nickname[k]):0);
                for(unsigned k=0;k<6;++k)rt.memory().store8(at+132+k,peers[n].mac[k]);
                rt.memory().store16(at+138,0);rt.memory().store32(at+140,0x0400u);
                rt.memory().store32(at+144,std::uint32_t(now));rt.memory().store32(at+148,std::uint32_t(now>>32));}
            rt.memory().store32(size_ptr,count*entry);c.set_gpr(2,0);
        });
        // PDP / PTP. Arguments past a3 arrive in t0..t3 (gpr 8..11).
        const auto read_mac=[this](std::uint32_t address){kon::Mac m{};if(address && rt.memory().contains(address,6))for(unsigned n=0;n<6;++n)m[n]=rt.memory().load8(address+n);return m;};
        rt.nids().add("sceNetAdhoc",0x6F92741Bu,"sceNetAdhocPdpCreate");
        rt.register_hle("sceNetAdhoc",0x6F92741Bu,[this](auto&,auto& c){
            const auto id=adhoc().pdp_create(std::uint16_t(c.gpr[5]));
            calls.push_back("NetAdhocPdpCreate port="+std::to_string(c.gpr[5]&0xFFFFu)+" id="+std::to_string(id));
            c.set_gpr(2,id>0?std::uint32_t(id):0x8041070Au);
        });
        rt.nids().add("sceNetAdhoc",0x7F27BB5Eu,"sceNetAdhocPdpDelete");
        rt.register_hle("sceNetAdhoc",0x7F27BB5Eu,[this](auto&,auto& c){adhoc().pdp_delete(int(c.gpr[4]));calls.push_back("NetAdhocPdpDelete id="+std::to_string(c.gpr[4]));c.set_gpr(2,0);});
        rt.nids().add("sceNetAdhoc",0xABED3790u,"sceNetAdhocPdpSend");
        rt.register_hle("sceNetAdhoc",0xABED3790u,[this,read_mac](auto&,auto& c){
            const auto size=c.gpr[8];if(size>65523u || (size && !rt.memory().contains(c.gpr[7],size))){c.set_gpr(2,0x80410705u);return;}
            std::vector<std::uint8_t> data(size);for(std::uint32_t n=0;n<size;++n)data[n]=rt.memory().load8(c.gpr[7]+n);
            const auto status=adhoc().pdp_send(int(c.gpr[4]),read_mac(c.gpr[5]),std::uint16_t(c.gpr[6]),data.data(),data.size());
            if(net_log_budget("PdpSend")){std::string hex;for(std::size_t n=0;n<std::min<std::size_t>(data.size(),40);++n){char b[4];std::snprintf(b,sizeof b,"%02x",data[n]);hex+=b;}
                const auto to=read_mac(c.gpr[5]);calls.push_back("NetAdhocPdpSend id="+std::to_string(c.gpr[4])+" to="+(to==kon::kBroadcastMac?std::string("broadcast"):std::string("peer"))+" port="+std::to_string(c.gpr[6]&0xFFFFu)+" size="+std::to_string(size)+" data="+hex);}
            c.set_gpr(2,status==kon::AdhocNet::Status::Ok?0u:0x80410701u);
        });
        rt.nids().add("sceNetAdhoc",0xDFE53E03u,"sceNetAdhocPdpRecv");
        rt.register_hle("sceNetAdhoc",0xDFE53E03u,[this](auto&,auto& c){
            const int id=int(c.gpr[4]);const std::uint32_t mac_out=c.gpr[5],port_out=c.gpr[6],buffer=c.gpr[7],length_ptr=c.gpr[8];
            if(!rt.memory().contains(length_ptr,4)){c.set_gpr(2,0x80410711u);return;}
            net_call(c,c.gpr[9],c.gpr[10]!=0,[this,id,mac_out,port_out,buffer,length_ptr]()->std::optional<std::uint32_t>{
                kon::AdhocNet::Datagram d;const auto status=adhoc().pdp_recv(id,d);
                if(status==kon::AdhocNet::Status::WouldBlock)return std::nullopt;
                if(net_log_budget("PdpRecv")){std::string hex;for(std::size_t n=0;n<std::min<std::size_t>(d.data.size(),40);++n){char b[4];std::snprintf(b,sizeof b,"%02x",d.data[n]);hex+=b;}
                    calls.push_back("NetAdhocPdpRecv id="+std::to_string(id)+" size="+std::to_string(d.data.size())+" from_port="+std::to_string(d.port)+(d.source==adhoc().mac()?" self":" peer")+" data="+hex);}
                if(status!=kon::AdhocNet::Status::Ok)return 0x80410701u;
                const auto room=rt.memory().load32(length_ptr);
                if(d.data.size()>room || (d.data.size() && !rt.memory().contains(buffer,std::uint32_t(d.data.size())))){rt.memory().store32(length_ptr,std::uint32_t(d.data.size()));return 0x80410706u;}
                for(std::size_t n=0;n<d.data.size();++n)rt.memory().store8(buffer+std::uint32_t(n),d.data[n]);
                rt.memory().store32(length_ptr,std::uint32_t(d.data.size()));
                if(mac_out && rt.memory().contains(mac_out,6))for(unsigned n=0;n<6;++n)rt.memory().store8(mac_out+n,d.source[n]);
                if(port_out && rt.memory().contains(port_out,2))rt.memory().store16(port_out,d.port);
                return 0u;});
        });
        rt.nids().add("sceNetAdhoc",0x877F6D66u,"sceNetAdhocPtpOpen");
        rt.register_hle("sceNetAdhoc",0x877F6D66u,[this,read_mac](auto&,auto& c){
            const auto id=adhoc().ptp_open(std::uint16_t(c.gpr[5]),read_mac(c.gpr[6]),std::uint16_t(c.gpr[7]));
            calls.push_back("NetAdhocPtpOpen port="+std::to_string(c.gpr[5]&0xFFFFu)+" to_port="+std::to_string(c.gpr[7]&0xFFFFu)+" id="+std::to_string(id));c.set_gpr(2,std::uint32_t(id));
        });
        rt.nids().add("sceNetAdhoc",0xE08BDAC1u,"sceNetAdhocPtpListen");
        rt.register_hle("sceNetAdhoc",0xE08BDAC1u,[this](auto&,auto& c){
            const auto id=adhoc().ptp_listen(std::uint16_t(c.gpr[5]));
            calls.push_back("NetAdhocPtpListen port="+std::to_string(c.gpr[5]&0xFFFFu)+" id="+std::to_string(id));c.set_gpr(2,id>0?std::uint32_t(id):0x8041070Au);
        });
        rt.nids().add("sceNetAdhoc",0xFC6FC07Bu,"sceNetAdhocPtpConnect");
        rt.register_hle("sceNetAdhoc",0xFC6FC07Bu,[this](auto&,auto& c){
            const auto id=int(c.gpr[4]);
            net_call(c,c.gpr[5],c.gpr[6]!=0,[this,id]()->std::optional<std::uint32_t>{
                const auto status=adhoc().ptp_connect(id);
                if(status==kon::AdhocNet::Status::WouldBlock)return std::nullopt;
                calls.push_back("NetAdhocPtpConnect id="+std::to_string(id)+" status="+std::to_string(int(status)));
                return status==kon::AdhocNet::Status::Ok?0u:status==kon::AdhocNet::Status::InvalidId?0x80410701u:0x80410718u;});
        });
        rt.nids().add("sceNetAdhoc",0x9DF81198u,"sceNetAdhocPtpAccept");
        rt.register_hle("sceNetAdhoc",0x9DF81198u,[this](auto&,auto& c){
            const int id=int(c.gpr[4]);const std::uint32_t mac_out=c.gpr[5],port_out=c.gpr[6];
            net_call(c,c.gpr[7],c.gpr[8]!=0,[this,id,mac_out,port_out]()->std::optional<std::uint32_t>{
                kon::AdhocNet::Accepted a;const auto status=adhoc().ptp_accept(id,a);
                if(status==kon::AdhocNet::Status::WouldBlock)return std::nullopt;
                if(status!=kon::AdhocNet::Status::Ok)return 0x80410701u;
                if(mac_out && rt.memory().contains(mac_out,6))for(unsigned n=0;n<6;++n)rt.memory().store8(mac_out+n,a.peer[n]);
                if(port_out && rt.memory().contains(port_out,2))rt.memory().store16(port_out,a.port);
                calls.push_back("NetAdhocPtpAccept listener="+std::to_string(id)+" id="+std::to_string(a.id));
                return std::uint32_t(a.id);});
        });
        rt.nids().add("sceNetAdhoc",0x4DA4C788u,"sceNetAdhocPtpSend");
        rt.register_hle("sceNetAdhoc",0x4DA4C788u,[this](auto&,auto& c){
            const int id=int(c.gpr[4]);const std::uint32_t data=c.gpr[5],length_ptr=c.gpr[6];
            if(!rt.memory().contains(length_ptr,4)){c.set_gpr(2,0x80410711u);return;}
            net_call(c,c.gpr[7],c.gpr[8]!=0,[this,id,data,length_ptr]()->std::optional<std::uint32_t>{
                const auto length=rt.memory().load32(length_ptr);if(length && !rt.memory().contains(data,length))return 0x80410711u;
                std::vector<std::uint8_t> bytes(length);for(std::uint32_t n=0;n<length;++n)bytes[n]=rt.memory().load8(data+n);
                std::size_t size=bytes.size();const auto status=adhoc().ptp_send(id,bytes.data(),size);
                if(status==kon::AdhocNet::Status::WouldBlock)return std::nullopt;
                if(status!=kon::AdhocNet::Status::Ok)return status==kon::AdhocNet::Status::InvalidId?0x80410701u:0x8041070Cu;
                rt.memory().store32(length_ptr,std::uint32_t(size));return 0u;});
        });
        rt.nids().add("sceNetAdhoc",0x8BEA2B3Eu,"sceNetAdhocPtpRecv");
        rt.register_hle("sceNetAdhoc",0x8BEA2B3Eu,[this](auto&,auto& c){
            const int id=int(c.gpr[4]);const std::uint32_t buffer=c.gpr[5],length_ptr=c.gpr[6];
            if(!rt.memory().contains(length_ptr,4)){c.set_gpr(2,0x80410711u);return;}
            net_call(c,c.gpr[7],c.gpr[8]!=0,[this,id,buffer,length_ptr]()->std::optional<std::uint32_t>{
                const auto room=rt.memory().load32(length_ptr);if(room && !rt.memory().contains(buffer,room))return 0x80410711u;
                std::vector<std::uint8_t> bytes(room);std::size_t size=bytes.size();const auto status=adhoc().ptp_recv(id,bytes.data(),size);
                if(status==kon::AdhocNet::Status::WouldBlock)return std::nullopt;
                if(status!=kon::AdhocNet::Status::Ok)return status==kon::AdhocNet::Status::InvalidId?0x80410701u:0x8041070Cu;
                for(std::size_t n=0;n<size;++n)rt.memory().store8(buffer+std::uint32_t(n),bytes[n]);
                rt.memory().store32(length_ptr,std::uint32_t(size));return 0u;});
        });
        rt.nids().add("sceNetAdhoc",0x9AC2EEACu,"sceNetAdhocPtpFlush");
        rt.register_hle("sceNetAdhoc",0x9AC2EEACu,[this](auto&,auto& c){
            const auto status=adhoc().ptp_flush(int(c.gpr[4]));c.set_gpr(2,status==kon::AdhocNet::Status::Ok?0u:status==kon::AdhocNet::Status::InvalidId?0x80410701u:0x8041070Cu);
        });
        rt.nids().add("sceNetAdhoc",0x157E6225u,"sceNetAdhocPtpClose");
        rt.register_hle("sceNetAdhoc",0x157E6225u,[this](auto&,auto& c){adhoc().ptp_close(int(c.gpr[4]));calls.push_back("NetAdhocPtpClose id="+std::to_string(c.gpr[4]));c.set_gpr(2,0);});
        rt.nids().add("sceAtrac3plus",0x132F1ECAu,"sceAtracReinit");
        rt.register_hle("sceAtrac3plus",0x132F1ECAu,[this](auto&,auto& c){
            const auto result=media_.reinit_atrac(static_cast<std::int32_t>(c.gpr[4]),static_cast<std::int32_t>(c.gpr[5]));
            calls.push_back("AtracReinit atrac3="+std::to_string(static_cast<std::int32_t>(c.gpr[4]))+" atrac3plus="+std::to_string(static_cast<std::int32_t>(c.gpr[5]))+" native_slots="+std::to_string(media_.atrac_capacity())+" result="+psprecomp::hex32(result));c.set_gpr(2,result);
        });
        rt.nids().add("sceMpeg",0x682A619Bu,"sceMpegInit");
        rt.register_hle("sceMpeg",0x682A619Bu,[this](auto&,auto& c){
            const auto result=media_.initialize_video();calls.push_back("MpegInit native_h264_result="+psprecomp::hex32(result));c.set_gpr(2,result);
        });
        rt.register_hle("sceMpeg",0x874624D6u,[this](auto&,auto& c){c.set_gpr(2,media_.finish_video());calls.push_back("MpegFinish released_native_decoder");});
        rt.nids().add("sceUtility",0x2A2B3DE0u,"sceUtilityLoadModule");
        rt.nids().add("sceUtility",0xE49BFE92u,"sceUtilityUnloadModule");
        rt.register_hle("sceUtility",0x2A2B3DE0u,[this](auto&,auto& c){
            // Network modules (0x100 common, 0x101 adhoc, ...): served natively.
            if(c.gpr[4]>=0x100u && c.gpr[4]<=0x107u){calls.push_back("UtilityLoadModule id="+psprecomp::hex32(c.gpr[4])+" result=0x00000000 native_network");c.set_gpr(2,0);return;}
            try {
                const auto result=media_.load_module(c.gpr[4]);
                calls.push_back("UtilityLoadModule id="+psprecomp::hex32(c.gpr[4])+" result="+psprecomp::hex32(result)+" native_codec="+media_.version());c.set_gpr(2,result);
            }catch(const std::exception& error){rt.stop(error.what());}
        });
        rt.register_hle("sceUtility",0xE49BFE92u,[this](auto&,auto& c){
            if(c.gpr[4]>=0x100u && c.gpr[4]<=0x107u){calls.push_back("UtilityUnloadModule id="+psprecomp::hex32(c.gpr[4])+" result=0x00000000 native_network");c.set_gpr(2,0);return;}
            const auto result=media_.unload_module(c.gpr[4]);calls.push_back("UtilityUnloadModule id="+psprecomp::hex32(c.gpr[4])+" result="+psprecomp::hex32(result));c.set_gpr(2,result);
        });
        // Cached/uncached guest aliases share native storage; synchronize
        // publication to the native drawing backend without a fake PSP cache.
        for(const auto nid:{0x3EE30821u,0x34B9FA9Eu})rt.register_hle("UtilsForUser",nid,[this,nid](auto&,auto& c){
            if(c.gpr[5] && !rt.memory().contains(c.gpr[4],c.gpr[5])) {rt.stop("Dcache range outside mapped memory");return;}
            rt.memory().memory_barrier();
            calls.push_back(std::string(nid==0x3EE30821u?"DcacheWritebackRange":"DcacheWritebackInvalidateRange")+" address="+psprecomp::hex32(c.gpr[4])+" size="+std::to_string(c.gpr[5]));
        });
        rt.register_hle("sceDisplay",0x0E20F177u,[this](auto&,auto& c){
            const auto result=display_.set_mode(c.gpr[4],c.gpr[5],c.gpr[6]);
            calls.push_back("DisplaySetMode width="+std::to_string(c.gpr[5])+" height="+std::to_string(c.gpr[6]));c.set_gpr(2,result);
        });
        rt.register_hle("sceDisplay",0x289D82FEu,[this](auto&,auto& c){
            trace(8,c.gpr[4]);const auto result=display_.set_buffer(c.gpr[4],c.gpr[5],c.gpr[6],c.gpr[7]);
            if(!result && c.gpr[4] && gpu_raster_backend_)gpu_raster_backend_->note_display_address(c.gpr[4]);
            if(!result && c.gpr[4])finish_render_frame(c.gpr[4],c.gpr[5],c.gpr[6]);
            calls.push_back("DisplaySetFrameBuffer address="+psprecomp::hex32(c.gpr[4])+" stride="+std::to_string(c.gpr[5])+" format="+std::to_string(c.gpr[6])+" sync="+std::to_string(c.gpr[7]));c.set_gpr(2,result);
        });
        rt.nids().add("sceDisplay",0x77ED8B3Au,"sceDisplayWaitVblankStartMultiCB");
        rt.register_hle("sceDisplay",0x77ED8B3Au,[this](auto&,auto& c){
            auto cycles=c.gpr[4];
            if(!cycles || cycles>0x7FFFFFFFu){c.set_gpr(2,0x80000107u);return;}
            // Diagnostic only (--logic-vblank-cycles): measures how the game's
            // timing reacts to a different logic rate. Never a default.
            if(!dispatch_callbacks(c))return;
            // PC frame-rate modes: the finished frame is shown at once and the next
            // starts at the target rate's deadline (immediately if this one ran
            // late); the patched game code advances by the real time in between.
            // The original 30 FPS mode keeps the game's own vblank wait.
            if(logic_vblank_cycles)cycles=logic_vblank_cycles;
            std::uint64_t pc_start=0;
            language_restart_step();
            if(settings_fps_)settings_fps_->frame_end();
            scale_schedule_step();follow_window_screen_mode();
            controls_.set_frame_interval(logic_rate.original()?0u:1000000u/logic_rate.target());
            display_.set_pc_presentation(!logic_rate.original());
            if(logic_rate.original())logic_rate.original_frame();
            else{
                // The finished frame is shown at once. With the GPU batch shading
                // in parallel it is usually done by now (deferring the latch to
                // the next frame measured no FPS gain and up to a frame more lag).
                display_.latch_now();pc_start=logic_rate.pace(clock_.system_microseconds());cycles=1;
            }
            // At most two finished frames wait for the render thread to show them.
            try{renderer_.wait_presents(2);}catch(const std::exception& error){rt.stop(error.what());return;}
            ++vblank_waits_[std::min<std::uint32_t>(cycles,3u)];
            ++kon_logic_frame_index;
            if(ram_snapshot_count)ram_snapshot(clock_.system_microseconds());
            if(!ram_variation_file.empty())ram_variation(clock_.system_microseconds());
            if(speed_audit_end_us)speed_audit(clock_.system_microseconds());
            const auto now=clock_.system_microseconds();
            const auto delay=logic_rate.original()?display_.until_vblank(now,cycles):(pc_start>now?pc_start-now:std::uint64_t{1}); // a 0 us wait would mean no timeout
            trace(5,delay);calls.push_back("DisplayWaitVblankStartMultiCB cycles="+std::to_string(cycles)+" elapsed_us="+std::to_string(now)+" delay_us="+std::to_string(delay));
            block_wait(c,WaitKind::Delay,0,0,0,0,0,delay,true);
        });
        // Each thread waits for the vblank after the last one it was given. A
        // thread woken late by this cooperative scheduler (a PSP thread of its
        // priority would have run at once) does not lose the vblank that passed
        // meanwhile: CRI's audio thread counts vblanks as its playback clock,
        // and lost ones made songs drift behind the music. More than four behind
        // (the thread was not waiting), it starts again from the next vblank.
        const auto vblank=[this](auto&,auto& c){
            const auto now=clock_.system_microseconds();const auto current=display_.frame_count(now);
            const auto uid=static_cast<std::uint32_t>(psprecomp::runtime_thread_uid());
            static const bool lossy=std::getenv("KON_LOSSY_VBLANK")!=nullptr; // previous behaviour (A/B)
            auto& last=vblank_given_[uid];
            std::uint64_t target=last && !lossy?last+1:current+1;
            if(target+4<=current)target=current+1;
            last=target;
            if(target<=current){++vblank_caught_up_;c.set_gpr(2,0);return;}
            block_wait(c,WaitKind::Delay,0,0,0,0,0,std::max<std::uint64_t>(1,display_.vblank_time(target)-now));
        };
        rt.register_hle("sceDisplay",0x984C27E7u,vblank);
        rt.register_hle("sceDisplay",0x46F186C3u,[this](auto&,auto& c){
            if(!dispatch_callbacks(c))return;
            ++vblank_waits_[0];
            if(ram_snapshot_count)ram_snapshot(clock_.system_microseconds());
            if(!ram_variation_file.empty())ram_variation(clock_.system_microseconds());
            block_wait(c,WaitKind::Delay,0,0,0,0,0,display_.until_next_vblank(clock_.system_microseconds()),true);
        });
        const auto enqueue=[this](auto&,auto& c,bool front){
            unsigned stack_limit=32;
            if(c.gpr[7]) {
                const auto ptr=c.gpr[7];
                if(!rt.memory().contains(ptr,4)){c.set_gpr(2,0x80000103u);return;}
                const auto size=rt.memory().load32(ptr);
                if(size<4 || size>16 || !rt.memory().contains(ptr,size)){c.set_gpr(2,0x80000103u);return;}
                const auto context=size>=8?rt.memory().load32(ptr+4u):0u;
                const auto stacks=size>=12?rt.memory().load32(ptr+8u):0u;
                calls.push_back("GeListArgs size="+std::to_string(size)+" context="+psprecomp::hex32(context)+" stacks="+std::to_string(stacks));
                if(context){rt.stop("GE saved context buffer not yet implemented");return;}
                if(stacks>32){rt.stop("GE explicit stack count above hardware maximum");return;}
                if(stacks)stack_limit=stacks;
            }
            const auto callback=static_cast<std::int32_t>(c.gpr[6]);
            if(callback!=-1 && !ge_callbacks_.contains(c.gpr[6])){c.set_gpr(2,0x80000100u);return;}
            const auto id=ge_.enqueue(c.gpr[4],c.gpr[5],callback,front);trace(1,id);
            calls.push_back("GeEnqueue id="+std::to_string(id)+" list="+psprecomp::hex32(c.gpr[4])+" stall="+psprecomp::hex32(c.gpr[5]));
            if(id<64)ge_.lists.at(id).stack_limit=stack_limit;
            c.set_gpr(2,id);
        };
        rt.register_hle("sceGe_user",0xAB49E76Au,[enqueue](auto& r,auto& c){enqueue(r,c,false);});
        rt.register_hle("sceGe_user",0x1C0D95A6u,[enqueue](auto& r,auto& c){enqueue(r,c,true);});
        rt.register_hle("sceGe_user",0xE0D68148u,[this](auto&,auto& c){
            trace(2,c.gpr[4]);c.set_gpr(2,ge_.update_stall(c.gpr[4],c.gpr[5]));
        });
        rt.register_hle("sceGe_user",0x03444EB4u,[this](auto&,auto& c){
            if(c.gpr[5]>1){c.set_gpr(2,0x80000107u);return;}
            const auto state=ge_.list_status(c.gpr[4]);
            trace(3,state);if(!c.gpr[5] && state>0 && state<5){block_wait(c,WaitKind::GeList,c.gpr[4],0,0,0,0);return;}
            c.set_gpr(2,state);
        });
        rt.register_hle("sceGe_user",0xB287BD61u,[this](auto&,auto& c){
            if(c.gpr[4]>1){c.set_gpr(2,0x80000107u);return;}
            const auto state=ge_.draw_status();
            trace(4,state);if(!c.gpr[4] && state){block_wait(c,WaitKind::GeDraw,0,0,0,0,0);return;}
            c.set_gpr(2,state);
        });
        rt.nids().add("sceGe_user",0xA4FC06A4u,"sceGeSetCallback");
        rt.register_hle("sceGe_user",0xA4FC06A4u,[this](auto&,auto& c){
            if(!rt.memory().contains(c.gpr[4],16)){c.set_gpr(2,0x800200D3u);return;}
            unsigned id=0;while(id<16 && ge_callbacks_.contains(id))++id;
            if(id==16){c.set_gpr(2,0x80020190u);return;}
            GeCallback cb{rt.memory().load32(c.gpr[4]),rt.memory().load32(c.gpr[4]+4u),
                          rt.memory().load32(c.gpr[4]+8u),rt.memory().load32(c.gpr[4]+12u)};
            ge_callbacks_.emplace(id,cb);calls.push_back("GeSetCallback id="+std::to_string(id)+" signal="+psprecomp::hex32(cb.signal)+" finish="+psprecomp::hex32(cb.finish));c.set_gpr(2,id);
        });
        rt.register_hle("sceGe_user",0x05DB22CEu,[this](auto&,auto& c){
            if(c.gpr[4]>=16){c.set_gpr(2,0x80000100u);return;}
            ge_callbacks_.erase(c.gpr[4]);c.set_gpr(2,0);
        });
        rt.register_hle("sceGe_user",0xE47E40E4u,[this](auto&,auto& c){
            calls.push_back("GeEdramGetAddr mapped="+psprecomp::hex32(psprecomp::GuestMemory::kVramPhysicalBase));
            c.set_gpr(2,psprecomp::GuestMemory::kVramPhysicalBase);
        });
        rt.register_hle("sceGe_user",0x1F6752ADu,[this](auto&,auto& c){c.set_gpr(2,rt.memory().vram_size());});
        rt.register_hle("IoFileMgrForUser",0x109F50BCu,[this](auto&,auto& c){
            const auto name=guest_name(c.gpr[4]);const auto id=files_.open(name,c.gpr[5]);
            calls.push_back("IoOpen path="+name+" flags="+psprecomp::hex32(c.gpr[5])+" descriptor="+psprecomp::hex32(id));c.set_gpr(2,id);
        });
        rt.nids().add("IoFileMgrForUser",0xACE946E8u,"sceIoGetstat");
        rt.register_hle("IoFileMgrForUser",0xACE946E8u,[this](auto&,auto& c){
            const auto name=guest_name(c.gpr[4]);const auto out=c.gpr[5];
            if(!rt.memory().contains(out,0x58)){c.set_gpr(2,kon::NativeFiles::invalid);return;}
            std::array<std::uint8_t,0x58> stat{};const auto result=files_.stat(name,stat);
            if(!result)rt.memory().copy_in(out,stat);
            calls.push_back("IoGetstat path="+name+" result="+psprecomp::hex32(result));c.set_gpr(2,result);
        });
        rt.register_hle("IoFileMgrForUser",0x810C4BC3u,[this](auto&,auto& c){c.set_gpr(2,files_.close(c.gpr[4]));});
        rt.register_hle("IoFileMgrForUser",0x6A638D83u,[this](auto&,auto& c){
            const auto size=c.gpr[6],address=c.gpr[5],id=c.gpr[4];
            if(size>0x7FFFFFFFu || (size && !rt.memory().contains(address,size))){c.set_gpr(2,kon::NativeFiles::invalid);return;}
            if(const auto f=files_.files.find(id);f!=files_.files.end() && f->second.disc)note_disc_read(f->second.path,files_.seek(id,0,1));
            std::vector<std::uint8_t> data(size);const auto count=files_.read(id,data);
            if(count<=size && count)rt.memory().copy_in(address,std::span<const std::uint8_t>(data.data(),count));
            calls.push_back("IoRead descriptor="+std::to_string(id)+" requested="+std::to_string(size)+" result="+psprecomp::hex32(count));c.set_gpr(2,count);
        });
        rt.register_hle("IoFileMgrForUser",0x42EC03ACu,[this](auto&,auto& c){
            const auto size=c.gpr[6],address=c.gpr[5],id=c.gpr[4];
            if(size>0x7FFFFFFFu || (size && !rt.memory().contains(address,size))){c.set_gpr(2,kon::NativeFiles::invalid);return;}
            std::vector<std::uint8_t> data(size);if(size)rt.memory().copy_out(address,data);c.set_gpr(2,files_.write(id,data));
        });
        rt.register_hle("IoFileMgrForUser",0x27EB27B8u,[this](auto&,auto& c){
            const auto bits=static_cast<std::uint64_t>(c.gpr[6])|(static_cast<std::uint64_t>(c.gpr[7])<<32u);
            const auto offset=std::bit_cast<std::int64_t>(bits);const auto result=files_.seek(c.gpr[4],offset,c.gpr[8]);
            calls.push_back("IoSeek descriptor="+std::to_string(c.gpr[4])+" offset="+std::to_string(offset)+" origin="+std::to_string(c.gpr[8])+" result="+std::to_string(result));
            const auto encoded=static_cast<std::uint64_t>(result);c.set_gpr(2,static_cast<std::uint32_t>(encoded));c.set_gpr(3,static_cast<std::uint32_t>(encoded>>32u));
        });
        rt.nids().add("scePower",0x04B7766Eu,"scePowerRegisterCallback");
        rt.register_hle("scePower",0x04B7766Eu,[this](auto&,auto& c){
            auto slot=static_cast<std::int32_t>(c.gpr[4]);const auto id=c.gpr[5];const bool automatic=slot==-1;
            if(slot<-1 || slot>=32){c.set_gpr(2,0x80000102u);return;}
            if(slot>=16){c.set_gpr(2,0x80000023u);return;}
            if(!callbacks_.contains(id)){c.set_gpr(2,0x80000100u);return;}
            if(automatic){slot=-1;for(int n=0;n<16;n++)if(!power_slots_[n]){slot=n;break;}}
            if(slot<0){c.set_gpr(2,0x80000022u);return;}
            if(power_slots_[slot]){c.set_gpr(2,0x80000020u);return;}
            const auto state=kon::host_power_flags();power_slots_[slot]=id;notify_callback(id,state);
            calls.push_back("PowerRegisterCallback slot="+std::to_string(slot)+" uid="+std::to_string(id)+" host_flags="+psprecomp::hex32(state));
            c.set_gpr(2,automatic?static_cast<std::uint32_t>(slot):0u);
        });
        rt.register_hle("scePower",0xDFA8BAF8u,[this](auto&,auto& c){
            const auto slot=static_cast<std::int32_t>(c.gpr[4]);
            if(slot<0 || slot>=32){c.set_gpr(2,0x80000102u);return;}
            if(slot>=16){c.set_gpr(2,0x80000023u);return;}
            if(!power_slots_[slot]){c.set_gpr(2,0x80000025u);return;}
            power_slots_[slot]=0;c.set_gpr(2,0);
        });
        rt.register_hle("IoFileMgrForUser",0x54F5FB11u,[this](auto&,auto& c){
            const auto name=guest_name(c.gpr[4]);const auto command=c.gpr[5];
            if(name=="fatms0:" && command==0x02415821u) {
                if(c.gpr[7]<4 || !rt.memory().contains(c.gpr[6],c.gpr[7])){c.set_gpr(2,0x80010016u);return;}
                const auto id=rt.memory().load32(c.gpr[6]);
                if(!callbacks_.contains(id) || memory_callbacks_.size()>=32){c.set_gpr(2,0x80010016u);return;}
                std::filesystem::create_directories(content_.root()/"userdata/ms0");
                memory_callbacks_.push_back(id);notify_callback(id,1u);
                calls.push_back("MemoryStorageCallback uid="+std::to_string(id)+" folder=userdata/ms0 mounted=1");c.set_gpr(2,0);return;
            }
            if(name=="fatms0:" && command==0x02415822u) {
                if(c.gpr[7]<4 || !rt.memory().contains(c.gpr[6],4)){c.set_gpr(2,0x80010016u);return;}
                const auto id=rt.memory().load32(c.gpr[6]);const auto it=std::find(memory_callbacks_.begin(),memory_callbacks_.end(),id);
                if(it==memory_callbacks_.end()){c.set_gpr(2,0x80010016u);return;}
                memory_callbacks_.erase(it);c.set_gpr(2,0);return;
            }
            if(name=="fatms0:" && command==0x02425823u) {
                if(!rt.memory().contains(c.gpr[8],4)){c.set_gpr(2,0x80010016u);return;}
                // Polled every frame: once the folder exists it stays mounted.
                static bool mounted=false;if(!mounted)mounted=std::filesystem::is_directory(content_.root()/"userdata/ms0");
                rt.memory().store32(c.gpr[8],mounted?1u:0u);c.set_gpr(2,0);return;
            }
            rt.stop("Unsupported storage command "+name+" "+psprecomp::hex32(command));
        });
        rt.nids().add("ThreadManForUser",0x349D6D6Cu,"sceKernelCheckCallback");
        rt.register_hle("ThreadManForUser",0x349D6D6Cu,[this](auto&,auto& c){
            if(callback_depth_){c.set_gpr(2,0x80020064u);return;}
            unsigned dispatched=0;if(dispatch_callbacks(c,&dispatched))c.set_gpr(2,dispatched?1u:0u);
        });
        rt.nids().add("ThreadManForUser",0x71BC9871u,"sceKernelChangeThreadPriority");
        rt.register_hle("ThreadManForUser",0x71BC9871u,[this](auto&,auto& c){
            const auto current=static_cast<std::uint32_t>(psprecomp::runtime_thread_uid());
            const auto id=c.gpr[4]?c.gpr[4]:current;
            const auto found=threads.find(id);
            if(found==threads.end()){c.set_gpr(2,0x80020198u);return;}
            const auto priority=c.gpr[5]?c.gpr[5]:threads.at(current).priority;
            if(priority<8 || priority>119){c.set_gpr(2,0x80020193u);return;}
            found->second.priority=priority;
            calls.push_back("ChangeThreadPriority uid="+std::to_string(id)+" priority="+std::to_string(priority));
            c.set_gpr(2,0);maybe_preempt(c);
        });
        const auto delay=[this](auto&,auto& c){block_wait(c,WaitKind::Delay,0,0,0,0,0,std::max(c.gpr[4],200u));};
        rt.register_hle("ThreadManForUser",0xCEADEB47u,delay);
        rt.register_hle("ThreadManForUser",0x68DA9E36u,[this](auto&,auto& c){
            if(!dispatch_callbacks(c))return;
            block_wait(c,WaitKind::Delay,0,0,0,0,0,std::max(c.gpr[4],200u),true);
        });
        for(const auto nid:{0x9ACE131Eu,0x82826F70u}){
            const bool callbacks=nid==0x82826F70u;
            rt.nids().add("ThreadManForUser",nid,callbacks?"sceKernelSleepThreadCB":"sceKernelSleepThread");
            rt.register_hle("ThreadManForUser",nid,[this,callbacks](auto&,auto& c){
                if(callback_depth_ || !interrupts_enabled_){c.set_gpr(2,0x80020064u);return;}
                const auto id=static_cast<std::uint32_t>(psprecomp::runtime_thread_uid());
                const auto found=threads.find(id);
                if(found==threads.end()){rt.stop("SleepThread outside registered worker");return;}
                if(callbacks && !dispatch_callbacks(c))return;
                auto& t=found->second;
                calls.push_back("SleepThread uid="+std::to_string(id)+" callbacks="+std::to_string(callbacks)+" pending_wakeups="+std::to_string(t.wakeup_count));
                if(t.wakeup_count){--t.wakeup_count;c.set_gpr(2,0);return;}
                block_wait(c,WaitKind::Sleep,0,0,0,0,0,0,callbacks);
            });
        }
        rt.nids().add("ThreadManForUser",0xD59EAD2Fu,"sceKernelWakeupThread");
        rt.register_hle("ThreadManForUser",0xD59EAD2Fu,[this](auto&,auto& c){
            const auto id=c.gpr[4],current=static_cast<std::uint32_t>(psprecomp::runtime_thread_uid());
            if(!id || id==current){c.set_gpr(2,0x80020197u);return;}
            const auto found=threads.find(id);
            if(found==threads.end()){c.set_gpr(2,0x80020198u);return;}
            auto& t=found->second;
            if(!t.started || t.finished){c.set_gpr(2,0x800201A2u);return;}
            c.set_gpr(2,0);
            if(t.wait==WaitKind::Sleep){wake(t);calls.push_back("WakeupThread uid="+std::to_string(id)+" released=1");maybe_preempt(c);}
            else {
                if(t.wakeup_count==0x7FFFFFFFu){rt.stop("Wakeup count overflow requires measured kernel error");return;}
                ++t.wakeup_count;calls.push_back("WakeupThread uid="+std::to_string(id)+" pending="+std::to_string(t.wakeup_count));
            }
        });
        rt.register_hle("ThreadManForUser",0x369ED59Du,[this](auto&,auto& c){++time_calls_[0];c.set_gpr(2,static_cast<std::uint32_t>(clock_.system_microseconds()));});
        rt.register_hle("ThreadManForUser",0x82BC5777u,[this](auto&,auto& c){++time_calls_[1];
            const auto time=clock_.system_microseconds();c.set_gpr(2,static_cast<std::uint32_t>(time));c.set_gpr(3,static_cast<std::uint32_t>(time>>32u));
        });
        rt.nids().add("ThreadManForUser",0x94AA61EEu,"sceKernelGetThreadCurrentPriority");
        rt.register_hle("ThreadManForUser",0x94AA61EEu,[this](auto&,auto& c){
            const auto f=threads.find(static_cast<std::uint32_t>(psprecomp::runtime_thread_uid()));
            if(f==threads.end()){rt.stop("GetThreadCurrentPriority outside registered worker");return;}
            c.set_gpr(2,f->second.priority);
        });
        rt.register_hle("ThreadManForUser",0x55C20A00u,[this](auto&,auto& c){
            if(c.gpr[7]){rt.stop("CreateEventFlag options not implemented");return;}
            const auto name=guest_name(c.gpr[4]);const auto uid=next_uid_++;
            const auto error=signals_.create_event(uid,name,c.gpr[5],c.gpr[6]);
            if(error){c.set_gpr(2,error);return;}
            calls.push_back("CreateEventFlag uid="+std::to_string(uid)+" name="+name);c.set_gpr(2,uid);
        });
        rt.register_hle("ThreadManForUser",0x1FB15A32u,[this](auto&,auto& c){
            const auto id=c.gpr[4],error=signals_.set_event(id,c.gpr[5]);c.set_gpr(2,error);
            if(!error){wake_waiters(WaitKind::Event,id);maybe_preempt(c);}
        });
        rt.register_hle("ThreadManForUser",0x812346E4u,[this](auto&,auto& c){c.set_gpr(2,signals_.clear_event(c.gpr[4],c.gpr[5]));});
        const auto wait_event=[this](auto&,auto& c){
            const auto outcome=signals_.acquire_event(c.gpr[4],c.gpr[5],c.gpr[6]);
            if(outcome.blocked){block_wait(c,WaitKind::Event,c.gpr[4],c.gpr[5],c.gpr[6],c.gpr[7],c.gpr[8]);return;}
            if(c.gpr[7]) {
                if(!rt.memory().contains(c.gpr[7],4)){rt.stop("WaitEventFlag invalid output pointer");return;}
                rt.memory().store32(c.gpr[7],outcome.pattern);
            }
            c.set_gpr(2,outcome.error);
        };
        rt.register_hle("ThreadManForUser",0x402FCF22u,wait_event);
        rt.register_hle("ThreadManForUser",0x328C546Au,[this,wait_event](auto& r,auto& c){
            if(!dispatch_callbacks(c))return;
            const auto outcome=signals_.acquire_event(c.gpr[4],c.gpr[5],c.gpr[6]);
            if(outcome.blocked){block_wait(c,WaitKind::Event,c.gpr[4],c.gpr[5],c.gpr[6],c.gpr[7],c.gpr[8],0,true);return;}
            if(c.gpr[7]) {
                if(!rt.memory().contains(c.gpr[7],4)){rt.stop("WaitEventFlagCB invalid output pointer");return;}
                rt.memory().store32(c.gpr[7],outcome.pattern);
            }
            c.set_gpr(2,outcome.error);
        });
        rt.register_hle("ThreadManForUser",0xD6DA4BA1u,[this](auto&,auto& c){
            if(c.gpr[8]){rt.stop("CreateSema options not implemented");return;}
            const auto name=guest_name(c.gpr[4]);const auto uid=next_uid_++;
            const auto error=signals_.create_semaphore(uid,name,c.gpr[5],static_cast<std::int32_t>(c.gpr[6]),static_cast<std::int32_t>(c.gpr[7]));
            if(error){c.set_gpr(2,error);return;}
            calls.push_back("CreateSema uid="+std::to_string(uid)+" name="+name);c.set_gpr(2,uid);
        });
        rt.nids().add("ThreadManForUser",0x28B6489Cu,"sceKernelDeleteSema");
        rt.register_hle("ThreadManForUser",0x28B6489Cu,[this](auto&,auto& c){
            const auto id=c.gpr[4];
            if(!signals_.semaphores.erase(id)){c.set_gpr(2,0x80020199u);return;}
            unsigned released=0;
            for(auto& [uid,t]:threads)if(t.wait==WaitKind::Semaphore && t.wait_id==id){wake(t,0x800201B5u);++released;}
            calls.push_back("DeleteSema uid="+std::to_string(id)+" released_waiters="+std::to_string(released));
            c.set_gpr(2,0);maybe_preempt(c);
        });
        rt.register_hle("ThreadManForUser",0x4E3A1105u,[this](auto&,auto& c){
            const auto outcome=signals_.acquire_semaphore(c.gpr[4],static_cast<std::int32_t>(c.gpr[5]));
            if(outcome.blocked){block_wait(c,WaitKind::Semaphore,c.gpr[4],c.gpr[5],0,0,c.gpr[6]);return;}
            if(outcome.error || (++nonblocking_semaphore_waits_ % 65536u)==0){
                const auto object=signals_.semaphores.find(c.gpr[4]);
                calls.push_back("WaitSemaDiagnostic uid="+std::to_string(psprecomp::runtime_thread_uid())+" object="+std::to_string(c.gpr[4])+" requested="+std::to_string(c.gpr[5])+" result="+psprecomp::hex32(outcome.error)+" remaining="+(object==signals_.semaphores.end()?std::string("missing"):std::to_string(object->second.count))+" ra="+psprecomp::hex32(c.gpr[31])+" interrupts_enabled="+std::to_string(interrupts_enabled_));
            }
            c.set_gpr(2,outcome.error);
        });
        rt.register_hle("ThreadManForUser",0x3F53E640u,[this](auto&,auto& c){
            const auto id=c.gpr[4],error=signals_.signal_semaphore(id,static_cast<std::int32_t>(c.gpr[5]));c.set_gpr(2,error);
            if(!error){wake_waiters(WaitKind::Semaphore,id);maybe_preempt(c);}
        });
        rt.nids().add("ThreadManForUser",0x6D212BACu,"sceKernelWaitSemaCB");
        rt.register_hle("ThreadManForUser",0x6D212BACu,[this](auto&,auto& c){
            if(!dispatch_callbacks(c))return;
            const auto outcome=signals_.acquire_semaphore(c.gpr[4],static_cast<std::int32_t>(c.gpr[5]));
            if(outcome.blocked){block_wait(c,WaitKind::Semaphore,c.gpr[4],c.gpr[5],0,0,c.gpr[6],0,true);return;}
            c.set_gpr(2,outcome.error);
        });
        rt.nids().add("ThreadManForUser",0x58B1F937u,"sceKernelPollSema");
        rt.register_hle("ThreadManForUser",0x58B1F937u,[this](auto&,auto& c){
            const auto count=static_cast<std::int32_t>(c.gpr[5]);
            if(count<=0){c.set_gpr(2,0x800201BDu);return;}
            const auto outcome=signals_.acquire_semaphore(c.gpr[4],count);
            c.set_gpr(2,outcome.blocked?0x800201ADu:outcome.error);
        });
        rt.nids().add("ThreadManForUser",0xBC6FEBC5u,"sceKernelReferSemaStatus");
        rt.register_hle("ThreadManForUser",0xBC6FEBC5u,[this](auto&,auto& c){
            const auto id=c.gpr[4],out=c.gpr[5];
            const auto found=signals_.semaphores.find(id);
            if(found==signals_.semaphores.end()){c.set_gpr(2,0x80020199u);return;}
            if(!out || !rt.memory().contains(out,4)){rt.stop("ReferSemaStatus invalid output pointer");return;}
            const auto requested=rt.memory().load32(out);
            if(!requested){c.set_gpr(2,0);return;}
            // PSP accepts nonzero sizes and reports its fixed 56-byte structure.
            // Short-buffer copy extents have not been measured; do not guess them.
            if(requested<56){rt.stop("ReferSemaStatus short-buffer copy extent requires measurement");return;}
            if(!rt.memory().contains(out,56)){rt.stop("ReferSemaStatus output outside mapped memory");return;}
            const auto& sema=found->second;
            std::uint32_t waiters=0;
            for(const auto& [uid,t]:threads)if(t.wait==WaitKind::Semaphore && t.wait_id==id)++waiters;
            rt.memory().store32(out,56);
            for(unsigned n=0;n<32;++n)rt.memory().store8(out+4+n,n<sema.name.size()?static_cast<std::uint8_t>(sema.name[n]):0);
            const std::uint32_t fields[]{sema.attributes,static_cast<std::uint32_t>(sema.initial),static_cast<std::uint32_t>(sema.count),static_cast<std::uint32_t>(sema.maximum),waiters};
            for(unsigned n=0;n<5;++n)rt.memory().store32(out+36+n*4,fields[n]);
            calls.push_back("ReferSemaStatus uid="+std::to_string(id)+" count="+std::to_string(sema.count)+" waiters="+std::to_string(waiters)+" requested="+std::to_string(requested));
            c.set_gpr(2,0);
        });
        rt.nids().add("ThreadManForUser",0x8FFDF9A2u,"sceKernelCancelSema");
        rt.register_hle("ThreadManForUser",0x8FFDF9A2u,[this](auto&,auto& c){
            const auto id=c.gpr[4],out=c.gpr[6];
            if(out && !rt.memory().contains(out,4)){rt.stop("CancelSema invalid waiter output pointer");return;}
            const auto requested=static_cast<std::int32_t>(c.gpr[5]);
            const auto error=signals_.cancel_semaphore(id,requested);c.set_gpr(2,error);if(error)return;
            std::uint32_t cancelled=0;
            for(auto& [uid,t]:threads)if(t.wait==WaitKind::Semaphore && t.wait_id==id){++cancelled;wake(t,0x800201A9u);}
            if(out)rt.memory().store32(out,cancelled);
            calls.push_back("CancelSema uid="+std::to_string(id)+" requested="+std::to_string(requested)+" restored_count="+std::to_string(signals_.semaphores.at(id).count)+" cancelled_waiters="+std::to_string(cancelled));
            maybe_preempt(c);
        });
        rt.register_hle("sceAtrac3plus",0x7A20E7AFu,[this](auto&,auto& c){try{
            const auto id=atrac_.set_data(c.gpr[4],c.gpr[5]);
            calls.push_back("AtracSetDataAndGetID buffer="+psprecomp::hex32(c.gpr[4])+" bytes="+std::to_string(c.gpr[5])+" result="+psprecomp::hex32(id));
            if(id&0x80000000u){c.set_gpr(2,id);return;}
            const auto& stream=atrac_.streams.at(id);
            calls.push_back("AtracOriginalStream samples="+std::to_string(stream.wave.samples)+" initial_skip="+std::to_string(stream.wave.first_sample)+" loop_start="+std::to_string(stream.wave.loop_start)+" loop_end="+std::to_string(stream.wave.loop_end));
            complete_codec(c,id,{});
        }catch(const std::exception& e){rt.stop(e.what());}});
        rt.register_hle("sceAtrac3plus",0x61EB33F5u,[this](auto&,auto& c){c.set_gpr(2,atrac_.release(c.gpr[4]));});
        rt.register_hle("sceAtrac3plus",0x2DD3E298u,[this](auto&,auto& c){try{
            const auto info=atrac_.reset_info(c.gpr[4],c.gpr[5]);if(info.result){c.set_gpr(2,info.result);return;}
            const auto output=c.gpr[6];if(!output || !rt.memory().contains(output,32))throw std::runtime_error("ATRAC reset buffer info outside mapped memory");
            const std::uint32_t words[]{info.address,info.bytes,info.minimum,info.offset,0,0,0,0};for(unsigned n=0;n<8;++n)rt.memory().store32(output+n*4,words[n]);
            calls.push_back("AtracResetInfo id="+std::to_string(c.gpr[4])+" sample="+std::to_string(c.gpr[5])+" offset="+std::to_string(info.offset)+" minimum="+std::to_string(info.minimum)+" writable="+std::to_string(info.bytes));c.set_gpr(2,0);
        }catch(const std::exception& e){rt.stop(e.what());}});
        rt.register_hle("sceAtrac3plus",0x644E5607u,[this](auto&,auto& c){try{
            const auto result=atrac_.reset(c.gpr[4],c.gpr[5],c.gpr[6],c.gpr[7]);
            calls.push_back("AtracResetPosition id="+std::to_string(c.gpr[4])+" sample="+std::to_string(c.gpr[5])+" bytes="+std::to_string(c.gpr[6])+" result="+psprecomp::hex32(result));
            if(result)c.set_gpr(2,result);else complete_codec(c,result,{});
        }catch(const std::exception& e){rt.stop(e.what());}});
        for(const auto nid:{0x31668BAAu,0xA2BBA8BEu,0xFAA4F89Bu,0x868120B5u,0xA554A158u,0x36FAABFBu,0x9AE849A7u,0x5D268707u,0x7DB31251u}){
            rt.register_hle("sceAtrac3plus",nid,[this,nid](auto&,auto& c){try{
                const auto id=c.gpr[4];if(!atrac_.streams.contains(id)){c.set_gpr(2,0x80630005u);return;}
                auto& s=atrac_.streams.at(id);std::uint32_t result=0;
                const auto output=[&](unsigned reg,std::uint32_t value){const auto address=c.gpr[reg];if(address){if(!rt.memory().contains(address,4))throw std::runtime_error("ATRAC metadata output outside mapped memory");rt.memory().store32(address,value);}};
                switch(nid){
                    case 0x31668BAAu:output(5,s.wave.channels);break;
                    case 0xA2BBA8BEu:output(5,s.wave.samples-1);output(6,static_cast<std::uint32_t>(s.wave.loop_start));output(7,static_cast<std::uint32_t>(s.wave.loop_end));break;
                    case 0xFAA4F89Bu:output(5,static_cast<std::uint32_t>(s.loops));output(6,s.wave.loop_start>=0?1:0);break;
                    case 0x868120B5u:result=atrac_.set_loops(id,static_cast<std::int32_t>(c.gpr[5]));break;
                    case 0xA554A158u:output(5,((s.wave.block_bytes*352800u/1000u>>11u)+8u)&~15u);break;
                    case 0x36FAABFBu:++time_calls_[3];output(5,atrac_.next_samples(id));break;
                    case 0x9AE849A7u:output(5,static_cast<std::uint32_t>(atrac_.remaining(id)));break;
                    case 0x5D268707u:{const auto refill=atrac_.refill(id);output(5,refill.address);output(6,refill.bytes);output(7,refill.offset);break;}
                    case 0x7DB31251u:result=atrac_.add(id,c.gpr[5]);break;
                }
                calls.push_back("AtracService nid="+psprecomp::hex32(nid)+" id="+std::to_string(id)+" result="+psprecomp::hex32(result));c.set_gpr(2,result);
            }catch(const std::exception& e){rt.stop(e.what());}});
        }
        rt.register_hle("sceAtrac3plus",0x6A8C3CD5u,[this](auto&,auto& c){try{
            const auto id=c.gpr[4];if(!atrac_.streams.contains(id)){c.set_gpr(2,0x80630005u);return;}
            for(unsigned reg=6;reg<=8;++reg)if(c.gpr[reg] && !rt.memory().contains(c.gpr[reg],4))throw std::runtime_error("ATRAC decode result outside mapped memory");
            if(c.gpr[5] && ((c.gpr[5]&1u) || !rt.memory().contains(c.gpr[5],8192)))throw std::runtime_error("ATRAC PCM output outside mapped memory");
            const auto restarts_before=atrac_.streams.at(id).loop_restarts;
            const auto decoded=atrac_.decode(id);std::vector<CodecWrite> writes;
            if(atrac_.streams.at(id).loop_restarts!=restarts_before)calls.push_back("AtracLoopRestart id="+std::to_string(id)+" position="+std::to_string(atrac_.streams.at(id).position)+" remaining="+std::to_string(decoded.remains));
            const auto word=[&](unsigned reg,std::uint32_t value){if(c.gpr[reg])writes.push_back({c.gpr[reg],{static_cast<std::uint8_t>(value),static_cast<std::uint8_t>(value>>8),static_cast<std::uint8_t>(value>>16),static_cast<std::uint8_t>(value>>24)}});};
            word(6,decoded.frames);word(7,decoded.finish);if(!decoded.result)word(8,static_cast<std::uint32_t>(decoded.remains));
            if(c.gpr[5] && !decoded.pcm.empty()){CodecWrite pcm{c.gpr[5],{}};pcm.bytes.reserve(decoded.pcm.size()*2);for(auto sample:decoded.pcm){const auto bits=static_cast<std::uint16_t>(sample);pcm.bytes.push_back(bits&255u);pcm.bytes.push_back(bits>>8u);}writes.push_back(std::move(pcm));}
            calls.push_back("AtracDecodeOriginal id="+std::to_string(id)+" frames="+std::to_string(decoded.frames)+" finish="+std::to_string(decoded.finish)+" remaining="+std::to_string(decoded.remains)+" result="+psprecomp::hex32(decoded.result));
            if(!decoded.result){complete_codec(c,decoded.result,std::move(writes));return;}
            for(const auto& write:writes)for(unsigned n=0;n<write.bytes.size();++n)rt.memory().store8(write.address+n,write.bytes[n]);c.set_gpr(2,decoded.result);
        }catch(const std::exception& e){rt.stop(e.what());}});
        rt.nids().add("ThreadManForUser",0x19CFF145u,"sceKernelCreateLwMutex");
        rt.register_hle("ThreadManForUser",0x19CFF145u,[this](auto&,auto& c){
            if(c.gpr[8]){rt.stop("CreateLwMutex options require native adaptation");return;}
            if(c.gpr[6]&~0x300u){rt.stop("CreateLwMutex unsupported attribute behavior");return;}
            if(!c.gpr[5]){c.set_gpr(2,0x80020001u);return;}
            const auto name=guest_name(c.gpr[5]);const auto uid=next_uid_++;
            const auto error=lw_.create(uid,c.gpr[4],name,c.gpr[6],static_cast<std::int32_t>(c.gpr[7]),psprecomp::runtime_thread_uid());
            calls.push_back("CreateLwMutex uid="+std::to_string(uid)+" area="+psprecomp::hex32(c.gpr[4])+" name="+name+" result="+psprecomp::hex32(error));c.set_gpr(2,error);
        });
        rt.register_hle("ThreadManForUser",0x60107536u,[this](auto&,auto& c){
            const auto result=lw_.erase(c.gpr[4]);c.set_gpr(2,result.error);
            if(!result.error){for(auto& [id,t]:threads)if(t.wait==WaitKind::LwMutex && t.wait_id==result.uid)wake(t,0x800201B5u);maybe_preempt(c);}
        });
        rt.register_hle("Kernel_Library",0xBEA46419u,[this](auto&,auto& c){
            const auto result=lw_.acquire(c.gpr[4],psprecomp::runtime_thread_uid(),static_cast<std::int32_t>(c.gpr[5]));
            if(result.blocked){block_wait(c,WaitKind::LwMutex,result.uid,c.gpr[5],0,0,c.gpr[6]);return;}
            c.set_gpr(2,result.error);
        });
        rt.register_hle("Kernel_Library",0x15B6446Bu,[this](auto&,auto& c){
            const auto result=lw_.release(c.gpr[4],psprecomp::runtime_thread_uid(),static_cast<std::int32_t>(c.gpr[5]));c.set_gpr(2,result.error);
            if(!result.error){wake_waiters(WaitKind::LwMutex,result.uid);maybe_preempt(c);}
        });
        rt.nids().add("ThreadManForUser",0xB7D098C6u,"sceKernelCreateMutex");
        rt.register_hle("ThreadManForUser",0xB7D098C6u,[this](auto&,auto& c){
            if(c.gpr[7]){rt.stop("CreateMutex options not implemented");return;}
            if(c.gpr[5]&~0x300u){rt.stop("CreateMutex unsupported attribute behavior");return;}
            const auto uid=next_uid_++;const auto name=guest_name(c.gpr[4]);
            const auto error=mutexes_.create(uid,name,c.gpr[5],static_cast<std::int32_t>(c.gpr[6]),psprecomp::runtime_thread_uid());
            if(error){c.set_gpr(2,error);return;}
            calls.push_back("CreateMutex uid="+std::to_string(uid)+" name="+name);c.set_gpr(2,uid);
        });
        rt.register_hle("ThreadManForUser",0xB011B11Fu,[this](auto&,auto& c){
            const auto outcome=mutexes_.acquire(c.gpr[4],psprecomp::runtime_thread_uid(),static_cast<std::int32_t>(c.gpr[5]));
            if(outcome.blocked){block_wait(c,WaitKind::Mutex,c.gpr[4],c.gpr[5],0,0,c.gpr[6]);return;}
            if(!outcome.error)note_mutex_lock(c.gpr[4],c);
            if(outcome.error)calls.push_back("LockMutex uid="+std::to_string(c.gpr[4])+" thread="+std::to_string(psprecomp::runtime_thread_uid())+" result="+psprecomp::hex32(outcome.error));
            c.set_gpr(2,outcome.error);
        });
        rt.nids().add("ThreadManForUser",0x5BF4DD27u,"sceKernelLockMutexCB");
        rt.register_hle("ThreadManForUser",0x5BF4DD27u,[this](auto&,auto& c){
            if(!dispatch_callbacks(c))return;
            const auto outcome=mutexes_.acquire(c.gpr[4],psprecomp::runtime_thread_uid(),static_cast<std::int32_t>(c.gpr[5]));
            if(outcome.blocked){block_wait(c,WaitKind::Mutex,c.gpr[4],c.gpr[5],0,0,c.gpr[6],0,true);return;}
            if(!outcome.error)note_mutex_lock(c.gpr[4],c);
            if(outcome.error)calls.push_back("LockMutexCB uid="+std::to_string(c.gpr[4])+" thread="+std::to_string(psprecomp::runtime_thread_uid())+" result="+psprecomp::hex32(outcome.error));
            c.set_gpr(2,outcome.error);
        });
        rt.register_hle("ThreadManForUser",0x6B30100Fu,[this](auto&,auto& c){
            const auto id=c.gpr[4],error=mutexes_.release(id,psprecomp::runtime_thread_uid(),static_cast<std::int32_t>(c.gpr[5]));c.set_gpr(2,error);
            // Errors only (a failed unlock leaves the mutex held: a later freeze).
            if(error)calls.push_back("UnlockMutex uid="+std::to_string(id)+" thread="+std::to_string(psprecomp::runtime_thread_uid())+" result="+psprecomp::hex32(error));
            if(!error){wake_waiters(WaitKind::Mutex,id);maybe_preempt(c);}
        });
        rt.register_hle("sceUmdUser",0xAEE7404Du,[this](auto&,auto& c){
            if(!callbacks_.contains(c.gpr[4])){c.set_gpr(2,0x80010016u);return;}
            umd_callback_uid_=c.gpr[4];calls.push_back("UmdRegisterCallback uid="+std::to_string(umd_callback_uid_));c.set_gpr(2,0);
        });
        rt.register_hle("sceUmdUser",0xBD2BDE07u,[this](auto&,auto& c){
            if(umd_callback_uid_!=c.gpr[4]){c.set_gpr(2,0x80010016u);return;}
            umd_callback_uid_=0;c.set_gpr(2,0);
        });
        rt.register_hle("sceUmdUser",0x4A9E5E29u,[this](auto&,auto& c){
            if(!dispatch_callbacks(c))return;
            const auto requested=c.gpr[4],state=umd_active_?0x32u:0x12u;
            if((requested&0x37u)==0){c.set_gpr(2,0x80010016u);return;}
            if((requested&state)==0){rt.stop("UmdWaitDriveStatCB would block; scheduler incomplete");return;}
            calls.push_back("UmdWaitDriveStatCB ready="+psprecomp::hex32(requested));c.set_gpr(2,0);
        });
        rt.register_hle("sceUmdUser",0xC6183D47u,[this](auto&,auto& c){
            if(c.gpr[4]<1 || c.gpr[4]>2 || guest_name(c.gpr[5])!="disc0:") {c.set_gpr(2,0x80010016u);return;}
            // ContentMount validates the real selected data and shared sound bank.
            content_.resolve("disc0:/PSP_GAME/USRDIR/std.cpk");
            content_.resolve("disc0:/PSP_GAME/USRDIR/snd.cpk");
            if(!umd_active_ && umd_callback_uid_) {auto& cb=callbacks_.at(umd_callback_uid_);++cb.notifications;cb.notification_arg=0x32u;}
            umd_active_=true;calls.push_back("UmdActivate mounted=disc0:");c.set_gpr(2,0);
        });
        rt.register_hle("sceUmdUser",0x46EBB729u,[this](auto&,auto& c){c.set_gpr(2,1);});
        rt.register_hle("sceUmdUser",0x6B4A146Cu,[this](auto&,auto& c){c.set_gpr(2,umd_active_?0x32u:0x12u);});
        rt.register_hle("sceUmdUser",0x8EF08FCEu,[this](auto&,auto& c){
            const auto requested=c.gpr[4],state=umd_active_?0x32u:0x12u;
            if((requested&0x37u)==0){c.set_gpr(2,0x80010016u);return;}
            if((requested&state)==0){rt.stop("UmdWaitDriveStat would block; scheduler incomplete");return;}
            calls.push_back("UmdWaitDriveStat ready="+psprecomp::hex32(requested));c.set_gpr(2,0);
        });
        rt.register_hle("Kernel_Library",0x293B45B8u,[this](auto&,auto& c){c.set_gpr(2,static_cast<std::uint32_t>(psprecomp::runtime_thread_uid()));});
        rt.nids().add("Kernel_Library",0x092968F4u,"sceKernelCpuSuspendIntr");
        rt.nids().add("Kernel_Library",0x5F10D406u,"sceKernelCpuResumeIntr");
        rt.register_hle("Kernel_Library",0x092968F4u,[this](auto&,auto& c){
            record_interrupt_snapshot(c,InterruptObservation::Suspend);
            const auto previous=interrupts_enabled_?1u:0u;
            if(previous)record_interrupt_transition(c,false,"CpuSuspendIntrTransition");
            interrupts_enabled_=false;
            rt.memory().memory_barrier();c.set_gpr(2,previous);
            if(++interrupt_suspend_calls_<=32)calls.push_back("CpuSuspendIntr previous="+std::to_string(previous));
        });
        rt.register_hle("Kernel_Library",0x5F10D406u,[this](auto&,auto& c){
            record_interrupt_snapshot(c,InterruptObservation::Resume);
            // Evidence only: preserve original execution and freeze the bounded
            // history at the first unexpected unmasked -> masked restore.
            if(interrupts_enabled_ && !(c.gpr[4]&1u) && !callback_depth_ && !interrupt_anomaly_recorded_){
                interrupt_anomaly_recorded_=true;dump_interrupt_history(calls);
            }
            rt.memory().memory_barrier();const bool enabled=(c.gpr[4]&1u)!=0;
            if(enabled!=interrupts_enabled_)record_interrupt_transition(c,enabled,"CpuResumeIntrTransition");
            interrupts_enabled_=enabled;
            if(++interrupt_resume_calls_<=32)calls.push_back("CpuResumeIntr enabled="+std::to_string(interrupts_enabled_?1:0));
        });
        rt.register_hle("Kernel_Library",0xA089ECA4u,[this](auto&,auto& c){
            const auto dst=c.gpr[4],size=c.gpr[6];
            if(size && !rt.memory().contains(dst,size)) {rt.stop("KernelMemset invalid destination");return;}
            if(size && !sync_vram(dst,size))return;
            if(size) {
                if((c.gpr[5]&255u)==0)rt.memory().zero(dst,size);
                else {std::vector<std::uint8_t> data(size,static_cast<std::uint8_t>(c.gpr[5]));rt.memory().copy_in(dst,data);}
            }
            c.set_gpr(2,dst);
        });
        rt.register_hle("Kernel_Library",0x1839852Au,[this](auto&,auto& c){
            const auto dst=c.gpr[4],src=c.gpr[5],size=c.gpr[6];
            if(size && (!rt.memory().contains(dst,size) || !rt.memory().contains(src,size))) {rt.stop("KernelMemcpy invalid memory range");return;}
            if(size && (!sync_vram(dst,size) || !sync_vram(src,size)))return;
            if(size) {
                std::vector<std::uint8_t> data(size);rt.memory().copy_out(src,data);rt.memory().copy_in(dst,data);
            }
            c.set_gpr(2,dst);
        });
        rt.nids().add("sceHprm",0x7E69EDA4u,"sceHprmIsHeadphoneExist");
        rt.register_hle("sceHprm",0x7E69EDA4u,[this](auto&,auto& c){
            const auto before=headphones_.queries();
            const auto& state=headphones_.poll(clock_.system_microseconds());
            if(headphones_.queries()!=before)calls.push_back("HprmIsHeadphoneExist windows_endpoint_available="+std::to_string(state.endpoint_available)+" form_factor="+std::to_string(state.form_factor)+" result="+std::to_string(state.present));
            c.set_gpr(2,state.present?1u:0u);
        });
        rt.register_hle("sceCtrl",0x6A2774F3u,[this](auto&,auto& c){
            const auto cycle=c.gpr[4];
            if((cycle>0 && cycle<5555u) || cycle>20000u) {c.set_gpr(2,0x800001FEu);return;}
            const auto previous=ctrl_cycle_;ctrl_cycle_=cycle;controls_.set_cycle(cycle,clock_.system_microseconds());
            calls.push_back("CtrlSetSamplingCycle="+std::to_string(cycle));c.set_gpr(2,previous);
        });
        rt.register_hle("sceCtrl",0x1F4011E6u,[this](auto&,auto& c){
            if(c.gpr[4]>1u) {c.set_gpr(2,0x80000107u);return;}
            const auto previous=ctrl_mode_;ctrl_mode_=c.gpr[4];controls_.set_mode(ctrl_mode_);
            calls.push_back("CtrlSetSamplingMode="+std::to_string(ctrl_mode_));c.set_gpr(2,previous);
        });
        rt.register_hle("sceCtrl",0x1F803938u,[this](auto&,auto& c){
            const auto destination=c.gpr[4],count=c.gpr[5];
            // Firmware rejects a read spanning the entire 64-slot ring.
            if(count>=64u){c.set_gpr(2,0x80000104u);return;}
            if(!count){rt.stop("CtrlReadBufferPositive zero count requires firmware contract verification");return;}
            if(!rt.memory().contains(destination,static_cast<std::size_t>(count)*16u)){rt.stop("CtrlReadBufferPositive invalid sample destination");return;}
            controls_.update(clock_.system_microseconds());
            const auto taken=controls_.read(rt.memory(),destination,count);
            acknowledge_keyboard_batch(destination,taken);
            if(taken){calls.push_back("CtrlReadBufferPositive samples="+std::to_string(taken)+" destination="+psprecomp::hex32(destination)+" last_timestamp="+std::to_string(rt.memory().load32(destination+(taken-1)*16u))+" last_buttons="+psprecomp::hex32(rt.memory().load32(destination+(taken-1)*16u+4u)));c.set_gpr(2,taken);return;}
            const auto now=clock_.system_microseconds(),next=controls_.next_sample();
            block_wait(c,WaitKind::Controller,0,count,0,destination,0,static_cast<std::uint32_t>(next>now?next-now:1u));
        });
        // Several game screens (including the rhythm stage) poll the pad with
        // Peek instead of Read; without this HLE their input is ignored.
        rt.nids().add("sceCtrl",0x3A622550u,"sceCtrlPeekBufferPositive");
        rt.register_hle("sceCtrl",0x3A622550u,[this](auto&,auto& c){
            const auto destination=c.gpr[4],count=c.gpr[5];
            if(count>=64u){c.set_gpr(2,0x80000104u);return;}
            if(!count || !rt.memory().contains(destination,static_cast<std::size_t>(count)*16u)){c.set_gpr(2,0x80000103u);return;}
            // Peek reports the live pad without consuming the buffer the game
            // may read later. Returning an empty ring here left the rhythm
            // stage and several menus deaf to input.
            const auto now=clock_.system_microseconds();
            controls_.update(now);
            const auto pad=controls_.peek_pad();
            rt.memory().store32(destination,static_cast<std::uint32_t>(now));
            rt.memory().store32(destination+4u,pad.buttons);
            rt.memory().store8(destination+8u,pad.x);rt.memory().store8(destination+9u,pad.y);
            for(unsigned b=10;b<16;++b)rt.memory().store8(destination+b,0);
            acknowledge_keyboard_batch(destination,1);
            calls.push_back("CtrlPeekBufferPositive destination="+psprecomp::hex32(destination)+" last_buttons="+psprecomp::hex32(pad.buttons));
            c.set_gpr(2,1);
        });
        rt.nids().add("sceRtc",0x3F7AD767u,"sceRtcGetCurrentTick");
        rt.register_hle("sceRtc",0x3F7AD767u,[this](auto&,auto& c){++time_calls_[2];
            const auto address=c.gpr[4];
            if((address&3u)!=0 || !rt.memory().contains(address,8)) {rt.stop("RtcGetCurrentTick invalid output pointer");return;}
            const auto tick=clock_.rtc_tick();
            rt.memory().store32(address,static_cast<std::uint32_t>(tick));
            rt.memory().store32(address+4u,static_cast<std::uint32_t>(tick>>32u));
            calls.push_back("RtcGetCurrentTick destination="+psprecomp::hex32(address));
            c.set_gpr(2,0);
        });
        rt.nids().add("sceRtc",0x34885E0Du,"sceRtcConvertUtcToLocalTime");
        rt.register_hle("sceRtc",0x34885E0Du,[this](auto&,auto& c){
            const auto source=c.gpr[4],destination=c.gpr[5];
            if(!rt.memory().contains(source,8) || !rt.memory().contains(destination,8)){c.set_gpr(2,1);return;}
            const auto tick=static_cast<std::uint64_t>(rt.memory().load32(source))|(static_cast<std::uint64_t>(rt.memory().load32(source+4u))<<32u);
            const auto local=kon::utc_to_local_tick(tick);
            rt.memory().store32(destination,static_cast<std::uint32_t>(local));rt.memory().store32(destination+4u,static_cast<std::uint32_t>(local>>32u));
            calls.push_back("RtcConvertUtcToLocalTime source="+psprecomp::hex32(source)+" destination="+psprecomp::hex32(destination));c.set_gpr(2,0);
        });
        rt.nids().add("sceRtc",0x7ED29E40u,"sceRtcSetTick");
        rt.register_hle("sceRtc",0x7ED29E40u,[this](auto&,auto& c){
            const auto destination=c.gpr[4],source=c.gpr[5];
            if(!rt.memory().contains(source,8) || !rt.memory().contains(destination,16)){rt.stop("RtcSetTick invalid calendar pointer");return;}
            const auto tick=static_cast<std::uint64_t>(rt.memory().load32(source))|(static_cast<std::uint64_t>(rt.memory().load32(source+4u))<<32u);
            const auto date=kon::HostClock::calendar_date(tick);
            for(unsigned n=0;n<6;++n)rt.memory().store16(destination+n*2u,static_cast<std::uint16_t>(date[n]));
            rt.memory().store32(destination+12u,date[6]);c.set_gpr(2,0);
            calls.push_back("RtcSetTick date="+std::to_string(date[0])+"-"+std::to_string(date[1])+"-"+std::to_string(date[2]));
        });
        rt.nids().add("ModuleMgrForUser",0xD8B73127u,"sceKernelGetModuleIdByAddress");
        rt.nids().add("ModuleMgrForUser",0xF0A26395u,"sceKernelGetModuleId");
        rt.nids().add("SysMemUserForUser",0x35669D4Cu,"sceKernelSetCompiledSdkVersion600_602");
        rt.nids().add("sceImpose",0x36AA6E91u,"sceImposeSetLanguageMode");
        rt.nids().add("sceImpose",0x24FD7BCFu,"sceImposeGetLanguageMode");
        rt.register_hle("sceImpose",0x36AA6E91u,[this](auto&,auto& c){
            system_language_=c.gpr[4];confirm_button_=c.gpr[5];
            calls.push_back("SetLanguageMode system_language="+std::to_string(system_language_)+" confirm_button="+std::to_string(confirm_button_));
            c.set_gpr(2,0);
        });
        rt.register_hle("sceImpose",0x24FD7BCFu,[this](auto&,auto& c){
            if(!rt.memory().contains(c.gpr[4],4) || !rt.memory().contains(c.gpr[5],4)) {
                rt.stop("GetLanguageMode invalid output pointers");return;
            }
            rt.memory().store32(c.gpr[4],system_language_);
            rt.memory().store32(c.gpr[5],confirm_button_);c.set_gpr(2,0);
        });
        rt.register_hle("ThreadManForUser",0xE81CAF8Fu,[this](auto&,auto& c){
            const auto address=c.gpr[5];
            if((address&3u)!=0 || !rt.memory().contains(address,4)) {c.set_gpr(2,0x80020192u);return;}
            const auto name=guest_name(c.gpr[4]);
            const auto uid=next_uid_++;
            callbacks_.emplace(uid,Callback{uid,address,c.gpr[6],psprecomp::runtime_thread_uid(),name});
            calls.push_back("CreateCallback uid="+std::to_string(uid)+" name="+name+" entry="+psprecomp::hex32(address));
            c.set_gpr(2,uid);
        });
        rt.nids().add("ThreadManForUser",0xC11BA8C4u,"sceKernelNotifyCallback");
        rt.register_hle("ThreadManForUser",0xC11BA8C4u,[this](auto&,auto& c){
            const auto found=callbacks_.find(c.gpr[4]);
            if(found==callbacks_.end()){c.set_gpr(2,0x800201A1u);return;}
            notify_callback(c.gpr[4],c.gpr[5]);c.set_gpr(2,0);
        });
        rt.register_hle("LoadExecForUser",0x4AC57943u,[this](auto&,auto& c){
            if(!callbacks_.contains(c.gpr[4])) {c.set_gpr(2,0x800201A1u);return;}
            exit_callback_uid_=c.gpr[4];calls.push_back("RegisterExitCallback uid="+std::to_string(exit_callback_uid_));c.set_gpr(2,0);
        });
        rt.register_hle("ModuleMgrForUser",0xD8B73127u,[this](auto&,auto& c){
            const auto address=psprecomp::GuestMemory::canonical(c.gpr[4]);
            for(std::size_t n=0;n<image_.segments().size();n++) {
                const auto& segment=image_.segments()[n];if(segment.type!=1u)continue;
                const auto begin=image_.segment_runtime_address(n);
                if(address>=begin && static_cast<std::uint64_t>(address)-begin<segment.memory_size) {
                    calls.push_back("GetModuleIdByAddress="+psprecomp::hex32(address)+" id="+psprecomp::hex32(main_module_uid_));
                    c.set_gpr(2,main_module_uid_);return;
                }
            }
            c.set_gpr(2,0x8002012Eu);
        });
        rt.register_hle("ModuleMgrForUser",0xF0A26395u,[this](auto&,auto& c){c.set_gpr(2,main_module_uid_);});
        rt.register_hle("SysMemUserForUser",0x237DBD4Fu,[this](auto&,auto& c){
            if(c.gpr[4]!=2u) {rt.stop("Memory partition other than user partition 2 unimplemented");return;}
            const auto name=guest_name(c.gpr[5]);
            const auto uid=arena_.allocate(c.gpr[6],c.gpr[7],c.gpr[8]);
            calls.push_back("AllocPartitionMemory name="+name+" size="+std::to_string(c.gpr[7])+" result="+psprecomp::hex32(uid));
            c.set_gpr(2,uid);
        });
        rt.register_hle("SysMemUserForUser",0x9D9A5BA1u,[this](auto&,auto& c){
            const auto b=arena_.block(c.gpr[4]);
            if(!b) {rt.stop("GetBlockHeadAddr unknown memory UID");return;}
            calls.push_back("GetBlockHeadAddr="+psprecomp::hex32(b->address));c.set_gpr(2,b->address);
        });
        rt.register_hle("SysMemUserForUser",0xB6D61D02u,[this](auto&,auto& c){c.set_gpr(2,arena_.release(c.gpr[4]));});
        rt.register_hle("SysMemUserForUser",0x35669D4Cu,[this](auto&,auto& c){
            if (c.gpr[4]<0x06000000u || c.gpr[4]>0x0602FFFFu) {
                rt.stop("SDK600_602 version outside implemented range"); return;
            }
            sdk=c.gpr[4]; calls.push_back("SetCompiledSdkVersion600_602="+psprecomp::hex32(sdk)); c.set_gpr(2,0);
        });
        rt.register_hle("SysMemUserForUser",0xF77D77CBu,[this](auto&,auto& c){
            compiler=c.gpr[4]; calls.push_back("SetCompilerVersion="+psprecomp::hex32(compiler)); c.set_gpr(2,0);
        });
        rt.register_hle("ThreadManForUser",0xEA748E31u,[this](auto&,auto& c){
            const auto clear=c.gpr[4],set=c.gpr[5];
            if(((clear|set)&~0x4000u)!=0) {c.set_gpr(2,0x80020191u);return;}
            const auto found=threads.find(static_cast<std::uint32_t>(psprecomp::runtime_thread_uid()));
            if(found==threads.end()) {rt.stop("ChangeCurrentThreadAttr without a registered worker");return;}
            auto& attr=found->second.attributes;
            attr&=~clear;attr|=set;
            calls.push_back("ChangeCurrentThreadAttr uid="+std::to_string(found->first)+" clear="+psprecomp::hex32(clear)+" set="+psprecomp::hex32(set)+" new="+psprecomp::hex32(attr));
            c.set_gpr(2,0);
        });
        rt.register_hle("ThreadManForUser",0x446D8DE6u,[this](auto&,auto& c){create(c);});
        rt.register_hle("ThreadManForUser",0xF475845Du,[this](auto&,auto& c){start(c);});
        rt.nids().add("ThreadManForUser",0x17C1684Eu,"sceKernelReferThreadStatus");
        rt.register_hle("ThreadManForUser",0x17C1684Eu,[this](auto&,auto& c){try{
            const auto uid=c.gpr[4]?c.gpr[4]:static_cast<std::uint32_t>(psprecomp::runtime_thread_uid());
            const auto found=threads.find(uid);if(found==threads.end()){c.set_gpr(2,0x80020198u);return;}
            const auto output=c.gpr[5];if((output&3u) || !rt.memory().contains(output,4))throw std::runtime_error("Thread status output outside mapped memory");
            const auto size=rt.memory().load32(output),native_size=sdk>0x02060010u?108u:104u;
            if(sdk>0x02060010u && size>108){c.set_gpr(2,0x800201BCu);return;}
            const auto copied=std::min(size,native_size);if(!rt.memory().contains(output,copied))throw std::runtime_error("Thread status structure outside mapped memory");
            account_running();const auto& t=found->second;std::uint32_t wait_type=0;
            switch(t.wait){case WaitKind::None:break;case WaitKind::Sleep:wait_type=1;break;case WaitKind::Delay:wait_type=2;break;case WaitKind::Semaphore:wait_type=3;break;
                case WaitKind::Event:wait_type=4;break;case WaitKind::ThreadEnd:wait_type=9;break;case WaitKind::Mutex:wait_type=13;break;
                case WaitKind::LwMutex:wait_type=14;break;case WaitKind::Audio:wait_type=4;break;case WaitKind::Network:wait_type=3;break;default:throw std::runtime_error("Thread status for native device wait requires verified PSP wait mapping");}
            const auto status=(!t.started || t.finished)?16u:t.wait!=WaitKind::None?4u:t.ready?2u:1u;
            std::array<std::uint8_t,108> bytes{};const auto word=[&](unsigned at,std::uint32_t value){for(unsigned n=0;n<4;++n)bytes[at+n]=static_cast<std::uint8_t>(value>>(n*8));};
            word(0,native_size);for(unsigned n=0;n<std::min(std::size_t{31},t.name.size());++n)bytes[4+n]=static_cast<std::uint8_t>(t.name[n]);
            word(36,t.attributes);word(40,status);word(44,t.entry);word(48,t.stack_base);word(52,t.stack_size);word(56,t.context.gpr[28]);
            word(60,t.initial_priority);word(64,t.priority);word(68,wait_type);word(72,t.wait==WaitKind::None?0:t.wait==WaitKind::Audio?audio_event_uid:t.wait_id);word(76,t.wakeup_count);word(80,t.exit_status);
            word(84,static_cast<std::uint32_t>(t.running_us));word(88,static_cast<std::uint32_t>(t.running_us>>32));word(96,t.preemptions);word(100,t.releases);
            for(unsigned n=0;n<copied;++n)rt.memory().store8(output+n,bytes[n]);
            calls.push_back("ReferThreadStatus uid="+std::to_string(uid)+" status="+std::to_string(status)+" copied="+std::to_string(copied)+" runtime_us="+std::to_string(t.running_us));c.set_gpr(2,0);
        }catch(const std::exception& e){rt.stop(e.what());}});
        rt.nids().add("ThreadManForUser",0xAA73C935u,"sceKernelExitThread");
        rt.register_hle("ThreadManForUser",0xAA73C935u,[this](auto&,auto& c){finish_worker(c,c.gpr[4],"ThreadExit");});
        rt.nids().add("ThreadManForUser",0x3B183E26u,"sceKernelGetThreadExitStatus");
        rt.register_hle("ThreadManForUser",0x3B183E26u,[this](auto&,auto& c){
            const auto found=threads.find(c.gpr[4]);
            if(found==threads.end()){c.set_gpr(2,0x80020198u);return;}
            const auto& target=found->second;
            c.set_gpr(2,!target.started?0x800201A2u:!target.finished?0x800201A4u:target.exit_status);
        });
        for(const auto nid:{0x278C0DF5u,0x840E8133u}){
            const bool callbacks=nid==0x840E8133u;
            rt.nids().add("ThreadManForUser",nid,callbacks?"sceKernelWaitThreadEndCB":"sceKernelWaitThreadEnd");
            rt.register_hle("ThreadManForUser",nid,[this,callbacks](auto&,auto& c){
                const auto id=c.gpr[4];
                if(!id || id==static_cast<std::uint32_t>(psprecomp::runtime_thread_uid())){c.set_gpr(2,0x80020197u);return;}
                if(!threads.contains(id)){c.set_gpr(2,0x80020198u);return;}
                if(callbacks && !dispatch_callbacks(c))return;
                const auto found=threads.find(id);
                if(found==threads.end()){c.set_gpr(2,0x80020198u);return;}
                const auto& target=found->second;
                if(!target.started || target.finished){c.set_gpr(2,target.exit_status);return;}
                block_wait(c,WaitKind::ThreadEnd,id,0,0,0,c.gpr[5],0,callbacks);
            });
        }
        rt.nids().add("ThreadManForUser",0x9FA03CD3u,"sceKernelDeleteThread");
        rt.register_hle("ThreadManForUser",0x9FA03CD3u,[this](auto&,auto& c){delete_worker(c);});
    }
    void returned(psprecomp::AllegrexContext& c) {
        if(psprecomp::runtime_thread_uid()==1) {
            module_returned=true;module_status=c.gpr[2];rt.stop("module_start returned");
        } else {
            finish_worker(c,c.gpr[2],"ThreadReturned");
        }
    }
    void mark_running(std::uint32_t uid) {threads.at(uid).ready=false;running_uid_=uid;running_since_=clock_.system_microseconds();}
    void import_boundary(psprecomp::AllegrexContext& c) {record_interrupt_snapshot(c,InterruptObservation::PostImport);if(callback_depth_)return;dispatch_waiting_callbacks(c,true);pump_timeouts();pump_ge_interrupts(c);maybe_preempt(c);check_visible_checkpoint();}
    void dispatch_boundary(psprecomp::AllegrexContext& c) {if(callback_depth_)return;dispatch_waiting_callbacks(c,false);pump_timeouts();pump_graphics(c);maybe_preempt(c,false);}
private:
    std::vector<std::array<std::uint64_t,6>> frame_samples_;
    std::uint64_t next_frame_sample_us_=0;
    std::uint64_t interrupt_suspend_calls_=0,interrupt_resume_calls_=0,next_progress_us_=5000000,next_capture_us_=0;
public: std::uint64_t capture_begin_us=0; // --capture-begin-ms: periodic captures start here (diagnostics)
private:
    std::uint64_t nonblocking_semaphore_waits_=0;
    std::uint32_t running_uid_=0;
    std::uint64_t running_since_=0;
    void acknowledge_keyboard_batch(std::uint32_t destination,unsigned count){
        // Original callers can inspect the oldest sample in a returned batch.
        // Retain a short host event until every sample in one batch contains it.
        if(!count)return;std::uint32_t delivered=0xFFFFFFFFu;
        for(unsigned n=0;n<count;++n)delivered&=rt.memory().load32(destination+n*16u+4u);
        display_.acknowledge_key_presses(delivered);
    }
    void account_running(){const auto now=clock_.system_microseconds();if(running_uid_ && threads.contains(running_uid_))threads.at(running_uid_).running_us+=now-running_since_;running_since_=now;}
    std::uint64_t next_visual_check_=0;
    unsigned texture_probes_=0;std::uint64_t projected_draws_=0;
    kon::NativeWait scheduler_wait_;
    struct RenderProfile {std::uint64_t calls=0,total_us=0,max_us=0,covered=0,drawn=0;};
    std::map<std::array<unsigned,14>,RenderProfile> render_profile_;
    std::uint64_t render_profile_overflow_=0;bool projected_capture_written_=false;
    std::array<std::uint64_t,3> render_time_us_{}; // clear, through, projected
    void check_visible_checkpoint(){
        if(!stop_after_visible_frame || rt.stopped() || !renderer_.draw_pixels || !savedata_.startup_complete())return;
        const auto now=clock_.system_microseconds();if(now<next_visual_check_)return;next_visual_check_=now+100000u;
        const auto pixels=display_.pixels();
        const auto visible=std::count_if(pixels.begin(),pixels.end(),[](auto pixel){return (pixel&0xFFFFFFu)!=0;});
        if(visible>=32){calls.push_back("OriginalVisibleFrame nonblack_pixels="+std::to_string(visible));rt.stop("Diagnostic original visible frame checkpoint");}
    }
    void finish_worker(psprecomp::AllegrexContext& c,std::uint32_t status,const char* reason){
        if(callback_depth_){rt.stop("Thread exit inside callback requires asynchronous callback teardown");return;}
        const auto found=threads.find(static_cast<std::uint32_t>(psprecomp::runtime_thread_uid()));
        if(found==threads.end()){rt.stop("Thread exit outside registered worker");return;}
        auto& t=found->second;t.exit_status=status;t.finished=true;t.ready=false;t.wait=WaitKind::None;t.wait_callbacks=false;t.deadline=0;t.timeout_pointer=0;t.context=c;
        calls.push_back(std::string(reason)+" uid="+std::to_string(t.uid)+" status="+psprecomp::hex32(status));
        wake_waiters(WaitKind::ThreadEnd,t.uid);dispatch_next(c);
    }
    void delete_worker(psprecomp::AllegrexContext& c){
        const auto uid=c.gpr[4];
        if(!uid || uid==static_cast<std::uint32_t>(psprecomp::runtime_thread_uid())){c.set_gpr(2,0x800201A4u);return;}
        const auto found=threads.find(uid);if(found==threads.end()){c.set_gpr(2,0x80020198u);return;}
        const auto& t=found->second;if(t.started && !t.finished){c.set_gpr(2,0x800201A4u);return;}
        const auto size=t.stack_size;
        if(!t.stack_memory_uid || arena_.release(t.stack_memory_uid))throw std::runtime_error("Native thread stack allocation identity lost");
        for(auto it=callbacks_.begin();it!=callbacks_.end();){
            if(it->second.owner!=static_cast<std::int32_t>(uid)){++it;continue;}
            const auto callback=it->first;
            for(auto& slot:power_slots_)if(slot==callback)slot=0;
            std::erase(memory_callbacks_,callback);
            if(umd_callback_uid_==callback)umd_callback_uid_=0;
            if(exit_callback_uid_==callback)exit_callback_uid_=0;
            it=callbacks_.erase(it);
        }
        threads.erase(found);
        for(auto& [id,waiting]:threads)if(waiting.wait==WaitKind::ThreadEnd && waiting.wait_id==uid)wake(waiting,0x800201B5u);
        calls.push_back("DeleteThread uid="+std::to_string(uid)+" released_stack="+std::to_string(size));c.set_gpr(2,0);maybe_preempt(c);
    }
    struct GeCallback {std::uint32_t signal,signal_arg,finish,finish_arg;};
    std::map<std::uint32_t,GeCallback> ge_callbacks_;
    struct Callback {std::uint32_t uid,address,argument;std::int32_t owner;std::string name;std::uint32_t notifications=0,notification_arg=0;};
    std::map<std::uint32_t,Callback> callbacks_;
    std::uint32_t exit_callback_uid_=0;
    unsigned callback_depth_=0;
    std::vector<std::uint32_t> memory_callbacks_;
    std::array<std::uint32_t,16> power_slots_{};
    std::set<std::uint32_t> import_pcs_;
    void notify_callback(std::uint32_t id,std::uint32_t argument) {
        auto& cb=callbacks_.at(id);++cb.notifications;cb.notification_arg=argument;
    }
    bool dispatch_callbacks(psprecomp::AllegrexContext& c,unsigned* dispatched=nullptr) {
        if(callback_depth_){rt.stop("Nested callback dispatch requires asynchronous callback frames");return false;}
        deliver_adhoc_events(c);if(rt.stopped())return false;
        const auto owner=psprecomp::runtime_thread_uid();
        std::vector<std::uint32_t> pending;
        for(const auto& [id,cb]:callbacks_)if(cb.owner==owner && cb.notifications)pending.push_back(id);
        for(const auto id:pending) {
            auto it=callbacks_.find(id);if(it==callbacks_.end())continue;
            auto& cb=it->second;const auto count=cb.notifications,arg=cb.notification_arg;
            cb.notifications=0;cb.notification_arg=0;
            ++callback_depth_;
            const auto outcome=kon::run_native_callback(rt,c,cb.address,count,arg,cb.argument,threads.at(static_cast<std::uint32_t>(owner)).stack_base);
            --callback_depth_;
            if(!outcome.completed)return false;
            calls.push_back("CallbackExecuted uid="+std::to_string(id)+" notifications="+std::to_string(count)+" argument="+psprecomp::hex32(arg)+" result="+std::to_string(outcome.value));
            if(dispatched)++*dispatched;
            if(outcome.value)callbacks_.erase(id);
        }
        return true;
    }
    // PSP system preferences are independent of the selected content pack.
    std::uint32_t system_language_=0,confirm_button_=0;
    psprecomp::Runtime& rt;
    std::uint32_t gp_,next_uid_=2;
    std::uint64_t order_=0;
    kon::GuestArena arena_;
    kon::HostClock clock_;
    kon::KernelMutexes mutexes_;
    kon::KernelLwMutexes lw_;
    kon::KernelSignals signals_;
    std::uint32_t ctrl_cycle_=0,ctrl_mode_=0;
    bool interrupts_enabled_=true;
    std::deque<std::string> interrupt_transitions_;
    std::uint64_t masked_context_switches_=0;
    enum class InterruptObservation : unsigned {PostImport,Suspend,Resume};
    struct InterruptSnapshot {
        InterruptObservation observation;std::int32_t uid;std::uint32_t pc,ra,result;
        std::array<std::uint32_t,4> arguments;
        std::uint32_t depth,saved_mask,callback_depth;bool enabled;
    };
    std::array<InterruptSnapshot,128> interrupt_history_{};
    unsigned interrupt_history_size_=0,interrupt_history_next_=0;
    bool interrupt_anomaly_recorded_=false;
    void record_interrupt_snapshot(const psprecomp::AllegrexContext& c,InterruptObservation observation){
        if(interrupt_anomaly_recorded_)return;
        interrupt_history_[interrupt_history_next_]={observation,psprecomp::runtime_thread_uid(),c.pc,c.gpr[31],c.gpr[2],
            {c.gpr[4],c.gpr[5],c.gpr[6],c.gpr[7]},rt.memory().load32(0x08AF4400u),rt.memory().load32(0x08AF443Cu),callback_depth_,interrupts_enabled_};
        interrupt_history_next_=(interrupt_history_next_+1u)%interrupt_history_.size();
        interrupt_history_size_=std::min(interrupt_history_size_+1u,static_cast<unsigned>(interrupt_history_.size()));
    }
    void dump_interrupt_history(DiagnosticCalls& output) const {
        output.push_back("InterruptRestoreAnomaly elapsed_us="+std::to_string(clock_.system_microseconds())+" snapshots="+std::to_string(interrupt_history_size_));
        const auto first=(interrupt_history_next_+interrupt_history_.size()-interrupt_history_size_)%interrupt_history_.size();
        for(unsigned n=0;n<interrupt_history_size_;++n){
            const auto& h=interrupt_history_[(first+n)%interrupt_history_.size()];
            std::string message="InterruptImportHistory index="+std::to_string(n)+" observation="+std::to_string(static_cast<unsigned>(h.observation))+" uid="+std::to_string(h.uid)+" pc="+psprecomp::hex32(h.pc)+" ra="+psprecomp::hex32(h.ra)+" result="+psprecomp::hex32(h.result);
            for(unsigned a=0;a<4;++a)message+=" a"+std::to_string(a)+"="+psprecomp::hex32(h.arguments[a]);
            output.push_back(message+" enabled="+std::to_string(h.enabled)+" callback_depth="+std::to_string(h.callback_depth)+" cri_depth="+std::to_string(h.depth)+" cri_saved_mask="+std::to_string(h.saved_mask));
        }
    }
    void record_interrupt_transition(const psprecomp::AllegrexContext& c,bool enabled,const char* operation){
        if(interrupt_transitions_.size()==32)interrupt_transitions_.pop_front();
        interrupt_transitions_.push_back(std::string(operation)+" elapsed_us="+std::to_string(clock_.system_microseconds())+" uid="+std::to_string(psprecomp::runtime_thread_uid())+" ra="+psprecomp::hex32(c.gpr[31])+" arg="+psprecomp::hex32(c.gpr[4])+" enabled="+std::to_string(enabled)+" callback_depth="+std::to_string(callback_depth_)+" cri_depth="+std::to_string(rt.memory().load32(0x08AF4400u))+" cri_saved_mask="+std::to_string(rt.memory().load32(0x08AF443Cu)));
    }
    const psprecomp::Elf32Image& image_;
    const kon::ContentMount& content_;
    kon::NativeFiles files_;
    kon::NativeSavedata savedata_{rt.memory(),files_};
    kon::GeQueue ge_;
    kon::NativeDisplay display_;
    kon::NativeControls controls_;
    kon::HostHeadphones headphones_;
    kon::NativeRenderer renderer_;
    kon::NativeMedia media_;
    kon::NativeAtrac atrac_{rt.memory(),media_};
    struct CodecWrite {std::uint32_t address;std::vector<std::uint8_t> bytes;};
    std::map<std::uint32_t,std::vector<CodecWrite>> codec_writes_;
    void complete_codec(psprecomp::AllegrexContext& c,std::uint32_t result,std::vector<CodecWrite> writes){
        if(callback_depth_){rt.stop("Blocking codec call inside native callback requires asynchronous callback frames");return;}
        const auto uid=static_cast<std::uint32_t>(psprecomp::runtime_thread_uid());
        if(codec_writes_.contains(uid))throw std::runtime_error("Native codec task already has a pending output");
        codec_writes_.emplace(uid,std::move(writes));block_wait(c,WaitKind::Codec,0,0,result,0,0,3500);
    }
    kon::NativeAudio audio_;
    bool umd_active_=false;
    std::uint32_t umd_callback_uid_=0;
    const std::uint32_t main_module_uid_=0x00100000u;
    std::uint32_t ge_interrupt_stack_=0;
    struct PendingGeInterrupt {std::uint32_t address,value,argument,command_pc,list_id,callback_id;};
    std::deque<PendingGeInterrupt> pending_ge_interrupts_;
    void pump_ge_interrupts(psprecomp::AllegrexContext& c) {
        if(callback_depth_ || !interrupts_enabled_ || rt.stopped())return;
        while(!pending_ge_interrupts_.empty() && !rt.stopped()){
            const auto pending=pending_ge_interrupts_.front();pending_ge_interrupts_.pop_front();
            if(!ge_interrupt_stack_){const auto allocation=arena_.allocate(1,16384);const auto block=arena_.block(allocation);
                if(!block)throw std::runtime_error("Cannot reserve native GE interrupt stack");ge_interrupt_stack_=block->address;}
            // Hardware finish can occur while CPU interrupts are masked, but
            // guest handlers must wait until the critical section restores them.
            // The interrupted CPU's mask is restored on interrupt return.
            const auto cri_depth_before=rt.memory().load32(0x08AF4400u),cri_mask_before=rt.memory().load32(0x08AF443Cu);
            const auto previous=interrupts_enabled_;interrupts_enabled_=false;++callback_depth_;
            kon::CallbackResult result{};
            try{result=kon::run_native_interrupt(rt,c,pending.address,pending.value,pending.argument,pending.command_pc,ge_interrupt_stack_,16384);}
            catch(...){--callback_depth_;interrupts_enabled_=previous;throw;}
            --callback_depth_;interrupts_enabled_=previous;
            const auto cri_depth_after=rt.memory().load32(0x08AF4400u),cri_mask_after=rt.memory().load32(0x08AF443Cu);
            if(cri_depth_before!=cri_depth_after || cri_mask_before!=cri_mask_after)calls.push_back("GeInterruptCriticalStateChange elapsed_us="+std::to_string(clock_.system_microseconds())+" uid="+std::to_string(psprecomp::runtime_thread_uid())+" depth_before="+std::to_string(cri_depth_before)+" depth_after="+std::to_string(cri_depth_after)+" mask_before="+std::to_string(cri_mask_before)+" mask_after="+std::to_string(cri_mask_after));
            if(!result.completed)throw std::runtime_error("GE finish callback failed");
            calls.push_back("GeFinishCallback id="+std::to_string(pending.callback_id));
            wake_waiters(WaitKind::GeList,pending.list_id);
        }
    }
    // Execute queued native graphics at a cooperative work boundary, rather
    // than inside enqueue/sync. A blocking sync can then release the CPU to
    // resource-loading tasks until the actual rendering work completes. Keep
    // memory access on one host thread: guest RAM and VRAM are shared buffers.
    void pump_graphics(psprecomp::AllegrexContext& c) {
        if(!rt.stopped() && !ge_.queue.empty() && ge_.draw_status()!=3){process_ge(c);pump_timeouts();}
        pump_ge_interrupts(c);
    }
public:
    // Diagnostic (--dump-draws path begin_ms end_ms): one line per GE draw with
    // texture state and the decoded vertices (screen position, UV).
    std::unique_ptr<std::ofstream> draw_dump_;std::uint64_t draw_dump_begin_us_=0,draw_dump_end_us_=0;
    void dump_draw(const kon::GeState& state,unsigned kind,unsigned count,std::uint64_t now){
        const auto& r=state.registers;const auto vt=r[0x12]&0xFFFFFFu;auto& out=*draw_dump_;
        out<<now<<" kind="<<kind<<" count="<<count<<" vtype="<<psprecomp::hex32(vt)<<" tex="<<psprecomp::hex32((r[0xA0]&0xFFFFF0u)|((r[0xA8]<<8u)&0x0F000000u))
           <<" size="<<(1u<<(r[0xB8]&15u))<<"x"<<(1u<<((r[0xB8]>>8u)&15u))<<" fmt="<<(r[0xC3]&15u)<<" textured="<<(r[0x1E]&1u)<<" clut="<<psprecomp::hex32((r[0xB0]&0xFFFFF0u)|((r[0xB1]<<8u)&0x0F000000u))<<" fb="<<psprecomp::hex32(r[0x9C]&0x1FFFF0u);
        const unsigned tc=vt&3u,col=(vt>>2u)&7u,nrm=(vt>>5u)&3u,pos=(vt>>7u)&3u,wt=(vt>>9u)&3u,idx=(vt>>11u)&3u;
        if(idx || wt || !pos || !rt.memory().contains(state.vertex_address,4)){out<<"\n";return;}
        const unsigned tsz[]={0,1,2,4},csz[]={0,0,0,0,2,2,2,4},psz[]={0,1,2,4};unsigned off=0,align=1;
        const auto place=[&](unsigned size,unsigned n){off=(off+size-1u)/size*size;const auto at=off;off+=size*n;align=std::max(align,size);return at;};
        const auto at_tc=tc?place(tsz[tc],2):0u;const auto at_col=col?place(csz[col],1):0u;if(nrm)place(psz[nrm],3);const auto at_pos=place(psz[pos],3);
        const auto stride=(off+align-1u)/align*align;(void)at_col;
        const auto value=[&](std::uint32_t a,unsigned size,bool is_signed)->double{
            if(size==4)return std::bit_cast<float>(rt.memory().load32(a));
            if(size==2)return is_signed?double(std::int16_t(rt.memory().load16(a))):double(rt.memory().load16(a));
            return is_signed?double(std::int8_t(rt.memory().load8(a))):double(rt.memory().load8(a));};
        for(unsigned n=0;n<std::min(count,64u);++n){const auto v=state.vertex_address+n*stride;if(!rt.memory().contains(v,stride))break;
            out<<" | "<<value(v+at_pos,psz[pos],true)<<","<<value(v+at_pos+psz[pos],psz[pos],true);
            if(tc)out<<" uv "<<value(v+at_tc,tsz[tc],false)<<","<<value(v+at_tc+tsz[tc],tsz[tc],false);}
        out<<"\n";
    }
private:
    void process_ge(psprecomp::AllegrexContext& c) {
        trace(6,0);const auto draws_before=renderer_.draw_primitives+renderer_.clear_primitives;in_ge.store(true,std::memory_order_relaxed);
        struct TraceEnd {BootstrapServices& s;std::uint64_t before;~TraceEnd(){s.in_ge.store(false,std::memory_order_relaxed);s.trace(7,s.renderer_.draw_primitives+s.renderer_.clear_primitives-before);}} trace_end{*this,draws_before};
        const auto quantum_deadline=std::chrono::steady_clock::now()+std::chrono::milliseconds(1);
        try {
            renderer_.begin_guest_epoch(); // guest code may have run since the last GE pass
            if(gpu_batch)renderer_.begin_gpu_batch();
            ge_.process([this](auto& state,auto pc,unsigned kind,unsigned count){
                if(settings_fps_ && !(state.registers[0xD3]&1u) && settings_fps_->draw(state,kind,count,[this](kon::GeState& s,unsigned k,unsigned n){
                    try{renderer_.draw(s,k,n);}catch(const std::exception& error){calls.push_back(std::string("SettingsFpsDraw failed: ")+error.what());}}))return;
                if(trace_render && (state.registers[0x1E]&1u) && (++texture_probes_<=16 || texture_probes_%100u==0)){
                    std::string info="GeTextureProbe loaded_palette_bytes="+std::to_string(state.clut_loaded_bytes);for(unsigned command:{0xA0u,0xA8u,0xB8u,0xB0u,0xB1u,0xC2u,0xC3u,0xC4u,0xC5u,0xDBu,0xDEu})info+=" "+psprecomp::hex32(command)+":"+psprecomp::hex32(state.registers[command]);
                    info+=" palette=";for(unsigned n=0;n<32;++n)info+=psprecomp::hex32(state.clut[n])+",";
                    info+=" palette_rgb_entries=";unsigned colored=0;for(unsigned n=0;n<256;++n)if(state.clut[n*2u] || (state.clut[n*2u+1u]&15u))++colored;info+=std::to_string(colored);
                    if((state.registers[0xC3]&15u)<=8 && (state.registers[0x12]&3u)==2){kon::NativeTexture diagnostic(rt.memory(),state);unsigned nonblack=0,opaque=0;for(unsigned y=0;y<(1u<<((state.registers[0xB8]>>8u)&15u));++y)for(unsigned x=0;x<(1u<<(state.registers[0xB8]&15u));++x){const auto pixel=diagnostic.sample(x+.5,y+.5,false);nonblack+=(pixel&0xFFFFFFu)!=0;opaque+=(pixel>>24u)!=0;}info+=" texture_nonblack="+std::to_string(nonblack)+" texture_opaque="+std::to_string(opaque);}
                    calls.push_back(info);
                    if(texture_probes_==1 || texture_probes_%100u==0){
                        std::ofstream palette(content_.root()/"analysis"/("native_palette_"+content_.language()+".bin"),std::ios::binary);palette.write(reinterpret_cast<const char*>(state.clut.data()),2048);
                        const auto pointer=(state.registers[0xA0]&0xFFFFF0u)|((state.registers[0xA8]<<8u)&0x0F000000u);
                        std::ofstream pixels(content_.root()/"analysis"/("native_texels_"+content_.language()+".bin"),std::ios::binary);for(unsigned n=0;n<4096 && rt.memory().contains(pointer+n,1);++n){const char value=static_cast<char>(rt.memory().load8(pointer+n));pixels.write(&value,1);}
                    }
                }
                if(trace_render)calls.push_back("GeDrawRequest pc="+psprecomp::hex32(pc)+" vtype="+psprecomp::hex32(state.registers[0x12])+" vertex="+psprecomp::hex32(state.vertex_address)+" clear="+psprecomp::hex32(state.registers[0xD3])+" framebuffer="+psprecomp::hex32(state.registers[0x9C])+" width="+psprecomp::hex32(state.registers[0x9D])+" format="+psprecomp::hex32(state.registers[0xD2]));
                if(trace_render && rt.memory().contains(state.vertex_address,64)) {
                    std::string words="GeVertexWords ";for(unsigned n=0;n<16;n++)words+=psprecomp::hex32(rt.memory().load32(state.vertex_address+n*4u))+" ";
                    calls.push_back(words);
                }
                if(!quad_capture_written_ && !quad_draw_capture.empty() && !(state.registers[0xD3]&1u) && (state.registers[0x12]&0xFFFFFFu)==0x80011Au && count==4 && (state.registers[0xC3]&15u)==5u && rt.memory().contains(state.vertex_address,48)){
                    int x0=32767,y0=32767,x1=-32768,y1=-32768;
                    for(unsigned n=0;n<4;++n){const auto at=state.vertex_address+n*12u;const auto x=static_cast<std::int16_t>(rt.memory().load16(at+6)),y=static_cast<std::int16_t>(rt.memory().load16(at+8));x0=std::min(x0,int(x));y0=std::min(y0,int(y));x1=std::max(x1,int(x));y1=std::max(y1,int(y));}
                    const auto work=static_cast<std::int64_t>(x1-x0)*(y1-y0)*2u;
                    if(work>=8192 && work<32768){kon::NativeDrawCapture::write(quad_draw_capture,rt.memory(),state,pc,kind,count);quad_capture_written_=true;calls.push_back("OriginalSmallQuadCaptured path="+quad_draw_capture.string()+" estimated_bbox_pixels="+std::to_string(work));}
                }
                if(!projected_capture_written_ && !projected_draw_capture.empty() && !(state.registers[0xD3]&1u) && !(state.registers[0x12]&0x800000u) && count>=projected_capture_min_vertices && clock_.system_microseconds()>=projected_capture_after_us){
                    kon::NativeDrawCapture::write(projected_draw_capture,rt.memory(),state,pc,kind,count);projected_capture_written_=true;
                    calls.push_back("Native3DCapture pc="+psprecomp::hex32(pc)+" vertices="+std::to_string(count)+" elapsed_us="+std::to_string(clock_.system_microseconds())+" path="+projected_draw_capture.string());
                }
                if(draw_dump_ && !(state.registers[0xD3]&1u)){const auto now=clock_.system_microseconds();if(now>=draw_dump_begin_us_ && now<draw_dump_end_us_)dump_draw(state,kind,count,now);}
                if(frame_stream)frame_stream->source(pc);
                const auto pixels_before=renderer_.draw_pixels,covered_before=renderer_.draw_covered_pixels;
                const auto render_start=std::chrono::steady_clock::now();
                bool rendered=false;std::string render_error;
                try{rendered=renderer_.draw(state,kind,count);}catch(const std::exception& error){render_error=error.what();}
                const auto render_category=(state.registers[0xD3]&1u)?0u:(state.registers[0x12]&0x800000u)?1u:2u;
                const auto render_us=std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now()-render_start).count();
                render_time_us_[render_category]+=render_us;
                if(profile_render){const auto& r=state.registers;
                    const std::array<unsigned,14> key{render_category,kind,count,r[0x12]&0xFFFFFFu,r[0xD2]&3u,r[0x1E]&1u,r[0x21]&1u,r[0x23]&1u,r[0x22]&1u,r[0xC3]&15u,r[0xC6]&0x107u,r[0xC9]&0x1FFu,r[0xDF]&0xFFFFFFu,r[0xDE]&7u};
                    if(render_profile_.contains(key) || render_profile_.size()<512){auto& v=render_profile_[key];++v.calls;v.total_us+=render_us;v.max_us=std::max(v.max_us,static_cast<std::uint64_t>(render_us));v.covered+=renderer_.draw_covered_pixels-covered_before;v.drawn+=renderer_.draw_pixels-pixels_before;}else ++render_profile_overflow_;
                }
                if(rendered) {
                    if(!(state.registers[0xD3]&1u) && !(state.registers[0x12]&0x800000u) && ++projected_draws_==1)
                        calls.push_back("GeFirstNative3D pc="+psprecomp::hex32(pc)+" vertices="+std::to_string(count)+" vtype="+psprecomp::hex32(state.registers[0x12])+" written_pixels="+std::to_string(renderer_.draw_pixels-pixels_before)+" covered_pixels="+std::to_string(renderer_.draw_covered_pixels-covered_before));
                    // Boot milestone (first full-screen clear): read once the render thread has run it.
                    if((state.registers[0xD3]&1u) && (trace_render || !clear_milestone_logged_)){renderer_.drain();if(trace_render || renderer_.clear_pixels==130560){calls.push_back("GeNativeClear vertices="+std::to_string(count)+" total_pixels="+std::to_string(renderer_.clear_pixels));clear_milestone_logged_=renderer_.clear_pixels>=130560;}else if(renderer_.clear_pixels>130560)clear_milestone_logged_=true;}
                    else if(trace_render)calls.push_back("GeNativeDraw type="+std::to_string(kind)+" vertices="+std::to_string(count)+" total_pixels="+std::to_string(renderer_.draw_pixels)+" covered="+std::to_string(renderer_.draw_covered_pixels)+" alpha_rejected="+std::to_string(renderer_.alpha_rejected_pixels)+" depth_rejected="+std::to_string(renderer_.depth_rejected_pixels));return;
                }
                std::string state_words="GeStateWords";
                for(unsigned n=0;n<256;++n)if(state.registers[n])state_words+=" "+psprecomp::hex32(n)+":"+psprecomp::hex32(state.registers[n]);
                calls.push_back(state_words);
                const auto diagnostic=content_.root()/"analysis"/("native_pending_draw_"+content_.language()+".json");
                std::ofstream draw_dump(diagnostic);
                draw_dump<<"{\"pc\":"<<pc<<",\"kind\":"<<kind<<",\"count\":"<<count<<",\"vertex_address\":"<<state.vertex_address<<",\"registers\":[";
                for(unsigned n=0;n<256;++n){if(n)draw_dump<<',';draw_dump<<state.registers[n];}
                const auto matrix=[&](const char* name,const auto& values){draw_dump<<"],\""<<name<<"\":[";for(unsigned n=0;n<values.size();++n){if(n)draw_dump<<',';draw_dump<<values[n];}};
                matrix("world",state.world);matrix("view",state.view);matrix("projection",state.projection);matrix("texture_matrix",state.texture_matrix);matrix("bones",state.bones);
                draw_dump<<"],\"vertex_bytes\":[";
                for(unsigned n=0;n<32768 && rt.memory().contains(state.vertex_address+n,1);++n){if(n)draw_dump<<',';draw_dump<<unsigned(rt.memory().load8(state.vertex_address+n));}
                draw_dump<<"]}\n";
                kon::NativeDrawCapture::write(content_.root()/"analysis"/("native_pending_draw_"+content_.language()+".kondraw"),rt.memory(),state,pc,kind,count);
                if(!render_error.empty()){rt.stop(render_error+" pc="+psprecomp::hex32(pc));throw std::runtime_error(rt.stop_reason());}
                rt.stop("GE primitive requires native renderer pc="+psprecomp::hex32(pc)+" type="+std::to_string(kind)+" vertices="+std::to_string(count));
                throw std::runtime_error(rt.stop_reason());
            },[this,&c](const auto& list,auto value,auto command_pc){
                // Submit without waiting: original tasks run while the GPU
                // shades; publication happens on the first dependent access.
                // Lists already queued behind this one continue the same batch;
                // VRAM ownership still publishes before any dependent access.
                if(gpu_batch && resident_gpu_ownership && ge_.queue.size()<=1)submit_gpu_batch();
                calls.push_back("GeListFinished id="+std::to_string(list.id)+" commands="+std::to_string(list.commands));
                if(list.callback>=0) {
                    const auto& cb=ge_callbacks_.at(static_cast<std::uint32_t>(list.callback));
                    if(cb.finish) {
                        pending_ge_interrupts_.push_back({cb.finish,value,cb.finish_arg,command_pc,list.id,static_cast<std::uint32_t>(list.callback)});
                        return;
                    }
                }
                wake_waiters(WaitKind::GeList,list.id);
            },[&]{return std::chrono::steady_clock::now()>=quantum_deadline;});
            // Resident mode submits without waiting (collection happens on the
            // first dependent access); the copy-back mode completes each pass.
            // Queued draws are built and recorded before original code runs again.
            renderer_.process_pending();
            if(gpu_batch && resident_gpu_ownership && ge_.queue.empty())submit_gpu_batch();
            else if(gpu_batch && !resident_gpu_ownership){const auto start=std::chrono::steady_clock::now();renderer_.end_gpu_batch();gpu_batch_time_us+=std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now()-start).count();}
            if(ge_.queue.empty())wake_waiters(WaitKind::GeDraw,0);
        }catch(const std::exception& e){if(!rt.stopped())rt.stop(e.what());}
    }
    void dispatch_waiting_callbacks(psprecomp::AllegrexContext& c,bool at_import,bool idle=false) {
        const auto current=static_cast<std::uint32_t>(psprecomp::runtime_thread_uid());
        if(current==1 || !threads.contains(current) || !interrupts_enabled_)return;
        auto saved=c;const auto name=threads.at(current).name;
        // Import wrappers deliberately refuse to overwrite PC after any owner
        // transition, including switch-away/switch-back for a waiting callback.
        // Save the real post-import continuation so the syscall is not replayed.
        if(at_import && import_pcs_.contains(saved.pc))saved.pc=saved.gpr[31];
        for(auto& [id,t]:threads) {
            if(t.wait==WaitKind::None || !t.wait_callbacks || t.callback_paused || (!idle && t.priority>=threads.at(current).priority))continue;
            bool pending=false;for(const auto& [cbid,cb]:callbacks_)if(cb.owner==static_cast<std::int32_t>(id) && cb.notifications)pending=true;
            if(!pending)continue;
            const auto now=clock_.system_microseconds();const auto remaining=t.deadline>now?t.deadline-now:0;
            const auto kind=t.wait;const auto object=t.wait_id;t.callback_paused=true;
            c=t.context;psprecomp::set_runtime_thread_identity(static_cast<std::int32_t>(id),t.name);
            if(!dispatch_callbacks(c))return;
            t.context=c;t.callback_paused=false;
            if(t.deadline)t.deadline=clock_.system_microseconds()+remaining;
            c=saved;psprecomp::set_runtime_thread_identity(static_cast<std::int32_t>(current),name);
            if(kind!=WaitKind::Delay)wake_waiters(kind,object);
        }
    }
    void update_lw_waiters(std::uint32_t id){
        std::uint32_t count=0;for(const auto& [uid,t]:threads)if(t.wait==WaitKind::LwMutex && t.wait_id==id)count++;
        lw_.set_waiters(id,count);
    }
    void make_ready(Thread& t) {const auto kind=t.wait;const auto id=t.wait_id;t.wait=WaitKind::None;t.wait_callbacks=false;t.ready=true;t.order=++order_;if(kind==WaitKind::LwMutex)update_lw_waiters(id);}
    void wake(Thread& t,std::uint32_t error=0) {
        if(t.wait==WaitKind::Codec){const auto pending=codec_writes_.find(t.uid);if(pending==codec_writes_.end())throw std::runtime_error("Codec completion lost its task output");
            for(const auto& write:pending->second)for(unsigned n=0;n<write.bytes.size();++n)rt.memory().store8(write.address+n,write.bytes[n]);codec_writes_.erase(pending);}
        t.context.set_gpr(2,error);
        if(t.timeout_pointer) {
            const auto now=clock_.system_microseconds();
            const auto left=t.deadline>now?t.deadline-now:0;
            rt.memory().store32(t.timeout_pointer,static_cast<std::uint32_t>(std::min(left,static_cast<std::uint64_t>(0xFFFFFFFFu))));
        }
        ++t.releases;make_ready(t);
    }
    void pump_timeouts() {
        const auto now=clock_.system_microseconds();display_.update(now);audio_.poll();controls_.update(now);
        if(requested_render_frames && verified_render_frames<requested_render_frames && !frame_stream && now>=verify_render_after_us){if(resident_gpu_ownership)complete_gpu_batch();frame_stream=std::make_unique<kon::NativeFrameStream>(rt.memory());renderer_.set_frame_stream(frame_stream.get());}
        if(profile_frames && now>=next_frame_sample_us_ && frame_samples_.size()<18001u){
            std::uint64_t cycles=0;
#ifdef _WIN32
            {ULONG64 native_cycles=0;QueryThreadCycleTime(GetCurrentThread(),&native_cycles);cycles=native_cycles;} // CPU work, independent of clock speed
#endif
            frame_samples_.push_back({now,display_.submitted_frames(),render_time_us_[1],render_time_us_[2],cycles,renderer_.gpu_collect_wait_us});next_frame_sample_us_=now+100000u;
        }
        if(runtime_limit_us && now>=runtime_limit_us){rt.stop("Diagnostic runtime limit reached");return;}
        if(result_deadline_us && now>=result_deadline_us){rt.stop("Result screen reached");return;}
        if(now>=next_progress_us_){next_progress_us_=now+5000000;const auto latency=display_.take_latency();check_freeze();
            try{renderer_.drain();}catch(const std::exception& error){rt.stop(error.what());return;} // counters below belong to the render thread
            std::cout<<"native_progress elapsed_ms="<<now/1000u<<" active_uid="<<psprecomp::runtime_thread_uid()<<" pc="<<psprecomp::hex32(rt.cpu().pc)<<" ra="<<psprecomp::hex32(rt.cpu().gpr[31])<<" draw_pixels="<<renderer_.draw_pixels<<" render_clear_us="<<render_time_us_[0]<<" render_2d_us="<<render_time_us_[1]<<" render_3d_us="<<render_time_us_[2]<<" projected_draws="<<projected_draws_<<" gpu_clear_primitives="<<renderer_.gpu_clear_primitives<<" gpu_clear_pixels="<<renderer_.gpu_clear_pixels<<" gpu_triangle_primitives="<<renderer_.gpu_triangle_primitives<<" gpu_triangle_pixels="<<renderer_.gpu_triangle_pixels<<" render_gpu_batch_us="<<gpu_batch_time_us<<" gpu_raster_primitives="<<renderer_.gpu_raster_primitives<<" gpu_raster_pixels="<<renderer_.gpu_raster_pixels<<" texture_cache_preparations="<<renderer_.texture_cache_preparations<<" texture_cache_hits="<<renderer_.texture_cache_hits<<" submitted_frames="<<display_.submitted_frames()<<" gpu_resident_words_reused="<<renderer_.gpu_resident_words_reused<<" gpu_collect_wait_us="<<renderer_.gpu_collect_wait_us<<" gpu_upload_us="<<renderer_.gpu_profile()[0]/1000u<<" gpu_shade_us="<<renderer_.gpu_profile()[1]/1000u<<" gpu_readback_us="<<renderer_.gpu_profile()[2]/1000u<<" gpu_dispatches="<<renderer_.gpu_profile()[3]<<" gpu_barriers="<<renderer_.gpu_profile()[4]<<" gpu_tile_job_pixels="<<renderer_.gpu_profile()[5]<<" input_latency_avg_us="<<(latency.frames?latency.total_us/latency.frames:0)<<" input_latency_max_us="<<latency.max_us<<" render_drains="<<renderer_.async_drains<<" drain_wait_us="<<renderer_.drain_wait_us<<" present_wait_us="<<renderer_.present_wait_us<<" vram_service_syncs="<<vram_service_syncs_<<" vram_hook_calls="<<vram_hook_calls_<<" vram_hook_collects="<<vram_hook_collects_<<" queued_draws="<<renderer_.pending_draws<<" queue_flushes="<<renderer_.pending_flushes<<" nonfinite_triangles="<<renderer_.nonfinite_triangles<<" overlap_waits="<<renderer_.overlap_waits[0]<<","<<renderer_.overlap_waits[1]<<","<<renderer_.overlap_waits[2]<<","<<renderer_.overlap_waits[3]<<","<<renderer_.overlap_waits[4]<<","<<renderer_.overlap_waits[5]<<" draw_covered_pixels="<<renderer_.draw_covered_pixels<<" display_present_us="<<display_.present_us()<<" presenter="<<display_.presenter()<<" hires_presents="<<display_.hires_presents()<<" hires_fallback_tiles="<<display_.hires_fallback_tiles()<<" hires_bad_presents="<<display_.hires_bad_presents()<<" idle_wait_us="<<idle_wait_us_<<" hook_collect_wait_us="<<hook_collect_wait_us<<" vblank_caught_up="<<vblank_caught_up_<<" masked_blocks="<<masked_blocks_<<" logic_ticks="<<logic_rate.published_ticks<<" logic_steps30="<<logic_rate.published_steps30<<" batch_clears="<<renderer_.batch_clear_primitives<<" flush_clear_vram="<<renderer_.flush_reasons[1]<<" flush_unsafe="<<renderer_.flush_reasons[2]<<" flush_identity="<<renderer_.flush_reasons[3]<<" flush_capacity="<<renderer_.flush_reasons[4]<<" texture_compared_bytes="<<kon::NativeTexture::compared_bytes()<<" vram_texture_flushes="<<renderer_.vram_texture_flushes<<" gpu_submits="<<renderer_.gpu_profile()[3]<<" vblank_waits="<<vblank_waits_[0]<<","<<vblank_waits_[1]<<","<<vblank_waits_[2]<<","<<vblank_waits_[3]<<" time_calls="<<time_calls_[0]<<","<<time_calls_[1]<<","<<time_calls_[2]<<","<<time_calls_[3]<<" guest_task_us="<<[&]{std::uint64_t total=0;for(const auto& [uid,t]:threads)total+=t.running_us;return total;}()<<"\n";
            std::cout<<"native_audio_device elapsed_ms="<<now/1000u<<" block_frames="<<kon::NativeAudio::block_frames()<<" underruns="<<audio_.device_underruns()<<'\n';
            for(unsigned channel=0;channel<8;++channel){const auto d=audio_.diagnostics(channel);if(d.submitted_frames)
                std::cout<<"native_audio_progress elapsed_ms="<<now/1000u<<" channel="<<channel<<" submitted_frames="<<d.submitted_frames<<" copied_frames="<<d.completed_frames<<" device_played_frames="<<d.played_frames<<" queued_buffers="<<d.queued_buffers<<" empty_queue_submissions="<<d.empty_queue_submissions<<" mixer_gap_count="<<d.gap_count<<" mixer_gap_us="<<d.gap_microseconds<<" max_mixer_gap_us="<<d.max_gap_microseconds<<"\n";
            }std::cout<<std::flush;}
        if(capture_interval_us && !progress_capture.empty() && now>=next_capture_us_ && now>=capture_begin_us){
            next_capture_us_=now+capture_interval_us;
            const auto path=progress_capture.parent_path()/(progress_capture.stem().string()+"_"+std::to_string(now/1000u)+"ms.bmp");
            display_.save_bitmap(path,true);calls.push_back("ProgressCapture elapsed_ms="+std::to_string(now/1000u)+" path="+path.string());
        }
        if(controls_.available()){
            Thread* waiting=nullptr;for(auto& [id,t]:threads)if(t.wait==WaitKind::Controller && !t.callback_paused && (!waiting || t.order<waiting->order))waiting=&t;
            if(waiting){const auto taken=controls_.read(rt.memory(),waiting->out_pointer,waiting->wait_bits);
                acknowledge_keyboard_batch(waiting->out_pointer,taken);
                calls.push_back("CtrlReadBufferPositive resumed_samples="+std::to_string(taken));wake(*waiting,taken);}
        }
        for(auto& [id,t]:threads)if(t.wait==WaitKind::Audio && !t.callback_paused && audio_.completed(t.wait_id,t.wait_bits)){
            const auto bit=1u<<t.wait_id;
            // A completed copy into the real host mixer releases source ownership.
            // Its blocking output consumes that bit with CLEAR (0x20).
            if(signals_.set_event(audio_event_uid,bit))throw std::runtime_error("Native audio completion event was deleted");
            const auto event=signals_.acquire_event(audio_event_uid,bit,0x20u);
            if(event.error || event.blocked)throw std::runtime_error("Native audio completion event failed to release its writer");
            wake(t,t.wait_mode);
            wake_waiters(WaitKind::Event,audio_event_uid);
        }
        for(auto& [id,t]:threads)if(t.wait==WaitKind::Network && !t.callback_paused){
            const auto pending=net_waits_.find(id);if(pending==net_waits_.end())continue;
            if(const auto result=pending->second()){net_waits_.erase(pending);wake(t,*result);}
        }
        if(display_.closed()){rt.stop("Native game window closed");return;}
        for(auto& [id,t]:threads) if(t.wait!=WaitKind::None && t.wait!=WaitKind::Controller && !t.callback_paused && t.deadline && t.deadline<=now) {
            if(t.wait==WaitKind::Network)net_waits_.erase(id);
            wake(t,t.wait==WaitKind::Codec?t.wait_mode:t.wait==WaitKind::Delay?0u:t.wait==WaitKind::Network?0x80410715u:0x800201A8u);
        }
    }
    Thread* best_ready() {
        Thread* best=nullptr;
        for(auto& [id,t]:threads)if(t.ready && !t.finished && (!best || t.priority<best->priority ||
            (t.priority==best->priority && t.order<best->order)))best=&t;
        return best;
    }
    std::uint64_t idle_wait_us_=0,hook_collect_wait_us=0,vram_hook_calls_=0,vram_hook_collects_=0; // host time with every guest task waiting
    void dispatch_next(psprecomp::AllegrexContext& c) {
        // The interrupt state belongs to the thread context, as on the PSP: a
        // thread that blocks with interrupts suspended keeps them suspended
        // only for itself (it was global, and a switch while suspended left
        // every other thread masked: no preemption, callbacks or GE
        // interrupts, and CRI's waveout thread spun forever).
        static const bool global_intr=std::getenv("KON_GLOBAL_INTERRUPT_STATE")!=nullptr; // previous behaviour (A/B)
        if(!global_intr){if(const auto it=threads.find(static_cast<std::uint32_t>(psprecomp::runtime_thread_uid()));it!=threads.end())it->second.interrupts=interrupts_enabled_;
            if(!interrupts_enabled_)++masked_blocks_;interrupts_enabled_=true;}
        account_running();running_uid_=0;
        pump_timeouts();auto* next=best_ready();
        while(!next) {
            if(rt.stopped())return;
            dispatch_waiting_callbacks(c,false,true);if(rt.stopped())return;
            next=best_ready();if(next)break;
            pump_graphics(c);if(rt.stopped())return;
            next=best_ready();if(next)break;
            // A partially processed list is runnable host work even when every
            // guest task is waiting for graphics and no audio timer exists.
            if(!ge_.queue.empty() && ge_.draw_status()!=3)continue;
            std::uint64_t deadline=std::numeric_limits<std::uint64_t>::max();
            for(const auto& [id,t]:threads)if(t.wait!=WaitKind::None && t.deadline)deadline=std::min(deadline,t.deadline);
            if(audio_.has_pending())deadline=std::min(deadline,clock_.system_microseconds()+1000u);
            if(deadline==std::numeric_limits<std::uint64_t>::max()) {
                rt.stop("No runnable guest task and no timed wakeup; pending external event");return;
            }
            const auto now=clock_.system_microseconds();
            if(deadline>now){scheduler_wait_.wait_for(std::min(deadline-now,std::uint64_t{1000}));const auto waited=clock_.system_microseconds()-now;idle_wait_us_+=waited;trace(10,waited);}
            pump_timeouts();next=best_ready();
        }
        if(!interrupts_enabled_ && masked_context_switches_++<32)calls.push_back("MaskedContextSwitch previous_uid="+std::to_string(psprecomp::runtime_thread_uid())+" next_uid="+std::to_string(next->uid)+" pc="+psprecomp::hex32(c.pc)+" ra="+psprecomp::hex32(c.gpr[31]));
        if(!global_intr)interrupts_enabled_=next->interrupts;
        next->ready=false;c=next->context;
        running_uid_=next->uid;running_since_=clock_.system_microseconds();trace(9,next->uid);
        psprecomp::set_runtime_thread_identity(static_cast<std::int32_t>(next->uid),next->name);
        calls.push_back("Schedule uid="+std::to_string(next->uid)+" name="+next->name);
    }
    void maybe_preempt(psprecomp::AllegrexContext& c,bool at_import=true) {
        const auto current=static_cast<std::uint32_t>(psprecomp::runtime_thread_uid());
        if(callback_depth_ || !interrupts_enabled_ || current==1 || !threads.contains(current))return;
        auto* best=best_ready();auto& active=threads.at(current);
        if(!best || best->priority>=active.priority)return;
        // A post-import hook can already be running a different, resumed task.
        // Its PC is a saved continuation, so only an actual import stub returns
        // through RA. Replacing a resumed PC with its caller RA replays syscalls.
        ++active.preemptions;active.context=c;if(at_import && import_pcs_.contains(c.pc))active.context.pc=c.gpr[31];make_ready(active);dispatch_next(c);
    }
    void block_wait(psprecomp::AllegrexContext& c,WaitKind kind,std::uint32_t id,std::uint32_t bits,
                    std::uint32_t mode,std::uint32_t out,std::uint32_t timeout,std::uint64_t delay=0,bool callbacks=false) {
        if(callback_depth_){rt.stop("Blocking wait inside callback requires asynchronous callback frames");return;}
        const auto current=static_cast<std::uint32_t>(psprecomp::runtime_thread_uid());
        if(!threads.contains(current)){rt.stop("Wait outside registered worker");return;}
        trace(12,(std::uint64_t(current)<<32)|(std::uint64_t(static_cast<unsigned>(kind))<<16)|std::min<std::uint64_t>(delay/1000u,0xFFFFu));
        if((out && !rt.memory().contains(out,4)) || (timeout && !rt.memory().contains(timeout,4))) {
            rt.stop("Invalid wait output or timeout pointer");return;
        }
        if(kind==WaitKind::Event && !(signals_.events.at(id).attributes&0x200u)) {
            for(const auto& [uid,t]:threads)if(t.wait==kind && t.wait_id==id){c.set_gpr(2,0x800201B0u);return;}
        }
        auto& active=threads.at(current);c.set_gpr(2,0);active.context=c;active.context.pc=c.gpr[31];
        active.ready=false;active.wait=kind;active.wait_callbacks=callbacks;active.wait_id=id;active.wait_bits=bits;active.wait_mode=mode;
        active.out_pointer=out;active.timeout_pointer=timeout;active.order=++order_;active.deadline=0;
        if(delay)active.deadline=clock_.system_microseconds()+delay;
        else if(timeout){auto duration=rt.memory().load32(timeout);if(kind==WaitKind::LwMutex){const auto signed_duration=std::bit_cast<std::int32_t>(duration);if(signed_duration<=3)duration=25;else if(signed_duration<250)duration=250;}active.deadline=clock_.system_microseconds()+std::max(duration,25u);}
        if(kind==WaitKind::LwMutex)update_lw_waiters(id);
        calls.push_back("Wait uid="+std::to_string(current)+" kind="+std::to_string(static_cast<unsigned>(kind))+" object="+std::to_string(id));
        dispatch_next(c);
    }
    void wake_waiters(WaitKind kind,std::uint32_t id) {
        std::vector<Thread*> waiters;
        for(auto& [uid,t]:threads)if(t.wait==kind && t.wait_id==id && !t.callback_paused)waiters.push_back(&t);
        bool priority=false;
        if(kind==WaitKind::Mutex)priority=(mutexes_.objects.at(id).attributes&0x100u)!=0;
        if(kind==WaitKind::LwMutex)priority=(lw_.objects.at(id).attributes&0x100u)!=0;
        if(kind==WaitKind::Semaphore)priority=(signals_.semaphores.at(id).attributes&0x100u)!=0;
        std::stable_sort(waiters.begin(),waiters.end(),[=](const Thread* a,const Thread* b){
            if(priority && a->priority!=b->priority)return a->priority<b->priority;
            return a->order<b->order;
        });
        for(auto* t:waiters) {
            if(kind==WaitKind::Event) {
                const auto r=signals_.acquire_event(id,t->wait_bits,t->wait_mode);
                if(r.blocked)continue;
                if(t->out_pointer)rt.memory().store32(t->out_pointer,r.pattern);wake(*t,r.error);
            } else if(kind==WaitKind::Semaphore) {
                const auto r=signals_.acquire_semaphore(id,static_cast<std::int32_t>(t->wait_bits));
                if(!r.blocked)wake(*t,r.error);
            } else if(kind==WaitKind::Mutex) {
                const auto r=mutexes_.acquire(id,static_cast<std::int32_t>(t->uid),static_cast<std::int32_t>(t->wait_bits));
                if(!r.blocked){mutex_lock_site_[id]={static_cast<std::int32_t>(t->uid),t->context.gpr[31],t->context.pc,clock_.system_microseconds(),callback_depth_,interrupts_enabled_};wake(*t,r.error);}
            } else if(kind==WaitKind::LwMutex) {
                const auto r=lw_.acquire(lw_.objects.at(id).area,static_cast<std::int32_t>(t->uid),static_cast<std::int32_t>(t->wait_bits));
                if(!r.blocked)wake(*t,r.error);
            } else if(kind==WaitKind::ThreadEnd)wake(*t,threads.at(id).exit_status);
            else if(kind==WaitKind::GeList || kind==WaitKind::GeDraw)wake(*t);
        }
    }
    std::string guest_name(std::uint32_t address) {
        std::string result;
        for(unsigned n=0;n<128;n++) {
            const auto ch=rt.memory().load8(address+n);
            if(!ch) return result;
            result.push_back(static_cast<char>(ch));
        }
        throw std::runtime_error("Unterminated thread name");
    }
    void create(psprecomp::AllegrexContext& c) {
        const auto entry=c.gpr[5],priority=c.gpr[6],size=c.gpr[7],attr=c.gpr[8],option=c.gpr[9];
        if(priority<8 || priority>119 || size<512 || size>0x100000 || option!=0 ||
           (entry&3u)!=0 || !rt.memory().contains(entry,4)) {
            rt.stop("CreateThread request outside implemented bootstrap semantics"); return;
        }
        const auto name=guest_name(c.gpr[4]);
        const auto rounded=(size+255u)&~255u;
        const auto memory_uid=arena_.allocate(1u,rounded);
        const auto stack=arena_.block(memory_uid);
        if(!stack) {c.set_gpr(2,0x80020190u);return;}
        const auto uid=next_uid_++;
        Thread thread{uid,entry,priority,stack->address,rounded,attr,name};
        thread.initial_priority=priority;thread.exit_status=0x800201A2u;
        thread.stack_memory_uid=memory_uid;
        for(std::uint32_t n=0;n<rounded;n++) rt.memory().store8(thread.stack_base+n,0xFFu);
        kon::reset_guest_context(thread.context);
        kon::initialize_thread_stack(rt.memory(),thread.context,uid,thread.stack_base,thread.stack_size);
        thread.context.gpr[28]=gp_;
        thread.context.gpr[29]=thread.stack_base+thread.stack_size-0x100u;
        thread.context.gpr[31]=kReturnPc;
        thread.context.pc=entry;
        calls.push_back("CreateThread uid="+std::to_string(uid)+" name="+thread.name+" entry="+psprecomp::hex32(entry)+" stack="+std::to_string(rounded));
        threads.emplace(uid,std::move(thread)); c.set_gpr(2,uid);
    }
    void start(psprecomp::AllegrexContext& c) {
        const auto found=threads.find(c.gpr[4]);
        if(found==threads.end()) {rt.stop("StartThread unknown task");return;}
        auto& thread=found->second;
        if(thread.started && !thread.finished){calls.push_back("StartThreadNotDormant uid="+std::to_string(thread.uid)+" caller="+psprecomp::hex32(c.gpr[31]));c.set_gpr(2,0x800201A4u);return;}
        const auto size=c.gpr[5],src=c.gpr[6];
        if(size>thread.stack_size-0x140u || (size && !rt.memory().contains(src,size))) {
            rt.stop("StartThread invalid argument block");return;
        }
        // Stack and argument blocks belong to this task and survive preemption.
        if(thread.finished){kon::reset_guest_context(thread.context);kon::initialize_thread_stack(rt.memory(),thread.context,thread.uid,thread.stack_base,thread.stack_size);thread.context.gpr[28]=gp_;thread.context.gpr[29]=thread.stack_base+thread.stack_size-0x100u;thread.context.gpr[31]=kReturnPc;thread.context.pc=thread.entry;thread.finished=false;thread.wait=WaitKind::None;}
        const auto dst=(thread.context.gpr[29]-size)&~15u;
        std::vector<std::uint8_t> args;
        for(std::uint32_t n=0;n<size;n++) args.push_back(rt.memory().load8(src+n));
        for(std::uint32_t n=0;n<size;n++) rt.memory().store8(dst+n,args[n]);
        thread.context.gpr[4]=size;thread.context.gpr[5]=size?dst:0u;
        thread.context.gpr[29]=dst-0x40u;thread.ready=true;thread.started=true;
        calls.push_back("StartThread uid="+std::to_string(thread.uid)+" copied_args="+std::to_string(size)+" caller="+psprecomp::hex32(c.gpr[31]));
        thread.order=++order_;c.set_gpr(2,0);maybe_preempt(c);
    }
};
BootstrapServices* active_services=nullptr;
}
// PC frame-rate patch values (src/native_logic_rate.hpp); 30 FPS originals.
extern "C" std::uint32_t kon_logic_frame_ticks=2,kon_logic_step30=1,kon_logic_fps=30;
// Pad auto-repeat patch scratch (one button at a time, game thread only).
extern "C" std::uint32_t kon_pad_hold_before=0,kon_pad_repeat_due=0;
extern "C" float kon_logic_frame_ticks_f=2.0f,kon_logic_smooth_factor=0.5f;
// Smoothing `x += d/2` per 30 FPS frame becomes `x += d*factor` per logic frame
// (same convergence per second); at least 1 so small gaps still close.
extern "C" std::uint32_t kon_smooth_step(std::uint32_t difference){
    const auto d=static_cast<std::int32_t>(difference);
    auto step=static_cast<std::int32_t>(static_cast<double>(d)*kon_logic_smooth_factor);
    if(!step && d)step=d>0?1:-1;
    return static_cast<std::uint32_t>(step);
}
// Diagnostic for tools/instrument_frame_counters.py: per (site, guest address),
// how many distinct logic frames executed a +/-1 memory update.
std::uint64_t kon_logic_frame_index=0;
struct CounterSiteStats{std::uint64_t hits=0,frames=0,last_frame=~0ull,first_frame=0,max_run=0,run=0;};
std::map<std::uint64_t,CounterSiteStats> kon_counter_stats;
extern "C" void kon_counter_site(std::uint32_t site,std::uint32_t address){
    auto& s=kon_counter_stats[(std::uint64_t(site)<<32)|address];++s.hits;
    if(s.last_frame!=kon_logic_frame_index){
        if(!s.frames)s.first_frame=kon_logic_frame_index;
        s.run=s.last_frame+1==kon_logic_frame_index?s.run+1:1;s.max_run=std::max(s.max_run,s.run);
        ++s.frames;s.last_frame=kon_logic_frame_index;
    }
}
void kon_counter_summary(std::ostream& out){
    for(const auto& [key,s]:kon_counter_stats)out<<"native_counter_site id="<<(key>>32)<<" address="<<psprecomp::hex32(std::uint32_t(key))<<" hits="<<s.hits<<" frames="<<s.frames<<" first="<<s.first_frame<<" last="<<s.last_frame<<" max_run="<<s.max_run<<'\n';
}
// Diagnostic for tools/scan_step_sites.py: per (store site, guest caller), how
// much each store moved its value on short and on long frames while
// --speed-audit alternates them.  A value advanced by game time moves more on
// long frames; one advanced per logic frame moves the same.
bool kon_step_active=false;double kon_step_nominal=1.0;std::uint64_t kon_step_frames=0;std::int32_t kon_step_thread=0; // the logic thread (waits for vblank)
struct StepSiteStats{std::uint64_t frames=0,last_frame=~0ull,hits[2]{},moved[2]{},same=0;double sum[2]{};std::uint32_t first_address=0,addresses=0,first_delta_bits=0;bool have_delta=false;void* stack[8]{};unsigned depth=0;};
std::unordered_map<std::uint64_t,StepSiteStats> kon_step_stats;
#ifdef _WIN32
extern "C" void kon_step_site(std::uint32_t site,std::uint32_t address,std::uint32_t old_value,std::uint32_t new_value,std::uint32_t kind,std::uint32_t ra){
    static const bool all_threads=std::getenv("KON_STEP_ALL_THREADS")!=nullptr; // diagnostics: include other guest threads
    if(!kon_step_active || (!all_threads && psprecomp::runtime_thread_uid()!=kon_step_thread))return;
    auto& s=kon_step_stats[(std::uint64_t(site)<<32)|ra];
    if(s.last_frame!=kon_logic_frame_index){++s.frames;s.last_frame=kon_logic_frame_index;}
    const int f=kon_logic_frame_ticks_f>kon_step_nominal?1:0;++s.hits[f];
    if(!s.addresses++)s.first_address=address;else if(address==s.first_address)--s.addresses;
    double delta=0.0;
    if(kind==1){const auto a=std::bit_cast<float>(old_value),b=std::bit_cast<float>(new_value);if(std::isfinite(a) && std::isfinite(b))delta=double(b)-double(a);}
    else if(kind==8)delta=double(std::int8_t(new_value)-std::int8_t(old_value));
    else if(kind==16)delta=double(std::int16_t(new_value)-std::int16_t(old_value));
    else delta=double(std::int32_t(new_value-old_value));
    if(delta==0.0)return;
    ++s.moved[f];s.sum[f]+=std::fabs(delta);
    if(!s.depth)s.depth=CaptureStackBackTrace(1,8,s.stack,nullptr);
    const auto bits=new_value-old_value;
    if(!s.have_delta){s.have_delta=true;s.first_delta_bits=bits;}else if(bits==s.first_delta_bits)++s.same;
}
void kon_step_summary(std::ostream& out){
    if(kon_step_stats.empty())return;
    out<<"native_step_audit frames="<<kon_step_frames<<'\n';
    const auto process=GetCurrentProcess();SymSetOptions(SYMOPT_UNDNAME|SYMOPT_DEFERRED_LOADS|SYMOPT_LOAD_LINES);
    wchar_t module[MAX_PATH]{};GetModuleFileNameW(nullptr,module,MAX_PATH);const bool symbols=SymInitialize(process,std::filesystem::path(module).parent_path().string().c_str(),TRUE);
    for(const auto& [key,s]:kon_step_stats){
        if(!s.moved[0] || !s.moved[1])continue;
        out<<"native_step_site id="<<(key>>32)<<" ra="<<psprecomp::hex32(std::uint32_t(key))<<" frames="<<s.frames<<" hits="<<s.hits[0]<<"+"<<s.hits[1]<<" moved="<<s.moved[0]<<"+"<<s.moved[1]
           <<" short="<<s.sum[0]/double(s.moved[0])<<" long="<<s.sum[1]/double(s.moved[1])<<" same="<<s.same<<" address="<<psprecomp::hex32(s.first_address)<<" addresses="<<s.addresses<<" stack=";
        for(unsigned n=0;n<s.depth;++n){
            DWORD displacement=0;IMAGEHLP_LINE64 line{};line.SizeOfStruct=sizeof line;
            if(symbols && SymGetLineFromAddr64(process,reinterpret_cast<DWORD64>(s.stack[n]),&displacement,&line))out<<std::filesystem::path(line.FileName).stem().string()<<":"<<line.LineNumber<<",";else out<<"?,";
        }
        out<<'\n';
    }
    if(symbols)SymCleanup(process);
}
#ifdef _WIN32
namespace {
#endif
// Diagnostic for PSPRECOMP_WATCH_WRITE (+ _SIZE, optional _AFTER_MS): groups
// watched guest stores by native store site and, at exit, resolves each site
// to generated source lines (file:line maps to the guest address through the
// L_xxxxxxxx labels) with its write count and observed value deltas.
struct WatchSite{std::uint64_t writes=0;std::uint32_t first_address=0;std::vector<std::int64_t> deltas;std::vector<void*> stack;std::set<std::uint32_t> addresses;};
std::mutex watch_mutex;std::map<void*,WatchSite> watch_sites;
const auto watch_epoch=std::chrono::steady_clock::now();
void watch_write_stack(std::uint32_t address,std::size_t,std::uint64_t old_value,std::uint64_t new_value){
    static const auto after=std::chrono::milliseconds(std::getenv("PSPRECOMP_WATCH_WRITE_AFTER_MS")?std::stoull(std::getenv("PSPRECOMP_WATCH_WRITE_AFTER_MS")):0ull);
    if(std::chrono::steady_clock::now()-watch_epoch<after)return;
    void* frames[12]{};const auto count=CaptureStackBackTrace(2,12,frames,nullptr);if(!count)return;
    std::lock_guard lock(watch_mutex);auto& site=watch_sites[frames[0]];
    if(!site.writes++){site.first_address=address;site.stack.assign(frames,frames+count);}
    if(site.addresses.size()<64)site.addresses.insert(address);
    const auto delta=std::int64_t(new_value)-std::int64_t(old_value);
    if(site.deltas.size()<6 && std::find(site.deltas.begin(),site.deltas.end(),delta)==site.deltas.end())site.deltas.push_back(delta);
}
void watch_write_summary(std::ostream& out){
    std::lock_guard lock(watch_mutex);if(watch_sites.empty())return;
    const auto process=GetCurrentProcess();SymSetOptions(SYMOPT_UNDNAME|SYMOPT_DEFERRED_LOADS|SYMOPT_LOAD_LINES);
    wchar_t module[MAX_PATH]{};GetModuleFileNameW(nullptr,module,MAX_PATH);const bool symbols=SymInitialize(process,std::filesystem::path(module).parent_path().string().c_str(),TRUE);
    std::vector<std::pair<void*,WatchSite*>> sorted;for(auto& [key,site]:watch_sites)sorted.emplace_back(key,&site);
    std::sort(sorted.begin(),sorted.end(),[](const auto& a,const auto& b){return a.second->writes>b.second->writes;});
    for(const auto& [key,site]:sorted){
        out<<"native_watch_site writes="<<site->writes<<" addresses="<<site->addresses.size()<<" first="<<psprecomp::hex32(site->first_address)<<" deltas=";
        for(const auto d:site->deltas)out<<d<<",";
        if(site->addresses.size()<=8){out<<" at=";for(const auto a:site->addresses)out<<psprecomp::hex32(a)<<",";}
        for(std::size_t n=0;n<site->stack.size() && n<4;++n){
            DWORD line_displacement=0;IMAGEHLP_LINE64 line{};line.SizeOfStruct=sizeof line;
            if(symbols && SymGetLineFromAddr64(process,reinterpret_cast<DWORD64>(site->stack[n]),&line_displacement,&line))out<<" | "<<std::filesystem::path(line.FileName).filename().string()<<":"<<line.LineNumber;else out<<" | ?";
        }
        out<<'\n';
    }
    if(symbols)SymCleanup(process);
}
#else
// Android has no DbgHelp/CaptureStackBackTrace diagnostics. Keep the symbols
// the generated corpus and the runtime reference so linking still works.
extern "C" void kon_step_site(std::uint32_t,std::uint32_t,std::uint32_t,std::uint32_t,std::uint32_t,std::uint32_t){}
void kon_step_summary(std::ostream&){}
void watch_write_stack(std::uint32_t,std::size_t,std::uint64_t,std::uint64_t){}
namespace { void watch_write_summary(std::ostream&){} }
#endif
void guest_return(psprecomp::Runtime& rt,psprecomp::AllegrexContext& c) {
    if(active_services) active_services->returned(c); else rt.stop("Guest entry returned");
}
#ifdef _WIN32
}
#endif
#ifndef KON_SERVICE_FIXTURE
// Diagnostic (KON_TRACE_PCS=0xLO-0xHI, with PSPRECOMP_NO_CHAIN=1 so every
// call is dispatched): the first time each guest function address in the
// range runs, and how often, printed at the end.
namespace pc_trace {
std::uint32_t lo=0,hi=0;std::map<std::uint32_t,std::pair<std::uint64_t,std::uint64_t>> seen;
std::chrono::steady_clock::time_point start=std::chrono::steady_clock::now();
void hook(psprecomp::Runtime&,psprecomp::AllegrexContext&,std::uint32_t pc,std::int32_t){
    if(pc<lo || pc>hi)return;
    auto& e=seen[pc];if(!e.second)e.first=std::uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-start).count());++e.second;
}
void install(){
    const char* text=std::getenv("KON_TRACE_PCS");if(!text)return;
    const std::string range(text);const auto dash=range.find('-');if(dash==std::string::npos)return;
    lo=std::uint32_t(std::stoul(range.substr(0,dash),nullptr,0));hi=std::uint32_t(std::stoul(range.substr(dash+1),nullptr,0));
    psprecomp::set_runtime_pre_dispatch_hook(&hook);
}
void report(){for(const auto& [pc,e]:seen)std::cout<<"trace_pc pc="<<psprecomp::hex32(pc)<<" first_ms="<<e.first<<" hits="<<e.second<<'\n';}
}
int main(int argc,char** argv) {
    // Installation from the player's ISO (INSTALAR.cmd): "--install [--root folder] [iso...]".
#ifdef _WIN32
    if(argc>=2 && std::string(argv[1])=="--install"){
        SetConsoleOutputCP(CP_UTF8);
        std::filesystem::path root=std::filesystem::absolute(argv[0]).parent_path();std::vector<std::filesystem::path> isos;
        for(int i=2;i<argc;++i){if(std::string(argv[i])=="--root" && i+1<argc)root=argv[++i];else isos.emplace_back(argv[i]);}
        return kon::install_from_isos(root,isos);
    }
#endif
    try {
        std::filesystem::path root=std::filesystem::current_path();
        std::string language="en",gpu_adapter="auto";bool language_given=false;
        bool sample_ge_only=false,sample_render_thread=false;std::filesystem::path frame_dump,sample_profile,trace_frames;std::uint64_t trace_begin_ms=0,trace_end_ms=0;std::uint64_t sample_begin_ms=0,sample_end_ms=~0ull;bool bootstrap=false,window=false,visible_checkpoint=false,trace_render=false;unsigned runtime_limit_ms=0,capture_interval_ms=0,anchor_channel=3,raster_threads=4,verify_frames=0,verify_after_ms=0;
        bool test_fullscreen=false,show_test_window=false,resident_gpu_ownership=false,cpu_renderer=false,raster_threads_given=false;unsigned logic_cycles=0,logic_fps=0;bool logic_fixed_step=false;std::filesystem::path settings_file;std::string draw_dump;std::uint64_t draw_dump_begin_ms=0,draw_dump_end_ms=0;std::string ram_dir;std::uint64_t ram_start_ms=0,ram_interval_ms=0;unsigned ram_count=0;unsigned internal_scale=0;std::uint64_t capture_begin_ms=0;std::string ram_variation_file;std::uint64_t ram_variation_start_ms=0,ram_variation_ms=0,speed_audit_start_ms=0,speed_audit_ms=0;
        bool trace_hle=false,persistent_texture_cache=false,vulkan_clear=false,vulkan_triangles=false,vulkan_raster=false,vulkan_batch=false;
        std::uint64_t dispatch_budget=100000000u;bool dispatch_budget_given=false;
        std::filesystem::path capture;unsigned hold_ms=0;
        std::filesystem::path replay,storage,audio_replay,render_profile,frame_profile,projected_draw_capture,quad_draw_capture;unsigned projected_after_ms=0,projected_min_vertices=64;
        unsigned stop_on_result_ms=0; // --stop-on-result[=ms]: stop after the results screen loads
        for(int i=1;i<argc;i++) {
            const std::string arg=argv[i];
            if(arg=="--root" && i+1<argc) root=argv[++i];
            else if(arg=="--language" && i+1<argc){language=argv[++i];language_given=true;}
            else if(arg=="--gpu-adapter" && i+1<argc)gpu_adapter=argv[++i];
            else if(arg=="--bootstrap") bootstrap=true;
            else if(arg=="--vulkan-resident-batch"){vulkan_raster=true;vulkan_batch=true;resident_gpu_ownership=true;}
            else if(arg=="--cpu-renderer")cpu_renderer=true;
            else if(arg=="--logic-vblank-cycles" && i+1<argc)logic_cycles=std::stoul(argv[++i]);
            else if(arg=="--logic-fixed-step")logic_fixed_step=true;
            else if(arg=="--settings-file" && i+1<argc)settings_file=argv[++i];
            else if(arg=="--logic-fps" && i+1<argc){logic_fps=std::stoul(argv[++i]);if(!kon::NativeLogicRate::supported(logic_fps))throw std::runtime_error("--logic-fps accepts 30 to 240");}
            else if(arg=="--ram-snapshots" && i+3<argc){ram_dir=argv[++i];ram_start_ms=std::stoull(argv[++i]);ram_count=std::min(64ul,std::stoul(argv[++i]));}
            else if(arg=="--ram-snapshot-interval-ms" && i+1<argc)ram_interval_ms=std::stoull(argv[++i]);
            else if(arg=="--fullscreen")test_fullscreen=true;
            else if(arg=="--internal-scale" && i+1<argc){internal_scale=std::stoul(argv[++i]);if(internal_scale<1 || internal_scale>4)throw std::runtime_error("--internal-scale accepts 1 to 4");}
            else if(arg=="--capture-begin-ms" && i+1<argc)capture_begin_ms=std::stoull(argv[++i]);
            else if(arg=="--speed-audit" && i+2<argc){speed_audit_start_ms=std::stoull(argv[++i]);speed_audit_ms=std::stoull(argv[++i]);}
            else if(arg=="--ram-variation" && i+3<argc){ram_variation_file=argv[++i];ram_variation_start_ms=std::stoull(argv[++i]);ram_variation_ms=std::stoull(argv[++i]);}
            else if(arg=="--gdi-present")kon::NativeWindow::gdi_only()=true;
            else if(arg=="--silent-test"){kon::NativeWindow::hidden()=true;kon::NativeAudio::muted()=true;}
            else if(arg=="--mute-audio")kon::NativeAudio::muted()=true;
            else if(arg=="--show-test-window"){window=true;bootstrap=true;show_test_window=true;}
            else if(arg=="--verify-render-frames" && i+1<argc){verify_frames=std::stoul(argv[++i]);if(!verify_frames || verify_frames>4)throw std::runtime_error("Frame replay verification count must be 1-4");}
            else if(arg=="--verify-render-after-ms" && i+1<argc)verify_after_ms=std::stoul(argv[++i]);
            else if(arg=="--dump-render-frames" && i+1<argc)frame_dump=argv[++i];
            else if(arg=="--sample-profile" && i+1<argc)sample_profile=argv[++i];
            else if(arg=="--sample-ge-only")sample_ge_only=true;
            else if(arg=="--sample-render-thread")sample_render_thread=true; // profile the asynchronous render thread
            else if(arg=="--trace-frames" && i+3<argc){trace_frames=argv[++i];trace_begin_ms=std::stoull(argv[++i]);trace_end_ms=std::stoull(argv[++i]);}
            else if(arg=="--sample-window-ms" && i+2<argc){sample_begin_ms=std::stoull(argv[++i]);sample_end_ms=std::stoull(argv[++i]);}
            else if(arg=="--window"){window=true;bootstrap=true;}
            else if(arg=="--stop-after-visible-frame")visible_checkpoint=true;
            else if(arg=="--trace-hle")trace_hle=true;
            else if(arg=="--trace-render")trace_render=true;
            else if(arg=="--dump-draws" && i+3<argc){draw_dump=argv[++i];draw_dump_begin_ms=std::stoull(argv[++i]);draw_dump_end_ms=std::stoull(argv[++i]);}
            else if(arg=="--persistent-texture-cache")persistent_texture_cache=true;
            else if(arg=="--raster-threads" && i+1<argc){raster_threads_given=true;raster_threads=std::stoul(argv[++i]);if(!raster_threads || raster_threads>16)throw std::runtime_error("Raster threads must be between 1 and 16");}
            else if(arg=="--vulkan-clear")vulkan_clear=true;
            else if(arg=="--vulkan-triangles")vulkan_triangles=true;
            else if(arg=="--vulkan-raster")vulkan_raster=true;
            else if(arg=="--vulkan-batch"){vulkan_raster=true;vulkan_batch=true;}
            else if(arg=="--vulkan"){vulkan_clear=true;vulkan_raster=true;}
            else if(arg=="--profile-frames" && i+1<argc)frame_profile=argv[++i];
            else if(arg=="--capture-small-quad" && i+1<argc)quad_draw_capture=argv[++i];
            else if(arg=="--profile-render" && i+1<argc)render_profile=argv[++i];
            else if(arg=="--capture-3d-min-vertices" && i+1<argc){const auto n=std::stoul(argv[++i]);if(n<3 || n>65535)throw std::runtime_error("3D capture vertex count must be 3-65535");projected_min_vertices=static_cast<unsigned>(n);}
            else if(arg=="--capture-3d-draw" && i+1<argc)projected_draw_capture=argv[++i];
            else if(arg=="--capture-3d-after-ms" && i+1<argc){const auto ms=std::stoull(argv[++i]);if(ms>1800000)throw std::runtime_error("3D capture threshold exceeds diagnostic limit");projected_after_ms=static_cast<unsigned>(ms);}
            else if(arg=="--input-replay" && i+1<argc)replay=argv[++i];
            else if(arg=="--audio-anchored-input-replay" && i+1<argc)audio_replay=argv[++i];
            else if(arg=="--input-anchor-channel" && i+1<argc){const auto channel=std::stoul(argv[++i]);if(channel>=8)throw std::runtime_error("Replay anchor audio channel must be between 0 and 7");anchor_channel=static_cast<unsigned>(channel);}
            else if(arg=="--userdata" && i+1<argc)storage=argv[++i];
            else if(arg=="--max-dispatches" && i+1<argc){dispatch_budget=std::stoull(argv[++i]);dispatch_budget_given=true;if(!dispatch_budget)throw std::runtime_error("Dispatch budget must be positive");}
            else if(arg=="--max-runtime-ms" && i+1<argc){const auto value=std::stoull(argv[++i]);if(!value || value>1800000)throw std::runtime_error("Diagnostic runtime must be between 1 and 1800000 milliseconds");runtime_limit_ms=static_cast<unsigned>(value);}
            else if(arg=="--capture-frame" && i+1<argc)capture=argv[++i];
            else if(arg=="--capture-interval-ms" && i+1<argc){const auto value=std::stoull(argv[++i]);if(value<50 || value>1800000)throw std::runtime_error("Capture interval must be between 50 and 1800000 milliseconds");capture_interval_ms=static_cast<unsigned>(value);}
            else if(arg=="--hold-on-stop" && i+1<argc){hold_ms=std::stoul(argv[++i]);if(hold_ms>300000u)throw std::runtime_error("Diagnostic hold exceeds five minutes");}
            else if(arg=="--stop-on-result")stop_on_result_ms=5000;
            else if(arg.rfind("--stop-on-result=",0)==0){const auto value=std::stoul(arg.substr(17));if(value>600000u)throw std::runtime_error("--stop-on-result accepts 0-600000 milliseconds");stop_on_result_ms=static_cast<unsigned>(value);}
            else throw std::runtime_error("Usage: kon_boot_probe [--root path] [--language pack-id] [--bootstrap] [--window] [--userdata path] [--capture-frame path] [--capture-interval-ms milliseconds] [--hold-on-stop milliseconds] [--stop-on-result[=milliseconds]] [--max-dispatches count] [--max-runtime-ms milliseconds] [--stop-after-visible-frame] [--trace-render] [--trace-hle] [--persistent-texture-cache] [--raster-threads 1-16] [--show-test-window] [--verify-render-frames 1-4] [--verify-render-after-ms milliseconds] [--gpu-adapter auto/amd/nvidia/integrated/discrete/index/name] [--vulkan-clear] [--vulkan-triangles] [--vulkan-raster] [--vulkan-batch] [--vulkan-resident-batch] [--vulkan] [--profile-render path] [--profile-frames path] [--capture-small-quad path] [--capture-3d-draw path] [--capture-3d-after-ms milliseconds] [--capture-3d-min-vertices count] [--input-replay path] [--audio-anchored-input-replay path] [--input-anchor-channel 0-7]");
        }
        if(argc==1){
            // Double-click: an installation (tools/make_install.py) keeps its data
            // next to the executable and uses the language saved in Settings; the
            // development executable opens from its build folder in Japanese.
            const auto here=std::filesystem::absolute(argv[0]).parent_path();
            if(std::filesystem::is_regular_file(here/"analysis/executables/pkon.elf")){root=here;}
            // The game built by the launcher (src/launcher.cpp) lives in <installation>/bin.
            else if(std::filesystem::is_regular_file(here.parent_path()/"analysis/executables/pkon.elf")){root=here.parent_path();}
            else{root=here.parent_path().parent_path();language="ja";language_given=true;}
            bootstrap=true;window=true;hold_ms=300000;
        }
        // Without --language, the language chosen in Settings (config/settings.ini).
        if(!language_given){
            const auto file=settings_file.empty()?root/"config"/"settings.ini":settings_file;
            if(const auto saved=kon::read_port_setting(file,"language");saved && (*saved=="en" || *saved=="ja"))language=*saved;
        }
        // Playing (a normal window): no dispatch limit. The 100 million default
        // is a diagnostic bound; a long session at 120/165 FPS passed it and
        // the game stopped with "Dispatch limit reached".
        if(window && !show_test_window && !dispatch_budget_given)dispatch_budget=~std::uint64_t{0};
        kon::ContentMount content(root,language);
        pc_trace::install();
        if(!storage.empty() && storage.is_relative())storage=content.root()/storage;
        std::ofstream interactive_log;DiagnosticRedirect redirect;
        if(window && !show_test_window){
            const auto path=content.root()/"analysis"/("native_interactive_"+language+".log");
            // The previous session's log is kept (a freeze is often reported after restarting).
            {std::error_code ignored;std::filesystem::rename(path,content.root()/"analysis"/("native_interactive_"+language+"_anterior.log"),ignored);}
            interactive_log.open(path,std::ios::binary);if(!interactive_log)throw std::runtime_error("Cannot write native interactive diagnostic log");
            redirect.previous=std::cout.rdbuf(interactive_log.rdbuf());
            if(capture.empty())capture=content.root()/"analysis"/("native_interactive_"+language+".bmp");
            std::cerr<<"Port en desarrollo. Registro: "<<path.string()<<"\n";
        }
        // Resource mods (mods/<name>/disc and mods/<name>/textures, see native_mods.hpp).
        {
            const auto mods=kon::list_mods(content.root());std::vector<std::filesystem::path> disc;
            for(const auto& mod:mods){
                std::cout<<"mod="<<mod.name<<" enabled="<<mod.enabled<<'\n';
                if(mod.enabled && std::filesystem::is_directory(mod.path/"disc"))disc.push_back(mod.path/"disc");
            }
            content.set_disc_overrides(disc);
            const auto textures=kon::TextureMods::instance().load(mods,&kon::decode_image_file);
            if(!mods.empty())std::cout<<"mods="<<mods.size()<<" disc_folders="<<disc.size()<<" texture_replacements="<<textures<<'\n';
        }
        // Button remapping (config/controls.ini); a commented template is written when missing.
        {
            const auto file=content.root()/"config"/"controls.ini";std::error_code error;
            if(!std::filesystem::exists(file,error)){std::filesystem::create_directories(file.parent_path(),error);std::ofstream(file)<<kon::ButtonMap::template_text();}
            for(const auto& problem:kon::ButtonMap::instance().load(file))std::cout<<"controls_ini_problem="<<problem<<'\n';
        }
        // Synthetic input (diagnostics): a normal player window consumes it from
        // the pad source exactly like the headless bootstrap test, so a window no
        // longer rejects it. The audio-anchored chart still needs the menu replay
        // that reaches the song. Without these options the game path is unchanged.
        if(!replay.empty() && !window && !bootstrap)throw std::runtime_error("Controller replay requires a running guest (--window or --bootstrap)");
        if(!audio_replay.empty() && replay.empty())throw std::runtime_error("Audio-anchored input requires a menu replay (--input-replay)");
        if(!replay.empty() && window && !show_test_window)std::cout<<"synthetic_input_window=1 (test replay; hardware input ignored)\n";
        if(capture_interval_ms && capture.empty())throw std::runtime_error("Periodic captures require --capture-frame path");
        const auto executable=content.root()/"analysis/executables/pkon.elf";
        if(psprecomp::sha256_file(executable)!=kElfHash) throw std::runtime_error("Wrong ELF: rebuild generated code for this input");
        psprecomp::Runtime runtime;
        const auto image=psprecomp::Elf32Image::from_file(executable);
        const auto reloc=image.load_and_relocate(runtime.memory());
        if(reloc.invalid || reloc.unsupported) throw std::runtime_error("Unresolved ELF relocations");
        const auto module=image.find_module_info(runtime.memory());
        if(!module || module->name!="pkon") throw std::runtime_error("Wrong module-info");
        const auto imports=image.scan_imports(runtime.memory(),*module);
        if(imports.size()!=238) throw std::runtime_error("Wrong import table");
#ifdef __ANDROID__
        if(!kon_android_game_register)throw std::runtime_error("Android game module is not loaded");
        kon_android_game_register(runtime);
#else
        psprecomp::register_generated_functions(runtime);
#endif
        runtime.register_function(kReturnPc,guest_return,"bootstrap return sentinel");
        std::uint32_t loaded_end=0;
        for(std::size_t n=0;n<image.segments().size();n++) {
            const auto& segment=image.segments()[n];if(segment.type!=1u)continue;
            loaded_end=std::max(loaded_end,image.segment_runtime_address(n)+segment.memory_size);
        }
        const auto arena_begin=(loaded_end+0xFFFu)&~0xFFFu;
        // Keep GPU ownership outside services; its lifetime spans every draw.
        // Normal use needs no renderer options: the fastest verified path is the
        // resident Vulkan batch on the automatically selected GPU, with CPU
        // rendering as the fallback when no capable Vulkan device exists.
        const bool explicit_renderer=cpu_renderer || vulkan_clear || vulkan_triangles || vulkan_raster;
        if(!raster_threads_given)raster_threads=std::clamp(std::thread::hardware_concurrency(),1u,16u);
#ifdef KON_HAS_NATIVE_VULKAN
        if(!explicit_renderer){
            try{kon::NativeVulkanClear probe(false,true,gpu_adapter);if(probe.raster_supported()){vulkan_raster=vulkan_batch=resident_gpu_ownership=true;}}
            catch(const std::exception& error){std::cout<<"native_renderer_fallback=cpu reason="<<error.what()<<'\n';}
        }
        std::cout<<"native_renderer="<<(vulkan_raster?(resident_gpu_ownership?"vulkan_resident":"vulkan"):"cpu")<<" raster_threads="<<raster_threads<<'\n';
        std::unique_ptr<kon::NativeVulkanClear> gpu_clear,frame_verification_gpu;
        if(verify_frames)frame_verification_gpu=std::make_unique<kon::NativeVulkanClear>(false,true,gpu_adapter);
        if(vulkan_clear || vulkan_triangles || vulkan_raster){gpu_clear=std::make_unique<kon::NativeVulkanClear>(false,true,gpu_adapter);kon::NativeWindow::preferred_adapter()=gpu_clear->adapter();std::cout<<"native_vulkan_clear_adapter="<<gpu_clear->adapter()<<" (experimental; optional native clear, flat and textured raster passes)\n";std::cout<<"native_vulkan_staging_memory_flags="<<gpu_clear->memory_flags()<<'\n';}
#else
        if(vulkan_clear || vulkan_triangles || vulkan_raster)throw std::runtime_error("This build does not include the native Vulkan clear backend");
#endif
        BootstrapServices services(runtime,module->gp,arena_begin,image,content,window,storage,persistent_texture_cache,raster_threads);
#ifdef KON_HAS_NATIVE_VULKAN
        if(verify_frames){services.enable_frame_verification(verify_frames,frame_verification_gpu.get());services.verify_render_after_us=std::uint64_t(verify_after_ms)*1000;services.frame_dump=frame_dump;}
        services.profile_ge_only=sample_ge_only;services.logic_vblank_cycles=logic_cycles;services.port_settings_path=settings_file;
        // Audio output buffering (settings.ini audio_block_ms; KON_AUDIO_BLOCK_FRAMES wins).
        if(!std::getenv("KON_AUDIO_BLOCK_FRAMES"))if(const auto saved=kon::read_port_setting(services.port_settings_file(),"audio_block_ms")){
            try{kon::NativeAudio::block_frames()=kon::NativeAudio::clamp_block(unsigned(std::stod(*saved)*44.1));}catch(const std::exception&){}}
        services.running_language_=content.language();if(services.settings_fps_){services.settings_fps_->set_language(content.language());
            // One disc installed: the language column stays on it.
            const auto langs=content.available_languages();services.settings_fps_->set_language_choice(std::count(langs.begin(),langs.end(),"ja") && std::count(langs.begin(),langs.end(),"en"));}
        if(!logic_fps){const auto saved=kon::read_port_setting(services.port_settings_file(),"logic_fps");try{logic_fps=saved?std::stoul(*saved):30u;}catch(const std::exception&){logic_fps=30u;}if(!kon::NativeLogicRate::supported(logic_fps))logic_fps=30u;}
        // Screen mode: the window opens full screen when that was saved (played windows only).
        // FPS counter (F3): saved as show_fps in config/settings.ini.
        if(window && !show_test_window){const auto file=services.port_settings_file();const auto saved=kon::read_port_setting(file,"show_fps");
            kon::NativeWindow::show_fps()=!(saved && *saved=="0");
            kon::NativeWindow::fps_toggled()=[file](bool on){kon::write_port_setting(file,"show_fps",on?"1":"0");};}
        if(window && !show_test_window){const auto saved=kon::read_port_setting(services.port_settings_file(),"fullscreen");const bool full=saved && *saved=="1";
            services.init_screen_mode(full);}
        else if(test_fullscreen)services.init_screen_mode(true); // visible tests at the screen's real resolution
        services.logic_rate.set_target(logic_fps);if(services.settings_fps_)services.settings_fps_->set_rate(logic_fps);services.logic_rate.set_fixed_step(logic_fixed_step);
        services.ram_snapshot_dir=ram_dir;services.ram_snapshot_start_us=ram_start_ms*1000u;services.ram_snapshot_interval_us=ram_interval_ms*1000u;services.ram_snapshot_count=ram_dir.empty()?0u:ram_count;services.capture_begin_us=capture_begin_ms*1000u;if(speed_audit_ms){services.speed_audit_start_us=speed_audit_start_ms*1000u;services.speed_audit_end_us=(speed_audit_start_ms+speed_audit_ms)*1000u;services.logic_rate.set_jitter(true);services.logic_rate.set_fixed_step(true);}services.ram_variation_file=ram_variation_file;services.ram_variation_start_us=ram_variation_start_ms*1000u;services.ram_variation_end_us=(ram_variation_start_ms+ram_variation_ms)*1000u;
        if(!trace_frames.empty()){services.trace_begin_us=trace_begin_ms*1000;services.trace_end_us=trace_end_ms*1000;}
        if(vulkan_raster){if(!gpu_clear->raster_supported())throw std::runtime_error("Vulkan raster requires shaderInt64");services.set_gpu_raster(gpu_clear.get());
            // Internal resolution: --internal-scale, else settings.ini internal_scale (1 = original).
            if(!internal_scale){const auto saved=kon::read_port_setting(services.port_settings_file(),"internal_scale");try{internal_scale=saved?std::stoul(*saved):1u;}catch(const std::exception&){internal_scale=1u;}}
            if(internal_scale<1 || internal_scale>4 || !gpu_clear->set_internal_scale(internal_scale))internal_scale=1;
            services.set_sync_show(internal_scale>1 && std::getenv("KON_SYNC_HIRES_SHOW"));
            // Settings offers the multiples of 272 lines up to the screen's height (nearest).
            if(services.settings_fps_ && gpu_clear->set_internal_scale(internal_scale)){
                DEVMODEW mode{};mode.dmSize=sizeof(mode);unsigned limit=4;
                if(EnumDisplaySettingsW(nullptr,ENUM_CURRENT_SETTINGS,&mode) && mode.dmPelsHeight)limit=std::clamp((unsigned(mode.dmPelsHeight)+136u)/272u,1u,4u);
                services.settings_fps_->set_scale_limit(std::max(limit,internal_scale));services.settings_fps_->set_scale(internal_scale);}
            std::cout<<"internal_scale="<<internal_scale<<'\n';}
        if(vulkan_clear)services.set_gpu_clear(gpu_clear.get());
        if(vulkan_triangles)services.set_gpu_triangles(gpu_clear.get());
#endif
        std::cout<<"persistent_texture_cache="<<persistent_texture_cache<<" (experimental; whole-game speed benefit unverified)\n";
        if(window && !show_test_window)services.calls.stream=&interactive_log;
        else if(!replay.empty())services.calls.stream=&std::cout;
        services.calls.trace_all=trace_hle;
        services.gpu_batch=vulkan_batch;services.resident_gpu_ownership=resident_gpu_ownership;
        if(window && !show_test_window)services.freeze_report_path=root/"ultimo_fallo.txt";
        // Online room: the F8 window; KON_ONLINE_HOST=1 / KON_ONLINE_JOIN=<code> do the same at start (tests).
        if(window)services.attach_online();
        if(std::getenv("KON_ONLINE_HOST"))services.online_action(kon::NativeWindow::OnlineAction::Host,{});
        if(const char* code=std::getenv("KON_ONLINE_JOIN"))services.online_action(kon::NativeWindow::OnlineAction::Join,BootstrapServices::wide(code));
        // Discord Rich Presence (native_discord.hpp), only while playing: the
        // port's own Discord application is built in, so players configure
        // nothing; discord=0 in config/settings.ini turns it off, and
        // discord_client_id / discord_large_image replace the built-in ones.
        std::unique_ptr<kon::DiscordPresence> discord;
        {constexpr const char* kDiscordApplication="1558336943383904258"; // the port's Discord Application ID (public, not a secret)
         constexpr const char* kDiscordImage="logo";    // art asset uploaded to that application
         const auto file=services.port_settings_file();
         const auto setting=kon::read_port_setting(file,"discord");const auto custom=kon::read_port_setting(file,"discord_client_id");
         const std::string id=custom && !custom->empty()?*custom:kDiscordApplication;
         const bool enabled=!id.empty() && !(setting && (*setting=="0" || *setting=="off")) && ((window && !show_test_window) || std::getenv("KON_DISCORD_TEST"));
         if(enabled){const auto image=kon::read_port_setting(file,"discord_large_image");
             discord=std::make_unique<kon::DiscordPresence>(id,image?*image:(custom?std::string{}:std::string(kDiscordImage)));
             services.activity_.on_change=[d=discord.get(),&services,t0=std::chrono::steady_clock::now()](const kon::Presence& p){d->update(p);services.calls.push_back("DiscordPresence at_ms="+std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-t0).count())+" details="+p.details+" state="+p.state);};services.activity_.start();}
         std::cout<<"discord_presence="<<(enabled?"on":id.empty()?"off (no Discord application built in)":"off")<<'\n';}
        if(resident_gpu_ownership)services.enable_resident_gpu_ownership();
        services.stop_after_visible_frame=visible_checkpoint;
        services.trace_render=trace_render;if(!draw_dump.empty()){services.draw_dump_=std::make_unique<std::ofstream>(draw_dump);services.draw_dump_begin_us_=draw_dump_begin_ms*1000u;services.draw_dump_end_us_=draw_dump_end_ms*1000u;}services.profile_render=!render_profile.empty();services.profile_frames=!frame_profile.empty();
        services.quad_draw_capture=quad_draw_capture;services.projected_draw_capture=projected_draw_capture;services.projected_capture_min_vertices=projected_min_vertices;services.projected_capture_after_us=static_cast<std::uint64_t>(projected_after_ms)*1000u;
        services.runtime_limit_us=static_cast<std::uint64_t>(runtime_limit_ms)*1000u;
        services.stop_on_result_us=static_cast<std::uint64_t>(stop_on_result_ms)*1000u;
        if(stop_on_result_ms)services.activity_.on_results=[&services]{services.result_screen_loaded();};
        services.progress_capture=capture;services.capture_interval_us=static_cast<std::uint64_t>(capture_interval_ms)*1000u;
        if(!replay.empty()){services.control_replay.load(replay);std::cout<<"synthetic_controller_replay="<<replay.string()<<" (hardware input unverified)\n";}
        if(!audio_replay.empty()){services.audio_anchored_replay.load(audio_replay);services.input_anchor_channel=anchor_channel;std::cout<<"synthetic_audio_anchored_replay="<<audio_replay.string()<<" channel="<<anchor_channel<<" (timing hypotheses, hardware input unverified)\n";}
        active_services=&services;if(std::getenv("PSPRECOMP_WATCH_WRITE"))psprecomp::set_write_watch_observer(watch_write_stack);std::unique_ptr<kon::NativeSamplingProfiler> profiler;
        if(bootstrap) {
            services.install();
            if(!sample_profile.empty()){const auto origin=std::chrono::steady_clock::now();
                profiler=std::make_unique<kon::NativeSamplingProfiler>(sample_render_thread?services.render_thread_id():kon_current_thread_id(),[origin,sample_begin_ms,sample_end_ms,&services]{const auto ms=std::uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-origin).count());return ms>=sample_begin_ms && ms<sample_end_ms && (!services.profile_ge_only || services.in_ge.load(std::memory_order_relaxed));});}
            psprecomp::set_runtime_post_import_hook([](auto&,auto& c){if(active_services)active_services->import_boundary(c);});
            psprecomp::set_runtime_starvation_hook([](auto&,auto& c){if(active_services)active_services->dispatch_boundary(c);},1024);
        }
        auto& cpu=runtime.cpu();kon::reset_guest_context(cpu);cpu.gpr[28]=module->gp;cpu.gpr[29]=0x09FFF000u;cpu.gpr[31]=kReturnPc;
        const std::string program="disc0:/PSP_GAME/SYSDIR/EBOOT.BIN";
        const auto argptr=cpu.gpr[29]-0x100u;
        for(std::size_t n=0;n<=program.size();n++) runtime.memory().store8(argptr+static_cast<std::uint32_t>(n),n==program.size()?0u:static_cast<std::uint8_t>(program[n]));
        cpu.gpr[4]=static_cast<std::uint32_t>(program.size()+1);cpu.gpr[5]=argptr;
        std::cout<<"Native AOT bootstrap diagnostic; playable_port=false\n"
                 <<"language="<<content.language()<<"\nstd="<<content.resolve("disc0:/PSP_GAME/USRDIR/std.cpk").string()<<"\n"
                 <<"sound="<<content.resolve("disc0:/PSP_GAME/USRDIR/snd.cpk").string()<<"\n"
                 <<"ELF_SHA256="<<kElfHash<<"\nentry="<<psprecomp::hex32(image.runtime_entry())<<"\nimports="<<imports.size()<<"\n";
        psprecomp::set_runtime_thread_identity(1,"module_start");
        const auto report_missing_import=[&] {
            const auto missing=runtime.stop_reason().starts_with("Missing HLE import ");
            for(const auto& import:imports) if(import.stub_address==cpu.pc) {
                const auto label=missing?"missing_":"stopped_";
                std::cout<<label<<"library="<<import.library<<"\n"<<label<<"nid="<<psprecomp::hex32(import.nid)<<"\n";
                for(unsigned n=4;n<12;n++) std::cout<<"arg_r"<<n<<"="<<psprecomp::hex32(cpu.gpr[n])<<"\n";
                if(runtime.memory().contains(cpu.gpr[4],64)) {
                    std::cout<<"arg0_words=";
                    for(unsigned n=0;n<16;n++)std::cout<<psprecomp::hex32(runtime.memory().load32(cpu.gpr[4]+n*4u))<<(n==15?'\n':' ');
                }
                std::cout<<"guest_return_address="<<psprecomp::hex32(cpu.gpr[31])<<"\n";break;
            }
        };
        const auto run_with_failure_log=[&](std::uint32_t entry,std::size_t first_call) {
            try {runtime.run(entry,dispatch_budget);}
            catch(...) {
                for(std::size_t n=first_call;n<services.calls.size();n++)std::cout<<"HLE: "<<services.calls[n]<<"\n";
                std::cout.flush();throw;
            }
        };
        run_with_failure_log(image.runtime_entry(),0);report_missing_import();
        for(const auto& call:services.calls) std::cout<<"HLE: "<<call<<"\n";
        const auto bootstrap_call_count=services.calls.size();
        std::cout<<"module_stop="<<runtime.stop_reason()<<"\nPC="<<psprecomp::hex32(cpu.pc)<<"\n";
        if(bootstrap && services.module_returned) {
            std::cout<<"module_return_value="<<services.module_status<<"\n";
            for(auto& [uid,thread]:services.threads) {
                if(!thread.ready) continue;
                std::cout<<"Running native worker uid="<<uid<<" name="<<thread.name<<"\n";
                services.mark_running(uid);cpu=thread.context;psprecomp::set_runtime_thread_identity(static_cast<std::int32_t>(uid),thread.name);
                run_with_failure_log(thread.entry,bootstrap_call_count);report_missing_import();
                for(std::size_t n=bootstrap_call_count;n<services.calls.size();n++) std::cout<<"HLE: "<<services.calls[n]<<"\n";
                std::cout<<"worker_stop="<<runtime.stop_reason()<<"\nworker_PC="<<psprecomp::hex32(cpu.pc)<<"\n";
                // Played from the launcher: the console closes with the window,
                // so any stop other than closing it leaves a report next to the game.
                if(window && !show_test_window && runtime.stop_reason()!="Native game window closed" && runtime.stop_reason()!="Result screen reached" && runtime.stop_reason()!=BootstrapServices::kRestartReason){
                    std::ofstream report(root/"ultimo_fallo.txt");
                    const auto now=std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());char when[32]{};std::strftime(when,sizeof(when),"%Y-%m-%d %H:%M:%S",std::localtime(&now));
                    report<<"fecha="<<when<<"\nmotivo="<<runtime.stop_reason()<<"\nPC="<<psprecomp::hex32(cpu.pc)<<"\nRA="<<psprecomp::hex32(cpu.gpr[31])<<"\nhilo="<<psprecomp::runtime_thread_uid()<<"\n";
                    for(const auto& import:imports)if(import.stub_address==cpu.pc)report<<"importacion="<<import.library<<" nid="<<psprecomp::hex32(import.nid)<<"\n";
                    for(unsigned n=4;n<12;n++)report<<"r"<<n<<"="<<psprecomp::hex32(cpu.gpr[n])<<"\n";
                    for(const auto& call:services.calls.recent(150))report<<"HLE: "<<call<<"\n";
                    std::cout<<"failure_report="<<(root/"ultimo_fallo.txt").string()<<"\n";
                }
                break;
            }
        }
        // Exit 2 signals expected incomplete-port stop; never a game-success exit.
        if(vulkan_batch)services.complete_gpu_batch();
        services.inspect_display(capture,services.restart_requested_?0u:hold_ms);
        if(!trace_frames.empty()){std::ofstream out(trace_frames);for(const auto& e:services.frame_trace)out<<e[0]<<' '<<e[1]<<' '<<e[2]<<"\n";}
        if(profiler){profiler->write(sample_profile);std::cout<<"sample_profile="<<sample_profile.string()<<'\n';}
        if(verify_frames)std::cout<<"native_private_frame_replay_verified="<<services.verified_render_frames<<" rejected="<<services.rejected_render_frames<<"\n";
        if(!render_profile.empty())services.write_render_profile(render_profile);
        if(!frame_profile.empty())services.write_frame_profile(frame_profile);
#ifdef KON_HAS_NATIVE_VULKAN
        if(gpu_clear)std::cout<<"native_vulkan_clear_submissions="<<(gpu_clear->submissions()-gpu_clear->triangle_submissions()-gpu_clear->raster_submissions())<<"\nnative_vulkan_triangle_submissions="<<gpu_clear->triangle_submissions()<<"\nnative_vulkan_raster_submissions="<<gpu_clear->raster_submissions()<<"\n";
#endif
        psprecomp::report_counted_pcs();
        pc_trace::report();
        // Language changed in Settings: the same command starts again, without
        // --language so the saved choice applies (test windows only log it,
        // unless KON_TEST_RESTART=1 checks the restart itself).
#ifdef _WIN32
        if(services.restart_requested_ && window && (!show_test_window || std::getenv("KON_TEST_RESTART"))){
            std::wstring command;wchar_t module[MAX_PATH]{};GetModuleFileNameW(nullptr,module,MAX_PATH);
            const auto quote=[](const std::wstring& s){return L"\""+s+L"\"";};
            command=quote(module);
            int wide_count=0;LPWSTR* wide=CommandLineToArgvW(GetCommandLineW(),&wide_count);
            for(int i=1;wide && i<wide_count;++i){
                if(std::wstring_view(wide[i])==L"--language" && i+1<wide_count){++i;continue;}
                command+=L" "+quote(wide[i]);
            }
            if(wide)LocalFree(wide);
            std::cout<<std::flush;
            STARTUPINFOW startup{};startup.cb=sizeof(startup);PROCESS_INFORMATION process{};
            if(CreateProcessW(nullptr,command.data(),nullptr,nullptr,FALSE,0,nullptr,nullptr,&startup,&process)){CloseHandle(process.hThread);CloseHandle(process.hProcess);}
            else std::cerr<<"Language restart failed: "<<GetLastError()<<"\n";
        }
#else
        // Android: the Activity restarts the game with the saved language when
        // the native thread returns this code.
        if(services.restart_requested_)std::cout<<"language_restart_requested\n";
#endif
        return 2;
    } catch(const std::exception& error) {
        std::cerr<<"Diagnostic failed: "<<error.what()<<"\n";
        // Played from the launcher (which runs in the game folder): keep the reason.
        if(std::any_of(argv+1,argv+argc,[](const char* a){return std::string_view(a)=="--window";})){
            std::ofstream report(std::filesystem::current_path()/"ultimo_fallo.txt");
            const auto now=std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());char when[32]{};std::strftime(when,sizeof(when),"%Y-%m-%d %H:%M:%S",std::localtime(&now));
            report<<"fecha="<<when<<"\nerror="<<error.what()<<"\n";
        }
        return 1;
    }
}
#endif
