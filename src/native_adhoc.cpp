#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include "native_adhoc.hpp"
#include "native_network.hpp"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <sstream>
#include <thread>
#pragma comment(lib,"ws2_32.lib")
namespace kon {
namespace {
constexpr const char* kMulticastGroup="239.255.75.75";
constexpr std::uint64_t kBeaconUs=300000,kPeerTimeoutUs=3000000,kScanUs=1200000;
std::uint64_t now_us(){return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());}
void put16(std::vector<std::uint8_t>& b,std::uint16_t v){b.push_back(std::uint8_t(v));b.push_back(std::uint8_t(v>>8));}
std::uint16_t get16(const std::uint8_t* p){return std::uint16_t(p[0]|(p[1]<<8));}
void put32(std::vector<std::uint8_t>& b,std::uint32_t v){for(unsigned n=0;n<4;++n)b.push_back(std::uint8_t(v>>(8*n)));}
std::uint32_t get32(const std::uint8_t* p){return std::uint32_t(p[0])|(std::uint32_t(p[1])<<8)|(std::uint32_t(p[2])<<16)|(std::uint32_t(p[3])<<24);}
void nonblocking(SOCKET s){u_long on=1;ioctlsocket(s,FIONBIO,&on);}
bool would_block(){const int e=WSAGetLastError();return e==WSAEWOULDBLOCK || e==WSAEINPROGRESS || e==WSAEALREADY;}
}
struct AdhocNet::Impl {
    enum class Role : std::uint8_t {None=0,Host=1,Member=2};
    struct PeerInfo {Mac mac{};sockaddr_in address{};std::uint16_t data_port=0,tcp_port=0;Role role=Role::None;std::array<char,8> group{};std::string nickname,product;std::uint64_t seen=0;bool relayed=false;};
    struct Pdp {std::uint16_t port=0;std::deque<Datagram> queue;std::size_t bytes=0;};
    enum class PtpKind {Open,Listen,Connecting,Established,Closed};
    // A PTP stream is a TCP connection, or (relayed) a virtual stream through
    // the online relay named by its opener's MAC and the opener's id for it.
    struct Pending {SOCKET socket=INVALID_SOCKET;Accepted accepted;bool relayed=false;Mac opener{};std::uint32_t stream=0;};
    struct Ptp {PtpKind kind=PtpKind::Open;std::uint16_t port=0,peer_port=0;Mac peer{};SOCKET socket=INVALID_SOCKET;bool hello_sent=false;std::deque<Pending> backlog;
        bool relayed=false,remote_closed=false;Mac opener{};std::uint32_t stream=0;std::deque<std::uint8_t> rx;};
    struct Incoming {SOCKET socket=INVALID_SOCKET;std::vector<std::uint8_t> hello;std::uint64_t since=0;};
    mutable std::mutex mutex;
    bool wsa=false,ready=false;
    SOCKET discovery=INVALID_SOCKET,data=INVALID_SOCKET,listener=INVALID_SOCKET;
    std::uint16_t discovery_base=41500,discovery_port=0,data_port=0,tcp_port=0;bool lan=true;
    Mac mac{};std::string nickname,product;
    Role role=Role::None;std::array<char,8> group{};bool connect_announced=false;
    bool scanning=false;std::uint64_t scan_started=0;std::vector<Group> scanned;
    std::map<Mac,PeerInfo> peers;
    std::deque<std::pair<Event,int>> events;
    std::map<int,Pdp> pdp;std::map<int,Ptp> ptp;int next_id=1;
    std::vector<Incoming> incoming;
    std::thread thread;std::atomic<bool> stop{false};std::uint64_t last_beacon=0;
    bool relay_only=false;
    // Online relay. Frame: u32 length (little endian, of what follows), type,
    // destination MAC, source MAC, payload. The relay forwards each frame to
    // the client with that MAC, or to every other client for the broadcast MAC.
    struct Link {SOCKET socket=INVALID_SOCKET;std::vector<std::uint8_t> rx,tx;Mac mac{};bool known=false;std::deque<std::pair<std::uint64_t,std::vector<std::uint8_t>>> delayed;};
    // Diagnostic: KON_RELAY_DELAY_MS holds every relayed frame that long (Internet latency, one way).
    static std::uint64_t relay_delay_us(){static const std::uint64_t us=[]{const char* v=std::getenv("KON_RELAY_DELAY_MS");return v?std::strtoull(v,nullptr,10)*1000u:0u;}();return us;}
    Link relay;std::string relay_host;std::uint16_t relay_port=0;std::uint64_t relay_generation=0,relay_retry=0;bool relay_connecting=false;std::string relay_error;
    SOCKET hub=INVALID_SOCKET;std::uint16_t hub_port=0;std::vector<Link> hub_clients;
    std::vector<std::thread> connectors;
    static void queue_frame(Link& l,char type,const Mac& to,const Mac& from,const std::uint8_t* p,std::size_t n){
        const std::uint32_t length=std::uint32_t(13+n);const auto at=l.tx.size();l.tx.resize(at+4+length);
        std::memcpy(&l.tx[at],&length,4);l.tx[at+4]=std::uint8_t(type);std::copy(to.begin(),to.end(),l.tx.begin()+std::ptrdiff_t(at+5));std::copy(from.begin(),from.end(),l.tx.begin()+std::ptrdiff_t(at+11));
        if(n)std::memcpy(&l.tx[at+17],p,n);
    }
    void relay_send(char type,const Mac& to,const std::vector<std::uint8_t>& payload){if(relay.socket!=INVALID_SOCKET)queue_frame(relay,type,to,mac,payload.data(),payload.size());}
    static bool flush(Link& l){
        while(!l.tx.empty()){const int n=send(l.socket,reinterpret_cast<const char*>(l.tx.data()),int(std::min<std::size_t>(l.tx.size(),1u<<20)),0);
            if(n<=0)return n<0 && would_block();l.tx.erase(l.tx.begin(),l.tx.begin()+n);}
        return true;
    }
    // Reads what arrived and hands each complete frame to `frame` (type, to,
    // from, payload, whole frame); false once the link closed or misbehaved.
    template<class F> static bool pump(Link& l,F&& frame){
        char buffer[65536];
        for(;;){const int got=recv(l.socket,buffer,sizeof buffer,0);
            if(got>0){l.rx.insert(l.rx.end(),buffer,buffer+got);continue;}
            if(got==0 || !would_block())return false;break;}
        std::size_t at=0;
        while(l.rx.size()-at>=4){
            const auto length=get32(&l.rx[at]);if(length<13 || length>(1u<<21))return false;if(l.rx.size()-at-4<length)break;
            const std::vector<std::uint8_t> whole(l.rx.begin()+std::ptrdiff_t(at),l.rx.begin()+std::ptrdiff_t(at+4+length));
            const auto* f=whole.data()+4;Mac to,from;std::copy_n(f+1,6,to.begin());std::copy_n(f+7,6,from.begin());
            frame(char(f[0]),to,from,f+13,std::size_t(length-13),whole.data(),whole.size());at+=4+length;
        }
        l.rx.erase(l.rx.begin(),l.rx.begin()+std::ptrdiff_t(at));return true;
    }
    void run_hub(){
        if(hub==INVALID_SOCKET)return;
        for(;;){const SOCKET s=accept(hub,nullptr,nullptr);if(s==INVALID_SOCKET)break;nonblocking(s);BOOL on=TRUE;setsockopt(s,IPPROTO_TCP,TCP_NODELAY,reinterpret_cast<const char*>(&on),sizeof on);Link l;l.socket=s;hub_clients.push_back(std::move(l));}
        for(std::size_t n=0;n<hub_clients.size();++n){
            const bool alive=pump(hub_clients[n],[&](char,const Mac& to,const Mac& from,const std::uint8_t*,std::size_t,const std::uint8_t* whole,std::size_t size){
                hub_clients[n].mac=from;hub_clients[n].known=true;
                for(std::size_t m=0;m<hub_clients.size();++m)if(m!=n && (to==kBroadcastMac || (hub_clients[m].known && hub_clients[m].mac==to)))
                {if(relay_delay_us())hub_clients[m].delayed.push_back({now_us()+relay_delay_us(),std::vector<std::uint8_t>(whole,whole+size)});
                 else hub_clients[m].tx.insert(hub_clients[m].tx.end(),whole,whole+size);}
            });
            if(!alive){closesocket(hub_clients[n].socket);hub_clients[n].socket=INVALID_SOCKET;}
        }
        for(auto& c:hub_clients)while(!c.delayed.empty() && c.delayed.front().first<=now_us()){c.tx.insert(c.tx.end(),c.delayed.front().second.begin(),c.delayed.front().second.end());c.delayed.pop_front();}
        for(auto& c:hub_clients)if(c.socket!=INVALID_SOCKET && !flush(c)){closesocket(c.socket);c.socket=INVALID_SOCKET;}
        std::erase_if(hub_clients,[](const Link& l){return l.socket==INVALID_SOCKET;});
    }
    // Connects in the background (name resolution and TCP connect can take
    // seconds); the link is installed if nothing changed meanwhile.
    void start_relay_connect(){
        if(relay_connecting || relay_host.empty())return;
        relay_connecting=true;const auto host=relay_host;const auto port=relay_port;const auto generation=relay_generation;
        connectors.emplace_back([this,host,port,generation]{
            SOCKET s=INVALID_SOCKET;std::string error="address not found";
            addrinfo hints{};hints.ai_socktype=SOCK_STREAM;hints.ai_protocol=IPPROTO_TCP;addrinfo* list=nullptr;
            if(getaddrinfo(host.c_str(),std::to_string(port).c_str(),&hints,&list))list=nullptr;
            for(auto* a=list;a && s==INVALID_SOCKET;a=a->ai_next){
                SOCKET t=socket(a->ai_family,SOCK_STREAM,IPPROTO_TCP);if(t==INVALID_SOCKET)continue;nonblocking(t);
                if(connect(t,a->ai_addr,int(a->ai_addrlen)) && !would_block()){closesocket(t);continue;}
                fd_set w,e;FD_ZERO(&w);FD_ZERO(&e);FD_SET(t,&w);FD_SET(t,&e);timeval wait{6,0};
                if(select(0,nullptr,&w,&e,&wait)>0 && FD_ISSET(t,&w) && !FD_ISSET(t,&e)){s=t;break;}
                closesocket(t);error="no answer (closed port or firewall)";
            }
            if(list)freeaddrinfo(list);
            if(s!=INVALID_SOCKET){BOOL on=TRUE;setsockopt(s,IPPROTO_TCP,TCP_NODELAY,reinterpret_cast<const char*>(&on),sizeof on);}
            std::lock_guard lock(mutex);relay_connecting=false;
            if(generation!=relay_generation || stop.load()){if(s!=INVALID_SOCKET)closesocket(s);return;}
            if(s==INVALID_SOCKET){relay_error=error;relay_retry=now_us()+3000000;return;}
            relay={};relay.socket=s;relay_error.clear();send_beacon();
        });
    }
    void run_relay(){
        if(relay.socket==INVALID_SOCKET){if(!relay_host.empty() && now_us()>=relay_retry)start_relay_connect();return;}
        const bool alive=pump(relay,[&](char type,const Mac&,const Mac& from,const std::uint8_t* p,std::size_t n,const std::uint8_t*,std::size_t){on_relay_frame(type,from,p,n);});
        if(!alive || !flush(relay)){closesocket(relay.socket);relay={};relay_retry=now_us()+2000000;relay_error="the connection to the room was lost";
            for(auto it=peers.begin();it!=peers.end();)if(it->second.relayed)it=peers.erase(it);else ++it;}
    }
    Ptp* relayed_stream(const Mac& opener,std::uint32_t stream,const Mac& from){
        for(auto& [id,p]:ptp)if(p.relayed && p.opener==opener && p.stream==stream && p.peer==from)return &p;
        return nullptr;
    }
    void on_relay_frame(char type,const Mac& from,const std::uint8_t* p,std::size_t n){
        switch(type){
        case 'B':{sockaddr_in none{};on_beacon(p,int(n),none,true);break;}
        case 'D':on_data(p,int(n));break;
        case 'O':{ // open: stream, opener's port, listener's port
            if(n<8)break;const auto stream=get32(p);const auto from_port=get16(p+4),to_port=get16(p+6);
            Ptp* target=nullptr;for(auto& [id,q]:ptp)if(q.kind==PtpKind::Listen && q.port==to_port){target=&q;break;}
            if(target){Pending e;e.accepted.peer=from;e.accepted.port=from_port;e.relayed=true;e.opener=from;e.stream=stream;target->backlog.push_back(e);}
            std::vector<std::uint8_t> answer;put32(answer,stream);answer.push_back(target?1:0);relay_send('A',from,answer);break;}
        case 'A':{
            if(n<5)break;if(auto* q=relayed_stream(mac,get32(p),from);q && q->kind==PtpKind::Connecting)q->kind=p[4]?PtpKind::Established:PtpKind::Closed;break;}
        case 'S':case 'C':{
            if(n<10)break;Mac opener;std::copy_n(p,6,opener.begin());
            if(auto* q=relayed_stream(opener,get32(p+6),from)){if(type=='S')q->rx.insert(q->rx.end(),p+10,p+n);else q->remote_closed=true;}
            break;}
        }
    }
    void send_stream(const Ptp& p,char type,const std::uint8_t* d,std::size_t size){
        std::vector<std::uint8_t> b(p.opener.begin(),p.opener.end());put32(b,p.stream);if(size)b.insert(b.end(),d,d+size);relay_send(type,p.peer,b);
    }

