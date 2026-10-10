#pragma once
#include <algorithm>
#include <array>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <list>
#include <memory>
#include <mutex>
#include <span>
#include <thread>
#include <unordered_map>
#include <vector>
#ifdef _WIN32
extern "C" __declspec(dllimport) void* __stdcall GetCurrentThread();
extern "C" __declspec(dllimport) int __stdcall SetThreadPriority(void*,int);
#endif
// 2D textures at the internal resolution: an edge-directed 2x enlargement
// (in the spirit of Hyllian's xBR, level 2 rules), applied twice for 4x. Each
// corner of a texel follows the edge that crosses it, so diagonal and curved
// outlines (text, icons, frames) come out smooth instead of in blocks; flat
// areas and straight edges stay exactly as they were. Enlarged 2D draws then
// sample the result bilinearly at their own resolution.
namespace kon {
namespace upscale_detail {
struct Texel {int y,u,v,a;};
inline Texel analyse(std::uint32_t p){
    // Premultiplied, so fully transparent texels compare equal whatever their color.
    const int a=int(p>>24),r=int(p&255u)*a/255,g=int((p>>8)&255u)*a/255,b=int((p>>16)&255u)*a/255;
    return {(299*r+587*g+114*b)/1000,(-169*r-331*g+500*b)/1000,(500*r-419*g-81*b)/1000,a};
}
inline int distance(const Texel& p,const Texel& q){return 48*std::abs(p.y-q.y)+7*std::abs(p.u-q.u)+6*std::abs(p.v-q.v)+32*std::abs(p.a-q.a);}
// Premultiplied linear mix of two texels, weight w/8 of q.
inline std::uint32_t mix(std::uint32_t p,std::uint32_t q,unsigned w){
    const unsigned pa=p>>24,qa=q>>24,a=(pa*(8u-w)+qa*w+4u)/8u;
    if(!a)return p&0x00FFFFFFu; // transparent: keep a color for bilinear neighbours
    std::uint32_t out=a<<24;
    for(unsigned s=0;s<24;s+=8){
        const unsigned pc=((p>>s)&255u)*pa,qc=((q>>s)&255u)*qa;
        out|=std::min(255u,((pc*(8u-w)+qc*w)/8u+a/2u)/a)<<s;
    }
    return out;
}
}
// One 2x step: width*2 x height*2 texels.
inline std::vector<std::uint32_t> upscale_edges_2x(std::span<const std::uint32_t> src,unsigned width,unsigned height){
    using namespace upscale_detail;
    std::vector<Texel> t(src.size());for(std::size_t i=0;i<src.size();++i)t[i]=analyse(src[i]);
    const auto at=[&](int x,int y){x=std::clamp(x,0,int(width)-1);y=std::clamp(y,0,int(height)-1);return std::size_t(y)*width+std::size_t(x);};
    std::vector<std::uint32_t> out(std::size_t(width)*height*4u);
    const unsigned ow=width*2u;
    // Rotations taking the bottom-right corner's frame to each corner's:
    // (dx,dy) -> right/down of the bottom-right corner become the corner's own.
    static constexpr std::array<std::array<int,4>,4> rot{{{1,0,0,1},{0,-1,1,0},{-1,0,0,-1},{0,1,-1,0}}};
    // Output texel (in the 2x2 block) of each corner and of its two neighbours
    // along the edge (the one beside it across x, the one across y).
    static constexpr std::array<std::array<unsigned,3>,4> cells{{{3,2,1},{2,0,3},{0,1,2},{1,3,0}}};
    constexpr int same=64; // below this distance two texels count as equal
    for(unsigned y=0;y<height;++y)for(unsigned x=0;x<width;++x){
        const auto e=src[at(int(x),int(y))];
        std::array<std::uint32_t,4> block{e,e,e,e};
        for(unsigned k=0;k<4;++k){
            const auto& m=rot[k];
            const auto n=[&](int dx,int dy){return at(int(x)+m[0]*dx+m[1]*dy,int(y)+m[2]*dx+m[3]*dy);};
            const auto E=n(0,0),F=n(1,0),H=n(0,1),I=n(1,1),B=n(0,-1),D=n(-1,0),C=n(1,-1),G=n(-1,1),F4=n(2,0),H5=n(0,2),I4=n(2,1),I5=n(1,2);
            const auto d=[&](std::size_t p,std::size_t q){return distance(t[p],t[q]);};
            const int across=d(E,C)+d(E,G)+d(I,F4)+d(I,H5)+4*d(H,F),along=d(H,D)+d(H,I5)+d(F,I4)+d(F,B)+4*d(E,I);
            if(across>=along)continue;
            const auto px=d(E,F)<=d(E,H)?src[F]:src[H];
            const int ke=d(F,G),ki=d(H,C);
            const bool shallow=ke*2<=ki && d(E,G)>=same && d(D,G)>=same,steep=ki*2<=ke && d(E,C)>=same && d(B,C)>=same;
            const auto& c=cells[k];
            if(shallow && steep){block[c[0]]=mix(block[c[0]],px,7);block[c[1]]=mix(block[c[1]],px,2);block[c[2]]=mix(block[c[2]],px,2);}
            else if(shallow){block[c[0]]=mix(block[c[0]],px,6);block[c[1]]=mix(block[c[1]],px,2);}
            else if(steep){block[c[0]]=mix(block[c[0]],px,6);block[c[2]]=mix(block[c[2]],px,2);}
            else block[c[0]]=mix(block[c[0]],px,4);
        }
        const auto o=std::size_t(y)*2u*ow+std::size_t(x)*2u;
        out[o]=block[0];out[o+1]=block[1];out[o+ow]=block[2];out[o+ow+1]=block[3];
    }
    return out;
}
// Edges of the enlarged image steepened towards the original's local
// extremes: each channel of a sample moves away from the middle of the range
// its source texel's 3x3 neighbourhood spans, so a soft antialiased edge
// (text, outlines) becomes as narrow at the new size as it was at the old
// one. Values stay inside that range (no halos); flat or gently shaded areas,
// whose range is small, are left as they are.
inline void steepen_edges(std::vector<std::uint32_t>& big,std::span<const std::uint32_t> src,unsigned width,unsigned height,unsigned factor){
    const unsigned bw=width*factor;
    std::vector<std::array<std::uint8_t,8>> range(src.size()); // min/max of each channel around each source texel
    for(unsigned y=0;y<height;++y)for(unsigned x=0;x<width;++x){
        std::array<std::uint8_t,8> r{255,255,255,255,0,0,0,0};
        for(int dy=-1;dy<=1;++dy)for(int dx=-1;dx<=1;++dx){
            const auto p=src[std::size_t(std::clamp(int(y)+dy,0,int(height)-1))*width+std::size_t(std::clamp(int(x)+dx,0,int(width)-1))];
            for(unsigned c=0;c<4;++c){const auto v=std::uint8_t(p>>(c*8u));r[c]=std::min(r[c],v);r[c+4]=std::max(r[c+4],v);}
        }
        range[std::size_t(y)*width+x]=r;
    }
    for(unsigned y=0;y<height*factor;++y)for(unsigned x=0;x<bw;++x){
        auto& p=big[std::size_t(y)*bw+x];const auto& r=range[std::size_t(y/factor)*width+x/factor];std::uint32_t out=0;
        for(unsigned c=0;c<4;++c){
            const int lo=r[c],hi=r[c+4],v=int((p>>(c*8u))&255u),span=hi-lo;
            int result=v;
            if(span>=48){const int t=(v-lo)*256/span;result=lo+std::clamp((t-128)*2+128,0,256)*span/256;}
            out|=std::uint32_t(std::clamp(result,lo,hi))<<(c*8u);
        }
        p=out;
    }
}
// Smooth 2x step (Catmull-Rom, premultiplied alpha, edges clamped).
inline std::vector<std::uint32_t> upscale_cubic_2x(std::span<const std::uint32_t> src,unsigned width,unsigned height){
    const unsigned ow=width*2u,oh=height*2u;
    std::vector<std::array<float,4>> pm(src.size());
    for(std::size_t i=0;i<src.size();++i){const float a=float(src[i]>>24)/255.f;pm[i]={float(src[i]&255u)*a,float((src[i]>>8)&255u)*a,float((src[i]>>16)&255u)*a,float(src[i]>>24)};}
    // Output texel centers sit at -1/4 and +1/4 of a source texel: fixed weights
    // for the taps base-1..base+2 (even outputs: base is the texel before).
    static constexpr std::array<std::array<float,4>,2> w{{{-0.0234375f,0.2265625f,0.8671875f,-0.0703125f},{-0.0703125f,0.8671875f,0.2265625f,-0.0234375f}}};
    std::vector<std::array<float,4>> rows(std::size_t(width)*oh);
    for(unsigned y=0;y<oh;++y){const int base=int(y/2u)+(y&1u?0:-1);const auto& k=w[y&1u];
        for(unsigned x=0;x<width;++x){std::array<float,4> acc{};
            for(int i=0;i<4;++i){const auto& p=pm[std::size_t(std::clamp(base-1+i,0,int(height)-1))*width+x];for(unsigned c=0;c<4;++c)acc[c]+=k[i]*p[c];}
            rows[std::size_t(y)*width+x]=acc;}}
    std::vector<std::uint32_t> out(std::size_t(ow)*oh);
    for(unsigned y=0;y<oh;++y)for(unsigned x=0;x<ow;++x){const int base=int(x/2u)+(x&1u?0:-1);const auto& k=w[x&1u];std::array<float,4> acc{};
        for(int i=0;i<4;++i){const auto& p=rows[std::size_t(y)*width+std::size_t(std::clamp(base-1+i,0,int(width)-1))];for(unsigned c=0;c<4;++c)acc[c]+=k[i]*p[c];}
        const float a=std::clamp(acc[3],0.f,255.f);std::uint32_t v=std::uint32_t(a+0.5f)<<24;
        if(a>=0.5f)for(unsigned c=0;c<3;++c)v|=std::uint32_t(std::clamp(acc[c]/(a/255.f),0.f,255.f)+0.5f)<<(c*8u);
        else v|=src[std::size_t(y/2u)*width+x/2u]&0x00FFFFFFu; // transparent: the source texel's color (no dark fringes)
        out[std::size_t(y)*ow+x]=v;}
    return out;
}
inline std::vector<std::uint32_t> upscale_edges(std::span<const std::uint32_t> src,unsigned width,unsigned height,unsigned factor){
    static const int mode=[]{const char* m=std::getenv("KON_UPSCALE_MODE");return m?std::atoi(m):0;}(); // diagnostic variants
    std::vector<std::uint32_t> big;
    if(mode==1){big=upscale_edges_2x(src,width,height);if(factor>=4)big=upscale_edges_2x(big,width*2u,height*2u);}
    else{
        // Hard diagonals follow their edge (xBR), then a smooth step, then the edges are steepened.
        big=upscale_edges_2x(src,width,height);
        if(factor>=4)big=upscale_cubic_2x(big,width*2u,height*2u);
    }
    if(mode!=1 && !std::getenv("KON_NO_EDGE_STEEPEN"))steepen_edges(big,src,width,height,factor>=4?4u:2u);
    return big;
}
// Enlarged copies of decoded textures, keyed by their decode token (which
// identifies immutable pixels). Built by low-priority background threads so a
// scene's first frames never wait: until a copy is ready the draw uses the
// original. The most recent requests (the screen being shown) go first.
class TextureUpscaler {
public:
    static TextureUpscaler& instance(){static TextureUpscaler upscaler;return upscaler;}
    static bool enabled(){static const bool on=!std::getenv("KON_NO_TEXTURE_UPSCALE");return on;}
    // The enlarged texels (kept alive while held), or null (queued) until ready.
    std::shared_ptr<const std::vector<std::uint32_t>> find(std::uint32_t token,std::span<const std::uint32_t> pixels,unsigned width,unsigned height,unsigned factor){
        if(!enabled() || !token || width>512u || height>512u || width<8u || height<8u || pixels.size()!=std::size_t(width)*height)return nullptr;
        std::lock_guard lock(mutex_);
        const auto key=(std::uint64_t(token)<<8)|factor;
        if(const auto found=ready_.find(key);found!=ready_.end()){lru_.splice(lru_.begin(),lru_,found->second.second);return found->second.first;}
        if(queued_.insert({key,true}).second){
            jobs_.push_back({key,std::vector<std::uint32_t>(pixels.begin(),pixels.end()),width,height,factor});
            if(workers_.empty()){
                const unsigned n=std::clamp(std::thread::hardware_concurrency()/4u,2u,6u);
                for(unsigned i=0;i<n;++i)workers_.emplace_back([this]{run();});
            }
            wake_.notify_one();
        }
        return nullptr;
    }
    ~TextureUpscaler(){{std::lock_guard lock(mutex_);stop_=true;}wake_.notify_all();for(auto& w:workers_)if(w.joinable())w.join();}
private:
    struct Job {std::uint64_t key;std::vector<std::uint32_t> pixels;unsigned width,height,factor;};
    void run(){
#ifdef _WIN32
        SetThreadPriority(GetCurrentThread(),-1); // THREAD_PRIORITY_BELOW_NORMAL
#endif
        for(;;){
            Job job;
            {std::unique_lock lock(mutex_);wake_.wait(lock,[&]{return stop_ || !jobs_.empty();});if(stop_)return;job=std::move(jobs_.back());jobs_.pop_back();}
            auto result=std::make_shared<const std::vector<std::uint32_t>>(upscale_edges(job.pixels,job.width,job.height,job.factor));
            std::lock_guard lock(mutex_);
            bytes_+=result->size()*4u;lru_.push_front(job.key);ready_[job.key]={std::move(result),lru_.begin()};queued_.erase(job.key);
            // Oldest copies go first beyond the budget (a draw holding one keeps it alive).
            while(bytes_>kBudgetBytes && lru_.size()>1){const auto old=lru_.back();lru_.pop_back();const auto it=ready_.find(old);bytes_-=it->second.first->size()*4u;ready_.erase(it);}
        }
    }
    static constexpr std::size_t kBudgetBytes=384u*1024u*1024u;
    std::mutex mutex_;std::condition_variable wake_;std::deque<Job> jobs_;std::vector<std::thread> workers_;bool stop_=false;
    std::list<std::uint64_t> lru_;std::size_t bytes_=0;
    std::unordered_map<std::uint64_t,std::pair<std::shared_ptr<const std::vector<std::uint32_t>>,std::list<std::uint64_t>::iterator>> ready_;
    std::unordered_map<std::uint64_t,bool> queued_;
};
}
