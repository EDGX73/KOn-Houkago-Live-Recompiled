#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <vector>
// Online rooms (see the relay in native_adhoc.hpp):
//   * room codes: the host's address and port in a short code to pass to a
//     friend (IPv4: 10 characters, IPv6: 29), Crockford base 32 so that
//     O/0 and I/L/1 do not matter; "address:port" and names are accepted too;
//   * the host's addresses (LAN IPv4, global IPv6);
//   * UPnP: asks the home router to forward the room's TCP port to this PC
//     (a lease of a few hours, removed when the room closes) and for the
//     router's public address. Nothing else is contacted.
namespace kon {
struct RoomAddress {std::string host;std::uint16_t port=0;};
std::string room_code(const std::string& address,std::uint16_t port); // empty if the address is not numeric
std::optional<RoomAddress> parse_room_code(const std::string& text);
// vpn: addresses of game VPN adapters (Radmin VPN, ZeroTier, Tailscale,
// Hamachi), with the VPN's name; they are not listed in ipv4.
struct LocalAddresses {std::vector<std::string> ipv4,ipv6_global;std::vector<std::pair<std::string,std::string>> vpn;};
LocalAddresses local_addresses();
bool private_ipv4(const std::string& address); // 10/8, 172.16/12, 192.168/16, 100.64/10 (carrier NAT), 169.254/16, 127/8
// --- UPnP internals, exposed for tests ---
std::optional<std::string> ssdp_location(const std::string& response);
struct UpnpService {std::string type,control_url;};
std::optional<UpnpService> upnp_wan_service(const std::string& description,const std::string& location);
std::string xml_value(const std::string& xml,const std::string& tag);
class UpnpMapping {
public:
    // Discovers the router (about 2 s) and maps `port` (TCP) to this PC.
    static UpnpMapping open(std::uint16_t port);
    UpnpMapping()=default;UpnpMapping(UpnpMapping&&)noexcept;UpnpMapping& operator=(UpnpMapping&&)noexcept;
    ~UpnpMapping();
    bool mapped=false;std::string external_ip,internal_ip,router,error;
    void close(); // removes the mapping
private:
    std::string control_url_,service_;std::uint16_t port_=0;
};
}
