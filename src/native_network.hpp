#pragma once
#include <array>
#include <cstdint>
#include <optional>
namespace kon {
std::optional<std::array<std::uint8_t,6>> host_ethernet_address();
}
