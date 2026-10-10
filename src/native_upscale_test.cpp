#include "native_upscale.hpp"
#include <iostream>
#include <stdexcept>
// Edge-directed enlargement: flat areas and transparent texels are kept, a
// diagonal edge gets intermediate corners (not blocks), straight edges stay
// sharp and the edge steepening never leaves the local range of values.
using namespace kon;
static void check(bool ok,const char* what){if(!ok)throw std::runtime_error(what);}
int main(){try{
    constexpr unsigned n=16;constexpr std::uint32_t white=0xFFFFFFFFu,red=0xFF0000FFu;
    std::vector<std::uint32_t> flat(n*n,red);
    const auto f2=upscale_edges(flat,n,n,2);check(f2.size()==n*n*4 && std::all_of(f2.begin(),f2.end(),[](auto p){return p==0xFF0000FFu;}),"Flat texture stays flat");
    std::vector<std::uint32_t> clear(n*n,0x00FF00FFu);
    const auto c2=upscale_edges(clear,n,n,2);check(std::all_of(c2.begin(),c2.end(),[](auto p){return (p>>24)==0;}),"Transparent stays transparent");
    // Lower-left triangle red, the rest white: a 45 degree staircase.
    std::vector<std::uint32_t> diag(n*n);for(unsigned y=0;y<n;++y)for(unsigned x=0;x<n;++x)diag[y*n+x]=x<y?red:white;
    const auto d2=upscale_edges(diag,n,n,2);unsigned mixed=0;
    for(const auto p:d2)if(p!=red && p!=white)++mixed;
    check(mixed>=n,"Diagonal edge corners are blended");
    // Far from the edge nothing changes.
    check(d2[(2*n-1)*0+2*n-1]==white && d2[(2*n-1)*2*n]==red,"Areas away from the edge are kept");
    const auto d4=upscale_edges(diag,n,n,4);check(d4.size()==n*n*16 && d4[0]==white && d4[(4*n-1)*4*n]==red,"4x keeps the areas away from the edge");
    for(const auto p:d4)check(((p&255u)==255u),"Steepened values stay inside the local range");
    // A straight vertical edge stays straight.
    std::vector<std::uint32_t> half(n*n);for(unsigned y=0;y<n;++y)for(unsigned x=0;x<n;++x)half[y*n+x]=x<n/2?red:white;
    const auto h2=upscale_edges(half,n,n,2);for(unsigned y=0;y<2*n;++y)for(unsigned x=0;x<2*n;++x)check(h2[y*2*n+x]==(x<n?red:white),"Straight edges stay sharp");
    std::cout<<"PASS: edge-directed 2x/4x texture enlargement ("<<mixed<<" blended corners on the diagonal)\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<"\n";return 1;}}
