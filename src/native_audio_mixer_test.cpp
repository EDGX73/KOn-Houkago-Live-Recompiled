#include "native_audio_mixer.hpp"
#include <iostream>
void require(bool value,const char* why){if(!value)throw std::runtime_error(why);}
int main(){try{
    kon::NativeAudioMixer mix;mix.reserve(0,64,0);mix.reserve(1,64,0x10);
    mix.channels[0].left=mix.channels[0].right=0x8000;mix.channels[1].left=0x10000;mix.channels[1].right=0x4000;
    std::vector<std::int16_t> stereo(128),mono(64),out(64);
    for(unsigned n=0;n<64;++n){stereo[n*2]=n%2?20000:-20000;stereo[n*2+1]=n%2?-10000:10000;mono[n]=n%2?-12000:12000;}
    require(!mix.submit(0,stereo,100) && !mix.submit(1,mono,100),"First mixer packet invented a wait");
    require(mix.submit(0,stereo,100)==1,"Second mixer packet lost preceding token");
    mix.mix(out,4096,200);
    require(mix.channels[0].completed==0 && mix.remaining(0)==96 && mix.diagnostics(0).completed_frames==32,"Partial copy released the whole source buffer");
    for(unsigned n=0;n<32;++n)require(out[n*2]==(n%2?-4000:4000) && out[n*2+1]==(n%2?-16000:16000),"Stereo/mono gain or multi-channel accumulation changed");
    mix.played(4112);require(mix.diagnostics(0).played_frames==16 && mix.diagnostics(1).played_frames==16,"Partial device position did not count both channels");
    mix.played(4128);require(mix.diagnostics(0).played_frames==32 && mix.diagnostics(1).played_frames==32,"Repeated device accounting double-counted samples");
    mix.mix(out,4128,300);require(mix.channels[0].completed==1 && mix.channels[1].completed==1 && mix.diagnostics(0).completed_frames==64 && mix.diagnostics(0).played_frames==32,"Copy ownership and audible playback were conflated");
    mix.mix(out,4160,400);mix.mix(out,4192,500);require(mix.channels[0].completed==2 && !mix.remaining(0),"Second packet did not finish copying in order");
    require(mix.pending(),"Unplayed device audio was discarded from pending state");mix.played(4224);require(!mix.pending() && mix.diagnostics(0).played_frames==128 && mix.diagnostics(1).played_frames==64,"Final device playback did not consume all original frames");
    mix.reserve(2,64,0);mix.channels[2].left=mix.channels[2].right=0x10000;std::fill(stereo.begin(),stereo.end(),30000);mix.submit(2,stereo,600);mix.mix(out,4224,600);
    for(auto sample:out)require(sample==32767,"Final mix did not saturate");mix.mix(out,4256,600);
    mix.reserve(2,64,0);mix.played(4288);require(mix.diagnostics(2).played_frames==0,"Old device segments leaked into a new channel reservation");
    mix.mix(out,4288,700);for(auto sample:out)require(sample==0,"Idle channels did not render silence");
    // Independent quiet-signal oracle: two half-gain positive samples add
    // to one sample. Negative low-volume input keeps signed shift rounding.
    kon::NativeAudioMixer quiet;quiet.reserve(0,64,0);quiet.reserve(1,64,0);
    quiet.channels[0].left=quiet.channels[1].left=0x4000;
    quiet.channels[0].right=quiet.channels[1].right=1;
    for(unsigned n=0;n<64;++n){stereo[n*2]=1;stereo[n*2+1]=-1;}
    quiet.submit(0,stereo,100);quiet.submit(1,stereo,100);quiet.mix(out,0,200);
    for(unsigned n=0;n<32;++n)require(out[n*2]==1 && out[n*2+1]==-1,"Quiet mixing discarded gain fractions or changed signed rounding");
    std::cout<<"PASS: owned PCM mixer, mono/stereo volume, ordered tokens, partial copies, saturation, independent device accounting and channel generations\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
