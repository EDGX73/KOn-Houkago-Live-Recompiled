#pragma once
#include <cstdint>
namespace kon {
struct HostHeadphoneState {
    bool present=false,endpoint_available=false;
    unsigned form_factor=10;
};
constexpr bool headphone_form_factor(unsigned form){return form==3u || form==5u;}
HostHeadphoneState query_host_headphones();
class HostHeadphones {
public:
    const HostHeadphoneState& poll(std::uint64_t microseconds){
        if(!initialized_ || microseconds>=next_query_){
            state_=query_host_headphones();initialized_=true;
            next_query_=microseconds+1000000u;++queries_;
        }
        return state_;
    }
    unsigned queries()const{return queries_;}
private:
    bool initialized_=false;std::uint64_t next_query_=0;
    unsigned queries_=0;HostHeadphoneState state_;
};
}
