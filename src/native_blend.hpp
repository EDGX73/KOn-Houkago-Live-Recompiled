#pragma once
#include <algorithm>
#include <cstdint>
namespace kon {
// Own GE RGB blend implementation. Alpha/stencil writes remain the
// framebuffer mask's responsibility. Doubled alpha factors are not yet enabled.
class NativeBlend {
public:
    static bool supported(unsigned mode){
        const unsigned operation=(mode>>8u)&7u;
        const auto factor=[](unsigned f){return f<=5u || f>=10u;};
        return !(mode&~0x7FFu) && operation<=5u &&
            (operation>=3u || (factor(mode&15u) && factor((mode>>4u)&15u)));
    }
    // Two 32-bit-spaced lanes keep red/blue integer products independent.
    // Products use the original odd-integer GE rounding, not /255 blending.
    static std::uint32_t apply(unsigned mode,std::uint32_t source,std::uint32_t destination,
                               std::uint32_t source_fixed,std::uint32_t destination_fixed){
        if(mode!=0x32u && !(mode==0xA2u && (destination_fixed&0xFFFFFFu)==0xFFFFFFu))return apply_generic(mode,source,destination,source_fixed,destination_fixed);
        constexpr std::uint64_t lanes=0x000000FF000000FFull,ones=0x0000000100000001ull;
        const auto rb=[](std::uint32_t c){return std::uint64_t(c&255u)|(std::uint64_t(c&0xFF0000u)<<16u);};
        const unsigned alpha=source>>24u;
        const auto a=(((rb(source)*2u+ones)*(alpha*2u+1u))>>10u)&lanes;
        const auto green_source=(((source>>8u)&255u)*2u+1u)*(alpha*2u+1u)>>10u;
        if(mode==0x32u){
            const auto b=(((rb(destination)*2u+ones)*((255u-alpha)*2u+1u))>>10u)&lanes;
            const auto green_destination=(((destination>>8u)&255u)*2u+1u)*((255u-alpha)*2u+1u)>>10u;
            const auto sum=a+b; // complementary factors bound each lane to 255
            return static_cast<std::uint32_t>(sum&255u)|static_cast<std::uint32_t>((sum>>32u)&255u)<<16u|(green_source+green_destination)<<8u;
        }
        const auto sum=a+rb(destination);
        const auto red=std::min(static_cast<unsigned>(sum&0xFFFFFFFFu),255u),blue=std::min(static_cast<unsigned>(sum>>32u),255u);
        return red|(blue<<16u)|(std::min(green_source+((destination>>8u)&255u),255u)<<8u);
    }
    static std::uint32_t apply_generic(unsigned mode,std::uint32_t source,std::uint32_t destination,
                               std::uint32_t source_fixed,std::uint32_t destination_fixed){
        std::uint32_t output=0;
        for(unsigned shift:{0u,8u,16u}){
            const int s=(source>>shift)&255u,d=(destination>>shift)&255u;
            const auto factor=[&](unsigned f,bool incoming){
                switch(f){
                case 0:return incoming?d:s;
                case 1:return 255-(incoming?d:s);
                case 2:return static_cast<int>(source>>24u);
                case 3:return 255-static_cast<int>(source>>24u);
                case 4:return static_cast<int>(destination>>24u);
                case 5:return 255-static_cast<int>(destination>>24u);
                default:return static_cast<int>(((incoming?source_fixed:destination_fixed)>>shift)&255u);
                }
            };
            const auto product=[](int color,int scale){return ((2*color+1)*(2*scale+1))>>10;};
            int value=0;const unsigned operation=(mode>>8u)&7u;
            if(operation>=3u){value=operation==3u?std::min(s,d):operation==4u?std::max(s,d):std::abs(s-d);}
            else {
                const int a=product(s,factor(mode&15u,true)),b=product(d,factor((mode>>4u)&15u,false));
                value=operation==0u?a+b:operation==1u?a-b:b-a;
            }
            output|=static_cast<std::uint32_t>(std::clamp(value,0,255))<<shift;
        }
        return output;
    }
};
}
