#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>

// PC frame-rate modes (tools/apply_logic_rate_patch.py).  The original game
// runs its logic at 30 FPS and counts time in 1/60 s ticks (one PSP vblank,
// 1001000/60 us).  For every logic frame this paces the frame and computes the
// game time it represents, published to the patched generated code:
//   kon_logic_frame_ticks    whole ticks this frame (fractions accumulate)
//   kon_logic_frame_ticks_f  exact ticks this frame (0.5 at 120 FPS)
//   kon_logic_step30         whole 30 FPS frames this frame (for 30 FPS timers)
//   kon_logic_fps            logic frames per second
//   kon_logic_smooth_factor  per-frame factor equivalent to the original 1/2
// The 30 FPS mode keeps the original vblank pacing and constants.
extern "C" {
extern std::uint32_t kon_logic_frame_ticks,kon_logic_step30,kon_logic_fps;
extern float kon_logic_frame_ticks_f,kon_logic_smooth_factor;
}
namespace kon {
class NativeLogicRate {
public:
    static constexpr unsigned kOriginal=30;
    static constexpr double kTickUs=1001000.0/60.0;
    static bool supported(unsigned fps){return fps>=30 && fps<=240;}
    // Takes effect at the next logic frame; safe at any moment.
    void set_target(unsigned fps){if(supported(fps) && fps!=target_){target_=fps;have_last_=false;}}
    unsigned target()const{return target_;}
    bool original()const{return target_==kOriginal;}
    // Diagnostics (RAM snapshots, traces): every frame is exactly one period of
    // game time regardless of how long it really took.
    void set_fixed_step(bool fixed){fixed_step_=fixed;}
    // Diagnostics (--speed-audit): frames alternate 0.6 and 1.4 periods (same
    // average rate), so values advanced per frame instead of by game time
    // stand out.
    void set_jitter(bool jitter){jitter_=jitter;}
    // Original mode: the game's own vblank wait, published constants.
    // Whole 60 Hz ticks and 30 FPS steps published so far (diagnostics).
    std::uint64_t published_ticks=0,published_steps30=0;
    void original_frame(){published_ticks+=2;published_steps30+=1;publish(2u,2.0,1u,30u);acc60_=acc30_=0.0;have_last_=false;}
    // PC modes: called when a logic frame ends at `now` (us).  Returns when the
    // next frame starts.  Frames are due on a fixed timeline (one period
    // apart): after an occasional slow frame the next ones start at once until
    // the timeline is met again, so the average rate reaches the target; more
    // than two periods behind, the timeline restarts from now.  The step
    // published is the real game time between the two frame starts.
    std::uint64_t pace(std::uint64_t now){
        // PC rates are exact (165 FPS is 165 frames per real second); game time
        // still counts PSP ticks of 1001000/60 us from the real elapsed time.
        double period=1000000.0/double(target_);
        if(jitter_)period*=(flip_=!flip_)?0.6:1.4;
        double start=double(now),elapsed=period;
        if(have_last_){
            due_+=period;if(double(now)-due_>2.0*period)due_=double(now);
            start=std::max(double(now),due_);elapsed=fixed_step_?period:start-last_start_;
        }else due_=start;
        last_start_=start;have_last_=true;
        const double ticks=std::min(elapsed/kTickUs,4.0); // at most 1/15 s per frame
        acc60_+=ticks;const auto whole=std::floor(acc60_+1e-9);acc60_-=whole;
        acc30_+=ticks*0.5;const auto step30=std::floor(acc30_+1e-9);acc30_-=step30;
        published_ticks+=std::uint64_t(whole);published_steps30+=std::uint64_t(step30);
        publish(static_cast<std::uint32_t>(whole),ticks,static_cast<std::uint32_t>(step30),
                static_cast<std::uint32_t>(std::clamp(std::lround(60.0/ticks),15l,long(target_))));
        return static_cast<std::uint64_t>(std::ceil(start));
    }
private:
    static void publish(std::uint32_t ticks,double exact,std::uint32_t step30,std::uint32_t fps){
        kon_logic_frame_ticks=ticks;kon_logic_frame_ticks_f=static_cast<float>(exact);kon_logic_step30=step30;kon_logic_fps=fps;
        kon_logic_smooth_factor=static_cast<float>(1.0-std::pow(0.5,exact*0.5));
    }
    unsigned target_=kOriginal;double last_start_=0.0,due_=0.0;bool have_last_=false,fixed_step_=false,jitter_=false,flip_=false;double acc60_=0.0,acc30_=0.0;
};
}
