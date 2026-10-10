#pragma once
#include "native_gpu_raster.hpp"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <stdexcept>
#include <vector>
namespace kon {
// Render deadlines only. The clock never dispatches game code, changes audio
// time, or substitutes repaint counts for frames. Late work skips deadlines.
class NativeRenderTimeline {
public:
 explicit NativeRenderTimeline(unsigned fps=60){set_rate(fps);}
 void set_rate(unsigned fps){if(!fps || fps>1000)throw std::invalid_argument("Render FPS must be between 1 and 1000");fps_=fps;started_=false;}
 bool due(std::uint64_t now){
  if(!started_){origin_=now;last_=now;next_=1;started_=true;++requests_;return true;}
  if(now<last_)throw std::invalid_argument("Render time moved backwards");last_=now;
  const auto elapsed=now-origin_;if(elapsed>86400000000ull)throw std::invalid_argument("Render clock session exceeds one day");
  const auto slot=elapsed*fps_/1000000u;
  if(slot<next_)return false;if(slot>next_)skipped_+=slot-next_;next_=slot+1;++requests_;return true;
 }
 std::uint64_t until_due(std::uint64_t now)const{
  if(!started_)return 0;if(now<last_)throw std::invalid_argument("Render time moved backwards");
  const auto deadline=origin_+(next_*1000000u+fps_-1)/fps_;
  return deadline>now?deadline-now:0;
 }
 std::uint64_t requests()const{return requests_;}
 std::uint64_t skipped_deadlines()const{return skipped_;}
 // One original display period of buffering allows interpolation of completed
 // immutable geometry; it does not advance simulation to manufacture frames.
 static double fraction(std::uint64_t presentation,std::uint64_t previous,std::uint64_t current,std::uint64_t delay){
  if(current<=previous)throw std::invalid_argument("Frame timestamps must increase");
  const auto sampled=presentation>delay?presentation-delay:0;
  if(sampled<=previous)return 0;if(sampled>=current)return 1;
  return double(sampled-previous)/double(current-previous);
 }
private:
 unsigned fps_=60;bool started_=false;
 std::uint64_t origin_=0,last_=0,next_=0,requests_=0,skipped_=0;
};
// Corresponding immutable packets must retain the same topology/resources and
// draw state. Unmatched packets use their current geometry, never guessed IDs.
inline NativeGpuRasterTriangle interpolate_native_triangle(const NativeGpuRasterTriangle& a,const NativeGpuRasterTriangle& b,double alpha){
 if(!std::isfinite(alpha) || alpha<0 || alpha>1)throw std::invalid_argument("Invalid native render interpolation");
 if(alpha==0)return a;if(alpha==1)return b;
 auto out=b;
 for(unsigned n=0;n<6;++n)out.xy[n]=static_cast<std::int32_t>(std::llround(std::lerp(double(a.xy[n]),double(b.xy[n]),alpha)));
 for(unsigned n=0;n<24;++n)out.attributes[n]=std::lerp(a.attributes[n],b.attributes[n],alpha);
 const auto& xy=out.xy;const auto area=std::int64_t(xy[2]-xy[0])*(xy[5]-xy[1])-std::int64_t(xy[3]-xy[1])*(xy[4]-xy[0]);
 if(area<=0)return b;
 unsigned edges=0;for(unsigned n=0;n<3;++n){const auto i=n*2,j=((n+1)%3)*2;if(xy[j+1]<xy[i+1] || (xy[j+1]==xy[i+1] && xy[j]>xy[i]))edges|=8u<<n;}
 bool color=true,depth=true;for(unsigned v=1;v<3;++v){for(unsigned n=0;n<4;++n)color= color && out.attributes[n]==out.attributes[v*8+n];depth=depth && out.attributes[7]==out.attributes[v*8+7];}
 out.state[4]=(b.state[4]&1u)|(unsigned(color)<<1)|(unsigned(depth)<<2)|edges;out.state[5]=0;
 for(unsigned n=0;n<4;++n)out.state[5]|=static_cast<unsigned>(std::clamp(out.attributes[n],0.0,255.0))<<(n*8);
 const auto lo=[](int p){return static_cast<int>(std::floor(p/16.0));};const auto hi=[](int p){return static_cast<int>(std::ceil(p/16.0));};
 out.state[0]=std::max(std::min(a.state[0],b.state[0]),unsigned(std::max(0,lo(std::min({xy[0],xy[2],xy[4]})))));
 out.state[1]=std::max(std::min(a.state[1],b.state[1]),unsigned(std::max(0,lo(std::min({xy[1],xy[3],xy[5]})))));
 out.state[2]=std::min(std::max(a.state[2],b.state[2]),unsigned(std::max(0,hi(std::max({xy[0],xy[2],xy[4]})))));
 out.state[3]=std::min(std::max(a.state[3],b.state[3]),unsigned(std::max(0,hi(std::max({xy[1],xy[3],xy[5]})))));
 if(out.state[0]>=out.state[2] || out.state[1]>=out.state[3])return b;
 return out;
}
}