    std::vector<std::uint8_t> beacon()const{
        std::vector<std::uint8_t> b{'K','O','N','B',1,std::uint8_t(role)};
        for(unsigned n=0;n<10;++n)b.push_back(n<product.size()?std::uint8_t(product[n]):0);
        b.insert(b.end(),mac.begin(),mac.end());put16(b,data_port);put16(b,tcp_port);
        b.insert(b.end(),group.begin(),group.end());
        for(unsigned n=0;n<32;++n)b.push_back(n<nickname.size()?std::uint8_t(nickname[n]):0);
        return b;
    }
    void send_beacon(){
        const auto b=beacon();
        relay_send('B',kBroadcastMac,b);
        if(relay_only){last_beacon=now_us();return;}
        for(unsigned n=0;n<kInstances;++n){
            const auto port=std::uint16_t(discovery_base+n);if(port==discovery_port)continue;
            sockaddr_in to{};to.sin_family=AF_INET;to.sin_port=htons(port);
            inet_pton(AF_INET,"127.0.0.1",&to.sin_addr); // instances on this PC
            sendto(discovery,reinterpret_cast<const char*>(b.data()),int(b.size()),0,reinterpret_cast<sockaddr*>(&to),sizeof to);
            if(lan){inet_pton(AF_INET,kMulticastGroup,&to.sin_addr);sendto(discovery,reinterpret_cast<const char*>(b.data()),int(b.size()),0,reinterpret_cast<sockaddr*>(&to),sizeof to);}
        }
        last_beacon=now_us();
    }
    void on_beacon(const std::uint8_t* p,int size,const sockaddr_in& from,bool relayed=false){
        if(size<66 || std::memcmp(p,"KONB",4) || p[4]!=1 || (relay_only && !relayed))return;
        PeerInfo info;info.role=Role(p[5]);
        info.product.assign(reinterpret_cast<const char*>(p+6),strnlen(reinterpret_cast<const char*>(p+6),10));
        std::copy_n(p+16,6,info.mac.begin());if(info.mac==mac)return;
        if(!product.empty() && !info.product.empty() && info.product!=product)return;
        info.data_port=get16(p+22);info.tcp_port=get16(p+24);std::copy_n(p+26,8,info.group.begin());
        info.nickname.assign(reinterpret_cast<const char*>(p+34),strnlen(reinterpret_cast<const char*>(p+34),32));
        info.address=from;info.seen=now_us();info.relayed=relayed;
        // A peer heard both directly (same LAN) and through the room uses the
        // room: its link works both ways even when a firewall drops the
        // direct datagrams in one direction (seen on a first LAN game).
        if(const auto it=peers.find(info.mac);!relayed && it!=peers.end() && it->second.relayed && info.seen-it->second.seen<kPeerTimeoutUs){it->second.seen=info.seen;return;}
        // Prefer a LAN address over loopback once both were heard.
        if(const auto it=peers.find(info.mac);it!=peers.end() && from.sin_addr.s_addr==htonl(INADDR_LOOPBACK) && it->second.address.sin_addr.s_addr!=htonl(INADDR_LOOPBACK))info.address=it->second.address;
        peers[info.mac]=info;
    }
    void on_data(const std::uint8_t* p,int size){
        if(size<14 || std::memcmp(p,"KOND",4))return;
        Datagram d;std::copy_n(p+4,6,d.source.begin());d.port=get16(p+10);const auto to_port=get16(p+12);
        d.data.assign(p+14,p+size);
        for(auto& [id,s]:pdp)if(s.port==to_port){s.bytes+=d.data.size();s.queue.push_back(std::move(d));break;}
    }
    bool same_group(const PeerInfo& p)const{return role!=Role::None && p.role!=Role::None && p.group==group;}
    void update(){
        const auto now=now_us();
        for(auto it=peers.begin();it!=peers.end();)if(now-it->second.seen>kPeerTimeoutUs)it=peers.erase(it);else ++it;
        if(role==Role::Member && !connect_announced)
            for(const auto& [m,p]:peers)if(p.role==Role::Host && p.group==group){connect_announced=true;events.push_back({Event::Connect,0});break;}
        if(scanning && now-scan_started>=kScanUs){
            scanning=false;scanned.clear();
            for(const auto& [m,p]:peers)if(p.role==Role::Host)scanned.push_back({p.group,p.mac});
            events.push_back({Event::Scan,0});
        }
        if(now-last_beacon>=kBeaconUs)send_beacon();
        process_incoming();
        run_hub();run_relay();
    }
    // TCP connections to this instance: read the hello, hand them to the
    // listener of that PTP port (answer 1) or refuse them (answer 0).
    void process_incoming(){
        for(;;){
            sockaddr_in from{};int length=sizeof from;const SOCKET s=accept(listener,reinterpret_cast<sockaddr*>(&from),&length);
            if(s==INVALID_SOCKET)break;
            nonblocking(s);BOOL on=TRUE;setsockopt(s,IPPROTO_TCP,TCP_NODELAY,reinterpret_cast<const char*>(&on),sizeof on);
            incoming.push_back({s,{},now_us()});
        }
        for(auto it=incoming.begin();it!=incoming.end();){
            std::uint8_t buffer[14];const int want=14-int(it->hello.size());
            const int got=recv(it->socket,reinterpret_cast<char*>(buffer),want,0);
            if(got>0)it->hello.insert(it->hello.end(),buffer,buffer+got);
            const bool failed=got==0 || (got<0 && !would_block()) || now_us()-it->since>5000000;
            if(failed){closesocket(it->socket);it=incoming.erase(it);continue;}
            if(it->hello.size()<14){++it;continue;}
            Accepted a;bool valid=!std::memcmp(it->hello.data(),"KONP",4);
            std::copy_n(it->hello.data()+4,6,a.peer.begin());a.port=get16(it->hello.data()+10);const auto to_port=get16(it->hello.data()+12);
            Ptp* target=nullptr;for(auto& [id,p]:ptp)if(p.kind==PtpKind::Listen && p.port==to_port){target=&p;break;}
            const char answer=valid && target?1:0;send(it->socket,&answer,1,0);
            if(answer){Pending e;e.socket=it->socket;e.accepted=a;target->backlog.push_back(e);}else closesocket(it->socket);
            it=incoming.erase(it);
        }
    }
    void run(){
        std::vector<std::uint8_t> buffer(65536);
        while(!stop.load()){
            fd_set read;FD_ZERO(&read);FD_SET(discovery,&read);FD_SET(data,&read);
            timeval wait{0,20000};select(0,&read,nullptr,nullptr,&wait);
            std::lock_guard lock(mutex);
            for(const SOCKET s:{discovery,data})for(;;){
                sockaddr_in from{};int length=sizeof from;
                const int got=recvfrom(s,reinterpret_cast<char*>(buffer.data()),int(buffer.size()),0,reinterpret_cast<sockaddr*>(&from),&length);
                if(got<=0)break;
                if(s==discovery)on_beacon(buffer.data(),got,from);else on_data(buffer.data(),got);
            }
            update();
        }
    }
    const PeerInfo* peer(const Mac& m)const{const auto it=peers.find(m);return it==peers.end()?nullptr:&it->second;}
    void send_pdp(const PeerInfo& p,std::uint16_t from_port,std::uint16_t to_port,const std::uint8_t* d,std::size_t size){
        std::vector<std::uint8_t> b{'K','O','N','D'};b.insert(b.end(),mac.begin(),mac.end());put16(b,from_port);put16(b,to_port);b.insert(b.end(),d,d+size);
        if(p.relayed){relay_send('D',p.mac,b);return;}
        sockaddr_in to=p.address;to.sin_port=htons(p.data_port);
        sendto(data,reinterpret_cast<const char*>(b.data()),int(b.size()),0,reinterpret_cast<sockaddr*>(&to),sizeof to);
    }
    void close_ptp(Ptp& p){
        if(p.socket!=INVALID_SOCKET){closesocket(p.socket);p.socket=INVALID_SOCKET;}
        if(p.relayed && p.kind==PtpKind::Established && !p.remote_closed)send_stream(p,'C',nullptr,0);
        for(auto& e:p.backlog){if(e.socket!=INVALID_SOCKET)closesocket(e.socket);else if(e.relayed){Ptp refused;refused.relayed=true;refused.opener=e.opener;refused.stream=e.stream;refused.peer=e.accepted.peer;send_stream(refused,'C',nullptr,0);}}
        p.backlog.clear();p.kind=PtpKind::Closed;
    }
};

