#pragma once
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

// Port settings file (config/settings.ini): one `key=value` per line.  Writing
// a key keeps every other line, so separate features can share the file.
namespace kon {
inline std::optional<std::string> read_port_setting(const std::filesystem::path& file,const std::string& key){
    std::ifstream input(file);std::string line;
    while(std::getline(input,line)){
        if(!line.empty() && line.back()=='\r')line.pop_back();
        if(line.size()>key.size() && line.compare(0,key.size(),key)==0 && line[key.size()]=='=')return line.substr(key.size()+1);
    }
    return std::nullopt;
}
inline bool write_port_setting(const std::filesystem::path& file,const std::string& key,const std::string& value){
    std::vector<std::string> lines;bool replaced=false;
    {std::ifstream input(file);std::string line;
     while(std::getline(input,line)){
        if(!line.empty() && line.back()=='\r')line.pop_back();
        if(line.size()>key.size() && line.compare(0,key.size(),key)==0 && line[key.size()]=='='){line=key+"="+value;replaced=true;}
        lines.push_back(line);}}
    if(!replaced)lines.push_back(key+"="+value);
    std::error_code error;if(!file.parent_path().empty())std::filesystem::create_directories(file.parent_path(),error);
    const auto temporary=std::filesystem::path(file).concat(".tmp");
    {std::ofstream output(temporary,std::ios::trunc);if(!output)return false;for(const auto& l:lines)output<<l<<'\n';output.flush();if(!output)return false;}
    std::filesystem::rename(temporary,file,error);return !error;
}
}
