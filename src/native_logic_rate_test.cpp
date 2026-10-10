#include "native_logic_rate.hpp"
#include <cstdio>
#include <cstdlib>
#include <cmath>

extern "C" {
std::uint32_t kon_logic_frame_ticks=2,kon_logic_step30=1,kon_logic_fps=30;
float kon_logic_frame_ticks_f=2.0f,kon_logic_smooth_factor=0.5f;
}
namespace {
void check(bool ok,const char* what){if(!ok){std::fprintf(stderr,"FAIL: %s\n",what);std::exit(1);}}
struct Totals{std::uint64_t ticks=0,steps=0;double exact=0;};
// Runs logic frames that each take `work` us and returns what they published
// and the time reached.
std::uint64_t run(kon::NativeLogicRate& rate,std::uint64_t now,std::uint64_t until,double work,Totals& totals){
    while(now<until){
        now=rate.pace(now);totals.ticks+=kon_logic_frame_ticks;totals.steps+=kon_logic_step30;totals.exact+=kon_logic_frame_ticks_f;
        now+=static_cast<std::uint64_t>(work);
    }
    return now;
}
}
int main(){
    kon::NativeLogicRate rate;
    rate.original_frame();
    check(kon_logic_frame_ticks==2 && kon_logic_step30==1 && kon_logic_fps==30 && kon_logic_smooth_factor==0.5f,"30 FPS mode must publish the original constants");
    std::uint64_t now=5'000'000;
    for(const unsigned fps:{60u,120u,144u,165u}){
        // Fast frames: paced at the target rate; 4 s of game time whatever the rate.
        rate.set_target(fps);Totals fast;const auto begin=now;now=run(rate,now,now+4'004'000,1000.0,fast);
        const double seconds=double(now-begin)/1001000.0;
        check(std::fabs(fast.exact/60.0-seconds)<0.02,"Game time must follow real time at any rate");
        check(kon_logic_fps==fps && std::fabs(kon_logic_frame_ticks_f-float(1000000.0/fps/kon::NativeLogicRate::kTickUs))<1e-4,"Paced frames publish one exact period");
        // Halves the remaining distance every 2 ticks of game time, at any rate.
        check(std::fabs(std::pow(1.0-kon_logic_smooth_factor,2.0/kon_logic_frame_ticks_f)-0.5)<1e-4,"Smoothing must be frame-rate independent");
        check(fast.ticks+1>=std::uint64_t(fast.exact) && fast.ticks<=std::uint64_t(fast.exact)+1 && std::fabs(double(fast.steps)-fast.exact/2.0)<=1.0,"Whole ticks and 30 FPS steps accumulate exactly");
        // Slow frames (9 ms at 165 FPS): no waiting for another refresh, real duration.
        Totals slow;const auto slow_begin=now;now=run(rate,now,now+2'002'000,9000.0,slow);
        check(std::fabs(slow.exact/60.0-double(now-slow_begin)/1001000.0)<0.02,"Late frames carry their real duration");
    }
    rate.set_target(165);Totals late;now=rate.pace(now);now=rate.pace(now+9000);
    check(std::fabs(kon_logic_frame_ticks_f-9000.0f/1001000.0f*60.0f)<1e-3 && kon_logic_fps==111,"A 9 ms frame is 111 FPS of game time");
    {
        // Occasional slow frame at 120 FPS: the following fast frames start at
        // once until the fixed timeline is met, so the average rate holds.
        rate.set_target(120);Totals mixed;std::uint64_t t=now;const auto begin=t;unsigned frames=0;
        for(unsigned n=0;n<1200;++n){t=rate.pace(t);++frames;t+=(n%10==0)?12000:6000;mixed.exact+=kon_logic_frame_ticks_f;}
        const double fps=double(frames)*1000000.0/double(t-begin);
        check(fps>119.0 && fps<=120.5,"Fixed timeline: one 12 ms frame in ten keeps 120 FPS on average");
        check(std::fabs(mixed.exact/60.0-double(t-begin)/1001000.0)<0.02,"Catching up keeps game time equal to real time");
        // Far behind (a long stall): the timeline restarts instead of racing.
        const auto stalled=rate.pace(t+500'000);check(stalled==t+500'000,"A stall starts the next frame at once");
        const auto next=rate.pace(stalled+1000);check(next>=stalled+1000+7000,"After a stall pacing resumes from the new start");
        now=next;
    }
    rate.set_target(30);rate.original_frame();
    check(kon_logic_frame_ticks==2 && kon_logic_step30==1 && kon_logic_fps==30,"Returning to 30 FPS restores the original step");
    std::puts("PASS: 30 FPS constants, 60/120/144/165 FPS game time, overrun frames, smoothing and live switching");
}
