#pragma once
#include <cstdint>
#include <span>
namespace kon {
struct NativeGpuClearRectangle {
    unsigned left,top,right,bottom;
    std::uint32_t color,mask;
};
// Synchronous native backend contract: all rectangles are validated before
// mutation, overlap preserves submission order, and return means GPU completion.
// One uint32 word represents one destination pixel (including packed 16-bit).
class NativeGpuClear {
public:
    virtual ~NativeGpuClear()=default;
    virtual void clear(std::span<std::uint32_t> pixels,unsigned stride,
                       std::span<const NativeGpuClearRectangle> rectangles)=0;
};
}
