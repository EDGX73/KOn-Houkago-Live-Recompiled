#include "native_adhoc.hpp"
#include "native_online.hpp"
#include <cctype>
#include <chrono>
#include <cstring>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
// Ad hoc instances in one process, this PC only (127.0.0.1: no firewall
// prompt): discovery, scan, create/join, peer lists, PDP unicast and
// broadcast, PTP connect/accept/stream/close, disconnect events. The same
// scenario runs twice: over the local transport, and with no UDP discovery at
// all through the online relay (one instance hosts it, the other joins; a
// third joins to check the relay's broadcast).
using namespace kon;
static void check(bool ok,const char* what){if(!ok)throw std::runtime_error(what);}
static bool until(const std::function<bool()>& done,int ms=4000){
    const auto end=std::chrono::steady_clock::now()+std::chrono::milliseconds(ms);
    while(std::chrono::steady_clock::now()<end){if(done())return true;std::this_thread::sleep_for(std::chrono::milliseconds(10));}
    return done();
}
static bool has(const std::vector<std::pair<AdhocNet::Event,int>>& events,AdhocNet::Event e){for(const auto& [k,v]:events)if(k==e)return true;return false;}
static void scenario(AdhocNet& a,AdhocNet& b,const std::array<char,8>& band){
    check(a.ok() && b.ok(),"Sockets on 127.0.0.1");
    a.set_product("ULJM05709");b.set_product("ULJM05709");
    a.create(band);check(has(a.take_events(),AdhocNet::Event::Connect),"Create connects the host");
    b.scan();std::vector<std::pair<AdhocNet::Event,int>> events;
    check(until([&]{auto e=b.take_events();events.insert(events.end(),e.begin(),e.end());return has(events,AdhocNet::Event::Scan);}),"Scan finishes");
    const auto groups=b.groups();check(groups.size()==1 && groups[0].name==band && groups[0].host==a.mac(),"Scan finds the hosted group");
    b.join(band);events.clear();
    check(until([&]{auto e=b.take_events();events.insert(events.end(),e.begin(),e.end());return has(events,AdhocNet::Event::Connect);}),"Join connects once the host is heard");
    check(until([&]{return a.peers().size()==1 && b.peers().size()==1;}),"Both see each other as peers");
    check(a.peers()[0].mac==b.mac() && a.peers()[0].nickname=="guest","Peer identity");
    // PDP
    const int pa=a.pdp_create(100),pb=b.pdp_create(100);check(pa>0 && pb>0 && a.pdp_create(100)<0,"PDP create and port in use");
    const std::string text="hello band";
    check(a.pdp_send(pa,b.mac(),100,reinterpret_cast<const std::uint8_t*>(text.data()),text.size())==AdhocNet::Status::Ok,"PDP send");
    AdhocNet::Datagram d;check(until([&]{return b.pdp_recv(pb,d)==AdhocNet::Status::Ok;}),"PDP receive");
    check(d.source==a.mac() && d.port==100 && std::string(d.data.begin(),d.data.end())==text,"PDP datagram contents");
    check(b.pdp_send(pb,kBroadcastMac,100,reinterpret_cast<const std::uint8_t*>("x"),1)==AdhocNet::Status::Ok && until([&]{return a.pdp_recv(pa,d)==AdhocNet::Status::Ok;}) && d.data.size()==1,"PDP broadcast to the group");
    check(a.pdp_recv(pa,d)==AdhocNet::Status::WouldBlock,"PDP empty queue would block");
    // PTP
    const int listener=a.ptp_listen(200);check(listener>0 && a.ptp_listen(200)<0,"PTP listen");
    const int client=b.ptp_open(300,a.mac(),200);
    check(until([&]{return b.ptp_connect(client)==AdhocNet::Status::Ok;}),"PTP connect accepted by the listener");
    AdhocNet::Accepted accepted;check(until([&]{return a.ptp_accept(listener,accepted)==AdhocNet::Status::Ok;}),"PTP accept");
    check(accepted.peer==b.mac() && accepted.port==300,"PTP accepted peer");
    std::string big(100000,'k');for(std::size_t n=0;n<big.size();++n)big[n]=char('a'+n%26);std::size_t sent=0;
    check(until([&]{std::size_t n=big.size()-sent;if(n==0)return true;if(b.ptp_send(client,reinterpret_cast<const std::uint8_t*>(big.data()+sent),n)==AdhocNet::Status::Ok)sent+=n;return sent==big.size();}),"PTP send stream");
    std::string received;std::vector<std::uint8_t> buffer(4096);
    check(until([&]{std::size_t n=buffer.size();if(a.ptp_recv(accepted.id,buffer.data(),n)==AdhocNet::Status::Ok)received.append(reinterpret_cast<char*>(buffer.data()),n);return received.size()==big.size();}),"PTP receive stream");
    check(received==big,"PTP stream contents in order");
    std::size_t reply_size=5;check(a.ptp_send(accepted.id,reinterpret_cast<const std::uint8_t*>("reply"),reply_size)==AdhocNet::Status::Ok,"PTP reply");
    std::string reply;check(until([&]{std::size_t n=buffer.size();if(b.ptp_recv(client,buffer.data(),n)==AdhocNet::Status::Ok)reply.append(reinterpret_cast<char*>(buffer.data()),n);return reply=="reply";}),"PTP stream in the other direction");
    const int stranger=b.ptp_open(301,a.mac(),999);
    check(until([&]{return b.ptp_connect(stranger)==AdhocNet::Status::Refused;}),"PTP connect without listener is refused");
    b.ptp_close(client);
    check(until([&]{std::size_t n=buffer.size();return a.ptp_recv(accepted.id,buffer.data(),n)==AdhocNet::Status::Closed;}),"PTP close seen by the other side");
    a.ptp_close(accepted.id);a.ptp_close(listener);a.pdp_delete(pa);b.pdp_delete(pb);
}
static void room_codes(){
    const auto v4=room_code("203.0.113.45",41700);check(v4.size()==11 && v4[5]=='-',"IPv4 room code: 10 symbols");
    const auto back=parse_room_code(v4);check(back && back->host=="203.0.113.45" && back->port==41700,"IPv4 code round trip");
    std::string sloppy=v4;for(auto& c:sloppy)c=char(std::tolower(static_cast<unsigned char>(c)));sloppy.erase(5,1);
    for(auto& c:sloppy){if(c=='0')c='o';if(c=='1')c='l';}
    const auto lenient=parse_room_code("  "+sloppy+" ");check(lenient && lenient->host=="203.0.113.45","Codes ignore case, spaces, dashes and O/0, I/L/1");
    const auto v6=room_code("2001:db8::1",41700);const auto v6back=parse_room_code(v6);check(v6back && v6back->host=="2001:db8::1" && v6back->port==41700,"IPv6 code round trip");
    check(parse_room_code("192.168.1.20:41700")->host=="192.168.1.20" && parse_room_code("[2001:db8::2]:5000")->port==5000 && parse_room_code("amigo.example:41700")->host=="amigo.example","Plain addresses");
    check(!parse_room_code("ABCDE") && !parse_room_code("") && !parse_room_code("ABCDE-UUUUU"),"Bad codes rejected");
    check(private_ipv4("192.168.1.1") && private_ipv4("10.0.0.1") && private_ipv4("172.20.0.1") && private_ipv4("100.72.0.1") && !private_ipv4("203.0.113.45") && !private_ipv4("172.32.0.1"),"Private and carrier-NAT addresses");
    check(ssdp_location("HTTP/1.1 200 OK\r\nCACHE-CONTROL: max-age=120\r\nLocation: http://192.168.1.1:5000/rootDesc.xml\r\nST: x\r\n\r\n")=="http://192.168.1.1:5000/rootDesc.xml","SSDP location");
    const std::string description="<root><device><serviceList><service><serviceType>urn:schemas-upnp-org:service:Layer3Forwarding:1</serviceType><controlURL>/ctl/L3F</controlURL></service>"
        "<service><serviceType>urn:schemas-upnp-org:service:WANIPConnection:1</serviceType><serviceId>x</serviceId><controlURL>/ctl/IPConn</controlURL></service></serviceList></device></root>";
    const auto service=upnp_wan_service(description,"http://192.168.1.1:5000/rootDesc.xml");
    check(service && service->type=="urn:schemas-upnp-org:service:WANIPConnection:1" && service->control_url=="http://192.168.1.1:5000/ctl/IPConn","UPnP WAN service and control URL");
    check(xml_value("<s:Body><u:GetExternalIPAddressResponse><NewExternalIPAddress>203.0.113.45</NewExternalIPAddress>","NewExternalIPAddress")=="203.0.113.45","External address answer");
}
int main(){try{
    room_codes();
    const std::array<char,8> band{'K','O','N','B','A','N','D','1'};
    {
        AdhocNet::Options options;options.discovery_base=41620;options.lan=false;
        options.mac=Mac{0x02,0,0,0,0,0x01};AdhocNet a("host",options);
        options.mac=Mac{0x02,0,0,0,0,0x02};AdhocNet b("guest",options);
        scenario(a,b,band);
        b.disconnect();check(has(b.take_events(),AdhocNet::Event::Disconnect),"Disconnect event");
        check(until([&]{return a.peers().empty();}),"A disconnected member leaves the peer list");
    }
    {
        AdhocNet::Options options;options.discovery_base=41640;options.lan=false;options.relay_only=true;
        options.mac=Mac{0x02,0,0,0,1,0x01};AdhocNet a("host",options);
        options.mac=Mac{0x02,0,0,0,1,0x02};AdhocNet b("guest",options);
        options.mac=Mac{0x02,0,0,0,1,0x03};AdhocNet c("third",options);
        constexpr std::uint16_t port=41699;
        check(a.online_host(port),"Relay hosted");
        AdhocNet::Options other=options;other.mac=Mac{0x02,0,0,0,1,0x09};AdhocNet taken("x",other);check(!taken.online_host(port) && taken.online().error.find("en uso")!=std::string::npos,"A taken relay port is reported");
        check(b.online_join("127.0.0.1",port),"Join the relay");
        check(until([&]{return a.online().linked && b.online().linked && a.online().hosting;}),"Both linked to the relay");
        scenario(a,b,band);
        check(until([&]{return a.online().players==2;}),"Players counted through the relay");
        // A third player: broadcast through the relay reaches every member.
        c.set_product("ULJM05709");check(c.online_join("localhost",port),"Join by name");
        c.join(band);std::vector<std::pair<AdhocNet::Event,int>> events;
        check(until([&]{auto e=c.take_events();events.insert(events.end(),e.begin(),e.end());return has(events,AdhocNet::Event::Connect);}),"Third player joins the band");
        const int pa=a.pdp_create(110),pb=b.pdp_create(110),pc=c.pdp_create(110);AdhocNet::Datagram d;
        check(a.pdp_send(pa,kBroadcastMac,110,reinterpret_cast<const std::uint8_t*>("all"),3)==AdhocNet::Status::Ok,"Broadcast");
        check(until([&]{return b.pdp_recv(pb,d)==AdhocNet::Status::Ok;}) && until([&]{return c.pdp_recv(pc,d)==AdhocNet::Status::Ok;}),"Broadcast reaches both other members");
        check(a.pdp_recv(pa,d)==AdhocNet::Status::WouldBlock,"Not echoed to the sender");
        c.online_leave();check(until([&]{return !c.online().linked && a.online().players==2;},6000),"Leaving the room");
        a.online_leave();check(until([&]{return !b.online().linked;},6000),"The room closes with its host");
        check(until([&]{return b.peers().empty();},6000),"Relayed peers dropped when the room closes");
    }
    std::cout<<"PASS: ad hoc over 127.0.0.1 and through the online relay: discovery, scan, create/join, peers, PDP unicast/broadcast, PTP connect/accept/stream both ways/refuse/close, disconnect, three players, leave\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<"\n";return 1;}}
