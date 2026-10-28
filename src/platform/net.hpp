#pragma once
// Sockets and network interfaces for ad hoc play, the online room and UPnP
// (platform layer). The services use the BSD socket API; this header holds what
// differs: the socket type and its closing, non-blocking mode, "would block",
// select's first argument, timeouts, SIGPIPE, Winsock start-up, and the list of
// interface addresses (GetAdaptersAddresses on Windows, getifaddrs elsewhere).
#include <array>
#include <cstdint>
#include <initializer_list>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <iphlpapi.h>
#include <algorithm>
#include <cwctype>
#else
#include <algorithm>
#include <arpa/inet.h>
#include <cctype>
#include <cerrno>
#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>
#if defined(__linux__)
#include <linux/if_packet.h>
#endif
#endif
namespace kon::platform {
#ifdef _WIN32
using Socket=SOCKET;using SockLen=int;
inline constexpr Socket invalid_socket=INVALID_SOCKET;
inline constexpr int send_flags=0;
#else
using Socket=int;using SockLen=socklen_t;
inline constexpr Socket invalid_socket=-1;
// A peer that closed must be an error result, not a SIGPIPE that ends the game.
inline constexpr int send_flags=MSG_NOSIGNAL;
#endif
// Winsock 2.2 on Windows; nothing to start elsewhere.
inline bool network_start(){
#ifdef _WIN32
    WSADATA data{};return !WSAStartup(MAKEWORD(2,2),&data);
#else
    return true;
#endif
}
inline void network_stop(){
#ifdef _WIN32
    WSACleanup();
#endif
}
struct NetworkSession {bool ok;NetworkSession():ok(network_start()){}~NetworkSession(){if(ok)network_stop();}
    NetworkSession(const NetworkSession&)=delete;NetworkSession& operator=(const NetworkSession&)=delete;};
inline int close_socket(Socket s){
#ifdef _WIN32
    return closesocket(s);
#else
    return ::close(s);
#endif
}
inline void set_nonblocking(Socket s){
#ifdef _WIN32
    u_long on=1;ioctlsocket(s,FIONBIO,&on);
#else
    const int flags=fcntl(s,F_GETFL,0);if(flags>=0)fcntl(s,F_SETFL,flags|O_NONBLOCK);
#endif
}
// The last socket call could not complete yet (non-blocking connect/accept/recv/send).
inline bool would_block(){
#ifdef _WIN32
    const int e=WSAGetLastError();return e==WSAEWOULDBLOCK || e==WSAEINPROGRESS || e==WSAEALREADY;
#else
    return errno==EWOULDBLOCK || errno==EAGAIN || errno==EINPROGRESS || errno==EALREADY;
#endif
}
// Receive and send timeouts of a blocking socket.
inline void set_timeouts(Socket s,unsigned milliseconds){
#ifdef _WIN32
    DWORD timeout=milliseconds;
    setsockopt(s,SOL_SOCKET,SO_RCVTIMEO,reinterpret_cast<const char*>(&timeout),sizeof timeout);
    setsockopt(s,SOL_SOCKET,SO_SNDTIMEO,reinterpret_cast<const char*>(&timeout),sizeof timeout);
#else
    const timeval timeout{static_cast<time_t>(milliseconds/1000),static_cast<suseconds_t>((milliseconds%1000)*1000)};
    setsockopt(s,SOL_SOCKET,SO_RCVTIMEO,&timeout,sizeof timeout);
    setsockopt(s,SOL_SOCKET,SO_SNDTIMEO,&timeout,sizeof timeout);
#endif
}
// select()'s first argument: ignored by Winsock, the highest descriptor + 1 elsewhere.
inline int select_bound(std::initializer_list<Socket> sockets){
#ifdef _WIN32
    (void)sockets;return 0;
#else
    int highest=-1;for(const auto s:sockets)highest=std::max(highest,s);return highest+1;
#endif
}

// Addresses of the interfaces that are up (loopback excluded). label: the
// interface's names in lower case (Windows: friendly name and description), used
// to recognise game VPNs; stable_global_ipv6: a global unicast IPv6 address that
// is neither tentative nor a temporary (privacy) one, where the host tells.
struct InterfaceAddress {std::string label;int family=0;std::string text;bool stable_global_ipv6=false;};
inline std::vector<InterfaceAddress> interface_addresses(){
    std::vector<InterfaceAddress> out;
#ifdef _WIN32
    ULONG size=16384;std::vector<std::uint8_t> buffer(size);
    auto* first=reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data());
    if(GetAdaptersAddresses(AF_UNSPEC,GAA_FLAG_SKIP_ANYCAST|GAA_FLAG_SKIP_MULTICAST|GAA_FLAG_SKIP_DNS_SERVER,nullptr,first,&size)==ERROR_BUFFER_OVERFLOW){
        buffer.resize(size);first=reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data());
        if(GetAdaptersAddresses(AF_UNSPEC,GAA_FLAG_SKIP_ANYCAST|GAA_FLAG_SKIP_MULTICAST|GAA_FLAG_SKIP_DNS_SERVER,nullptr,first,&size)!=NO_ERROR)return out;
    }
    for(auto* a=first;a;a=a->Next){
        if(a->OperStatus!=IfOperStatusUp || a->IfType==IF_TYPE_SOFTWARE_LOOPBACK)continue;
        std::wstring wide=std::wstring(a->FriendlyName?a->FriendlyName:L"")+L" "+(a->Description?a->Description:L"");
        for(auto& c:wide)c=wchar_t(std::towlower(c));
        std::string label;
        if(const int n=WideCharToMultiByte(CP_UTF8,0,wide.c_str(),int(wide.size()),nullptr,0,nullptr,nullptr);n>0){
            label.resize(std::size_t(n));WideCharToMultiByte(CP_UTF8,0,wide.c_str(),int(wide.size()),label.data(),n,nullptr,nullptr);}
        for(auto* u=a->FirstUnicastAddress;u;u=u->Next){
            const auto* sa=u->Address.lpSockaddr;char text[INET6_ADDRSTRLEN]{};
            if(sa->sa_family==AF_INET){inet_ntop(AF_INET,&reinterpret_cast<const sockaddr_in*>(sa)->sin_addr,text,sizeof text);out.push_back({label,AF_INET,text,false});}
            else if(sa->sa_family==AF_INET6){
                const auto& b=reinterpret_cast<const sockaddr_in6*>(sa)->sin6_addr.s6_addr;inet_ntop(AF_INET6,b,text,sizeof text);
                out.push_back({label,AF_INET6,text,(b[0]&0xE0)==0x20 && u->DadState==IpDadStatePreferred && u->SuffixOrigin!=IpSuffixOriginRandom});
            }
        }
    }
