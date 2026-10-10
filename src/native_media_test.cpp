#include "native_media.hpp"
#include <algorithm>
#include <fstream>
#include <iostream>
#include <stdexcept>
void check(bool c,const char* message){if(!c)throw std::runtime_error(message);}
int main(int argc,char** argv){try{
    if(argc!=2)throw std::invalid_argument("Project root required");const std::filesystem::path root=argv[1];kon::NativeMedia media(root);
    check(media.load_module(0x300)==0 && media.load_module(0x300)==0x80111102u && media.load_module(0x302)==0,"Real codec modules did not load");
    check(media.reinit_atrac(0,3)==0 && media.atrac_capacity()==3,"Game ATRAC3+ pool was not allocated");
    check(media.reinit_atrac(6,3)==0x80000022u && media.atrac_capacity()==3,"ATRAC partial allocation/error semantics wrong");
    check(media.reinit_atrac(6,0)==0 && media.atrac_capacity()==6,"ATRAC3 capacity wrong");
    check(media.reinit_atrac(-1,-1)==0 && media.atrac_capacity()==0,"Negative ATRAC requests allocated contexts");
    check(media.reinit_atrac(0,0)==0 && media.atrac_capacity()==0,"ATRAC resource deinitialization failed");
    check(media.load_module(0x303)==0 && media.initialize_video()==0 && media.initialize_video()==0 && media.finish_video()==0,"Real native H264 decoder lifecycle failed");
    const auto sound=media.decode_audio_file(root/"work/audio_fixture/original.at3");
    check(sound.channels==2 && sound.sample_rate==44100 && sound.samples.size()>400000 && sound.samples.size()<600000,"Original audio duration/channel layout wrong");
    check(std::any_of(sound.samples.begin(),sound.samples.end(),[](auto n){return n!=0;}),"Original audio replaced with silence");
    std::ofstream output(root/"work/audio_fixture/native.s16le",std::ios::binary);output.write(reinterpret_cast<const char*>(sound.samples.data()),static_cast<std::streamsize>(sound.samples.size()*sizeof(std::int16_t)));check(static_cast<bool>(output),"Cannot preserve native audio evidence");
    check(media.unload_module(0x302)==0 && media.unload_module(0x302)==0x80111103u,"Module lifecycle lost");
    bool unsupported=false;try{media.load_module(0x301);}catch(const std::runtime_error&){unsupported=true;}check(unsupported,"Unimplemented sound synthesizer fabricated success");
    std::cout<<"PASS: real LGPL codec modules, original ATRAC3+ decoding, "<<sound.samples.size()/sound.channels<<" stereo frames at 44100 Hz; FFmpeg "<<media.version()<<"\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<"\n";return 1;}}
