#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <stdexcept>
namespace kon {
// Integer attribute interpolation shared by the CPU reference rasterizer and
// the Vulkan shader (shaders/native_raster.comp mirrors these functions).
// Every value is derived once per triangle on the CPU; per pixel only 64-bit
// integer multiply/add/shift (and one division per perspective coordinate)
// remain, so CPU and any GPU produce identical bits without Float64.
//
// Barycentric weights have 24 fractional bits and sum exactly to 2^24, so a
// constant attribute interpolates to itself. Z and colors carry 8 fractional
// bits; texture coordinates carry 12 (1/16 texel plus 8 guard bits).
struct NativeFixedTriangle {
    std::int64_t inverse=0;                       // floor(2^56 / area)
    std::array<std::int64_t,3> z{};               // z * 256
    std::array<std::array<std::int32_t,4>,3> color{}; // channel * 256
    std::array<std::int64_t,3> u{},v{};           // u*4096 (through) or u*4096*p (perspective)
    std::array<std::int32_t,3> p{};               // perspective weights 1..4096
    std::uint32_t z0=0;                           // constant-depth value
};
inline constexpr std::int64_t kFixedOne=std::int64_t(1)<<24;
// attributes: per vertex RGBA, U, V, Q, Z (NativeGpuRasterTriangle layout).
inline NativeFixedTriangle make_fixed_triangle(std::int64_t area,const std::array<double,24>& attributes,bool perspective){
    NativeFixedTriangle f;
    f.inverse=static_cast<std::int64_t>((std::uint64_t(1)<<56)/std::uint64_t(std::max<std::int64_t>(area,1)));
    // Inline round-half-away-from-zero (std::llround is an out-of-line call).
    const auto fixed=[](double value,double scale,double limit){const auto v=std::clamp(value*scale,-limit,limit);return v>=0?static_cast<std::int64_t>(v+0.5):-static_cast<std::int64_t>(0.5-v);};
    double qmax=0;for(unsigned v=0;v<3;++v)qmax=std::max(qmax,std::abs(attributes[v*8+6]));
    for(unsigned v=0;v<3;++v){
        const auto* a=&attributes[v*8];
        f.z[v]=fixed(a[7],256.0,2147483647.0);
        for(unsigned c=0;c<4;++c)f.color[v][c]=static_cast<std::int32_t>(fixed(a[c],256.0,65535.0));
        f.p[v]=perspective && qmax>0?static_cast<std::int32_t>(std::clamp<std::int64_t>(static_cast<std::int64_t>(std::abs(a[6])/qmax*4096.0+0.5),1,4096)):4096;
        const auto u=fixed(a[4],4096.0,2147483647.0),w=fixed(a[5],4096.0,2147483647.0);
        f.u[v]=perspective?u*f.p[v]:u;f.v[v]=perspective?w*f.p[v]:w;
    }
    f.z0=static_cast<std::uint32_t>(std::clamp(attributes[7],0.0,65535.0));
    return f;
}
// Textured triangles keep the explicit diagnostic for invalid coordinates.
// Interpolation is convex, so vertex bounds also bound every covered sample.
inline void validate_fixed_texture(const std::array<double,24>& attributes){
    for(unsigned v=0;v<3;++v)for(unsigned i:{4u,5u,6u}){const auto value=attributes[v*8+i];
        if(!std::isfinite(value) || (i!=6 && std::abs(value)>1000000))throw std::runtime_error("Invalid native texture coordinates");}
}
// eb/ec/ea are the non-negative edge values of a covered pixel (weights of
// vertices a/b/c respectively, as in the edge-function rasterizer).
struct NativeFixedWeights {std::int64_t a,b,c;};
inline NativeFixedWeights fixed_weights(std::int64_t eb,std::int64_t ec,std::int64_t inverse){
    const auto a=(eb*inverse)>>32,b=(ec*inverse)>>32;return {a,b,kFixedOne-a-b};
}
inline std::uint32_t fixed_depth(const NativeFixedTriangle& f,const NativeFixedWeights& w){
    const auto z=(f.z[0]*w.a+f.z[1]*w.b+f.z[2]*w.c)>>32;return static_cast<std::uint32_t>(std::clamp<std::int64_t>(z,0,65535));
}
inline std::uint32_t fixed_color(const NativeFixedTriangle& f,const NativeFixedWeights& w){
    std::uint32_t color=0;
    for(unsigned c=0;c<4;++c){const auto value=(std::int64_t(f.color[0][c])*w.a+std::int64_t(f.color[1][c])*w.b+std::int64_t(f.color[2][c])*w.c)>>32;color|=static_cast<std::uint32_t>(std::clamp<std::int64_t>(value,0,255))<<(c*8u);}
    return color;
}
// Texture position in sixteenths of a texel (floor).
inline void fixed_uv(const NativeFixedTriangle& f,const NativeFixedWeights& w,bool perspective,int& ux,int& vy){
    if(!perspective){ux=static_cast<int>((f.u[0]*w.a+f.u[1]*w.b+f.u[2]*w.c)>>32);vy=static_cast<int>((f.v[0]*w.a+f.v[1]*w.b+f.v[2]*w.c)>>32);return;}
    const auto a=w.a>>8,b=w.b>>8,c=w.c>>8;const auto q=f.p[0]*a+f.p[1]*b+f.p[2]*c;
    ux=static_cast<int>(((f.u[0]*a+f.u[1]*b+f.u[2]*c)/q)>>8);vy=static_cast<int>(((f.v[0]*a+f.v[1]*b+f.v[2]*c)/q)>>8);
}
// Checks of one normalized raster job against its draw's bounds, shared by
// the Vulkan backend and the renderer's parallel job builder.
template<class Job> void validate_raster_job(const Job& job,bool textured,unsigned left,unsigned top,unsigned right,unsigned bottom){
    const auto& b=job.state;
    for(auto v:job.xy)if(v< -1048576 || v>1048576)throw std::runtime_error("Vulkan raster edge exceeds validated range");
    for(auto v:job.attributes)if(!std::isfinite(v))throw std::runtime_error("Nonfinite Vulkan raster attribute");
    if(textured)validate_fixed_texture(job.attributes);
    const auto& xy=job.xy;const auto area=std::int64_t(xy[2]-xy[0])*(xy[5]-xy[1])-std::int64_t(xy[3]-xy[1])*(xy[4]-xy[0]);
    unsigned edges=0;for(unsigned e=0;e<3;++e){const auto a=e*2,c=((e+1)%3)*2;if(xy[c+1]<xy[a+1] || (xy[c+1]==xy[a+1] && xy[c]>xy[a]))edges|=8u<<e;}
    if(area<=0 || (b[4]&56)!=edges || (b[4]&~63u) || b[0]<left || b[1]<top || b[2]>right || b[3]>bottom || b[0]>=b[2] || b[1]>=b[3])throw std::runtime_error("Invalid normalized Vulkan raster job");
    if(b[6]){bool bounded=b[2]<=512 && b[3]<=512;for(auto v:job.xy)bounded=bounded && v>=-8192 && v<=8191;if(!bounded)throw std::runtime_error("Unsafe narrow raster edge");}
}
// GPU packet: fixed words per job (see native_raster.comp shadeJob).
inline constexpr unsigned kFixedJobWords=50;
template<class Job> void write_fixed_job(std::uint32_t* out,const Job& job,bool perspective){
    const auto& xy=job.xy;
    const auto area=static_cast<std::int64_t>(xy[2]-xy[0])*(xy[5]-xy[1])-static_cast<std::int64_t>(xy[3]-xy[1])*(xy[4]-xy[0]);
    const auto f=make_fixed_triangle(area,job.attributes,perspective);
    const auto put64=[&](unsigned at,std::int64_t value){out[at]=static_cast<std::uint32_t>(value);out[at+1]=static_cast<std::uint32_t>(std::uint64_t(value)>>32);};
    for(unsigned n=0;n<6;++n)out[n]=static_cast<std::uint32_t>(xy[n]);
    for(unsigned n=0;n<4;++n)out[6+n]=job.state[n];
    out[10]=job.state[4];out[11]=job.state[5];out[12]=job.state[6];out[13]=f.z0;put64(14,f.inverse);
    for(unsigned v=0;v<3;++v){put64(16+v*2,f.z[v]);for(unsigned c=0;c<4;++c)out[22+v*4+c]=static_cast<std::uint32_t>(f.color[v][c]);put64(34+v*2,f.u[v]);put64(40+v*2,f.v[v]);out[46+v]=static_cast<std::uint32_t>(f.p[v]);}
    out[49]=0;
}
}
