#pragma once
#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>
// PSP ad hoc networking carried over the PC's local network, with no server:
//   * discovery: each instance owns one UDP port of a small range and sends
//     a beacon (product, virtual MAC, nickname, group, ports) to every port of
//     the range on this PC and, in LAN mode, by multicast to the network, so
//     instances on one PC or on several PCs find each other;
//   * sceNetAdhocctl groups: scan, create, join, disconnect, peer list, and
//     the events the game's handler receives;
//   * PDP datagrams over UDP; PTP streams over TCP (a hello names the PTP
//     ports, the listener answers once it accepted);
//   * online play over the Internet: one player hosts a relay (a TCP port
//     this instance opens) and the others connect to it; beacons, datagrams
//     and PTP streams (as virtual streams) travel through it in frames, so
//     the game sees the same ad hoc peers as on a LAN.
// Every call returns at once (WouldBlock when nothing is ready yet); the
// guest bindings turn that into blocking waits with the PSP's timeouts.
namespace kon {
using Mac=std::array<std::uint8_t,6>;
inline constexpr Mac kBroadcastMac{0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};
class AdhocNet {
public:
    enum class Status : int {Ok=0,WouldBlock=1,Closed=2,Refused=3,InvalidId=4,PortInUse=5,NoPeer=6};
    enum class Event : int {Error=0,Connect=1,Disconnect=2,Scan=3};
    struct Group {std::array<char,8> name{};Mac host{};};
    struct Peer {Mac mac{};std::string nickname;std::uint64_t last_seen_us=0;};
    struct Datagram {Mac source{};std::uint16_t port=0;std::vector<std::uint8_t> data;};
    struct Accepted {int id=0;Mac peer{};std::uint16_t port=0;};
    struct Options {
        std::uint16_t discovery_base=41500; // ports base..base+kInstances-1
        bool lan=true;      // false: this PC only (127.0.0.1, no firewall prompt)
        std::optional<Mac> mac; // default: host adapter and process, so instances differ
        bool relay_only=false;  // no UDP discovery: peers only through the online relay (tests)
    };
    static constexpr unsigned kInstances=8;
    AdhocNet(std::string nickname,Options options);
    explicit AdhocNet(std::string nickname):AdhocNet(std::move(nickname),Options{}){}
    ~AdhocNet();
    AdhocNet(const AdhocNet&)=delete;AdhocNet& operator=(const AdhocNet&)=delete;
    const Mac& mac()const;
    bool ok()const; // sockets ready (false: no network stack)
    // --- groups (sceNetAdhocctl) ---
    void set_product(const std::string& code);
    void scan();
    void create(const std::array<char,8>& name);  // host a group
    void join(const std::array<char,8>& name);    // join (or wait for) a group
    void disconnect();
    bool connected()const;
    std::array<char,8> group()const;
    std::vector<Group> groups()const;             // seen by the last scan
    bool hosted(const std::array<char,8>& name)const; // a host of this group is heard now
    std::vector<Peer> peers()const;               // other members of my group
    std::vector<std::pair<Event,int>> take_events();
    // --- PDP ---
    int pdp_create(std::uint16_t port);           // id > 0, or -Status
    Status pdp_send(int id,const Mac& to,std::uint16_t port,const std::uint8_t* data,std::size_t size);
    Status pdp_recv(int id,Datagram& out);
    std::size_t pdp_pending(int id)const;         // bytes waiting
    void pdp_delete(int id);
    // --- PTP ---
    int ptp_open(std::uint16_t port,const Mac& to,std::uint16_t to_port);
    int ptp_listen(std::uint16_t port);
    Status ptp_connect(int id);                   // Ok once the listener accepted
    Status ptp_accept(int id,Accepted& out);
    Status ptp_send(int id,const std::uint8_t* data,std::size_t& size); // size: bytes taken
    Status ptp_recv(int id,std::uint8_t* data,std::size_t& size);       // size: in capacity, out received
    Status ptp_flush(int id);
    void ptp_close(int id);
    // --- online play (Internet): a relay one player hosts ---
    bool online_host(std::uint16_t port);         // start the relay here and join it (false: port taken)
    bool online_join(const std::string& host,std::uint16_t port); // connect to a relay (retried until online_leave)
    void online_leave();
    struct OnlineState {bool hosting=false,linked=false;unsigned players=0;std::uint16_t port=0;std::string error;};
    OnlineState online()const;
    // Diagnostics.
    std::string describe()const;
    struct Impl;
private:
    std::unique_ptr<Impl> impl_;
};
}
