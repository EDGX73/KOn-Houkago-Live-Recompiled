#include "host_headphones.hpp"
#include <iostream>
#include <stdexcept>
int main(){try{
    for(unsigned form=0;form<=11;++form)
        if(kon::headphone_form_factor(form)!=(form==3 || form==5))throw std::runtime_error("Endpoint categories misclassified");
    kon::HostHeadphones detector;const auto first=detector.poll(0);
    detector.poll(999999);
    if(detector.queries()!=1)throw std::runtime_error("Repeated game queries bypassed the device-query cache");
    const auto next=detector.poll(1000000);
    if(detector.queries()!=2 || next.present!=kon::headphone_form_factor(next.form_factor))throw std::runtime_error("Timed host device refresh failed");
    std::cout<<"PASS: actual Windows playback endpoint available="<<first.endpoint_available
             <<" form_factor="<<first.form_factor<<" headphones="<<first.present<<"; category and cached refresh checks\n";return 0;
}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}}
