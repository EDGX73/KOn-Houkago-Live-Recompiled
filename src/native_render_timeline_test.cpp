#include "native_render_timeline.hpp"
#include <iostream>
void check(bool value,const char* why){if(!value)throw std::runtime_error(why);}
int main(){try{
 for(unsigned fps:{30u,60u,120u,165u,237u,1000u}){
  kon::NativeRenderTimeline render(fps);unsigned simulation=0;std::uint64_t next_simulation=0,rendered=0;
  // A fixed original simulation clock remains identical at every render rate.
  for(std::uint64_t now=0;now<10000000;++now){if(now>=next_simulation){++simulation;next_simulation=(std::uint64_t(simulation)*1001000+29)/30;}if(render.due(now))++rendered;}
  check(simulation==300,"Rendering changed the original simulation count");check(rendered==fps*10,"Arbitrary render rate drifted");check(render.requests()==rendered,"Render deadline diagnostics wrong");
 }
 kon::NativeRenderTimeline deadline(165);check(deadline.until_due(0)==0 && deadline.due(0) && deadline.until_due(0)==6061 && deadline.until_due(6061)==0,"Fractional FPS wait deadline drifted");
 kon::NativeRenderTimeline late(165);check(late.due(0)&&late.due(1000000)&&!late.due(1000000),"Late rendering replayed missed deadlines");check(late.skipped_deadlines()==164,"Missed render deadlines were not counted");
 check(kon::NativeRenderTimeline::fraction(100000,33333,66666,33334)==1,"Render delay endpoint wrong");check(kon::NativeRenderTimeline::fraction(75000,25000,75000,25000)==0.5,"Render interpolation fraction wrong");
 kon::NativeGpuRasterTriangle a,b;a.xy={0,0,160,0,0,160};a.state={0,0,10,10,31,0,0,0};b=a;for(unsigned n=0;n<6;n+=2)b.xy[n]+=160;b.state[0]=10;b.state[2]=20;
 for(unsigned v=0;v<3;++v){a.attributes[v*8+6]=b.attributes[v*8+6]=1;a.attributes[v*8]=20;b.attributes[v*8]=100;}
 const auto halfway=kon::interpolate_native_triangle(a,b,.5);check(halfway.xy[0]==80 && halfway.attributes[0]==60 && halfway.state[0]==5,"Native vertex interpolation did not create an intermediate frame");
 check(kon::interpolate_native_triangle(a,b,0).xy==a.xy && kon::interpolate_native_triangle(a,b,1).attributes==b.attributes,"Interpolation changed an original endpoint");
 bool invalid=false;try{kon::NativeRenderTimeline bad(0);}catch(const std::invalid_argument&){invalid=true;}check(invalid,"Invalid render rate accepted");
 std::cout<<"PASS: independent render deadlines at 30/60/120/165/custom FPS, fixed simulation ticks, skipped backlog and immutable vertex interpolation. Full game integration pending.\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