#else
    ifaddrs* list=nullptr;if(getifaddrs(&list))return out;
    for(auto* a=list;a;a=a->ifa_next){
        if(!a->ifa_addr || !(a->ifa_flags&IFF_UP) || (a->ifa_flags&IFF_LOOPBACK))continue;
        std::string label=a->ifa_name?a->ifa_name:"";for(auto& c:label)c=char(std::tolower(static_cast<unsigned char>(c)));
        char text[INET6_ADDRSTRLEN]{};
        if(a->ifa_addr->sa_family==AF_INET){inet_ntop(AF_INET,&reinterpret_cast<const sockaddr_in*>(a->ifa_addr)->sin_addr,text,sizeof text);out.push_back({label,AF_INET,text,false});}
        else if(a->ifa_addr->sa_family==AF_INET6){
            const auto& b=reinterpret_cast<const sockaddr_in6*>(a->ifa_addr)->sin6_addr.s6_addr;inet_ntop(AF_INET6,b,text,sizeof text);
            // getifaddrs does not report temporary/tentative state: any global unicast.
            out.push_back({label,AF_INET6,text,(b[0]&0xE0)==0x20});
        }
    }
    freeifaddrs(list);
#endif
    return out;
}
// The physical (MAC) address of the active wired or wireless adapter, for the
// PSP's WLAN address; none when the host has or reveals no such adapter.
inline std::optional<std::array<std::uint8_t,6>> ethernet_address(){
#ifdef _WIN32
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
#elif defined(__linux__)
    // Linux packet addresses; Android hides them from apps (then: none).
    ifaddrs* list=nullptr;if(getifaddrs(&list))return std::nullopt;
    std::optional<std::array<std::uint8_t,6>> found;bool found_up=false;
    for(auto* a=list;a;a=a->ifa_next){
        if(!a->ifa_addr || a->ifa_addr->sa_family!=AF_PACKET || (a->ifa_flags&IFF_LOOPBACK))continue;
        const auto* ll=reinterpret_cast<const sockaddr_ll*>(a->ifa_addr);
        if(ll->sll_halen!=6 || (ll->sll_addr[0]&1u) || std::all_of(ll->sll_addr,ll->sll_addr+6,[](auto b){return b==0;}))continue;
        const bool up=(a->ifa_flags&IFF_UP) && (a->ifa_flags&IFF_RUNNING);
        if(!found || (!found_up && up)){std::array<std::uint8_t,6> address{};std::copy_n(ll->sll_addr,6,address.begin());found=address;found_up=up;}
    }
    freeifaddrs(list);return found;
#else
    return std::nullopt;
#endif
}
}
