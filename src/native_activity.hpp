#pragma once
#include <cctype>
#include <chrono>
#include <iterator>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
// What the player is doing, derived from the game files being loaded (std.cpk
// members, see cpk_index.hpp), for Discord Rich Presence:
//   teatime_* / tt_chr_*        the club room (tea time)
//   menu.bin                    menus (song, part and item selection)
//   play_chr_2d_<xx>.bin        the part chosen (yu, mi, ri, mu, az)
//   play_mNN_single.bin         song NN alone; play_mNN.bin: with the band (local play)
//   result.bin                  results of the song
//   mrg_pv_*                    the PV (music video) mode
namespace kon {
struct Presence {
    std::string details,state;   // first and second line
    std::int64_t start=0;        // Unix seconds the activity started (elapsed timer), 0 = none
    bool operator==(const Presence&)const=default;
};
class GameActivity {
public:
    static constexpr std::string_view kSongs[]{
        "Cagayake!GIRLS","Don't say \"lazy\"","Fuwa Fuwa Time","Watashi no Koi wa Hotchkiss","Fudepen ~Ballpen~",
        "Curry Nochi Rice","Let's Go","Happy!? Sorry!!","Sweet Bitter Beauty Song","Guitar ni Kubittake",
        "Sunday Siesta","Heart Goes Boom!!","Hello Little Girl","Jajauma Way To Go","Watashi wa Watashi no Michi wo Yuku",
        "Dear My Keys ~Kenban no Mahou~","Humming Bird","Girly Storm Shissou Stick","Mezase Happy 100%"};
    // Called on every change of presence.
    std::function<void(const Presence&)> on_change;
    // Called once per song when the results screen is loaded (result.bin), for
    // diagnostics that stop the runtime there (--stop-on-result).
    std::function<void()> on_results;
    void on_member(std::string name){
        for(auto& c:name)if(c=='\\')c='/';
        const auto slash=name.rfind('/');const std::string file=slash==std::string::npos?name:name.substr(slash+1);
        const auto starts=[&](std::string_view p){return file.compare(0,p.size(),p)==0;};
        if(starts("play_chr_2d_") && file.size()>=14){part_=part_name(file.substr(12,2));return;}
        if(starts("play_m") && file.size()>=8 && std::isdigit(static_cast<unsigned char>(file[6])) && std::isdigit(static_cast<unsigned char>(file[7]))){
            const int song=(file[6]-'0')*10+(file[7]-'0');if(song<0 || song>=int(std::size(kSongs)))return;
            if(file.find("_2")!=std::string::npos)return; // the song's second pack (title card), loaded alongside
            const bool band=file.find("_single")==std::string::npos;
            if(song!=song_ || mode_!=Mode::Song){song_=song;++played_;}
            mode_=Mode::Song;band_=band;since_=now();publish();return;
        }
        if(file=="result.bin"){
            if(mode_!=Mode::Results){
                const bool from_song=mode_==Mode::Song;
                mode_=Mode::Results;
                if(from_song)publish();
                if(on_results)on_results();
            }
            return;
        }
        if(file=="menu.bin"){if(mode_!=Mode::Menu){mode_=Mode::Menu;since_=now();publish();}return;}
        if(starts("teatime_") || starts("tt_chr_")){if(mode_!=Mode::Club){mode_=Mode::Club;since_=now();publish();}return;}
        if(starts("mrg_pv_")){if(mode_!=Mode::Pv){mode_=Mode::Pv;since_=now();publish();}return;}
    }
    void start(){if(mode_==Mode::None){since_=now();publish();}}
    Presence presence()const{
        Presence p;p.start=since_;
        const auto session=played_?" · "+std::to_string(played_)+(played_==1?" song":" songs")+" played":std::string{};
        switch(mode_){
        case Mode::Song:p.details="Playing "+std::string(kSongs[song_]);p.state=(band_?"With the band":"Solo")+(part_.empty()?std::string{}:" · "+part_);break;
        case Mode::Results:p.details="Results: "+std::string(kSongs[song_]);p.state=(part_.empty()?std::string("Song cleared"):part_)+session;break;
        case Mode::Menu:p.details="Choosing a song";p.state=played_?std::to_string(played_)+(played_==1?" song":" songs")+" played":"In the menus";break;
        case Mode::Club:p.details="In the light music club";p.state="Tea time"+session;break;
        case Mode::Pv:p.details="Watching a music video";p.state="PV mode";break;
        default:p.details="Starting the game";break;
        }
        return p;
    }
private:
    enum class Mode {None,Club,Menu,Song,Results,Pv};
    Mode mode_=Mode::None;int song_=0;bool band_=false;unsigned played_=0;std::string part_;std::int64_t since_=0;
    static std::int64_t now(){return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();}
    static std::string part_name(const std::string& code){
        if(code=="yu")return "Yui (lead guitar)";if(code=="mi")return "Mio (bass)";if(code=="ri")return "Ritsu (drums)";
        if(code=="mu")return "Mugi (keyboard)";if(code=="az")return "Azusa (rhythm guitar)";return {};
    }
    void publish(){if(on_change)on_change(presence());}
};
}
