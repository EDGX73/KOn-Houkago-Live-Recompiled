#include "native_audio.hpp"
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <thread>
void check(bool ok,const char* message){if(!ok)throw std::runtime_error(message);}
int main(){try{
    {   // Blocking writers may run ahead until enough frames are queued after
        // the buffer they wait for (cooperative-scheduler headroom).
        kon::NativeAudioMixer mixer;mixer.reserve(0,448,0);mixer.min_queued_frames=1000;std::vector<std::int16_t> pcm(896,0);
        const auto a=mixer.submit(0,pcm,0),b=mixer.submit(0,pcm,0),c=mixer.submit(0,pcm,0),d=mixer.submit(0,pcm,0);
        check(!a && !b && !c && d==1,"Deep blocking queue waits for the wrong buffer");
        mixer.min_queued_frames=0;kon::NativeAudioMixer original;original.reserve(0,448,0);
        check(!original.submit(0,pcm,0) && original.submit(0,pcm,0)==1,"Original one-ahead blocking semantics changed");
    }
    _putenv_s("KON_AUDIO_QUEUE_MS","0"); // the device checks below use the original one-ahead semantics
    kon::NativeAudio audio;
    check(audio.reserve(8,448,0)==kon::NativeAudio::invalid_channel && audio.reserve(-1,450,0)==0x80260006u && audio.reserve(-1,448,1)==0x80260007u,"Invalid channel parameters accepted");
    const auto channel=audio.reserve(-1,448,0);check(channel==0 && audio.sample_count(channel)==448,"Actual Windows device did not reserve channel");
    check(audio.reserve(channel,448,0)==kon::NativeAudio::invalid_channel && audio.set_volume(channel,0x10000,0)==0x8026000Bu,"Reservation/volume errors lost");
    std::vector<std::int16_t> samples(896,0); // Validate the real driver silently.
    const auto first=audio.output(channel,0x8000,0x8000,samples,true),second=audio.output(channel,0x8000,0x8000,samples,true);
    check(first.value==448 && !first.wait_token && second.value==448 && second.wait_token,"Blocking output lost buffer ownership");
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(3);
    while(audio.has_pending() && std::chrono::steady_clock::now()<deadline){audio.poll();std::this_thread::sleep_for(std::chrono::milliseconds(1));}
    check(!audio.has_pending() && audio.completed(channel,second.wait_token) && audio.rest_frames(channel)==0,"Real driver did not consume PCM/completion tokens");
    const auto consumed=audio.diagnostics(channel);
    check(consumed.submitted_frames==896 && consumed.completed_frames==896 && consumed.played_frames==896 && !consumed.queued_buffers,"Diagnostic frame accounting disagrees with driver completion");
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    const auto after_gap=audio.output(channel,0x8000,0x8000,samples,true);
    const auto gaps=audio.diagnostics(channel);
    check(after_gap.value==448 && gaps.submitted_frames==1344 && gaps.completed_frames>=896 && gaps.completed_frames<=1344 && gaps.queued_buffers<=1 && gaps.gap_count>=1 && gaps.max_gap_microseconds>=20000,"Intentional empty-queue gap was not measured from a real completion");
    const auto gap_deadline=std::chrono::steady_clock::now()+std::chrono::seconds(3);
    while(audio.has_pending() && std::chrono::steady_clock::now()<gap_deadline){audio.poll();std::this_thread::sleep_for(std::chrono::milliseconds(1));}
    check(!audio.has_pending() && audio.diagnostics(channel).completed_frames==1344 && audio.diagnostics(channel).played_frames==1344,"Post-gap buffer was not completed by the driver");
    check(audio.release(channel)==0 && audio.release(channel)==kon::NativeAudio::not_reserved,"Audio lifecycle failed");
    std::cout<<"PASS: real Windows output device, owned PCM, real copy completions, actual device playback and empty-queue timing diagnostics, errors and channel lifecycle; silent driver check, no audition claim\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<"\n";return 1;}}