AdhocNet::AdhocNet(std::string nickname,Options options):impl_(std::make_unique<Impl>()){
    auto& i=*impl_;i.nickname=std::move(nickname);i.discovery_base=options.discovery_base;i.lan=options.lan;i.relay_only=options.relay_only;
    if(options.mac)i.mac=*options.mac;
    else{
        Mac base{0x02,0x4B,0x4F,0x4E,0x00,0x00};
        try{if(const auto host=host_ethernet_address())base=*host;}catch(...){}
        const auto pid=GetCurrentProcessId();
        i.mac=base;i.mac[0]=std::uint8_t((base[0]|0x02)&0xFE);i.mac[3]^=std::uint8_t(pid>>16);i.mac[4]^=std::uint8_t(pid>>8);i.mac[5]^=std::uint8_t(pid);
    }
    WSADATA wsa{};if(WSAStartup(MAKEWORD(2,2),&wsa))return;i.wsa=true;
    i.discovery=socket(AF_INET,SOCK_DGRAM,IPPROTO_UDP);i.data=socket(AF_INET,SOCK_DGRAM,IPPROTO_UDP);i.listener=socket(AF_INET,SOCK_STREAM,IPPROTO_TCP);
    if(i.discovery==INVALID_SOCKET || i.data==INVALID_SOCKET || i.listener==INVALID_SOCKET)return;
    // One discovery port per instance (the first free one of the range).
    sockaddr_in any{};any.sin_family=AF_INET;any.sin_addr.s_addr=htonl(i.lan?INADDR_ANY:INADDR_LOOPBACK);
    for(unsigned n=0;n<kInstances && !i.discovery_port;++n){any.sin_port=htons(std::uint16_t(i.discovery_base+n));if(!bind(i.discovery,reinterpret_cast<sockaddr*>(&any),sizeof any))i.discovery_port=std::uint16_t(i.discovery_base+n);}
    if(!i.discovery_port)return;
    if(i.lan){
        ip_mreq group{};inet_pton(AF_INET,kMulticastGroup,&group.imr_multiaddr);group.imr_interface.s_addr=htonl(INADDR_ANY);
        setsockopt(i.discovery,IPPROTO_IP,IP_ADD_MEMBERSHIP,reinterpret_cast<const char*>(&group),sizeof group);
        DWORD ttl=1;setsockopt(i.discovery,IPPROTO_IP,IP_MULTICAST_TTL,reinterpret_cast<const char*>(&ttl),sizeof ttl);
    }
    any.sin_port=0;
    if(bind(i.data,reinterpret_cast<sockaddr*>(&any),sizeof any) || bind(i.listener,reinterpret_cast<sockaddr*>(&any),sizeof any) || listen(i.listener,SOMAXCONN))return;
    sockaddr_in bound{};int length=sizeof bound;getsockname(i.data,reinterpret_cast<sockaddr*>(&bound),&length);i.data_port=ntohs(bound.sin_port);
    length=sizeof bound;getsockname(i.listener,reinterpret_cast<sockaddr*>(&bound),&length);i.tcp_port=ntohs(bound.sin_port);
    for(const SOCKET s:{i.discovery,i.data,i.listener})nonblocking(s);
    i.ready=true;i.thread=std::thread([&i]{i.run();});
}
AdhocNet::~AdhocNet(){
    auto& i=*impl_;i.stop.store(true);if(i.thread.joinable())i.thread.join();
    for(auto& t:i.connectors)if(t.joinable())t.join();
    for(auto& [id,p]:i.ptp)i.close_ptp(p);for(auto& in:i.incoming)closesocket(in.socket);
    if(i.relay.socket!=INVALID_SOCKET){Impl::flush(i.relay);closesocket(i.relay.socket);}
    for(auto& c:i.hub_clients)closesocket(c.socket);if(i.hub!=INVALID_SOCKET)closesocket(i.hub);
    for(const SOCKET s:{i.discovery,i.data,i.listener})if(s!=INVALID_SOCKET)closesocket(s);
    if(i.wsa)WSACleanup();
}
const Mac& AdhocNet::mac()const{return impl_->mac;}
bool AdhocNet::ok()const{return impl_->ready;}
void AdhocNet::set_product(const std::string& code){std::lock_guard lock(impl_->mutex);impl_->product=code.substr(0,10);}
void AdhocNet::scan(){std::lock_guard lock(impl_->mutex);impl_->scanning=true;impl_->scan_started=now_us();}
void AdhocNet::create(const std::array<char,8>& name){
    std::lock_guard lock(impl_->mutex);auto& i=*impl_;i.role=Impl::Role::Host;i.group=name;i.connect_announced=true;i.events.push_back({Event::Connect,0});i.send_beacon();
}
void AdhocNet::join(const std::array<char,8>& name){
    std::lock_guard lock(impl_->mutex);auto& i=*impl_;i.role=Impl::Role::Member;i.group=name;i.connect_announced=false;i.send_beacon();
}
void AdhocNet::disconnect(){
    std::lock_guard lock(impl_->mutex);auto& i=*impl_;const bool was=i.role!=Impl::Role::None;i.role=Impl::Role::None;i.group={};i.connect_announced=false;
    if(was)i.events.push_back({Event::Disconnect,0});i.send_beacon();
}
bool AdhocNet::connected()const{std::lock_guard lock(impl_->mutex);return impl_->role!=Impl::Role::None && impl_->connect_announced;}
std::array<char,8> AdhocNet::group()const{std::lock_guard lock(impl_->mutex);return impl_->group;}
bool AdhocNet::hosted(const std::array<char,8>& name)const{
    std::lock_guard lock(impl_->mutex);
    for(const auto& [m,p]:impl_->peers)if(p.role==Impl::Role::Host && p.group==name)return true;
    return false;
}
std::vector<AdhocNet::Group> AdhocNet::groups()const{std::lock_guard lock(impl_->mutex);return impl_->scanned;}
std::vector<AdhocNet::Peer> AdhocNet::peers()const{
    std::lock_guard lock(impl_->mutex);std::vector<Peer> out;
    for(const auto& [m,p]:impl_->peers)if(impl_->same_group(p))out.push_back({m,p.nickname,p.seen});
    return out;
}
std::vector<std::pair<AdhocNet::Event,int>> AdhocNet::take_events(){
    std::lock_guard lock(impl_->mutex);std::vector<std::pair<Event,int>> out(impl_->events.begin(),impl_->events.end());impl_->events.clear();return out;
}
int AdhocNet::pdp_create(std::uint16_t port){
    std::lock_guard lock(impl_->mutex);auto& i=*impl_;
    if(!port){port=0x8000;while(std::any_of(i.pdp.begin(),i.pdp.end(),[&](const auto& e){return e.second.port==port;}))++port;}
    for(const auto& [id,s]:i.pdp)if(s.port==port)return -int(Status::PortInUse);
    const int id=i.next_id++;i.pdp[id].port=port;return id;
}
AdhocNet::Status AdhocNet::pdp_send(int id,const Mac& to,std::uint16_t port,const std::uint8_t* data,std::size_t size){
    std::lock_guard lock(impl_->mutex);auto& i=*impl_;const auto it=i.pdp.find(id);if(it==i.pdp.end())return Status::InvalidId;
    if(to==i.mac){Datagram d;d.source=i.mac;d.port=it->second.port;d.data.assign(data,data+size);
        for(auto& [n,s]:i.pdp)if(s.port==port){s.bytes+=d.data.size();s.queue.push_back(std::move(d));break;}return Status::Ok;}
    if(to==kBroadcastMac){
        for(const auto& [m,p]:i.peers)if(i.same_group(p))i.send_pdp(p,it->second.port,port,data,size);
        // Not delivered to the sender, as on the PSP: the game treats an echo
        // of its own band claim as a conflicting claim.
        return Status::Ok;
    }
    if(const auto* p=i.peer(to))i.send_pdp(*p,it->second.port,port,data,size);
    return Status::Ok; // a datagram to an unknown address is lost, as on the PSP
}
AdhocNet::Status AdhocNet::pdp_recv(int id,Datagram& out){
    std::lock_guard lock(impl_->mutex);auto& i=*impl_;const auto it=i.pdp.find(id);if(it==i.pdp.end())return Status::InvalidId;
    auto& s=it->second;if(s.queue.empty())return Status::WouldBlock;
    out=std::move(s.queue.front());s.queue.pop_front();s.bytes-=out.data.size();return Status::Ok;
}
std::size_t AdhocNet::pdp_pending(int id)const{std::lock_guard lock(impl_->mutex);const auto it=impl_->pdp.find(id);return it==impl_->pdp.end()?0:it->second.bytes;}
void AdhocNet::pdp_delete(int id){std::lock_guard lock(impl_->mutex);impl_->pdp.erase(id);}
int AdhocNet::ptp_open(std::uint16_t port,const Mac& to,std::uint16_t to_port){
    std::lock_guard lock(impl_->mutex);auto& i=*impl_;const int id=i.next_id++;auto& p=i.ptp[id];
    p.kind=Impl::PtpKind::Open;p.port=port?port:std::uint16_t(0x9000+id);p.peer=to;p.peer_port=to_port;return id;
}
int AdhocNet::ptp_listen(std::uint16_t port){
    std::lock_guard lock(impl_->mutex);auto& i=*impl_;
    for(const auto& [n,p]:i.ptp)if(p.kind==Impl::PtpKind::Listen && p.port==port)return -int(Status::PortInUse);
    const int id=i.next_id++;auto& p=i.ptp[id];p.kind=Impl::PtpKind::Listen;p.port=port;return id;
}
AdhocNet::Status AdhocNet::ptp_connect(int id){
    std::lock_guard lock(impl_->mutex);auto& i=*impl_;const auto it=i.ptp.find(id);if(it==i.ptp.end())return Status::InvalidId;
    auto& p=it->second;
    switch(p.kind){
    case Impl::PtpKind::Established:return Status::Ok;
    case Impl::PtpKind::Closed:return Status::Refused;
    case Impl::PtpKind::Listen:return Status::InvalidId;
    case Impl::PtpKind::Open:{
        const auto* peer=i.peer(p.peer);if(!peer)return Status::WouldBlock; // not heard yet
        if(peer->relayed){
            p.relayed=true;p.opener=i.mac;p.stream=std::uint32_t(id);p.kind=Impl::PtpKind::Connecting;
            std::vector<std::uint8_t> open;put32(open,p.stream);put16(open,p.port);put16(open,p.peer_port);i.relay_send('O',p.peer,open);
            return Status::WouldBlock;}
        p.socket=socket(AF_INET,SOCK_STREAM,IPPROTO_TCP);if(p.socket==INVALID_SOCKET)return Status::Refused;
        nonblocking(p.socket);BOOL on=TRUE;setsockopt(p.socket,IPPROTO_TCP,TCP_NODELAY,reinterpret_cast<const char*>(&on),sizeof on);
        sockaddr_in to=peer->address;to.sin_port=htons(peer->tcp_port);
        if(connect(p.socket,reinterpret_cast<sockaddr*>(&to),sizeof to) && !would_block()){i.close_ptp(p);return Status::Refused;}
        p.kind=Impl::PtpKind::Connecting;p.hello_sent=false;return Status::WouldBlock;}
    case Impl::PtpKind::Connecting:{
        if(p.relayed)return Status::WouldBlock; // until the listener answers through the relay
        if(!p.hello_sent){
            fd_set write,error;FD_ZERO(&write);FD_ZERO(&error);FD_SET(p.socket,&write);FD_SET(p.socket,&error);timeval zero{0,0};
            if(select(0,nullptr,&write,&error,&zero)<=0)return Status::WouldBlock;
            if(FD_ISSET(p.socket,&error)){i.close_ptp(p);return Status::Refused;}
            std::vector<std::uint8_t> hello{'K','O','N','P'};hello.insert(hello.end(),i.mac.begin(),i.mac.end());put16(hello,p.port);put16(hello,p.peer_port);
            if(send(p.socket,reinterpret_cast<const char*>(hello.data()),int(hello.size()),0)!=int(hello.size())){i.close_ptp(p);return Status::Refused;}
            p.hello_sent=true;
        }
        char answer=0;const int got=recv(p.socket,&answer,1,0);
        if(got<0 && would_block())return Status::WouldBlock;
        if(got!=1 || answer!=1){i.close_ptp(p);return Status::Refused;}
        p.kind=Impl::PtpKind::Established;return Status::Ok;}
    }
    return Status::Refused;
}
AdhocNet::Status AdhocNet::ptp_accept(int id,Accepted& out){
    std::lock_guard lock(impl_->mutex);auto& i=*impl_;i.process_incoming();
    const auto it=i.ptp.find(id);if(it==i.ptp.end() || it->second.kind!=Impl::PtpKind::Listen)return Status::InvalidId;
    auto& backlog=it->second.backlog;if(backlog.empty())return Status::WouldBlock;
    const auto e=backlog.front();backlog.pop_front();const auto port=it->second.port;
    const int nid=i.next_id++;auto& p=i.ptp[nid];p.kind=Impl::PtpKind::Established;p.socket=e.socket;p.port=port;p.peer=e.accepted.peer;p.peer_port=e.accepted.port;
    p.relayed=e.relayed;p.opener=e.opener;p.stream=e.stream;
    out=e.accepted;out.id=nid;return Status::Ok;
}
AdhocNet::Status AdhocNet::ptp_send(int id,const std::uint8_t* data,std::size_t& size){
    std::lock_guard lock(impl_->mutex);auto& i=*impl_;const auto it=i.ptp.find(id);if(it==i.ptp.end())return Status::InvalidId;
    auto& p=it->second;if(p.kind!=Impl::PtpKind::Established){size=0;return p.kind==Impl::PtpKind::Closed?Status::Closed:Status::WouldBlock;}
    if(p.relayed){if(p.remote_closed){size=0;return Status::Closed;}size=std::min<std::size_t>(size,60000);i.send_stream(p,'S',data,size);return Status::Ok;}
    const int sent=send(p.socket,reinterpret_cast<const char*>(data),int(std::min<std::size_t>(size,1u<<20)),0);
    if(sent<0){size=0;if(would_block())return Status::WouldBlock;i.close_ptp(p);return Status::Closed;}
    size=std::size_t(sent);return Status::Ok;
}
AdhocNet::Status AdhocNet::ptp_recv(int id,std::uint8_t* data,std::size_t& size){
    std::lock_guard lock(impl_->mutex);auto& i=*impl_;const auto it=i.ptp.find(id);if(it==i.ptp.end())return Status::InvalidId;
    auto& p=it->second;if(p.kind!=Impl::PtpKind::Established){size=0;return p.kind==Impl::PtpKind::Closed?Status::Closed:Status::WouldBlock;}
    if(p.relayed){
        if(p.rx.empty()){size=0;return p.remote_closed?Status::Closed:Status::WouldBlock;}
        size=std::min(size,p.rx.size());std::copy_n(p.rx.begin(),size,data);p.rx.erase(p.rx.begin(),p.rx.begin()+std::ptrdiff_t(size));return Status::Ok;}
    const int got=recv(p.socket,reinterpret_cast<char*>(data),int(std::min<std::size_t>(size,1u<<20)),0);
    if(got>0){size=std::size_t(got);return Status::Ok;}
    size=0;if(got<0 && would_block())return Status::WouldBlock;
    i.close_ptp(p);return Status::Closed;
}
AdhocNet::Status AdhocNet::ptp_flush(int id){
    std::lock_guard lock(impl_->mutex);const auto it=impl_->ptp.find(id);if(it==impl_->ptp.end())return Status::InvalidId;
    return it->second.kind==Impl::PtpKind::Closed?Status::Closed:Status::Ok; // TCP sends at once (TCP_NODELAY)
}
void AdhocNet::ptp_close(int id){
    std::lock_guard lock(impl_->mutex);const auto it=impl_->ptp.find(id);if(it==impl_->ptp.end())return;
    impl_->close_ptp(it->second);impl_->ptp.erase(it);
}
bool AdhocNet::online_host(std::uint16_t port){
    std::lock_guard lock(impl_->mutex);auto& i=*impl_;
    if(i.hub==INVALID_SOCKET){
        // Dual stack: IPv6 and IPv4 players reach the same port. This PC only
        // (lan=false: tests) listens on 127.0.0.1, with no firewall prompt.
        SOCKET s=socket(i.lan?AF_INET6:AF_INET,SOCK_STREAM,IPPROTO_TCP);if(s==INVALID_SOCKET){i.relay_error="no network";return false;}
        DWORD off=0;if(i.lan)setsockopt(s,IPPROTO_IPV6,IPV6_V6ONLY,reinterpret_cast<const char*>(&off),sizeof off);
        sockaddr_in6 any{};any.sin6_family=AF_INET6;any.sin6_port=htons(port);
        sockaddr_in loop{};loop.sin_family=AF_INET;loop.sin_port=htons(port);loop.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
        const bool bound=i.lan?!bind(s,reinterpret_cast<sockaddr*>(&any),sizeof any):!bind(s,reinterpret_cast<sockaddr*>(&loop),sizeof loop);
        if(!bound || listen(s,SOMAXCONN)){closesocket(s);i.relay_error="port "+std::to_string(port)+" is in use";return false;}
        nonblocking(s);i.hub=s;i.hub_port=port;
    }
    i.relay_host="127.0.0.1";i.relay_port=port;++i.relay_generation;i.relay_retry=0;
    if(i.relay.socket!=INVALID_SOCKET){closesocket(i.relay.socket);i.relay={};}
    i.start_relay_connect();return true;
}
bool AdhocNet::online_join(const std::string& host,std::uint16_t port){
    std::lock_guard lock(impl_->mutex);auto& i=*impl_;
    if(host.empty() || !port)return false;
    i.relay_host=host;i.relay_port=port;++i.relay_generation;i.relay_retry=0;i.relay_error.clear();
    if(i.relay.socket!=INVALID_SOCKET){closesocket(i.relay.socket);i.relay={};}
    i.start_relay_connect();return true;
}
void AdhocNet::online_leave(){
    std::lock_guard lock(impl_->mutex);auto& i=*impl_;
    i.relay_host.clear();++i.relay_generation;
    if(i.relay.socket!=INVALID_SOCKET){closesocket(i.relay.socket);i.relay={};}
    for(auto& c:i.hub_clients)closesocket(c.socket);i.hub_clients.clear();
    if(i.hub!=INVALID_SOCKET){closesocket(i.hub);i.hub=INVALID_SOCKET;i.hub_port=0;}
    for(auto it=i.peers.begin();it!=i.peers.end();)if(it->second.relayed)it=i.peers.erase(it);else ++it;
}
AdhocNet::OnlineState AdhocNet::online()const{
    std::lock_guard lock(impl_->mutex);const auto& i=*impl_;OnlineState s;
    s.hosting=i.hub!=INVALID_SOCKET;s.linked=i.relay.socket!=INVALID_SOCKET;s.port=i.hub_port?i.hub_port:i.relay_port;s.error=i.relay_error;
    if(s.linked){s.players=1;for(const auto& [m,p]:i.peers)if(p.relayed)++s.players;}
    return s;
}
std::string AdhocNet::describe()const{
    std::lock_guard lock(impl_->mutex);const auto& i=*impl_;std::ostringstream out;
    out<<"ready="<<i.ready<<" lan="<<i.lan<<" discovery_port="<<i.discovery_port<<" role="<<int(i.role)<<" group="<<std::string(i.group.data(),strnlen(i.group.data(),8))<<" peers="<<i.peers.size()<<" pdp="<<i.pdp.size()<<" ptp="<<i.ptp.size()<<" data_port="<<i.data_port<<" tcp_port="<<i.tcp_port<<" relay="<<(i.relay.socket!=INVALID_SOCKET)<<" hub_clients="<<i.hub_clients.size();
    return out.str();
}
}
