#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <iphlpapi.h>
#include <algorithm>
#include <vector>
#include <stdexcept>
#include "native_network.hpp"
namespace kon {
std::optional<std::array<std::uint8_t,6>> host_ethernet_address() {
    ULONG size=15000;
    for(unsigned retry=0;retry<3;++retry){
        std::vector<std::uint64_t> storage((size+7u)/8u);
        auto* first=reinterpret_cast<IP_ADAPTER_ADDRESSES*>(storage.data());
        const auto result=GetAdaptersAddresses(AF_UNSPEC,GAA_FLAG_SKIP_UNICAST|GAA_FLAG_SKIP_ANYCAST|GAA_FLAG_SKIP_MULTICAST|GAA_FLAG_SKIP_DNS_SERVER,nullptr,first,&size);
        if(result==ERROR_BUFFER_OVERFLOW)continue;
        if(result==ERROR_NO_DATA)return std::nullopt;
        if(result!=NO_ERROR)throw std::runtime_error("Windows network adapter enumeration failed: "+std::to_string(result));
        IP_ADAPTER_ADDRESSES* selected=nullptr;
        for(auto* adapter=first;adapter;adapter=adapter->Next){
            if(adapter->PhysicalAddressLength!=6 || adapter->IfType==IF_TYPE_SOFTWARE_LOOPBACK)continue;
            if((adapter->PhysicalAddress[0]&1u) || std::all_of(adapter->PhysicalAddress,adapter->PhysicalAddress+6,[](auto b){return b==0;}))continue;
            if(!selected || (selected->OperStatus!=IfOperStatusUp && adapter->OperStatus==IfOperStatusUp))selected=adapter;
        }
        if(!selected)return std::nullopt;
        std::array<std::uint8_t,6> address{};std::copy_n(selected->PhysicalAddress,6,address.begin());return address;
    }
    throw std::runtime_error("Windows network adapters changed repeatedly during enumeration");
}
}
