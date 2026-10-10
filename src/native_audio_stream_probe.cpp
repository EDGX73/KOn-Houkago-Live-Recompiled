#include "native_audio.hpp"
#include "native_wait.hpp"
#include <chrono>
#include <fstream>
#include <iostream>
int main(int argc,char** argv){try{
    if(argc!=2)throw std::runtime_error("Usage: native_audio_stream_probe report.json");
    std::ofstream out(argv[1]);if(!out)throw std::runtime_error("Cannot create audio stream report");
    kon::NativeWait wait;out<<std::boolalpha<<"{\"backend\":\"continuous_native_mixer\",\"silent_device_probe\":true,\"audio_audition\":false,\"high_resolution_wait\":"<<wait.high_resolution()<<",\"cases\":[";
    bool first=true;
    for(unsigned frames:{256u,512u,1024u,2048u}){
        kon::NativeAudio audio;const auto channel=audio.reserve(-1,frames,0);if(channel!=0)throw std::runtime_error("Cannot reserve stream probe device");
        const std::vector<std::int16_t> silence(frames*2u);
        const auto start=std::chrono::steady_clock::now(),end=start+std::chrono::milliseconds(2500);
        std::chrono::steady_clock::time_point measured_start{};std::uint64_t played_start=0;
        while(std::chrono::steady_clock::now()<end){
            audio.poll();if(audio.diagnostics(channel).queued_buffers<2){const auto result=audio.output(channel,0x8000,0x8000,silence,true);if(result.value!=frames)throw std::runtime_error("Stream probe output rejected");}
            else wait.wait_for(500);
            if(measured_start.time_since_epoch().count()==0 && std::chrono::steady_clock::now()-start>=std::chrono::milliseconds(500)){measured_start=std::chrono::steady_clock::now();played_start=audio.diagnostics(channel).played_frames;}
        }
        const auto elapsed=std::chrono::duration<double>(std::chrono::steady_clock::now()-measured_start).count();const auto d=audio.diagnostics(channel);
        const auto played=d.played_frames-played_start;
        if(!first)out<<',';first=false;
        out<<"{\"buffer_frames\":"<<frames<<",\"elapsed_seconds\":"<<elapsed<<",\"warmup_milliseconds\":500,\"source_copied_frames\":"<<d.completed_frames<<",\"measurement_start_played_frame\":"<<played_start<<",\"submitted_frames\":"<<d.submitted_frames<<",\"driver_played_frames\":"<<played<<",\"played_frames_per_second\":"<<played/elapsed<<",\"expected_sample_rate\":44100,\"mixer_gap_count\":"<<d.gap_count<<",\"mixer_gap_microseconds\":"<<d.gap_microseconds<<",\"max_mixer_gap_microseconds\":"<<d.max_gap_microseconds<<"}";
        std::cout<<"Silent stream: buffer="<<frames<<" driver frames/sec="<<played/elapsed<<" expected=44100 mixer gaps="<<d.gap_count<<'\n';
    }
    out<<"]}\n";if(!out)throw std::runtime_error("Cannot save audio stream report");return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
