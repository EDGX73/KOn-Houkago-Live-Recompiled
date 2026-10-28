#include "native_media.hpp"
#include "platform/library.hpp"
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswresample/swresample.h>
#include <libavutil/channel_layout.h>
#include <libavutil/error.h>
}
#include <array>
#include <algorithm>
#include <set>
#include <stdexcept>
#include <utility>
#include <cstring>
namespace kon {
namespace {
#ifdef __ANDROID__
// Static FFmpeg for Android: the functions are linked directly, no DLLs.
class SharedLibrary {
public:
    explicit SharedLibrary(const std::filesystem::path&){}
};
#else
class SharedLibrary {
public:
    explicit SharedLibrary(const std::filesystem::path& path):library_(path,platform::DynamicLibrary::Search::LibraryFolder){
        if(!library_)throw std::runtime_error("Cannot load native media library "+path.filename().string()+" "+library_.error());
    }
    template<class T>T symbol(const char* name) const {
        const auto pointer=library_.symbol<T>(name);if(!pointer)throw std::runtime_error(std::string("Missing native media API ")+name);
        return pointer;
    }
private:platform::DynamicLibrary library_;
};
#endif
template<class F>struct Cleanup {F fn;~Cleanup(){fn();}};
}
struct NativeMedia::Impl {
    SharedLibrary util,codec,format,resample;
    std::set<std::uint32_t> modules;
    struct AtracSlot {AVCodecID type;AVCodecContext* decoder=nullptr;bool bound=false;SwrContext* converter=nullptr;};
    std::vector<AtracSlot> atrac_slots;
    AVCodecContext* video_decoder=nullptr;
#ifdef __ANDROID__
#define MEDIA_API(name,lib) decltype(&::name) name=&::name
#else
#define MEDIA_API(name,lib) decltype(&::name) name=lib.symbol<decltype(&::name)>(#name)
#endif
    MEDIA_API(av_version_info,util);MEDIA_API(av_strerror,util);MEDIA_API(av_frame_alloc,util);MEDIA_API(av_frame_free,util);
    MEDIA_API(avcodec_license,codec);MEDIA_API(avcodec_configuration,codec);MEDIA_API(avcodec_find_decoder,codec);
    MEDIA_API(avcodec_alloc_context3,codec);MEDIA_API(avcodec_free_context,codec);MEDIA_API(avcodec_parameters_to_context,codec);
    MEDIA_API(avcodec_open2,codec);MEDIA_API(avcodec_send_packet,codec);MEDIA_API(avcodec_receive_frame,codec);
    MEDIA_API(av_packet_alloc,codec);MEDIA_API(av_packet_free,codec);MEDIA_API(av_packet_unref,codec);
    MEDIA_API(av_new_packet,codec);
    MEDIA_API(avformat_open_input,format);MEDIA_API(avformat_close_input,format);MEDIA_API(avformat_find_stream_info,format);
    MEDIA_API(av_find_best_stream,format);MEDIA_API(av_read_frame,format);
    MEDIA_API(swr_alloc_set_opts2,resample);MEDIA_API(swr_init,resample);MEDIA_API(swr_free,resample);
    MEDIA_API(swr_convert,resample);MEDIA_API(swr_get_out_samples,resample);
#undef MEDIA_API
    explicit Impl(const std::filesystem::path& folder):util(folder/L"avutil-60.dll"),codec(folder/L"avcodec-62.dll"),format(folder/L"avformat-62.dll"),resample(folder/L"swresample-6.dll") {
        const std::string configuration=avcodec_configuration(),license=avcodec_license();
        if(configuration.find("--enable-gpl")!=std::string::npos || configuration.find("--enable-nonfree")!=std::string::npos || license.find("LGPL")==std::string::npos)throw std::runtime_error("Unexpected native codec license/configuration");
    }
    ~Impl(){clear_atrac();avcodec_free_context(&video_decoder);}
    void clear_atrac(){for(auto& slot:atrac_slots){swr_free(&slot.converter);avcodec_free_context(&slot.decoder);}atrac_slots.clear();}
    void checked(int result,const char* operation) const {
        if(result>=0)return;char text[AV_ERROR_MAX_STRING_SIZE]{};av_strerror(result,text,sizeof(text));
        throw std::runtime_error(std::string(operation)+": "+text);
    }
    void probe(AVCodecID id) const {
        const auto* decoder=avcodec_find_decoder(id);if(!decoder)throw std::runtime_error("Required native media decoder absent");
        auto* context=avcodec_alloc_context3(decoder);if(!context)throw std::bad_alloc();avcodec_free_context(&context);
    }
};
NativeMedia::NativeMedia(std::filesystem::path project_root):root_(std::filesystem::weakly_canonical(std::filesystem::absolute(project_root))){}
NativeMedia::~NativeMedia()=default;
NativeMedia::Impl& NativeMedia::backend(){if(!impl_)impl_=std::make_unique<Impl>(root_/"third_party/ffmpeg/bin");return *impl_;}
std::string NativeMedia::version(){return backend().av_version_info();}
std::uint32_t NativeMedia::load_module(std::uint32_t id){
    if(id!=0x300 && id!=0x302 && id!=0x303)throw std::runtime_error("Unsupported native AV module id="+std::to_string(id));
    auto& api=backend();if(api.modules.contains(id))return 0x80111102u;
    if(id==0x300 || id==0x302){api.probe(AV_CODEC_ID_ATRAC3);api.probe(AV_CODEC_ID_ATRAC3P);}
    if(id==0x300 || id==0x303)api.probe(AV_CODEC_ID_H264);
    api.modules.insert(id);return 0;
}
std::uint32_t NativeMedia::unload_module(std::uint32_t id){return impl_ && impl_->modules.erase(id)?0u:0x80111103u;}
std::uint32_t NativeMedia::reinit_atrac(std::int32_t atrac3_count,std::int32_t atrac3plus_count){
    auto& api=backend();
    if(!api.modules.contains(0x302))throw std::runtime_error("ATRAC initialization requires its loaded native module");
    if(std::any_of(api.atrac_slots.begin(),api.atrac_slots.end(),[](const auto& slot){return slot.bound;}))return 0x80000021u;
    const auto regular=std::max(0,atrac3_count),plus=std::max(0,atrac3plus_count);
    const auto allocated_plus=std::min(plus,3),allocated_regular=std::min(regular,6-allocated_plus*2);
    api.clear_atrac();api.atrac_slots.reserve(6);
    for(int n=0;n<allocated_plus+allocated_regular;n++){
        const auto type=n<allocated_plus?AV_CODEC_ID_ATRAC3P:AV_CODEC_ID_ATRAC3;
        const auto* factory=api.avcodec_find_decoder(type);
        if(!factory)throw std::runtime_error("Native ATRAC decoder factory disappeared");
        auto* context=api.avcodec_alloc_context3(factory);if(!context){api.clear_atrac();return 0x80000022u;}
        api.atrac_slots.push_back({type,context,false});
    }
    // PSP resource allocation is partial when requested codec units exceed six.
    return static_cast<std::int64_t>(plus)*2+regular>6?0x80000022u:0u;
}
unsigned NativeMedia::atrac_capacity() const{return impl_?static_cast<unsigned>(impl_->atrac_slots.size()):0;}
std::uint32_t NativeMedia::bind_atrac3plus(unsigned channels,unsigned rate,unsigned block_bytes){
    if((channels!=1 && channels!=2) || rate!=44100 || !block_bytes || block_bytes>8192)throw std::runtime_error("Unsupported ATRAC3+ codec parameters");
    auto& api=backend();
    for(unsigned id=0;id<api.atrac_slots.size();++id){auto& slot=api.atrac_slots[id];
        if(slot.type!=AV_CODEC_ID_ATRAC3P || slot.bound)continue;
        if(!slot.decoder)slot.decoder=api.avcodec_alloc_context3(api.avcodec_find_decoder(slot.type));
        if(!slot.decoder)throw std::bad_alloc();
        slot.decoder->block_align=static_cast<int>(block_bytes);slot.decoder->sample_rate=static_cast<int>(rate);
        slot.decoder->ch_layout=channels==1?AVChannelLayout(AV_CHANNEL_LAYOUT_MONO):AVChannelLayout(AV_CHANNEL_LAYOUT_STEREO);
        api.checked(api.avcodec_open2(slot.decoder,api.avcodec_find_decoder(slot.type),nullptr),"Open streaming ATRAC3+ decoder");
        slot.bound=true;return id;
    }
    return 0x80630003u;
}
std::uint32_t NativeMedia::release_atrac(std::uint32_t id){
    auto& api=backend();if(id>=api.atrac_slots.size() || !api.atrac_slots[id].bound)return 0x80630005u;
    auto& slot=api.atrac_slots[id];api.swr_free(&slot.converter);api.avcodec_free_context(&slot.decoder);slot.bound=false;return 0;
}
void NativeMedia::restart_atrac(std::uint32_t id){
    auto& api=backend();if(id>=api.atrac_slots.size() || !api.atrac_slots[id].bound)throw std::runtime_error("ATRAC restart has no native decoder");
    auto& slot=api.atrac_slots[id];const auto channels=slot.decoder->ch_layout.nb_channels,rate=slot.decoder->sample_rate,block=slot.decoder->block_align;
    auto* replacement=api.avcodec_alloc_context3(api.avcodec_find_decoder(slot.type));if(!replacement)throw std::bad_alloc();
    replacement->block_align=block;replacement->sample_rate=rate;
    replacement->ch_layout=channels==1?AVChannelLayout(AV_CHANNEL_LAYOUT_MONO):AVChannelLayout(AV_CHANNEL_LAYOUT_STEREO);
    const int result=api.avcodec_open2(replacement,api.avcodec_find_decoder(slot.type),nullptr);
    if(result<0){api.avcodec_free_context(&replacement);api.checked(result,"Restart streaming ATRAC3+ decoder");}
    api.swr_free(&slot.converter);api.avcodec_free_context(&slot.decoder);slot.decoder=replacement;
}
std::vector<std::int16_t> NativeMedia::decode_atrac_packet(std::uint32_t id,std::span<const std::uint8_t> encoded){
    auto& api=backend();if(id>=api.atrac_slots.size() || !api.atrac_slots[id].bound)throw std::runtime_error("ATRAC packet has no native decoder");
    auto& slot=api.atrac_slots[id];if(encoded.size()!=static_cast<unsigned>(slot.decoder->block_align))throw std::runtime_error("Incomplete ATRAC packet");
    auto* packet=api.av_packet_alloc();auto* frame=api.av_frame_alloc();
    Cleanup close_packet{[&]{api.av_packet_free(&packet);}};Cleanup close_frame{[&]{api.av_frame_free(&frame);}};
    if(!packet || !frame)throw std::bad_alloc();
    api.checked(api.av_new_packet(packet,static_cast<int>(encoded.size())),"Allocate ATRAC packet");
    std::memcpy(packet->data,encoded.data(),encoded.size());
    api.checked(api.avcodec_send_packet(slot.decoder,packet),"Send streaming ATRAC packet");
    api.checked(api.avcodec_receive_frame(slot.decoder,frame),"Decode streaming ATRAC packet");
    AVChannelLayout stereo=AV_CHANNEL_LAYOUT_STEREO;
    if(!slot.converter){
        api.checked(api.swr_alloc_set_opts2(&slot.converter,&stereo,AV_SAMPLE_FMT_S16,44100,&frame->ch_layout,static_cast<AVSampleFormat>(frame->format),frame->sample_rate,0,nullptr),"Create streaming PCM converter");
        api.checked(api.swr_init(slot.converter),"Initialize streaming PCM converter");
    }
    const auto capacity=api.swr_get_out_samples(slot.converter,frame->nb_samples);api.checked(capacity,"Streaming PCM capacity");
    std::vector<std::int16_t> pcm(static_cast<std::size_t>(capacity)*2u);auto* output=reinterpret_cast<std::uint8_t*>(pcm.data());
    std::vector<const std::uint8_t*> planes(frame->ch_layout.nb_channels);for(unsigned n=0;n<planes.size();++n)planes[n]=frame->extended_data[n];
    const auto frames=api.swr_convert(slot.converter,&output,capacity,planes.data(),frame->nb_samples);api.checked(frames,"Convert streaming PCM");
    pcm.resize(static_cast<std::size_t>(frames)*2u);return pcm;
}
std::uint32_t NativeMedia::initialize_video(){
    auto& api=backend();if(!api.modules.contains(0x303))throw std::runtime_error("MPEG initialization requires its loaded native module");
    if(api.video_decoder)return 0;
    const auto* decoder=api.avcodec_find_decoder(AV_CODEC_ID_H264);
    if(!decoder)throw std::runtime_error("Native H264 decoder absent");
    auto* context=api.avcodec_alloc_context3(decoder);if(!context)return 0x80000022u;
    context->thread_count=1;
    const int result=api.avcodec_open2(context,decoder,nullptr);
    if(result<0){api.avcodec_free_context(&context);api.checked(result,"Initialize native H264 decoder");}
    api.video_decoder=context;return 0;
}
std::uint32_t NativeMedia::finish_video(){if(impl_)impl_->avcodec_free_context(&impl_->video_decoder);return 0;}
PcmSound NativeMedia::decode_audio_file(const std::filesystem::path& file){
    auto& api=backend();AVFormatContext* input=nullptr;
    const auto utf8=file.u8string();api.checked(api.avformat_open_input(&input,reinterpret_cast<const char*>(utf8.c_str()),nullptr,nullptr),"Open original audio");
    Cleanup close_input{[&]{api.avformat_close_input(&input);}};
    api.checked(api.avformat_find_stream_info(input,nullptr),"Read original audio parameters");
    const AVCodec* decoder=nullptr;const int stream=api.av_find_best_stream(input,AVMEDIA_TYPE_AUDIO,-1,-1,&decoder,0);api.checked(stream,"Find original audio stream");
    auto* context=api.avcodec_alloc_context3(decoder);if(!context)throw std::bad_alloc();Cleanup close_codec{[&]{api.avcodec_free_context(&context);}};
    api.checked(api.avcodec_parameters_to_context(context,input->streams[stream]->codecpar),"Apply original codec parameters");
    api.checked(api.avcodec_open2(context,decoder,nullptr),"Initialize native decoder");
    auto* packet=api.av_packet_alloc();auto* frame=api.av_frame_alloc();
    Cleanup close_packet{[&]{api.av_packet_free(&packet);}};Cleanup close_frame{[&]{api.av_frame_free(&frame);}};
    if(!packet || !frame)throw std::bad_alloc();
    SwrContext* converter=nullptr;Cleanup close_converter{[&]{api.swr_free(&converter);}};
    PcmSound sound;AVChannelLayout output_layout=AV_CHANNEL_LAYOUT_STEREO;
    bool converter_ready=false;
    const auto receive=[&]{
        for(;;){
            const int status=api.avcodec_receive_frame(context,frame);
            if(status==AVERROR(EAGAIN) || status==AVERROR_EOF)break;api.checked(status,"Decode original audio frame");
            if(!converter_ready){
                api.checked(api.swr_alloc_set_opts2(&converter,&output_layout,AV_SAMPLE_FMT_S16,sound.sample_rate,&frame->ch_layout,static_cast<AVSampleFormat>(frame->format),frame->sample_rate,0,nullptr),"Create PCM converter");
                api.checked(api.swr_init(converter),"Initialize PCM converter");converter_ready=true;
            }
            const int capacity=api.swr_get_out_samples(converter,frame->nb_samples);api.checked(capacity,"PCM capacity");
            const auto start=sound.samples.size();sound.samples.resize(start+static_cast<std::size_t>(capacity)*2u);
            auto* destination=reinterpret_cast<std::uint8_t*>(sound.samples.data()+start);
            std::vector<const std::uint8_t*> planes(static_cast<std::size_t>(frame->ch_layout.nb_channels));
            for(std::size_t n=0;n<planes.size();n++)planes[n]=frame->extended_data[n];
            const int converted=api.swr_convert(converter,&destination,capacity,planes.data(),frame->nb_samples);api.checked(converted,"Convert original PCM");
            sound.samples.resize(start+static_cast<std::size_t>(converted)*2u);
        }
    };
    int status;
    while((status=api.av_read_frame(input,packet))>=0){
        if(packet->stream_index==stream){api.checked(api.avcodec_send_packet(context,packet),"Send original compressed packet");receive();}
        api.av_packet_unref(packet);
    }
    if(status!=AVERROR_EOF)api.checked(status,"Read original compressed packets");
    api.checked(api.avcodec_send_packet(context,nullptr),"Flush native decoder");receive();
    if(converter_ready){
        for(;;){
            const auto capacity=api.swr_get_out_samples(converter,0);api.checked(capacity,"Flush PCM capacity");if(!capacity)break;
            const auto start=sound.samples.size();sound.samples.resize(start+static_cast<std::size_t>(capacity)*2u);
            auto* destination=reinterpret_cast<std::uint8_t*>(sound.samples.data()+start);
            const int converted=api.swr_convert(converter,&destination,capacity,nullptr,0);api.checked(converted,"Flush original PCM");sound.samples.resize(start+static_cast<std::size_t>(converted)*2u);if(!converted)break;
        }
    }
    if(sound.samples.empty())throw std::runtime_error("Native decoder produced no original audio");return sound;
}
}
