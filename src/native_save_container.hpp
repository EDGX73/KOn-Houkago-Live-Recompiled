#pragma once
#include "native_files.hpp"
#include <array>
#include <chrono>
#include <fstream>
#include <span>
#include <vector>
namespace kon {
// Port-owned persistence of the game's plaintext state. This is deliberately a
// versioned PC container, not a fabricated encrypted PSP SYSTEM.DAT/PARAM.SFO.
struct NativeSaveContainer {
    std::string game,slot,file;
    std::array<std::uint8_t,1284> metadata{};
    std::array<std::uint8_t,16> key{};
    std::uint32_t secure_version=0;
    std::array<std::vector<std::uint8_t>,5> sections; // game state, ICON0/1, PIC1, SND0
    static constexpr unsigned max_section=16u*1024u*1024u,max_file=5u*max_section+2048u;
    static std::uint32_t checksum(std::span<const std::uint8_t> bytes){
        std::uint32_t crc=0xFFFFFFFFu;
        for(auto byte:bytes){crc^=byte;for(unsigned bit=0;bit<8;++bit)crc=(crc>>1u)^(crc&1u?0xEDB88320u:0u);}
        return crc^0xFFFFFFFFu;
    }
    std::vector<std::uint8_t> encode()const{
        std::vector<std::uint8_t> body;
        const auto word=[&](unsigned value){for(unsigned n=0;n<4;++n)body.push_back(static_cast<std::uint8_t>(value>>(n*8u)));};
        const auto name=[&](const std::string& value,unsigned limit){if(value.empty() || value.size()>=limit)throw std::runtime_error("Invalid PC save identity");for(unsigned n=0;n<limit;++n)body.push_back(n<value.size()?value[n]:0);};
        name(game,13);name(slot,20);name(file,13);
        for(const auto& section:sections){if(section.size()>max_section)throw std::runtime_error("PC save section exceeds bounds");word(static_cast<unsigned>(section.size()));}
        body.insert(body.end(),metadata.begin(),metadata.end());body.insert(body.end(),key.begin(),key.end());word(secure_version);
        for(const auto& section:sections)body.insert(body.end(),section.begin(),section.end());
        std::vector<std::uint8_t> result{'K','O','N','P','C','S','V','1'};
        const auto header_word=[&](unsigned value){for(unsigned n=0;n<4;++n)result.push_back(static_cast<std::uint8_t>(value>>(n*8u)));};
        header_word(1);header_word(static_cast<unsigned>(body.size()));header_word(checksum(body));result.insert(result.end(),body.begin(),body.end());return result;
    }
    static NativeSaveContainer decode(std::span<const std::uint8_t> bytes){
        unsigned cursor=0;
        const auto take=[&](unsigned count){if(count>bytes.size()-cursor)throw std::runtime_error("Truncated PC save container");auto part=bytes.subspan(cursor,count);cursor+=count;return part;};
        const auto word=[&](){const auto part=take(4);return std::uint32_t(part[0])|(std::uint32_t(part[1])<<8u)|(std::uint32_t(part[2])<<16u)|(std::uint32_t(part[3])<<24u);};
        const auto magic=take(8);if(std::string(magic.begin(),magic.end())!="KONPCSV1" || word()!=1)throw std::runtime_error("Unknown PC save version");
        const auto body_size=word(),crc=word();if(body_size!=bytes.size()-cursor || bytes.size()>max_file || checksum(bytes.subspan(cursor))!=crc)throw std::runtime_error("PC save length/checksum invalid");
        NativeSaveContainer out;
        const auto name=[&](unsigned limit){const auto part=take(limit);const auto end=std::find(part.begin(),part.end(),0);if(end==part.begin() || end==part.end())throw std::runtime_error("Invalid PC save identity");return std::string(part.begin(),end);};
        out.game=name(13);out.slot=name(20);out.file=name(13);std::array<unsigned,5> sizes{};for(auto& size:sizes){size=word();if(size>max_section)throw std::runtime_error("PC save section exceeds bounds");}
        const auto metadata=take(out.metadata.size()),key=take(out.key.size());std::copy(metadata.begin(),metadata.end(),out.metadata.begin());std::copy(key.begin(),key.end(),out.key.begin());out.secure_version=word();
        for(unsigned n=0;n<5;++n){const auto part=take(sizes[n]);out.sections[n].assign(part.begin(),part.end());}
        if(cursor!=bytes.size())throw std::runtime_error("Unexpected trailing PC save bytes");return out;
    }
    static NativeSaveContainer read(const std::filesystem::path& path){
        std::ifstream input(path,std::ios::binary|std::ios::ate);if(!input)throw std::runtime_error("Cannot open native save");
        const auto size=input.tellg();if(size<0 || size>max_file)throw std::runtime_error("Invalid native save size");
        std::vector<std::uint8_t> data(static_cast<std::size_t>(size));input.seekg(0);if(!input.read(reinterpret_cast<char*>(data.data()),data.size()))throw std::runtime_error("Cannot read complete native save");return decode(data);
    }
    void write(const std::filesystem::path& path,bool overwrite)const{
        // Validate the previous container before replacement; never overwrite an
        // unknown file or discard a damaged save to hide a loading error.
        if(std::filesystem::exists(path)){const auto previous=read(path);if(previous.game!=game || previous.slot!=slot || previous.file!=file)throw std::runtime_error("PC save identity mismatch");if(!overwrite)throw std::runtime_error("Native save overwrite was not requested");}
        const auto data=encode();std::filesystem::create_directories(path.parent_path());
        const auto milliseconds=std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
        auto temporary=path;temporary+=".pending."+std::to_string(platform::current_process_id())+"."+std::to_string(milliseconds);
        const auto written=platform::write_new_file_durably(temporary,data);
        if(written==platform::DurableWrite::CannotCreate)throw std::runtime_error("Cannot create native save transaction");
        if(written!=platform::DurableWrite::Ok)throw std::runtime_error("Native save transaction did not reach storage");
        if(!platform::move_file_durably(temporary,path,overwrite))throw std::runtime_error("Cannot commit native save transaction");
    }
};
}
