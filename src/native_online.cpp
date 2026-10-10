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
#include "native_online.hpp"
#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstring>
#include <cwctype>
namespace kon {
namespace {
constexpr char kAlphabet[]="0123456789ABCDEFGHJKMNPQRSTVWXYZ"; // Crockford base 32
std::string encode32(const std::vector<std::uint8_t>& bytes){
    std::string out;unsigned buffer=0,bits=0;
    for(const auto b:bytes){buffer=(buffer<<8)|b;bits+=8;while(bits>=5){out+=kAlphabet[(buffer>>(bits-5))&31u];bits-=5;}}
    if(bits)out+=kAlphabet[(buffer<<(5-bits))&31u];
    std::string grouped;for(std::size_t n=0;n<out.size();++n){if(n && n%5==0)grouped+='-';grouped+=out[n];}
    return grouped;
}
std::optional<std::vector<std::uint8_t>> decode32(const std::string& text,std::size_t bytes){
    std::vector<std::uint8_t> out;unsigned buffer=0,bits=0;
    for(char c:text){
        c=char(std::toupper(static_cast<unsigned char>(c)));
        if(c=='-' || c==' ')continue;
        if(c=='O')c='0';if(c=='I' || c=='L')c='1';
        const char* at=std::strchr(kAlphabet,c);if(!at || !c)return std::nullopt;
        buffer=(buffer<<5)|unsigned(at-kAlphabet);bits+=5;
        if(bits>=8){out.push_back(std::uint8_t(buffer>>(bits-8)));bits-=8;buffer&=(1u<<bits)-1u;}
    }
    if(out.size()!=bytes)return std::nullopt;
    return out;
}
std::string lower(std::string s){for(auto& c:s)c=char(std::tolower(static_cast<unsigned char>(c)));return s;}
struct Url {std::string host,path;std::uint16_t port=80;};
std::optional<Url> parse_url(const std::string& url){
    if(lower(url.substr(0,7))!="http://")return std::nullopt;
    const auto rest=url.substr(7);const auto slash=rest.find('/');
    Url u;auto authority=rest.substr(0,slash);u.path=slash==std::string::npos?"/":rest.substr(slash);
    if(const auto colon=authority.rfind(':');colon!=std::string::npos && authority.find(']')==std::string::npos){u.port=std::uint16_t(std::atoi(authority.c_str()+colon+1));authority.resize(colon);}
    u.host=authority;if(u.host.empty() || !u.port)return std::nullopt;
    return u;
}
// A small HTTP/1.1 request (Connection: close); the whole answer, or nothing.
std::optional<std::string> http(const Url& url,const std::string& request,std::string* local_ip=nullptr){
    addrinfo hints{};hints.ai_family=AF_INET;hints.ai_socktype=SOCK_STREAM;addrinfo* list=nullptr;
    if(getaddrinfo(url.host.c_str(),std::to_string(url.port).c_str(),&hints,&list) || !list)return std::nullopt;
    const SOCKET s=socket(AF_INET,SOCK_STREAM,IPPROTO_TCP);
    DWORD timeout=3000;setsockopt(s,SOL_SOCKET,SO_RCVTIMEO,reinterpret_cast<const char*>(&timeout),sizeof timeout);setsockopt(s,SOL_SOCKET,SO_SNDTIMEO,reinterpret_cast<const char*>(&timeout),sizeof timeout);
    const bool ok=s!=INVALID_SOCKET && !connect(s,list->ai_addr,int(list->ai_addrlen));freeaddrinfo(list);
    if(!ok){if(s!=INVALID_SOCKET)closesocket(s);return std::nullopt;}
    if(local_ip){sockaddr_in me{};int length=sizeof me;getsockname(s,reinterpret_cast<sockaddr*>(&me),&length);char text[INET_ADDRSTRLEN]{};inet_ntop(AF_INET,&me.sin_addr,text,sizeof text);*local_ip=text;}
    send(s,request.data(),int(request.size()),0);
    std::string answer;char buffer[8192];int got;
    while((got=recv(s,buffer,sizeof buffer,0))>0 && answer.size()<(1u<<20))answer.append(buffer,std::size_t(got));
    closesocket(s);
    if(answer.empty())return std::nullopt;
    return answer;
}
std::optional<std::string> soap(const Url& control,const std::string& service,const std::string& action,const std::string& arguments,std::string* local_ip=nullptr){
    const std::string body="<?xml version=\"1.0\"?><s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\"><s:Body><u:"+action+" xmlns:u=\""+service+"\">"+arguments+"</u:"+action+"></s:Body></s:Envelope>";
    const std::string request="POST "+control.path+" HTTP/1.1\r\nHost: "+control.host+":"+std::to_string(control.port)+"\r\nContent-Type: text/xml; charset=\"utf-8\"\r\nSOAPAction: \""+service+"#"+action+"\"\r\nContent-Length: "+std::to_string(body.size())+"\r\nConnection: close\r\n\r\n"+body;
    return http(control,request,local_ip);
}
bool http_ok(const std::string& answer){return answer.rfind("HTTP/1.",0)==0 && answer.size()>12 && answer.compare(9,3,"200")==0;}
struct Winsock {bool ok;Winsock(){WSADATA w{};ok=!WSAStartup(MAKEWORD(2,2),&w);}~Winsock(){if(ok)WSACleanup();}};
}
std::string room_code(const std::string& address,std::uint16_t port){
    std::vector<std::uint8_t> bytes;
    in_addr v4{};in6_addr v6{};
    if(inet_pton(AF_INET,address.c_str(),&v4)==1)bytes.assign(reinterpret_cast<std::uint8_t*>(&v4),reinterpret_cast<std::uint8_t*>(&v4)+4);
    else if(inet_pton(AF_INET6,address.c_str(),&v6)==1)bytes.assign(v6.s6_addr,v6.s6_addr+16);
    else return {};
    bytes.push_back(std::uint8_t(port>>8));bytes.push_back(std::uint8_t(port));
    return encode32(bytes);
}
std::optional<RoomAddress> parse_room_code(const std::string& raw){
    std::string text;for(const char c:raw)if(!std::isspace(static_cast<unsigned char>(c)))text+=c;
    if(text.empty())return std::nullopt;
    // "[v6]:port", "v4:port" or "name:port".
    if(text.front()=='['){const auto close=text.find("]:");if(close!=std::string::npos){const int port=std::atoi(text.c_str()+close+2);if(port>0 && port<65536)return RoomAddress{text.substr(1,close-1),std::uint16_t(port)};}return std::nullopt;}
    if(const auto colon=text.find(':');colon!=std::string::npos && text.find(':',colon+1)==std::string::npos){
        const int port=std::atoi(text.c_str()+colon+1);if(port>0 && port<65536 && colon)return RoomAddress{text.substr(0,colon),std::uint16_t(port)};return std::nullopt;}
    std::size_t symbols=0;for(const char c:text)if(c!='-')++symbols;
    if(symbols==10){if(const auto b=decode32(text,6)){char a[INET_ADDRSTRLEN]{};inet_ntop(AF_INET,b->data(),a,sizeof a);return RoomAddress{a,std::uint16_t(((*b)[4]<<8)|(*b)[5])};}}
    if(symbols==29){if(const auto b=decode32(text,18)){char a[INET6_ADDRSTRLEN]{};inet_ntop(AF_INET6,b->data(),a,sizeof a);return RoomAddress{a,std::uint16_t(((*b)[16]<<8)|(*b)[17])};}}
    return std::nullopt;
}
bool private_ipv4(const std::string& address){
    in_addr a{};if(inet_pton(AF_INET,address.c_str(),&a)!=1)return false;
    const auto v=ntohl(a.s_addr);
    return (v>>24)==10 || (v>>20)==0xAC1 || (v>>16)==0xC0A8 || (v>>22)==(100u<<2|1u) || (v>>16)==0xA9FE || (v>>24)==127 || v==0;
}
LocalAddresses local_addresses(){
    LocalAddresses out;ULONG size=16384;std::vector<std::uint8_t> buffer(size);
    auto* first=reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data());
    if(GetAdaptersAddresses(AF_UNSPEC,GAA_FLAG_SKIP_ANYCAST|GAA_FLAG_SKIP_MULTICAST|GAA_FLAG_SKIP_DNS_SERVER,nullptr,first,&size)==ERROR_BUFFER_OVERFLOW){
        buffer.resize(size);first=reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data());
        if(GetAdaptersAddresses(AF_UNSPEC,GAA_FLAG_SKIP_ANYCAST|GAA_FLAG_SKIP_MULTICAST|GAA_FLAG_SKIP_DNS_SERVER,nullptr,first,&size)!=NO_ERROR)return out;
    }
    for(auto* a=first;a;a=a->Next){
        if(a->OperStatus!=IfOperStatusUp || a->IfType==IF_TYPE_SOFTWARE_LOOPBACK)continue;
        std::wstring label=std::wstring(a->FriendlyName?a->FriendlyName:L"")+L" "+(a->Description?a->Description:L"");
        for(auto& c:label)c=wchar_t(std::towlower(c));
        std::string vpn;
        for(const auto& [key,name]:{std::pair{L"radmin",std::string("Radmin VPN")},{L"zerotier","ZeroTier"},{L"tailscale","Tailscale"},{L"hamachi","Hamachi"}})
            if(label.find(key)!=std::wstring::npos)vpn=name;
        for(auto* u=a->FirstUnicastAddress;u;u=u->Next){
            const auto* sa=u->Address.lpSockaddr;char text[INET6_ADDRSTRLEN]{};
            if(sa->sa_family==AF_INET){inet_ntop(AF_INET,&reinterpret_cast<const sockaddr_in*>(sa)->sin_addr,text,sizeof text);if(std::strncmp(text,"169.254.",8)){if(!vpn.empty())out.vpn.push_back({vpn,text});else out.ipv4.push_back(text);}}
            else if(sa->sa_family==AF_INET6){
                const auto& b=reinterpret_cast<const sockaddr_in6*>(sa)->sin6_addr.s6_addr;
                // Global unicast (2000::/3), not a temporary-only or link-local address.
                if((b[0]&0xE0)==0x20 && u->DadState==IpDadStatePreferred && u->SuffixOrigin!=IpSuffixOriginRandom){inet_ntop(AF_INET6,b,text,sizeof text);out.ipv6_global.push_back(text);}
            }
        }
    }
    return out;
}
std::optional<std::string> ssdp_location(const std::string& response){
    const auto l=lower(response);const auto at=l.find("\nlocation:");if(at==std::string::npos)return std::nullopt;
    auto start=at+10;while(start<response.size() && response[start]==' ')++start;
    const auto end=response.find_first_of("\r\n",start);return response.substr(start,end-start);
}
std::string xml_value(const std::string& xml,const std::string& tag){
    const auto l=lower(xml);const auto t=lower(tag);
    auto open=l.find("<"+t+">");std::size_t start;
    if(open!=std::string::npos)start=open+t.size()+2;
    else{ // a namespace prefix, as in <m:NewExternalIPAddress>
        open=l.find(":"+t+">");if(open==std::string::npos)return {};start=open+t.size()+2;}
    const auto end=xml.find('<',start);return end==std::string::npos?std::string{}:xml.substr(start,end-start);
}
std::optional<UpnpService> upnp_wan_service(const std::string& description,const std::string& location){
    for(const char* kind:{"urn:schemas-upnp-org:service:WANIPConnection:2","urn:schemas-upnp-org:service:WANIPConnection:1","urn:schemas-upnp-org:service:WANPPPConnection:1"}){
        const auto at=description.find(std::string(">")+kind+"<");if(at==std::string::npos)continue;
        const auto block_end=description.find("</service>",at);const auto block=description.substr(at,block_end==std::string::npos?std::string::npos:block_end-at);
        auto control=xml_value(block,"controlURL");if(control.empty())continue;
        if(lower(control.substr(0,7))!="http://"){
            const auto base=parse_url(location);if(!base)return std::nullopt;
            if(control.front()!='/')control="/"+control;
            control="http://"+base->host+":"+std::to_string(base->port)+control;
        }
        return UpnpService{kind,control};
    }
    return std::nullopt;
}
UpnpMapping UpnpMapping::open(std::uint16_t port){
    Winsock winsock;UpnpMapping m;m.port_=port;
    const SOCKET s=socket(AF_INET,SOCK_DGRAM,IPPROTO_UDP);
    if(s==INVALID_SOCKET){m.error="no network";return m;}
    const std::string search="M-SEARCH * HTTP/1.1\r\nHOST: 239.255.255.250:1900\r\nMAN: \"ssdp:discover\"\r\nMX: 2\r\nST: urn:schemas-upnp-org:device:InternetGatewayDevice:1\r\n\r\n";
    sockaddr_in to{};to.sin_family=AF_INET;to.sin_port=htons(1900);inet_pton(AF_INET,"239.255.255.250",&to.sin_addr);
    for(int n=0;n<2;++n)sendto(s,search.data(),int(search.size()),0,reinterpret_cast<sockaddr*>(&to),sizeof to);
    std::vector<std::string> locations;
    const auto end=std::chrono::steady_clock::now()+std::chrono::milliseconds(2500);
    while(std::chrono::steady_clock::now()<end){
        fd_set read;FD_ZERO(&read);FD_SET(s,&read);timeval wait{0,200000};
        if(select(0,&read,nullptr,nullptr,&wait)<=0)continue;
        char buffer[2048];const int got=recv(s,buffer,sizeof buffer,0);if(got<=0)continue;
        if(auto l=ssdp_location(std::string(buffer,std::size_t(got)));l && std::find(locations.begin(),locations.end(),*l)==locations.end())locations.push_back(*l);
    }
    closesocket(s);
    if(locations.empty()){m.error="no UPnP router answered (UPnP off or not supported)";return m;}
    for(const auto& location:locations){
        const auto url=parse_url(location);if(!url)continue;
        std::string local; // the address this PC uses towards the router
        const auto answer=http(*url,"GET "+url->path+" HTTP/1.1\r\nHost: "+url->host+":"+std::to_string(url->port)+"\r\nConnection: close\r\n\r\n",&local);
        if(!answer || !http_ok(*answer))continue;
        const auto service=upnp_wan_service(*answer,location);if(!service)continue;
        const auto control=parse_url(service->control_url);if(!control)continue;
        m.router=url->host;m.service_=service->type;m.control_url_=service->control_url;
        if(const auto ip=soap(*control,service->type,"GetExternalIPAddress","");ip && http_ok(*ip))m.external_ip=xml_value(*ip,"NewExternalIPAddress");
        for(const char* lease:{"7200","0"}){ // some routers accept only permanent mappings
            const auto args="<NewRemoteHost></NewRemoteHost><NewExternalPort>"+std::to_string(port)+"</NewExternalPort><NewProtocol>TCP</NewProtocol><NewInternalPort>"+std::to_string(port)+"</NewInternalPort><NewInternalClient>";
            const auto reply=soap(*control,service->type,"AddPortMapping",args+local+"</NewInternalClient><NewEnabled>1</NewEnabled><NewPortMappingDescription>K-On Houkago Live Recompiled</NewPortMappingDescription><NewLeaseDuration>"+lease+"</NewLeaseDuration>");
            if(reply && http_ok(*reply)){m.mapped=true;m.internal_ip=local;return m;}
            m.error="the router refused the port mapping"+(reply?" ("+xml_value(*reply,"errorDescription")+")":std::string{});
        }
        return m;
    }
    if(m.error.empty())m.error="the UPnP router offers no port mapping service";
    return m;
}
void UpnpMapping::close(){
    if(!mapped)return;mapped=false;
    Winsock winsock;
    if(const auto control=parse_url(control_url_))
        soap(*control,service_,"DeletePortMapping","<NewRemoteHost></NewRemoteHost><NewExternalPort>"+std::to_string(port_)+"</NewExternalPort><NewProtocol>TCP</NewProtocol>");
}
UpnpMapping::UpnpMapping(UpnpMapping&& o)noexcept{*this=std::move(o);}
UpnpMapping& UpnpMapping::operator=(UpnpMapping&& o)noexcept{
    if(this!=&o){close();mapped=o.mapped;external_ip=std::move(o.external_ip);internal_ip=std::move(o.internal_ip);router=std::move(o.router);error=std::move(o.error);
        control_url_=std::move(o.control_url_);service_=std::move(o.service_);port_=o.port_;o.mapped=false;}
    return *this;
}
UpnpMapping::~UpnpMapping(){close();}
}
