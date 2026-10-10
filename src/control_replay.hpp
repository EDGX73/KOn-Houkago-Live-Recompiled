#pragma once
#include "native_controls.hpp"
#include <fstream>
#include <filesystem>
#include <sstream>
#include <stdexcept>
namespace kon {
// Headless regression input only; never sends Windows keyboard/controller events.
class ControlReplay {
public:
    struct Event{std::uint64_t time;HostPad pad;};
    std::vector<Event> events;
    void load(const std::filesystem::path& path){
        std::ifstream input(path);if(!input)throw std::runtime_error("Cannot read controller replay");
        std::string line;std::uint64_t previous=0;
        while(std::getline(input,line)){
            if(line.empty() || line[0]=='#')continue;
            std::istringstream row(line);std::uint64_t time;unsigned buttons,x,y;std::string extra;
            if(!(row>>time>>buttons>>x>>y) || row>>extra || x>255 || y>255 || (!events.empty() && time<=previous) || events.size()>=10000)throw std::runtime_error("Invalid controller replay event");
            events.push_back({time,{buttons,static_cast<std::uint8_t>(x),static_cast<std::uint8_t>(y)}});previous=time;
        }
        if(events.empty())throw std::runtime_error("Empty controller replay");
    }
    HostPad at_relative(std::uint64_t time,std::uint64_t origin)const{
        return time<origin?HostPad{}:at(time-origin);
    }
    HostPad at(std::uint64_t time) const{
        auto found=std::upper_bound(events.begin(),events.end(),time,[](auto tick,const auto& event){return tick<event.time;});
        return found==events.begin()?HostPad{}:std::prev(found)->pad;
    }
};
}
