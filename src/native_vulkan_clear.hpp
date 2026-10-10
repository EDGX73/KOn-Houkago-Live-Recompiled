#pragma once
#include "native_gpu_clear.hpp"
#include "native_gpu_triangles.hpp"
#include "native_gpu_raster.hpp"
#include <memory>
#include <string>
#include <vector>
namespace kon {
class NativeVulkanClear final : public NativeGpuClear, public NativeGpuTriangles, public NativeGpuRaster {
public:
    struct Timings {
        std::uint64_t upload_ns=0,record_ns=0,submit_ns=0,wait_ns=0,download_ns=0,bin_ns=0;
        std::uint64_t gpu_elapsed_ns=0,timestamp_samples=0;
    };
    explicit NativeVulkanClear(bool measure=false,bool prefer_cached_reads=true,std::string adapter_selector={});
    ~NativeVulkanClear();
    NativeVulkanClear(const NativeVulkanClear&)=delete;
    NativeVulkanClear& operator=(const NativeVulkanClear&)=delete;
    void clear(std::span<std::uint32_t> pixels,unsigned stride,
               std::span<const NativeGpuClearRectangle> rectangles) override;
    NativeGpuTriangleStats triangles(std::span<std::uint32_t> pixels,unsigned stride,
        std::span<const NativeGpuFlatTriangle> triangles,unsigned left,unsigned top,
        unsigned right,unsigned bottom,std::uint32_t write_mask) override;
    void begin_batch()override;
    NativeGpuRasterBatch end_batch()override;
    void submit_batch()override;
    NativeGpuRasterBatch collect_batch()override;
    bool batch_ready()override;
    std::array<std::uint64_t,6> profile()const override;
    void set_encoded_jobs(std::span<const std::uint32_t> words)override;
    bool set_internal_scale(unsigned scale)override;
    unsigned internal_scale()const override;
    void set_hires_texture(std::span<const std::uint32_t> texels,unsigned scale,std::uint32_t token)override;
    void note_display_address(std::uint32_t address)override;
    void prepare_batch()override;
    NativeHiresView hires_view(std::uint32_t address)override;
    NativeGpuRasterStats raster(std::span<std::uint32_t> colors,std::span<std::uint32_t> depths,
        std::span<const std::uint32_t> texture,std::span<const NativeGpuRasterTriangle> jobs,
        const std::array<std::uint32_t,32>& parameters)override;
    std::uint64_t raster_submissions()const;
    bool raster_supported()const;
    std::uint64_t triangle_submissions()const;
    const std::string& adapter()const;
    std::uint64_t submissions()const;
    const Timings& timings()const;
    unsigned memory_flags()const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
