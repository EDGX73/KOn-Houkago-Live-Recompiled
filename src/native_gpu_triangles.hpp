#pragma once
#include <array>
#include <cstdint>
#include <span>
namespace kon {
// Screen coordinates are signed 1/16 pixels. Positive winding, prevalidated
// scissor bounds and CPU-packed color; no texture, blend or depth in this stage.
struct NativeGpuFlatTriangle {
    std::array<std::int32_t,6> xy;
    std::uint32_t color,edge_mask,alpha_pass,left,top,right,bottom;
    std::array<std::uint32_t,3> reserved{};
};
static_assert(sizeof(NativeGpuFlatTriangle)==64);
struct NativeGpuTriangleStats {std::uint64_t covered=0,alpha_rejected=0;};
class NativeGpuTriangles {
public:
    static constexpr unsigned capacity_words=1024u*1024u;
    virtual ~NativeGpuTriangles()=default;
    virtual NativeGpuTriangleStats triangles(std::span<std::uint32_t> pixels,unsigned stride,
        std::span<const NativeGpuFlatTriangle> triangles,unsigned left,unsigned top,
        unsigned right,unsigned bottom,std::uint32_t write_mask)=0;
};
}
