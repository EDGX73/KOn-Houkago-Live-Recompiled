#pragma once
#include <array>
#include <atomic>
#include <cstdint>
#include <span>
#include <vector>
#include <stdexcept>
namespace kon {
// Prevalidated, projected geometry. Each pixel processes its tile's triangles
// in original order; CPU geometry does not modify the guest framebuffer.
struct NativeGpuRasterTriangle {
    std::array<std::int32_t,6> xy{};
    std::array<double,24> attributes{}; // RGBA,U,V,Q,Z for each vertex
    std::array<std::uint32_t,8> state{}; // bounds, flags, flat RGBA, reserved
};
static_assert(sizeof(NativeGpuRasterTriangle)==248);
struct NativeGpuRasterStats {std::uint64_t covered=0,alpha=0,depth=0,drawn=0;bool deferred=false;};
struct NativeGpuRasterBatch {
    std::vector<std::uint32_t> colors,depths;
    // Or views of the backend's own memory, valid until its next submission
    // (no per-batch copy of the destination buffers).
    std::span<const std::uint32_t> color_words,depth_words;
    std::span<const std::uint32_t> color_span()const{return colors.empty()?color_words:std::span<const std::uint32_t>(colors);}
    std::span<const std::uint32_t> depth_span()const{return depths.empty()?depth_words:std::span<const std::uint32_t>(depths);}
    std::array<std::uint32_t,32> parameters{};
    NativeGpuRasterStats stats{};
    std::uint64_t draws=0;
};
struct NativeGpuBatchFull:std::runtime_error {NativeGpuBatchFull():std::runtime_error("GPU batch needs ordered capacity flush"){} };
// Internal resolution: a displayed framebuffer redrawn at `scale` times its
// size. `pixels` holds one opaque BGRA word per sample (row stride
// stride*scale) for `rows` original rows; `snapshot` is the original buffer
// as its last batch left it (stride words per row): wherever the guest's
// buffer no longer matches it, something else wrote there and the original
// pixels must be shown instead.
struct NativeHiresView {
    const std::uint32_t* pixels=nullptr;std::span<const std::uint32_t> snapshot;
    unsigned scale=1,stride=0,width=0,rows=0;
    // Held while `pixels` is read; the backend does not overwrite the plane meanwhile.
    std::atomic<int>* readers=nullptr;
    explicit operator bool()const{return pixels!=nullptr;}
};
class NativeGpuRaster {
public:
    virtual ~NativeGpuRaster()=default;
    virtual void begin_batch(){}
    virtual NativeGpuRasterBatch end_batch(){return {};}
    // Asynchronous form of end_batch: submit without waiting, then collect the
    // completed result before any further use of this backend.
    virtual void submit_batch(){submitted_=end_batch();}
    // GPU upload/shade/readback ns, dispatches, barriers (diagnostic only).
    virtual std::array<std::uint64_t,6> profile()const{return {};}
    // Whether collect_batch would return without waiting for the GPU.
    virtual bool batch_ready(){return true;}
    virtual NativeGpuRasterBatch collect_batch(){auto result=std::move(submitted_);submitted_={};return result;}
    // Optional, for the next raster() call only: its jobs already validated
    // and encoded (kFixedJobWords each) by the caller's worker threads.
    virtual void set_encoded_jobs(std::span<const std::uint32_t>){}
    // Internal resolution (1 = original only). Batches drawing to a buffer
    // recently passed to note_display_address are also shaded at `scale`.
    virtual bool set_internal_scale(unsigned){return false;}
    virtual unsigned internal_scale()const{return 1;}
    // Optional, for the next raster() call only: an enlarged copy of its
    // texture (`scale` times each side, see native_upscale.hpp) that its
    // samples at internal resolution use instead; `token` identifies it.
    virtual void set_hires_texture(std::span<const std::uint32_t>,unsigned,std::uint32_t){}
    virtual void note_display_address(std::uint32_t){}
    // Optional: prepare the open batch's submission before waiting for the batch in flight.
    virtual void prepare_batch(){}
    // After the batches drawing `address` were collected (same thread).
    virtual NativeHiresView hires_view(std::uint32_t){return {};}
    virtual NativeGpuRasterStats raster(std::span<std::uint32_t> colors,
        std::span<std::uint32_t> depths,std::span<const std::uint32_t> texture,
        std::span<const NativeGpuRasterTriangle> triangles,
        const std::array<std::uint32_t,32>& parameters)=0;
private:
    NativeGpuRasterBatch submitted_;
};
}
