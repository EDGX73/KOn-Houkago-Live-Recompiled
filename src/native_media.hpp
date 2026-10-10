#pragma once
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>
#include <span>
namespace kon {
struct PcmSound {unsigned sample_rate=44100,channels=2;std::vector<std::int16_t> samples;};
class NativeMedia {
public:
    explicit NativeMedia(std::filesystem::path project_root);
    ~NativeMedia();
    NativeMedia(const NativeMedia&)=delete;NativeMedia& operator=(const NativeMedia&)=delete;
    std::uint32_t load_module(std::uint32_t id);
    std::uint32_t unload_module(std::uint32_t id);
    std::uint32_t reinit_atrac(std::int32_t atrac3_count,std::int32_t atrac3plus_count);
    unsigned atrac_capacity() const;
    std::uint32_t bind_atrac3plus(unsigned channels,unsigned rate,unsigned block_bytes);
    std::uint32_t release_atrac(std::uint32_t id);
    void restart_atrac(std::uint32_t id);
    std::vector<std::int16_t> decode_atrac_packet(std::uint32_t id,std::span<const std::uint8_t> packet);
    std::uint32_t initialize_video();
    std::uint32_t finish_video();
    PcmSound decode_audio_file(const std::filesystem::path& file);
    std::string version();
private:
    struct Impl;std::filesystem::path root_;std::unique_ptr<Impl> impl_;
    Impl& backend();
};
}
