#include "native_network.hpp"
#include "platform/net.hpp"
namespace kon {
std::optional<std::array<std::uint8_t,6>> host_ethernet_address() {
    return platform::ethernet_address();
}
}
