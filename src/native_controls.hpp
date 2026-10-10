#pragma once
#include <atomic>
#include <algorithm>
#include <array>
#include <cstdint>
#include <deque>
#include <functional>
#include <vector>
#include "psprecomp/guest_memory.hpp"
#include "native_button_map.hpp"
namespace kon {
struct HostPad {std::uint32_t buttons=0;std::uint8_t x=128,y=128;};
// Keyboard bindings: ButtonMap (defaults, or config/controls.ini).
// Virtual-key values stay independent of Windows headers so combinations can
// be checked without generating physical keyboard events.
template<class KeyState> HostPad keyboard_pad(KeyState key){
    HostPad pad;
    for(const auto& [code,bit]:ButtonMap::instance().keys())if(key(code))pad.buttons|=bit;
    const bool left=key('J'),right=key('L'),up=key('I'),down=key('K');
    if(left!=right)pad.x=left?0:255;
    if(up!=down)pad.y=up?0:255;
    return pad;
}
// Written by the window thread, read by the game thread: atomic bit sets.
class NativeKeyboard {
public:
    void down(unsigned key,bool repeated){if(repeated)return;for(const auto& [code,button]:ButtonMap::instance().keys())if(code==key)pressed_.fetch_or(button);}
    void lose_focus(){pressed_.store(0);}
    std::uint32_t peek()const{return pressed_.load();}
    void acknowledge(std::uint32_t delivered){pressed_.fetch_and(~delivered);}
    std::uint32_t consume(){return pressed_.exchange(0);}
private:std::atomic<std::uint32_t> pressed_{0};
};
HostPad read_host_pad(bool focused);
class NativeControls {
public:
    struct Sample {std::uint32_t timestamp=0;HostPad pad;std::uint64_t sampled_at=0;};
    explicit NativeControls(std::function<HostPad()> source):source_(std::move(source)){}
    // Timestamped sources (diagnostic replays) can recover their known history
    // after a long render. Physical devices only supply the current state.
    explicit NativeControls(std::function<HostPad(std::uint64_t)> source):timed_source_(std::move(source)){}
    void set_cycle(unsigned cycle,std::uint64_t now){cycle_=cycle;next_=now+interval();}
    void set_mode(unsigned mode){analog_=mode!=0;}
    // PC frame-rate modes sample once per logic frame (0 = the game's cycle):
    // the game blocks on each read until a new sample exists.
    void set_frame_interval(std::uint64_t microseconds){frame_interval_=microseconds;}
    std::uint64_t next_sample() const{return next_;}
    void update(std::uint64_t now){
        if(!next_)next_=now;
        if(now<next_)return;
        const auto current=source_?source_():HostPad{};
        const auto step=interval();
        // Keep only the newest 63 unread samples, retaining the original clock
        // phase even when now is not aligned to a sampling deadline.
        const auto due=(now-next_)/step+1u;
        if(due>63u)next_+=(due-63u)*step;
        while(next_<=now){
            auto pad=timed_source_?timed_source_(next_):current;if(!analog_)pad.x=pad.y=128;
            samples_.push_back({static_cast<std::uint32_t>(next_),pad,next_});if(samples_.size()>63)samples_.pop_front();next_+=step;
        }
    }
    bool available() const{return !samples_.empty();}
    unsigned read(psprecomp::GuestMemory& memory,std::uint32_t destination,unsigned count){
        const auto taken=std::min(count,static_cast<unsigned>(samples_.size()));
        const auto begin=samples_.size()-taken;
        // The newest sample reports the device state at read time instead of
        // at its sampling deadline (up to one sampling interval earlier): less
        // input lag. Timed replays return the same state for the same instant.
        if(taken && (source_ || timed_source_)){auto pad=timed_source_?timed_source_(samples_.back().sampled_at):source_();if(!analog_)pad.x=pad.y=128;samples_.back().pad=pad;}
        for(unsigned n=0;n<taken;++n){
            const auto out=destination+n*16u;const auto& sample=samples_[begin+n];
            memory.store32(out,sample.timestamp);memory.store32(out+4u,sample.pad.buttons);
            memory.store8(out+8u,sample.pad.x);memory.store8(out+9u,sample.pad.y);
            for(unsigned b=10;b<16;++b)memory.store8(out+b,0);
        }
        if(taken)samples_.clear();return taken;
    }
private:
    unsigned cycle_=0;bool analog_=false;std::uint64_t next_=0,frame_interval_=0;
    std::function<HostPad()> source_;std::function<HostPad(std::uint64_t)> timed_source_;std::deque<Sample> samples_;
    std::uint64_t interval() const{return frame_interval_?frame_interval_:cycle_?cycle_:16683u;}
};
}
