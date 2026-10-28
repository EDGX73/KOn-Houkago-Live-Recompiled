#include "platform/library.hpp"
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#include "native_vulkan_clear.hpp"
#include "native_bytes.hpp"
#include "native_gpu_selection.hpp"
#include "native_fixed_raster.hpp"
#include "native_gpu_clear_shader.hpp"
#include "native_gpu_triangle_shader.hpp"
#include "native_gpu_raster_shader.hpp"
#include <algorithm>
#include <atomic>
#include <thread>
#include <optional>
#include <unordered_map>
#include <cmath>
#include <iostream>
#include <array>
#include <chrono>
#include <cstring>
#include <stdexcept>
#include <vector>
#include <limits>
namespace kon {
namespace {
void checked(VkResult result,const char* operation){
    if(result!=VK_SUCCESS)throw std::runtime_error(std::string(operation)+" VkResult="+std::to_string(result));
}
}
struct NativeVulkanClear::Impl {
    // Storage is persistent, but host data is copied before every submission:
    // compiled game code can write VRAM directly between native draw calls.
    // Two batch slots of kSlotWords, then the persistent texture region.
    static constexpr unsigned kSlotWords=8u*1024u*1024u,kTextureWords=32u*1024u*1024u,kTextureBase=2u*kSlotWords;
    // Internal resolution planes (color or depth of a displayed buffer at up
    // to 4x, 512x272 original words) after the textures; mirrored on the host.
    static constexpr unsigned kMaxScale=4,kPlaneWords=512u*kMaxScale*272u*kMaxScale,kPlanes=4,kPlaneBase=kTextureBase+kTextureWords;
    // Each plane's samples as presentable BGRA follow (converted on the GPU),
    // twice: a batch fills one image while the window may still copy the other.
    // Three presentable images per plane: one shown, one possibly still being
    // copied by the window, one free to fill (no wait for the window).
    static constexpr unsigned kImages=3;
    static constexpr unsigned kBgraBase=kPlaneBase+kPlanes*kPlaneWords,capacity=kBgraBase+kImages*kPlanes*kPlaneWords;
    static unsigned bgra_base(unsigned plane,unsigned image){return kBgraBase+(plane*kImages+image)*kPlaneWords;}
    struct Plane {std::uint32_t key=0;unsigned stride=0,width=0,rows=0,shown=0;std::vector<std::uint32_t> snapshot;std::uint64_t used=0;bool valid=false;};
    std::array<Plane,kPlanes> planes{};std::array<std::atomic<int>,kImages*kPlanes> plane_readers{};std::uint64_t plane_clock=0;unsigned scale=1; // readers: presentations still copying a plane's samples
    std::array<std::atomic<std::uint32_t>,2> display_addresses{};std::atomic<unsigned> display_next{0};
    static unsigned plane_base(unsigned plane){return kPlaneBase+plane*kPlaneWords;}
    bool displayed(std::uint32_t address)const{const auto a=address&0x1FFFFFF0u;for(const auto& d:display_addresses)if((d.load(std::memory_order_relaxed)&0x1FFFFFF0u)==a && a)return true;return false;}
    unsigned acquire_plane(std::uint32_t key,unsigned stride){
        for(unsigned n=0;n<kPlanes;++n)if(planes[n].key==key && planes[n].stride==stride){planes[n].used=++plane_clock;return n;}
        unsigned oldest=0;for(unsigned n=1;n<kPlanes;++n)if(planes[n].used<planes[oldest].used)oldest=n;
        auto& p=planes[oldest];p.key=key;p.stride=stride;p.width=p.rows=0;p.snapshot.clear();p.valid=false;p.used=++plane_clock;return oldest;
    }
    platform::DynamicLibrary loader;
    VkInstance instance=nullptr;VkDevice device=nullptr;VkQueue queue=nullptr;
    VkBuffer buffer=nullptr,raster_buffer=nullptr;VkDeviceMemory memory=nullptr,raster_memory=nullptr;
    VkCommandPool pool=nullptr;VkCommandBuffer command=nullptr;VkFence fence=nullptr;
    VkShaderModule raster_shader=nullptr;VkPipeline raster_pipeline=nullptr,raster_blocks_pipeline=nullptr,raster_samples_pipeline=nullptr,raster_blocks3_pipeline=nullptr;std::uint64_t completed_raster=0;bool precise_raster=false;
    VkShaderModule triangle_shader=nullptr;VkPipeline triangle_pipeline=nullptr;std::uint64_t completed_triangles=0;
    VkShaderModule shader=nullptr;VkDescriptorSetLayout descriptor_layout=nullptr;
    VkPipelineLayout pipeline_layout=nullptr;VkPipeline pipeline=nullptr;
    VkDescriptorPool descriptor_pool=nullptr;VkDescriptorSet descriptor_set=nullptr,raster_set=nullptr;
    bool batch_active=false,batch_recording=false;std::span<const std::uint32_t> encoded_jobs;
    std::span<const std::uint32_t> hires_texels;unsigned hires_scale=0;std::uint32_t hires_token=0; // next raster() only
    // Two batch slots split the staging and device buffers: the next batch is
    // recorded into one while the GPU still shades the previous one.
    unsigned batch_slot=0,batch_color=64,batch_depth=1024u*1024u+64,batch_start=2u*1024u*1024u+64,batch_end=kSlotWords;
    void use_slot(unsigned slot){const unsigned base=slot*kSlotWords;batch_slot=slot;batch_color=base+64;batch_depth=base+1024u*1024u+64;batch_start=base+2u*1024u*1024u+64;batch_end=base+kSlotWords;}
    std::array<VkCommandPool,2> batch_pools{};std::array<VkCommandBuffer,2> batch_commands{};std::array<VkFence,2> batch_fences{};
    unsigned batch_cursor=2u*1024u*1024u+64,batch_colors=0,batch_depths=0;
    struct BatchPacket {unsigned header,stats;};std::vector<BatchPacket> batch_packets;
    // Persistent GPU textures, keyed by decode token (equal tokens mean equal
    // pixels). A texture is uploaded once and later batches only reference it.
    // A ring allocator never reuses words that the batch being recorded or the
    // one in flight may still read (or whose upload is still pending).
    struct TextureEntry {unsigned offset=0,size=0;std::uint64_t used=0;};
    std::unordered_map<std::uint32_t,TextureEntry> texture_cache;unsigned texture_head=0;
    std::uint64_t batch_generation=0,recording_generation=0;std::vector<VkBufferCopy> texture_uploads;std::vector<std::uint32_t> texture_upload_tokens;
    std::uint64_t texture_cache_hits=0,texture_cache_uploads=0;
    unsigned cached_texture(std::span<const std::uint32_t> texture,std::uint32_t token){
        if(!token || texture.empty() || texture.size()>kTextureWords/4u)return 0;
        if(const auto found=texture_cache.find(token);found!=texture_cache.end() && found->second.size==texture.size()){found->second.used=recording_generation;++texture_cache_hits;return found->second.offset;}
        const auto busy=[&](const TextureEntry& e){return e.used==recording_generation || (batch_inflight && e.used==flight.generation);};
        const unsigned size=static_cast<unsigned>((texture.size()+1u)&~std::size_t(1));
        if(texture_head+size>kTextureWords)texture_head=0;
        const unsigned first=kTextureBase+texture_head,last=first+size;
        for(const auto& [key,e]:texture_cache)if(e.offset<last && first<e.offset+e.size && busy(e))return 0;
        std::erase_if(texture_cache,[&](const auto& item){const auto& e=item.second;return e.offset<last && first<e.offset+e.size;});
        texture_cache.erase(token);
        std::memcpy(static_cast<std::uint32_t*>(mapped)+first,texture.data(),texture.size_bytes());
        texture_uploads.push_back({VkDeviceSize(first)*4,VkDeviceSize(first)*4,VkDeviceSize(texture.size())*4});
        texture_cache[token]={first,static_cast<unsigned>(texture.size()),recording_generation};texture_upload_tokens.push_back(token);texture_head+=size;++texture_cache_uploads;
        return first;
    }
    struct Flight {std::uint64_t generation=0;unsigned samples=1,image=0,slot=0,color=0,depth=0,colors=0,depths=0;int color_plane=-1,depth_plane=-1;std::vector<BatchPacket> packets;std::array<std::uint32_t,32> parameters{};} flight;
    struct BatchTexture {unsigned offset,size;std::uintptr_t source;std::uint32_t token;};std::vector<BatchTexture> batch_textures;
    std::vector<unsigned> bin_fill,bin_bounds,bin_indices; // reused per-draw tile lists
    // Whole-batch tile entries: absolute tile, draw header, job word offset.
    static constexpr unsigned kBatchColumns=128;
    // Before each batch's tile table (4x): original color base and stride,
    // depth base and stride, then one bit per absolute 16x16 tile (up to 64
    // tile rows) for color and for depth that the guest or the CPU changed.
    static constexpr unsigned kDirtyBits=kBatchColumns*64u/32u,kDirtyWords=4u+2u*kDirtyBits;std::vector<std::array<unsigned,3>> batch_entries;unsigned batch_left=0,batch_top=0,batch_right=0,batch_bottom=0;
    std::uint64_t raster_dispatches=0,raster_barriers=0;bool batch_inflight=false;
    // Per-batch GPU timestamps: upload, shading and readback (nanoseconds).
    std::array<std::uint64_t,3> split_ns{};std::uint64_t split_batches=0;
    VkQueryPool raster_queries=nullptr;std::uint64_t tile_job_pixels=0,raster_upload_ns=0,raster_shade_ns=0,raster_readback_ns=0;
    std::array<std::uint32_t,32> batch_parameters{};
    std::vector<std::uint32_t> cached_raster_colors,cached_raster_depths,cached_raster_texture;
    unsigned cached_depth_offset=0,cached_texture_offset=0;bool colors_valid=false,depths_valid=false,texture_valid=false;
    void* mapped=nullptr;std::string name,selector;std::uint64_t completed=0;
    bool measure=false,prefer_cached_reads=true;Timings times;VkQueryPool query_pool=nullptr;
    unsigned timestamp_bits=0,allocation_flags=0;float timestamp_period=0;
#define FN(n) PFN_##n n=nullptr
    FN(vkDestroyInstance);FN(vkDestroyDevice);FN(vkDeviceWaitIdle);
    FN(vkDestroyBuffer);FN(vkFreeMemory);FN(vkDestroyCommandPool);
    FN(vkDestroyFence);FN(vkUnmapMemory);FN(vkDestroyShaderModule);
    FN(vkDestroyDescriptorSetLayout);FN(vkDestroyPipelineLayout);
    FN(vkDestroyPipeline);FN(vkDestroyDescriptorPool);
    FN(vkResetCommandPool);FN(vkResetFences);FN(vkBeginCommandBuffer);
    FN(vkCmdPipelineBarrier);FN(vkCmdBindPipeline);FN(vkCmdBindDescriptorSets);
    FN(vkCmdCopyBuffer);FN(vkCmdPushConstants);FN(vkCmdDispatch);FN(vkEndCommandBuffer);
    FN(vkQueueSubmit);FN(vkWaitForFences);FN(vkGetFenceStatus);
    FN(vkCreateQueryPool);FN(vkDestroyQueryPool);FN(vkCmdResetQueryPool);FN(vkCmdWriteTimestamp);FN(vkGetQueryPoolResults);
#undef FN
    ~Impl(){
        if(device){
            if(vkDeviceWaitIdle)vkDeviceWaitIdle(device);
            if(query_pool && vkDestroyQueryPool)vkDestroyQueryPool(device,query_pool,nullptr);
            if(raster_queries && vkDestroyQueryPool)vkDestroyQueryPool(device,raster_queries,nullptr);
            if(mapped && vkUnmapMemory)vkUnmapMemory(device,memory);
            if(fence && vkDestroyFence)vkDestroyFence(device,fence,nullptr);
            for(auto f:batch_fences)if(f && vkDestroyFence)vkDestroyFence(device,f,nullptr);
            for(auto p:batch_pools)if(p && vkDestroyCommandPool)vkDestroyCommandPool(device,p,nullptr);
            if(pool && vkDestroyCommandPool)vkDestroyCommandPool(device,pool,nullptr);
            if(descriptor_pool && vkDestroyDescriptorPool)vkDestroyDescriptorPool(device,descriptor_pool,nullptr);
            if(raster_pipeline && vkDestroyPipeline)vkDestroyPipeline(device,raster_pipeline,nullptr);
            if(raster_blocks_pipeline && vkDestroyPipeline)vkDestroyPipeline(device,raster_blocks_pipeline,nullptr);
            if(raster_samples_pipeline && vkDestroyPipeline)vkDestroyPipeline(device,raster_samples_pipeline,nullptr);
            if(raster_blocks3_pipeline && vkDestroyPipeline)vkDestroyPipeline(device,raster_blocks3_pipeline,nullptr);
            if(raster_shader && vkDestroyShaderModule)vkDestroyShaderModule(device,raster_shader,nullptr);
            if(triangle_pipeline && vkDestroyPipeline)vkDestroyPipeline(device,triangle_pipeline,nullptr);
            if(triangle_shader && vkDestroyShaderModule)vkDestroyShaderModule(device,triangle_shader,nullptr);
            if(pipeline && vkDestroyPipeline)vkDestroyPipeline(device,pipeline,nullptr);
            if(pipeline_layout && vkDestroyPipelineLayout)vkDestroyPipelineLayout(device,pipeline_layout,nullptr);
            if(descriptor_layout && vkDestroyDescriptorSetLayout)vkDestroyDescriptorSetLayout(device,descriptor_layout,nullptr);
            if(shader && vkDestroyShaderModule)vkDestroyShaderModule(device,shader,nullptr);
            if(raster_buffer && vkDestroyBuffer)vkDestroyBuffer(device,raster_buffer,nullptr);
            if(raster_memory && vkFreeMemory)vkFreeMemory(device,raster_memory,nullptr);
            if(buffer && vkDestroyBuffer)vkDestroyBuffer(device,buffer,nullptr);
            if(memory && vkFreeMemory)vkFreeMemory(device,memory,nullptr);
            if(vkDestroyDevice)vkDestroyDevice(device,nullptr);
        }
        if(instance && vkDestroyInstance)vkDestroyInstance(instance,nullptr);
        loader.close();
    }
    void initialize(){
        loader=platform::open_vulkan_loader();
        if(!loader)throw std::runtime_error("System Vulkan loader is unavailable for --vulkan-clear");
        const auto get_instance=loader.symbol<PFN_vkGetInstanceProcAddr>("vkGetInstanceProcAddr");
        if(!get_instance)throw std::runtime_error("Missing Vulkan loader entry point");
        const auto entry=[&](const char* symbol){auto result=get_instance(instance,symbol);if(!result)throw std::runtime_error(std::string("Missing Vulkan entry ")+symbol);return result;};
        const auto create_instance=reinterpret_cast<PFN_vkCreateInstance>(entry("vkCreateInstance"));
        VkApplicationInfo application{VK_STRUCTURE_TYPE_APPLICATION_INFO};application.pApplicationName="K-On native clear backend";application.apiVersion=VK_API_VERSION_1_0;
        VkInstanceCreateInfo instance_info{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};instance_info.pApplicationInfo=&application;
        checked(create_instance(&instance_info,nullptr,&instance),"Create native clear instance");
        vkDestroyInstance=reinterpret_cast<PFN_vkDestroyInstance>(entry("vkDestroyInstance"));
        const auto enumerate=reinterpret_cast<PFN_vkEnumeratePhysicalDevices>(entry("vkEnumeratePhysicalDevices"));
        const auto properties=reinterpret_cast<PFN_vkGetPhysicalDeviceProperties>(entry("vkGetPhysicalDeviceProperties"));
        const auto families=reinterpret_cast<PFN_vkGetPhysicalDeviceQueueFamilyProperties>(entry("vkGetPhysicalDeviceQueueFamilyProperties"));
        unsigned count=0;checked(enumerate(instance,&count,nullptr),"Enumerate clear adapters");
        std::vector<VkPhysicalDevice> physical(count);if(!count)throw std::runtime_error("No Vulkan adapter");
        checked(enumerate(instance,&count,physical.data()),"Read clear adapters");
        unsigned selected=~0u,queue_family=~0u;VkPhysicalDeviceProperties selected_properties{};
        for(unsigned n=0;n<count;++n){
            VkPhysicalDeviceProperties props{};properties(physical[n],&props);
            if(!native_gpu_matches(n,props.vendorID,props.deviceType,props.deviceName,selector))continue;
            unsigned queue_count=0;families(physical[n],&queue_count,nullptr);
            std::vector<VkQueueFamilyProperties> queues(queue_count);families(physical[n],&queue_count,queues.data());
            for(unsigned q=0;q<queue_count;++q)if(queues[q].queueCount && (queues[q].queueFlags&VK_QUEUE_COMPUTE_BIT)){
                if(selected==~0u || (props.deviceType==VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU && selected_properties.deviceType!=VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU)){
                    selected=n;queue_family=q;selected_properties=props;timestamp_bits=queues[q].timestampValidBits;
                }
                break;
            }
        }
        if(selected==~0u)throw std::runtime_error("No Vulkan compute adapter matches requested selector: "+selector);
        if(selected_properties.limits.maxStorageBufferRange<capacity*4u || selected_properties.limits.maxComputeWorkGroupInvocations<64u || selected_properties.limits.maxComputeWorkGroupSize[0]<64u || selected_properties.limits.maxComputeWorkGroupCount[0]<(1024u*1024u+63u)/64u || selected_properties.limits.maxPushConstantsSize<48u)throw std::runtime_error("Vulkan adapter cannot support bounded clear kernel");
        name=selected_properties.deviceName;timestamp_period=selected_properties.limits.timestampPeriod;
        const float priority=1;VkDeviceQueueCreateInfo queue_info{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};queue_info.queueFamilyIndex=queue_family;queue_info.queueCount=1;queue_info.pQueuePriorities=&priority;
        VkDeviceCreateInfo device_info{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};device_info.queueCreateInfoCount=1;device_info.pQueueCreateInfos=&queue_info;
        VkPhysicalDeviceFeatures available{},enabled{};
        reinterpret_cast<PFN_vkGetPhysicalDeviceFeatures>(entry("vkGetPhysicalDeviceFeatures"))(physical[selected],&available);
        precise_raster=available.shaderInt64; // integer interpolation: no Float64 needed
        if(precise_raster)enabled.shaderInt64=VK_TRUE;
        device_info.pEnabledFeatures=&enabled;
        // Diagnostic (KON_SHADER_INFO): the AMD driver's register and scratch use of the raster shader.
        const char* amd_info_name=VK_AMD_SHADER_INFO_EXTENSION_NAME;bool amd_info=false;
        if(std::getenv("KON_SHADER_INFO")){
            const auto enumerate=reinterpret_cast<PFN_vkEnumerateDeviceExtensionProperties>(entry("vkEnumerateDeviceExtensionProperties"));
            std::uint32_t count=0;enumerate(physical[selected],nullptr,&count,nullptr);std::vector<VkExtensionProperties> extensions(count);enumerate(physical[selected],nullptr,&count,extensions.data());
            for(const auto& e:extensions)if(std::string(e.extensionName)==amd_info_name)amd_info=true;
            if(amd_info){device_info.enabledExtensionCount=1;device_info.ppEnabledExtensionNames=&amd_info_name;}else std::cout<<"shader_info unavailable"<<std::endl;
        }
        const auto create_device=reinterpret_cast<PFN_vkCreateDevice>(entry("vkCreateDevice"));
        checked(create_device(physical[selected],&device_info,nullptr,&device),"Create persistent clear device");
        const auto get_device=reinterpret_cast<PFN_vkGetDeviceProcAddr>(entry("vkGetDeviceProcAddr"));
        const auto device_entry=[&](const char* symbol){auto result=get_device(device,symbol);if(!result)throw std::runtime_error(std::string("Missing Vulkan device entry ")+symbol);return result;};
#define LOAD(n) n=reinterpret_cast<PFN_##n>(device_entry(#n))
        LOAD(vkDestroyDevice);LOAD(vkDeviceWaitIdle);LOAD(vkDestroyBuffer);LOAD(vkFreeMemory);LOAD(vkDestroyCommandPool);LOAD(vkDestroyFence);LOAD(vkUnmapMemory);LOAD(vkDestroyShaderModule);LOAD(vkDestroyDescriptorSetLayout);LOAD(vkDestroyPipelineLayout);LOAD(vkDestroyPipeline);LOAD(vkDestroyDescriptorPool);
        LOAD(vkResetCommandPool);LOAD(vkResetFences);LOAD(vkBeginCommandBuffer);LOAD(vkCmdPipelineBarrier);LOAD(vkCmdBindPipeline);LOAD(vkCmdBindDescriptorSets);LOAD(vkCmdCopyBuffer);LOAD(vkCmdPushConstants);LOAD(vkCmdDispatch);LOAD(vkEndCommandBuffer);LOAD(vkQueueSubmit);LOAD(vkWaitForFences);LOAD(vkGetFenceStatus);
        LOAD(vkCreateQueryPool);LOAD(vkDestroyQueryPool);LOAD(vkCmdResetQueryPool);LOAD(vkCmdWriteTimestamp);LOAD(vkGetQueryPoolResults);
#undef LOAD
#define LOCAL(n) const auto n=reinterpret_cast<PFN_##n>(device_entry(#n))
        LOCAL(vkGetDeviceQueue);LOCAL(vkCreateBuffer);LOCAL(vkGetBufferMemoryRequirements);LOCAL(vkAllocateMemory);LOCAL(vkBindBufferMemory);LOCAL(vkCreateShaderModule);LOCAL(vkCreateDescriptorSetLayout);LOCAL(vkCreatePipelineLayout);LOCAL(vkCreateComputePipelines);LOCAL(vkCreateDescriptorPool);LOCAL(vkAllocateDescriptorSets);LOCAL(vkUpdateDescriptorSets);LOCAL(vkCreateCommandPool);LOCAL(vkAllocateCommandBuffers);LOCAL(vkCreateFence);LOCAL(vkMapMemory);
#undef LOCAL
        vkGetDeviceQueue(device,queue_family,0,&queue);
        VkBufferCreateInfo buffer_info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};buffer_info.size=capacity*4u;buffer_info.usage=VK_BUFFER_USAGE_STORAGE_BUFFER_BIT|VK_BUFFER_USAGE_TRANSFER_SRC_BIT|VK_BUFFER_USAGE_TRANSFER_DST_BIT;buffer_info.sharingMode=VK_SHARING_MODE_EXCLUSIVE;
        checked(vkCreateBuffer(device,&buffer_info,nullptr,&buffer),"Create persistent clear storage");
        VkMemoryRequirements requirements{};vkGetBufferMemoryRequirements(device,buffer,&requirements);
        VkPhysicalDeviceMemoryProperties memory_properties{};reinterpret_cast<PFN_vkGetPhysicalDeviceMemoryProperties>(entry("vkGetPhysicalDeviceMemoryProperties"))(physical[selected],&memory_properties);
        unsigned memory_type=~0u;
        for(unsigned n=0;n<memory_properties.memoryTypeCount;++n){
            const auto flags=memory_properties.memoryTypes[n].propertyFlags;
            if(!(requirements.memoryTypeBits&(1u<<n)) || (flags&(VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT))!=(VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT))continue;
            // Both upload and completed readback use this mapping. An uncached
            // CPU read can dominate the native draw even when GPU work is fast.
            const bool preferred=((flags&VK_MEMORY_PROPERTY_HOST_CACHED_BIT)!=0)==prefer_cached_reads;
            const bool selected_preferred=memory_type!=~0u && (((memory_properties.memoryTypes[memory_type].propertyFlags&VK_MEMORY_PROPERTY_HOST_CACHED_BIT)!=0)==prefer_cached_reads);
            if(memory_type==~0u || (preferred && !selected_preferred))memory_type=n;
        }
        if(memory_type==~0u)throw std::runtime_error("No coherent clear buffer memory");
        allocation_flags=memory_properties.memoryTypes[memory_type].propertyFlags;
        VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};allocation.allocationSize=requirements.size;allocation.memoryTypeIndex=memory_type;
        checked(vkAllocateMemory(device,&allocation,nullptr,&memory),"Allocate persistent clear storage");checked(vkBindBufferMemory(device,buffer,memory,0),"Bind persistent clear storage");
        if(precise_raster){
            checked(vkCreateBuffer(device,&buffer_info,nullptr,&raster_buffer),"Create device-local raster storage");
            VkMemoryRequirements gpu_requirements{};vkGetBufferMemoryRequirements(device,raster_buffer,&gpu_requirements);
            unsigned gpu_type=~0u;for(unsigned n=0;n<memory_properties.memoryTypeCount;++n)if((gpu_requirements.memoryTypeBits&(1u<<n)) && (memory_properties.memoryTypes[n].propertyFlags&VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)){gpu_type=n;break;}
            if(gpu_type==~0u)throw std::runtime_error("No device-local raster memory");
            VkMemoryAllocateInfo gpu_allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};gpu_allocation.allocationSize=gpu_requirements.size;gpu_allocation.memoryTypeIndex=gpu_type;
            checked(vkAllocateMemory(device,&gpu_allocation,nullptr,&raster_memory),"Allocate device-local raster memory");checked(vkBindBufferMemory(device,raster_buffer,raster_memory,0),"Bind device-local raster storage");
        }
        VkShaderModuleCreateInfo shader_info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};shader_info.codeSize=sizeof(native_clear_shader);shader_info.pCode=native_clear_shader;
        checked(vkCreateShaderModule(device,&shader_info,nullptr,&shader),"Create embedded own clear shader");
        // Binding 0: the working storage; binding 1: the host-visible copy (the
        // raster shader writes presentable images there directly).
        // Binding 2: the working storage again, read only in the raster shader.
        std::array<VkDescriptorSetLayoutBinding,3> bindings{};for(unsigned n=0;n<3;++n){bindings[n].binding=n;bindings[n].descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;bindings[n].descriptorCount=1;bindings[n].stageFlags=VK_SHADER_STAGE_COMPUTE_BIT;}
        VkDescriptorSetLayoutCreateInfo descriptor_info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};descriptor_info.bindingCount=3;descriptor_info.pBindings=bindings.data();
        checked(vkCreateDescriptorSetLayout(device,&descriptor_info,nullptr,&descriptor_layout),"Create clear binding layout");
        VkPushConstantRange push{VK_SHADER_STAGE_COMPUTE_BIT,0,48};VkPipelineLayoutCreateInfo layout_info{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};layout_info.setLayoutCount=1;layout_info.pSetLayouts=&descriptor_layout;layout_info.pushConstantRangeCount=1;layout_info.pPushConstantRanges=&push;
        checked(vkCreatePipelineLayout(device,&layout_info,nullptr,&pipeline_layout),"Create clear parameter layout");
        VkComputePipelineCreateInfo pipeline_info{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};pipeline_info.stage.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;pipeline_info.stage.stage=VK_SHADER_STAGE_COMPUTE_BIT;pipeline_info.stage.module=shader;pipeline_info.stage.pName="main";pipeline_info.layout=pipeline_layout;
        checked(vkCreateComputePipelines(device,nullptr,1,&pipeline_info,nullptr,&pipeline),"Compile persistent GPU clear pipeline");
        shader_info.codeSize=sizeof(native_triangle_shader);shader_info.pCode=native_triangle_shader;
        checked(vkCreateShaderModule(device,&shader_info,nullptr,&triangle_shader),"Create own flat triangle shader");
        pipeline_info.stage.module=triangle_shader;
        checked(vkCreateComputePipelines(device,nullptr,1,&pipeline_info,nullptr,&triangle_pipeline),"Compile native flat triangle pipeline");
        if(precise_raster){
            VkShaderModuleCreateInfo raster_info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};raster_info.codeSize=sizeof(native_raster_shader);raster_info.pCode=native_raster_shader;
            checked(vkCreateShaderModule(device,&raster_info,nullptr,&raster_shader),"Create embedded own raster shader");
            auto raster_create=pipeline_info;raster_create.stage.module=raster_shader;
            checked(vkCreateComputePipelines(device,nullptr,1,&raster_create,nullptr,&raster_pipeline),"Create exact ordered raster pipeline");
            // The 4x block pass alone (specialization constant 0 = 1): a smaller program.
            const std::uint32_t blocks_only=1;const VkSpecializationMapEntry entry_map{0,0,4};
            VkSpecializationInfo special{1,&entry_map,4,&blocks_only};raster_create.stage.pSpecializationInfo=&special;
            checked(vkCreateComputePipelines(device,nullptr,1,&raster_create,nullptr,&raster_blocks_pipeline),"Create 4x block raster pipeline");
            const std::uint32_t samples_only=2;special.pData=&samples_only;
            checked(vkCreateComputePipelines(device,nullptr,1,&raster_create,nullptr,&raster_samples_pipeline),"Create per-sample enlarged raster pipeline");
            const std::uint32_t blocks3=3;special.pData=&blocks3;
            checked(vkCreateComputePipelines(device,nullptr,1,&raster_create,nullptr,&raster_blocks3_pipeline),"Create 3x block raster pipeline");
                        if(amd_info)for(const auto pipe:{raster_pipeline,raster_blocks_pipeline}){const auto info=reinterpret_cast<PFN_vkGetShaderInfoAMD>(device_entry("vkGetShaderInfoAMD"));VkShaderStatisticsInfoAMD stats{};std::size_t size=sizeof(stats);
                if(info(device,pipe,VK_SHADER_STAGE_COMPUTE_BIT,VK_SHADER_INFO_TYPE_STATISTICS_AMD,&size,&stats)==VK_SUCCESS)
                    std::cout<<(pipe==raster_pipeline?"shader_info raster vgprs=":"shader_info blocks vgprs=")<<stats.resourceUsage.numUsedVgprs<<"/"<<stats.numAvailableVgprs<<" sgprs="<<stats.resourceUsage.numUsedSgprs<<"/"<<stats.numAvailableSgprs<<" lds="<<stats.resourceUsage.ldsUsageSizeInBytes<<" scratch="<<stats.resourceUsage.scratchMemUsageInBytes<<" physical_vgprs="<<stats.numPhysicalVgprs<<std::endl;}

        }
        VkDescriptorPoolSize descriptor_size{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,6};VkDescriptorPoolCreateInfo descriptor_pool_info{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};descriptor_pool_info.maxSets=2;descriptor_pool_info.poolSizeCount=1;descriptor_pool_info.pPoolSizes=&descriptor_size;
        checked(vkCreateDescriptorPool(device,&descriptor_pool_info,nullptr,&descriptor_pool),"Create persistent clear descriptor pool");
        VkDescriptorSetAllocateInfo set_info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};set_info.descriptorPool=descriptor_pool;set_info.descriptorSetCount=1;set_info.pSetLayouts=&descriptor_layout;
        checked(vkAllocateDescriptorSets(device,&set_info,&descriptor_set),"Allocate persistent clear descriptor");
        const auto bind=[&](VkDescriptorSet set,VkBuffer working){
            const std::array<VkDescriptorBufferInfo,3> buffers{{{working,0,capacity*4u},{buffer,0,capacity*4u},{working,0,capacity*4u}}};std::array<VkWriteDescriptorSet,3> writes{};
            for(unsigned n=0;n<3;++n){writes[n]={VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};writes[n].dstSet=set;writes[n].dstBinding=n;writes[n].descriptorCount=1;writes[n].descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;writes[n].pBufferInfo=&buffers[n];}
            vkUpdateDescriptorSets(device,3,writes.data(),0,nullptr);
        };
        bind(descriptor_set,buffer);
        if(precise_raster){
            checked(vkAllocateDescriptorSets(device,&set_info,&raster_set),"Allocate device-local raster descriptor");
            bind(raster_set,raster_buffer);
        }
        VkCommandPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};pool_info.queueFamilyIndex=queue_family;
        checked(vkCreateCommandPool(device,&pool_info,nullptr,&pool),"Create persistent clear command pool");
        VkCommandBufferAllocateInfo command_info{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};command_info.commandPool=pool;command_info.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY;command_info.commandBufferCount=1;
        checked(vkAllocateCommandBuffers(device,&command_info,&command),"Allocate persistent clear command");
        VkFenceCreateInfo fence_info{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};checked(vkCreateFence(device,&fence_info,nullptr,&fence),"Create persistent clear fence");
        for(unsigned slot=0;slot<2;++slot){
            checked(vkCreateCommandPool(device,&pool_info,nullptr,&batch_pools[slot]),"Create raster batch command pool");
            command_info.commandPool=batch_pools[slot];checked(vkAllocateCommandBuffers(device,&command_info,&batch_commands[slot]),"Allocate raster batch command");
            checked(vkCreateFence(device,&fence_info,nullptr,&batch_fences[slot]),"Create raster batch fence");
        }
        checked(vkMapMemory(device,memory,0,capacity*4u,0,&mapped),"Map persistent coherent clear storage");
        if(measure && timestamp_bits){
            VkQueryPoolCreateInfo query_info{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};query_info.queryType=VK_QUERY_TYPE_TIMESTAMP;query_info.queryCount=2;
            checked(vkCreateQueryPool(device,&query_info,nullptr,&query_pool),"Create diagnostic GPU timestamps");
        }
        if(precise_raster && timestamp_bits){
            VkQueryPoolCreateInfo query_info{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};query_info.queryType=VK_QUERY_TYPE_TIMESTAMP;query_info.queryCount=12; // six per batch slot
            checked(vkCreateQueryPool(device,&query_info,nullptr,&raster_queries),"Create raster batch GPU timestamps");
        }
    }
    void clear(std::span<std::uint32_t> pixels,unsigned stride,std::span<const NativeGpuClearRectangle> rectangles){
        if(pixels.empty() || pixels.size()>capacity || !stride || stride>2044u)throw std::runtime_error("Invalid GPU clear storage range");
        for(const auto& r:rectangles){
            if(r.left>r.right || r.top>r.bottom || r.right>stride || r.bottom>1024u || (r.left<r.right && r.top<r.bottom && (static_cast<std::uint64_t>(r.bottom-1u)*stride+r.right)>pixels.size()))throw std::runtime_error("GPU clear rectangle exceeds validated storage");
        }
        if(rectangles.empty())return;
        if(batch_inflight)throw std::runtime_error("GPU staging is owned by a submitted batch");
        checked(vkResetCommandPool(device,pool,0),"Reset completed clear commands");
        checked(vkResetFences(device,1,&fence),"Reset completed clear fence");
        auto point=measure?std::chrono::steady_clock::now():std::chrono::steady_clock::time_point{};
        const auto account=[&](std::uint64_t& total){if(measure){const auto now=std::chrono::steady_clock::now();total+=std::chrono::duration_cast<std::chrono::nanoseconds>(now-point).count();point=now;}};
        const auto bytes=pixels.size_bytes();std::memcpy(mapped,pixels.data(),bytes);account(times.upload_ns);
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};begin.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        checked(vkBeginCommandBuffer(command,&begin),"Begin persistent GPU clear");
        if(query_pool){vkCmdResetQueryPool(command,query_pool,0,2);vkCmdWriteTimestamp(command,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,query_pool,0);}
        VkMemoryBarrier upload{VK_STRUCTURE_TYPE_MEMORY_BARRIER};upload.srcAccessMask=VK_ACCESS_HOST_WRITE_BIT;upload.dstAccessMask=VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_SHADER_WRITE_BIT;
        vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_HOST_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,0,1,&upload,0,nullptr,0,nullptr);
        vkCmdBindPipeline(command,VK_PIPELINE_BIND_POINT_COMPUTE,pipeline);vkCmdBindDescriptorSets(command,VK_PIPELINE_BIND_POINT_COMPUTE,pipeline_layout,0,1,&descriptor_set,0,nullptr);
        for(const auto& r:rectangles){
            const std::array<unsigned,8> parameters{static_cast<unsigned>(pixels.size()),stride,r.left,r.top,r.right,r.bottom,r.color,r.mask};
            vkCmdPushConstants(command,pipeline_layout,VK_SHADER_STAGE_COMPUTE_BIT,0,sizeof(parameters),parameters.data());vkCmdDispatch(command,(r.right-r.left+63u)/64u,r.bottom-r.top,1);
            VkMemoryBarrier order{VK_STRUCTURE_TYPE_MEMORY_BARRIER};order.srcAccessMask=VK_ACCESS_SHADER_WRITE_BIT;order.dstAccessMask=VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_SHADER_WRITE_BIT;
            vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,0,1,&order,0,nullptr,0,nullptr);
        }
        VkMemoryBarrier readback{VK_STRUCTURE_TYPE_MEMORY_BARRIER};readback.srcAccessMask=VK_ACCESS_SHADER_WRITE_BIT;readback.dstAccessMask=VK_ACCESS_HOST_READ_BIT;
        vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,1,&readback,0,nullptr,0,nullptr);
        if(query_pool)vkCmdWriteTimestamp(command,VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,query_pool,1);
        checked(vkEndCommandBuffer(command),"Finish persistent GPU clear");account(times.record_ns);
        VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};submit.commandBufferCount=1;submit.pCommandBuffers=&command;
        checked(vkQueueSubmit(queue,1,&submit,fence),"Submit persistent GPU clear");account(times.submit_ns);
        checked(vkWaitForFences(device,1,&fence,VK_TRUE,5000000000ull),"Wait persistent GPU clear completion");account(times.wait_ns);
        std::memcpy(pixels.data(),mapped,bytes);account(times.download_ns);++completed;
        if(query_pool){
            std::array<std::uint64_t,2> values{};checked(vkGetQueryPoolResults(device,query_pool,0,2,sizeof(values),values.data(),sizeof(std::uint64_t),VK_QUERY_RESULT_64_BIT),"Read completed GPU timestamp queries");
            const auto wrap=timestamp_bits==64?~std::uint64_t{}:((std::uint64_t{1}<<timestamp_bits)-1u);
            times.gpu_elapsed_ns+=static_cast<std::uint64_t>(static_cast<double>((values[1]-values[0])&wrap)*timestamp_period);++times.timestamp_samples;
        }
    }

    void begin_batch(){if(batch_active || batch_recording)throw std::runtime_error("Nested GPU raster batch");use_slot(batch_inflight?1u-flight.slot:0u);recording_generation=++batch_generation;
        // Uploads of a batch that was never submitted did not happen.
        for(const auto token:texture_upload_tokens)texture_cache.erase(token);texture_uploads.clear();texture_upload_tokens.clear();batch_active=true;batch_cursor=batch_start;batch_colors=batch_depths=0;batch_packets.clear();batch_textures.clear();}
    NativeGpuRasterStats record_raster(std::span<std::uint32_t> colors,std::span<std::uint32_t> depths,
        std::span<const std::uint32_t> texture,std::span<const NativeGpuRasterTriangle> jobs,
        const std::array<std::uint32_t,32>& input,const std::vector<unsigned>& bounds,const std::vector<unsigned>& indices,unsigned tiles,std::span<const std::uint32_t> encoded,unsigned hires_offset=0,unsigned hires_factor=0){
        if(colors.size()>1024u*1024u-64 || depths.size()>1024u*1024u)throw std::runtime_error("GPU batch output exceeds fixed region");
        if(batch_recording)for(unsigned tag:{7u,8u,9u,28u,29u})if(batch_parameters[tag]!=input[tag])throw std::runtime_error("GPU batch changed framebuffer identity");
        auto* words=static_cast<std::uint32_t*>(mapped);
        unsigned shared_texture=cached_texture(texture,input[30]);
        // A nonzero decode token (input[30]) identifies immutable decoded
        // pixels exactly, so equal tokens reuse a packet without comparing.
        // Otherwise the address is only a hint and every byte is compared.
        if(!shared_texture && !texture.empty() && input[30])for(auto it=batch_textures.rbegin();it!=batch_textures.rend();++it)
            if(it->token==input[30] && it->size==texture.size()){shared_texture=it->offset;break;}
        // A tokened texture missing from the batch is a new decode: sharing by
        // content would only save staging space, and comparing it with every
        // same-sized texture cost several percent of each frame.
        if(!shared_texture && !texture.empty() && !input[30])for(auto it=batch_textures.rbegin();it!=batch_textures.rend();++it){
            if(it->source==reinterpret_cast<std::uintptr_t>(texture.data()) && it->size==texture.size() &&
               kon::bytes_equal(words+it->offset,texture.data(),texture.size_bytes())){shared_texture=it->offset;break;}
        }
        if(!shared_texture && !texture.empty() && !input[30])for(auto it=batch_textures.rbegin();it!=batch_textures.rend();++it){
            if(it->size==texture.size() && words[it->offset]==texture.front() && words[it->offset+it->size-1]==texture.back() &&
               kon::bytes_equal(words+it->offset,texture.data(),texture.size_bytes())){shared_texture=it->offset;break;}
        }
        auto p=input;const auto header=batch_cursor;std::size_t cursor=header+32;
        p[0]=batch_color;p[1]=batch_depth;p[2]=shared_texture?shared_texture:static_cast<unsigned>(cursor);
        if(!shared_texture)cursor+=texture.size();if(cursor&1)++cursor;
        p[3]=static_cast<unsigned>(cursor);cursor+=jobs.size()*kFixedJobWords;p[4]=static_cast<unsigned>(cursor);cursor+=8;
        p[5]=static_cast<unsigned>(cursor);cursor+=bounds.size();p[6]=static_cast<unsigned>(cursor);cursor+=indices.size();p[25]=tiles;
        p[31]=0;
        // Enlarged texture (2x or 4x): its word offset, with log2 of the factor in bits 28-29; 0 = none.
        p[26]=hires_offset && hires_offset<(1u<<28)?hires_offset|((hires_factor==4u?2u:1u)<<28):0u;
        std::size_t added=0;for(const auto& job:jobs){const auto& b=job.state;added+=std::size_t((b[2]-1)/16-b[0]/16+1)*((b[3]-1)/16-b[1]/16+1);}
        if(input[23]>kBatchColumns*16 || cursor+2*(batch_entries.size()+added)+2*std::size_t(kBatchColumns)*64+kDirtyWords>batch_end)throw NativeGpuBatchFull();
        if(!batch_recording){batch_recording=true;batch_parameters=input;colors_valid=depths_valid=texture_valid=false;batch_entries.clear();}
        std::memcpy(words+header,p.data(),sizeof(p));
        if(colors.size()>batch_colors)std::memcpy(words+batch_color+batch_colors,colors.data()+batch_colors,(colors.size()-batch_colors)*4);
        if(depths.size()>batch_depths)std::memcpy(words+batch_depth+batch_depths,depths.data()+batch_depths,(depths.size()-batch_depths)*4);
        if(!texture.empty() && !shared_texture){std::memcpy(words+p[2],texture.data(),texture.size_bytes());batch_textures.push_back({p[2],unsigned(texture.size()),reinterpret_cast<std::uintptr_t>(texture.data()),input[30]});}
        if(!encoded.empty())std::memcpy(words+p[3],encoded.data(),encoded.size_bytes());
        else for(std::size_t n=0;n<jobs.size();++n)write_fixed_job(words+p[3]+n*kFixedJobWords,jobs[n],(input[11]&1) && !(input[11]&2));
        std::memset(words+p[4],0,32);
        std::memcpy(words+p[5],bounds.data(),bounds.size()*4);std::memcpy(words+p[6],indices.data(),indices.size()*4);
        {static const bool trace=std::getenv("KON_COPY_TRACE")!=nullptr; // diagnostic: staging bytes by kind
         if(trace){static std::array<std::uint64_t,5> bytes{};static std::uint64_t draws=0;
          bytes[0]+=colors.size()>batch_colors?(colors.size()-batch_colors)*4:0;bytes[1]+=depths.size()>batch_depths?(depths.size()-batch_depths)*4:0;
          bytes[2]+=(!texture.empty() && !shared_texture)?texture.size_bytes():0;bytes[3]+=jobs.size()*kFixedJobWords*4;bytes[4]+=shared_texture?1:0;
          if(++draws%20000==0)std::cout<<"copy_trace draws="<<draws<<" color_MB="<<bytes[0]/1e6<<" depth_MB="<<bytes[1]/1e6<<" texture_MB="<<bytes[2]/1e6<<" jobs_MB="<<bytes[3]/1e6<<" shared_textures="<<bytes[4]<<"\n";}}
        // Commands are recorded at end_batch: fresh packets and extended
        // destination tails are disjoint from every prior dispatch, so one
        // upload precedes all shading. Guest accesses flush before mutation.
        for(unsigned n=0;n<jobs.size();++n){const auto& b=jobs[n].state;const unsigned job=p[3]+n*kFixedJobWords;
            for(unsigned ty=b[1]/16;ty<=(b[3]-1)/16;++ty)for(unsigned tx=b[0]/16;tx<=(b[2]-1)/16;++tx)batch_entries.push_back({ty*kBatchColumns+tx,header,job});}
        if(batch_packets.empty()){batch_left=p[21];batch_top=p[22];batch_right=p[23];batch_bottom=p[24];}
        else{batch_left=std::min(batch_left,p[21]);batch_top=std::min(batch_top,p[22]);batch_right=std::max(batch_right,p[23]);batch_bottom=std::max(batch_bottom,p[24]);}
        batch_cursor=static_cast<unsigned>((cursor+1)&~std::size_t(1));batch_colors=std::max(batch_colors,unsigned(colors.size()));batch_depths=std::max(batch_depths,unsigned(depths.size()));batch_packets.push_back({header,p[4]});
        return {0,0,0,0,true};
    }
    // Internal resolution: planes this batch shades at `scale` and the
    // original rectangles whose words changed since the plane's last batch
    // (written by the guest or the CPU renderer): they are copied, enlarged,
    // into the plane (on the GPU, and into its host mirror here) first.
    struct HiresPlan {int color=-1,depth=-1;unsigned rows=0,image=0;std::vector<std::array<unsigned,4>> color_rects,depth_rects;};
    void sync_plane(unsigned index,const std::uint32_t* original,unsigned count,std::vector<std::array<unsigned,4>>& rects,bool mirror){
        auto& plane=planes[index];const unsigned stride=plane.stride;if(!count || !stride)return;
        const unsigned rows=std::min(272u,(count+stride-1)/stride),width=std::min(stride,count-(rows-1)*stride);
        if(!plane.valid || plane.width!=width || plane.rows!=rows || plane.snapshot.size()<std::size_t(rows-1)*stride+width)rects.push_back({0,0,width,rows});
        else for(unsigned ty=0;ty<rows;ty+=16){unsigned run=~0u;
            // Unchanged 16-row bands (most of them) cost one bulk comparison.
            static const bool no_band=std::getenv("KON_NO_BAND_CMP")!=nullptr;
            if(!no_band){const std::size_t first=std::size_t(ty)*stride,last=std::size_t(std::min(rows,ty+16)-1)*stride+width;
             if(std::memcmp(original+first,plane.snapshot.data()+first,(last-first)*4u)==0)continue;}
            for(unsigned tx=0;tx<width+16;tx+=16){
                bool dirty=false;
                if(tx<width){const unsigned w=std::min(16u,width-tx)*4u;for(unsigned y=ty;y<std::min(rows,ty+16) && !dirty;++y)dirty=std::memcmp(original+std::size_t(y)*stride+tx,plane.snapshot.data()+std::size_t(y)*stride+tx,w)!=0;}
                if(dirty && run==~0u)run=tx;
                if(!dirty && run!=~0u){rects.push_back({run,ty,std::min(tx,width),std::min(rows,ty+16)});run=~0u;}
            }
        }
        plane.width=width;plane.rows=rows;
        if(!mirror)return;
        auto* out=static_cast<std::uint32_t*>(mapped)+plane_base(index);const unsigned n=scale,wide=stride*n;
        for(const auto& r:rects)for(unsigned y=r[1];y<r[3];++y){
            auto* row=out+std::size_t(y)*n*wide;
            for(unsigned x=r[0];x<r[2];++x){const auto v=original[std::size_t(y)*stride+x];for(unsigned i=0;i<n;++i)row[x*n+i]=v;}
            for(unsigned i=1;i<n;++i)std::memcpy(row+std::size_t(i)*wide+r[0]*n,row+r[0]*n,std::size_t(r[2]-r[0])*n*4u);
        }
    }
    // The plan in two parts: color (planes, image, color changes) and depth
    // changes; the color part can run before the batch in flight is collected.
    HiresPlan plan_hires(){HiresPlan plan;plan_color(plan);plan_depth(plan);return plan;}
    void plan_depth(HiresPlan& plan){
        if(plan.color<0 || !batch_depths)return;
        const auto t2=std::chrono::steady_clock::now();
        sync_plane(unsigned(plan.depth),static_cast<std::uint32_t*>(mapped)+batch_depth,batch_depths,plan.depth_rects,false);
        plan_split_ns[2]+=std::uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now()-t2).count());plan_split_ns[3]+=plan.depth_rects.size();
    }
    void plan_color(HiresPlan& plan){
        const auto& bp=batch_parameters;const unsigned stride=bp[7],zstride=bp[8]?bp[8]:bp[7];
        if(scale<2 || !displayed(bp[28]) || !stride || stride>512 || zstride>512)return;
        auto* words=static_cast<std::uint32_t*>(mapped);
        plan.color=int(acquire_plane(bp[28]&0x1FFFFFF0u,stride));plan.depth=int(acquire_plane(0x80000000u|(bp[29]&0x1FFFFFF0u),zstride));
        // This batch will overwrite the plane's presentable samples: wait for a
        // presentation still copying them (bounded: a hidden window may never paint).
        const auto t0=std::chrono::steady_clock::now();
        {const unsigned shown=planes[unsigned(plan.color)].shown,base=unsigned(plan.color)*kImages,next=(shown+1u)%kImages,other=(shown+2u)%kImages;
         plan.image=plane_readers[base+next].load()>0 && plane_readers[base+other].load()==0?other:next;
         for(unsigned spins=0;plane_readers[base+plan.image].load()>0 && spins<2000;++spins)std::this_thread::sleep_for(std::chrono::microseconds(50));}
        const auto t1=std::chrono::steady_clock::now();
        sync_plane(unsigned(plan.color),words+batch_color,batch_colors,plan.color_rects,false);
        plan_split_ns[0]+=std::uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(t1-t0).count());plan_split_ns[1]+=std::uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now()-t1).count());plan_split_ns[3]+=plan.color_rects.size();
        plan.rows=planes[unsigned(plan.color)].rows;
    }
    NativeGpuRasterBatch end_batch(){submit_batch();return collect_batch();}
    // Record and submit without waiting. Staging, command buffer and fence
    // stay owned by the GPU until collect_batch.
    // Diagnostic (KON_SUBMIT_SPLIT): host time of submit_batch by stage.
    std::array<std::uint64_t,5> submit_split_ns{};std::array<std::uint64_t,4> plan_split_ns{};std::uint64_t submit_split_batches=0;
    // Batch preparation that needs no GPU result (command buffer start, tile
    // binning and, when independent of the batch in flight, the plane plan)
    // runs before waiting for the batch in flight, while the GPU still works.
    struct Prepared {bool ready=false,planned=false;VkCommandPool pool=nullptr;VkCommandBuffer command=nullptr;VkFence fence=nullptr;unsigned q=0,tiles=0,dirty=0,table=0,entries=0,end=0,total=0;HiresPlan plan;} prepared;
    bool plan_independent_of_flight(){
        const auto& bp=batch_parameters;
        if(flight.color_plane<0 && flight.depth_plane<0)return true;
        const std::uint32_t color_key=bp[28]&0x1FFFFFF0u,depth_key=0x80000000u|(bp[29]&0x1FFFFFF0u);
        if(flight.color_plane>=0 && (planes[unsigned(flight.color_plane)].key==color_key || planes[unsigned(flight.color_plane)].key==depth_key))return false;
        if(flight.depth_plane>=0 && planes[unsigned(flight.depth_plane)].key==color_key)return false; // depth changes are planned after the collect
        return true;
    }
    void prepare_batch_now(){
        static const bool submit_split=std::getenv("KON_SUBMIT_SPLIT")!=nullptr;
        auto mark=std::chrono::steady_clock::now();
        const auto stage=[&](unsigned n){if(!submit_split)return;const auto now=std::chrono::steady_clock::now();submit_split_ns[n]+=std::uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(now-mark).count());mark=now;};
        if(!batch_recording || prepared.ready)return;
        const auto pool=batch_pools[batch_slot];const auto command=batch_commands[batch_slot];const auto fence=batch_fences[batch_slot];const unsigned q=batch_slot*6u;
        checked(vkResetCommandPool(device,pool,0),"Reset raster batch");checked(vkResetFences(device,1,&fence),"Reset raster batch fence");
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};begin.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;checked(vkBeginCommandBuffer(command,&begin),"Begin raster batch");
        // All coherent staging writes finished before this submit; no mapped
        // byte changes while the GPU owns it.
        VkMemoryBarrier host{VK_STRUCTURE_TYPE_MEMORY_BARRIER};host.srcAccessMask=VK_ACCESS_HOST_WRITE_BIT;host.dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT;
        vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_HOST_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,1,&host,0,nullptr,0,nullptr);
        if(raster_queries){vkCmdResetQueryPool(command,raster_queries,q,6);vkCmdWriteTimestamp(command,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,raster_queries,q);}
        // Stable counting sort of (tile, header, job) entries into the
        // absolute 16x16 tile grid; each tile keeps original draw/job order.
        auto* words=static_cast<std::uint32_t*>(mapped);
        const unsigned rows=(batch_bottom+15)/16,tiles=rows*kBatchColumns,dirty=batch_cursor,table=dirty+kDirtyWords,entries=table+tiles*2;
        auto& fill=bin_fill;fill.assign(tiles,0);for(const auto& entry:batch_entries)++fill[entry[0]];
        unsigned total=0;for(unsigned tile=0;tile<tiles;++tile){words[table+tile*2]=total;words[table+tile*2+1]=fill[tile];const auto count=fill[tile];fill[tile]=total;total+=count;}
        for(const auto& entry:batch_entries){const auto at=entries+2*fill[entry[0]]++;words[at]=entry[1];words[at+1]=entry[2];}
        const auto end=entries+2*total;
        prepared={true,false,pool,command,fence,q,tiles,dirty,table,entries,end,total,{}};stage(0);
        // The enlarged-plane plan reads plane snapshots the batch in flight
        // updates when collected; it is made now only when that batch cannot
        // change them (the color part, when the color plane is another one).
        static const bool no_early=std::getenv("KON_NO_EARLY_PLAN")!=nullptr;
        if(batch_inflight && !no_early && plan_independent_of_flight()){plan_color(prepared.plan);prepared.planned=true;stage(1);}
    }
    void submit_batch(){
        static const bool submit_split=std::getenv("KON_SUBMIT_SPLIT")!=nullptr;
        auto mark=std::chrono::steady_clock::now();
        const auto stage=[&](unsigned n){if(!submit_split)return;const auto now=std::chrono::steady_clock::now();submit_split_ns[n]+=std::uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(now-mark).count());mark=now;};
        batch_active=false;if(!batch_recording)return;
        if(batch_inflight)throw std::runtime_error("GPU batch submitted while another is in flight");
        if(!prepared.ready)prepare_batch_now();
        batch_recording=false;
        auto* words=static_cast<std::uint32_t*>(mapped);
        const auto command=prepared.command;const auto fence=prepared.fence;const unsigned q=prepared.q,tiles=prepared.tiles,dirty=prepared.dirty,table=prepared.table,entries=prepared.entries,end=prepared.end,total=prepared.total;(void)prepared.pool;
        HiresPlan hires;if(prepared.planned){hires=std::move(prepared.plan);plan_depth(hires);}else hires=plan_hires();prepared=Prepared{};stage(1);
        if(std::getenv("KON_GPU_BATCH_TRACE"))std::cout<<"gpu_batch draws="<<batch_packets.size()<<" union="<<batch_left<<","<<batch_top<<"-"<<batch_right<<","<<batch_bottom<<" tiles="<<tiles<<" entries="<<total<<" table="<<table<<" end="<<end<<" capacity="<<capacity<<" color="<<batch_color<<"+"<<batch_colors<<" depth="<<batch_depth<<"+"<<batch_depths<<" start="<<batch_start<<'\n';
        std::vector<VkBufferCopy> uploads{{VkDeviceSize(batch_start)*4,VkDeviceSize(batch_start)*4,VkDeviceSize(end-batch_start)*4},{VkDeviceSize(batch_color)*4,VkDeviceSize(batch_color)*4,VkDeviceSize(batch_colors)*4}};
        uploads.insert(uploads.end(),texture_uploads.begin(),texture_uploads.end());texture_uploads.clear();texture_upload_tokens.clear();
        if(batch_depths)uploads.push_back({VkDeviceSize(batch_depth)*4,VkDeviceSize(batch_depth)*4,VkDeviceSize(batch_depths)*4});
        vkCmdCopyBuffer(command,buffer,raster_buffer,uploads.size(),uploads.data());
        VkMemoryBarrier uploaded{VK_STRUCTURE_TYPE_MEMORY_BARRIER};uploaded.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;uploaded.dstAccessMask=VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_SHADER_WRITE_BIT;
        vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,0,1,&uploaded,0,nullptr,0,nullptr);
        if(raster_queries)vkCmdWriteTimestamp(command,VK_PIPELINE_STAGE_TRANSFER_BIT,raster_queries,q+1);
        vkCmdBindPipeline(command,VK_PIPELINE_BIND_POINT_COMPUTE,raster_pipeline);vkCmdBindDescriptorSets(command,VK_PIPELINE_BIND_POINT_COMPUTE,pipeline_layout,0,1,&raster_set,0,nullptr);
        const unsigned n=scale,hires_bottom=hires.color>=0?std::min(batch_bottom,hires.rows):0;
        // The enlarged pass reads tiles the guest or the CPU changed
        // (cleared, usually: the whole screen every frame) from the original
        // words itself; only changes outside the batch are enlarged first.
        std::memset(words+dirty,0,kDirtyWords*4u);
        {const unsigned zstride=batch_parameters[8]?batch_parameters[8]:batch_parameters[7];
         words[dirty]=batch_color;words[dirty+1]=batch_parameters[7];words[dirty+2]=batch_depth;words[dirty+3]=zstride;}
        std::array<std::vector<std::array<unsigned,4>>,2> enlarge;
        if(hires.color>=0)for(unsigned kind=0;kind<2;++kind)for(const auto& r:(kind?hires.depth_rects:hires.color_rects)){
            if(n>1 && r[0]>=batch_left && r[1]>=batch_top && r[2]<=batch_right && r[3]<=hires_bottom && r[0]%16==0 && r[1]%16==0 && (r[2]%16==0 || r[2]==batch_right) && (r[3]%16==0 || r[3]==hires_bottom)){
                for(unsigned ty=r[1]/16;ty<(r[3]+15)/16;++ty)for(unsigned tx=r[0]/16;tx<(r[2]+15)/16;++tx){const unsigned tile=ty*kBatchColumns+tx;words[dirty+4+kind*kDirtyBits+tile/32]|=1u<<(tile%32);}
            }else enlarge[kind].push_back(r);
        }
        if(hires.color>=0 && (!enlarge[0].empty() || !enlarge[1].empty())){
            // Enlarge what changed outside batches before either pass shades.
            const unsigned zstride=batch_parameters[8]?batch_parameters[8]:batch_parameters[7];
            for(unsigned kind=0;kind<2;++kind)for(const auto& r:enlarge[kind]){
                const std::array<unsigned,12> up{0xFFFFFFFEu,batch_color,batch_depth,batch_parameters[7],zstride,r[0],r[1],r[2],n,plane_base(unsigned(hires.color)),plane_base(unsigned(hires.depth)),r[3]|(kind?0x20000u:0x10000u)};
                vkCmdPushConstants(command,pipeline_layout,VK_SHADER_STAGE_COMPUTE_BIT,0,sizeof(up),up.data());vkCmdDispatch(command,((r[2]-r[0])*n+7)/8,((r[3]-r[1])*n+7)/8,1);
            }
            VkMemoryBarrier enlarged{VK_STRUCTURE_TYPE_MEMORY_BARRIER};enlarged.srcAccessMask=VK_ACCESS_SHADER_WRITE_BIT;enlarged.dstAccessMask=VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_SHADER_WRITE_BIT;
            vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,0,1,&enlarged,0,nullptr,0,nullptr);++raster_barriers;
        }
        // A batch wholly inside an enlarged plane takes its original pixels from
        // the enlarged result (one sample each) instead of shading them again:
        // at internal resolution the shown buffer's words are no longer exact
        // (KON_EXACT_DISPLAYED keeps the exact pass, for comparisons).
        static const bool exact_displayed=std::getenv("KON_EXACT_DISPLAYED")!=nullptr;
        const bool derived=!exact_displayed && hires.color>=0 && batch_top<hires_bottom && batch_bottom<=hires_bottom && batch_right<=planes[unsigned(hires.color)].width;
        if(!derived){
            // One dispatch shades the whole batch: each pixel walks its tile's
            // entries in original order, so no barrier separates the draws.
            const std::array<unsigned,12> push{0xFFFFFFFFu,table,entries,kBatchColumns,batch_packets.front().stats,batch_left,batch_top,batch_bottom,1u,0u,0u,0u};
            vkCmdPushConstants(command,pipeline_layout,VK_SHADER_STAGE_COMPUTE_BIT,0,sizeof(push),push.data());
            vkCmdDispatch(command,(batch_right-batch_left+7)/8,(batch_bottom-batch_top+7)/8,1);++raster_dispatches;
        }
        if(raster_queries)vkCmdWriteTimestamp(command,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,raster_queries,q+4);
        if(hires.color>=0 && batch_top<hires_bottom){
            // The same entries again on the enlarged planes (writes disjoint
            // from the original pass, which keeps the statistics).
            const std::array<unsigned,12> high{0xFFFFFFFFu,table,entries,kBatchColumns,derived?batch_packets.front().stats:0u,batch_left,batch_top,hires_bottom,n,plane_base(unsigned(hires.color)),plane_base(unsigned(hires.depth)),hires.rows*n};
            vkCmdPushConstants(command,pipeline_layout,VK_SHADER_STAGE_COMPUTE_BIT,0,sizeof(high),high.data());
            // The shader starts its workgroups at multiples of 8 samples (at 4x,
            // 16: each thread then owns 2x2 samples).
            // 4x and 3x: 2x2-sample blocks (KON_SAMPLES_3X keeps per-sample shading at 3x).
            static const bool samples3=std::getenv("KON_SAMPLES_3X")!=nullptr;
            if(n==4 || (n==3 && !samples3)){vkCmdBindPipeline(command,VK_PIPELINE_BIND_POINT_COMPUTE,n==4?raster_blocks_pipeline:raster_blocks3_pipeline);
                vkCmdDispatch(command,(batch_right*n-((batch_left*n)&~15u)+15)/16,(hires_bottom*n-((batch_top*n)&~15u)+15)/16,1);
                vkCmdBindPipeline(command,VK_PIPELINE_BIND_POINT_COMPUTE,raster_pipeline);}
            else{vkCmdBindPipeline(command,VK_PIPELINE_BIND_POINT_COMPUTE,raster_samples_pipeline);
                vkCmdDispatch(command,(batch_right*n-((batch_left*n)&~7u)+7)/8,(hires_bottom*n-((batch_top*n)&~7u)+7)/8,1);
                vkCmdBindPipeline(command,VK_PIPELINE_BIND_POINT_COMPUTE,raster_pipeline);}
            ++raster_dispatches;
        }
        if(derived){
            VkMemoryBarrier shaded{VK_STRUCTURE_TYPE_MEMORY_BARRIER};shaded.srcAccessMask=VK_ACCESS_SHADER_WRITE_BIT;shaded.dstAccessMask=VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_SHADER_WRITE_BIT;
            vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,0,1,&shaded,0,nullptr,0,nullptr);++raster_barriers;
            const unsigned zstride=batch_parameters[8]?batch_parameters[8]:batch_parameters[7];
            const std::array<unsigned,12> down{0xFFFFFFFCu,plane_base(unsigned(hires.color)),batch_color,batch_parameters[7],plane_base(unsigned(hires.depth)),batch_left,batch_top,batch_right,batch_bottom,n,batch_depths?batch_depth:0u,zstride};
            vkCmdPushConstants(command,pipeline_layout,VK_SHADER_STAGE_COMPUTE_BIT,0,sizeof(down),down.data());
            vkCmdDispatch(command,(batch_right-batch_left+7)/8,(batch_bottom-batch_top+7)/8,1);++raster_dispatches;
        }
        if(raster_queries)vkCmdWriteTimestamp(command,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,raster_queries,q+5);
        // Original rectangle whose enlarged samples changed (shaded or copied
        // from the original buffer): converted to BGRA for presentation.
        std::optional<std::array<unsigned,4>> present;
        if(hires.color>=0){
            const auto grow=[&](unsigned l,unsigned t,unsigned r,unsigned b){if(l>=r || t>=b)return;if(!present)present=std::array<unsigned,4>{l,t,r,b};else{auto& p=*present;p={std::min(p[0],l),std::min(p[1],t),std::max(p[2],r),std::max(p[3],b)};}};
            // The whole plane: the image being filled lacks the previous batch's changes.
            const auto& plane=planes[unsigned(hires.color)];grow(0,0,plane.width,plane.rows);
        }
        if(present){
            VkMemoryBarrier shaded{VK_STRUCTURE_TYPE_MEMORY_BARRIER};shaded.srcAccessMask=VK_ACCESS_SHADER_WRITE_BIT;shaded.dstAccessMask=VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_SHADER_WRITE_BIT;
            vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,0,1,&shaded,0,nullptr,0,nullptr);++raster_barriers;
            const auto& r=*present;
            const std::array<unsigned,12> convert{0xFFFFFFFDu,plane_base(unsigned(hires.color)),bgra_base(unsigned(hires.color),hires.image),batch_parameters[7]*n,batch_parameters[9],r[0]*n,r[1]*n,r[2]*n,r[3]*n,0u,0u,0u};
            vkCmdPushConstants(command,pipeline_layout,VK_SHADER_STAGE_COMPUTE_BIT,0,sizeof(convert),convert.data());
            vkCmdDispatch(command,((r[2]-r[0])*n+7)/8,((r[3]-r[1])*n+7)/8,1);++raster_dispatches;
        }
        VkMemoryBarrier copy{VK_STRUCTURE_TYPE_MEMORY_BARRIER};copy.srcAccessMask=VK_ACCESS_SHADER_WRITE_BIT;copy.dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT;
        vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,1,&copy,0,nullptr,0,nullptr);
        if(raster_queries)vkCmdWriteTimestamp(command,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,raster_queries,q+2);
        std::vector<VkBufferCopy> outputs{{batch_color*4,batch_color*4,VkDeviceSize(batch_colors)*4}};if(batch_depths)outputs.push_back({batch_depth*4,batch_depth*4,VkDeviceSize(batch_depths)*4});
        for(const auto& packet:batch_packets){outputs.push_back({VkDeviceSize(packet.header+31)*4,VkDeviceSize(packet.header+31)*4,4});outputs.push_back({VkDeviceSize(packet.stats)*4,VkDeviceSize(packet.stats)*4,32});}
        vkCmdCopyBuffer(command,raster_buffer,buffer,outputs.size(),outputs.data());
        VkMemoryBarrier readback{VK_STRUCTURE_TYPE_MEMORY_BARRIER};readback.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT|VK_ACCESS_SHADER_WRITE_BIT;readback.dstAccessMask=VK_ACCESS_HOST_READ_BIT;
        vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_TRANSFER_BIT|VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,1,&readback,0,nullptr,0,nullptr);
        if(raster_queries)vkCmdWriteTimestamp(command,VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,raster_queries,q+3);
        checked(vkEndCommandBuffer(command),"Finish raster batch");stage(2);
        VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};submit.commandBufferCount=1;submit.pCommandBuffers=&command;checked(vkQueueSubmit(queue,1,&submit,fence),"Submit raster batch");batch_inflight=true;stage(3);
        if(submit_split && ++submit_split_batches%500==0){std::cout<<"submit_split batches=500 bin_us="<<submit_split_ns[0]/500000<<" plan_hires_us="<<submit_split_ns[1]/500000<<" record_us="<<submit_split_ns[2]/500000<<" queue_submit_us="<<submit_split_ns[3]/500000<<" plan_wait_us="<<plan_split_ns[0]/500000<<" sync_color_us="<<plan_split_ns[1]/500000<<" sync_depth_us="<<plan_split_ns[2]/500000<<" rects_per_batch="<<plan_split_ns[3]/500<<std::endl;submit_split_ns={};plan_split_ns={};}
        flight.generation=recording_generation;flight.slot=batch_slot;flight.color=batch_color;flight.depth=batch_depth;flight.colors=batch_colors;flight.depths=batch_depths;flight.packets.swap(batch_packets);flight.parameters=batch_parameters;
        flight.color_plane=hires.color;flight.depth_plane=hires.depth;flight.image=hires.image;flight.samples=derived?n*n:1u;
    }
    NativeGpuRasterBatch collect_batch(){
        NativeGpuRasterBatch result;if(!batch_inflight)return result;
        batch_inflight=false;checked(vkWaitForFences(device,1,&batch_fences[flight.slot],VK_TRUE,5000000000ull),"Wait raster batch");
        if(raster_queries){std::array<std::uint64_t,6> v{};checked(vkGetQueryPoolResults(device,raster_queries,flight.slot*6u,6,sizeof(v),v.data(),sizeof(std::uint64_t),VK_QUERY_RESULT_64_BIT),"Read raster batch timestamps");
            const auto wrap=timestamp_bits==64?~std::uint64_t{}:((std::uint64_t{1}<<timestamp_bits)-1u);const auto ns=[&](unsigned a,unsigned b){return static_cast<std::uint64_t>(double((v[b]-v[a])&wrap)*timestamp_period);};
            raster_upload_ns+=ns(0,1);raster_shade_ns+=ns(1,2);raster_readback_ns+=ns(2,3);
            // Diagnostic (KON_GPU_SPLIT): original, enlarged and conversion passes.
            static const bool split=std::getenv("KON_GPU_SPLIT")!=nullptr;
            if(split){split_ns[0]+=ns(1,4);split_ns[1]+=ns(4,5);split_ns[2]+=ns(5,2);
                if(++split_batches%500==0){std::cout<<"gpu_split batches=500 native_us="<<split_ns[0]/500000<<" hires_us="<<split_ns[1]/500000<<" convert_us="<<split_ns[2]/500000<<'\n';split_ns={};}}}
        auto* words=static_cast<std::uint32_t*>(mapped);
        for(const auto& packet:flight.packets){if(words[packet.header+31])throw std::runtime_error("GPU batch rejected coordinates before guest commit");
            const auto count=[&](unsigned n){return std::uint64_t(words[packet.stats+n*2])|(std::uint64_t(words[packet.stats+n*2+1])<<32u);};
            // Derived batches counted enlarged samples: back to original pixels.
            result.stats.covered+=count(0)/flight.samples;result.stats.alpha+=count(1)/flight.samples;result.stats.depth+=count(2)/flight.samples;result.stats.drawn+=count(3)/flight.samples;
        }
        result.color_words={words+flight.color,flight.colors};result.depth_words={words+flight.depth,flight.depths};result.parameters=flight.parameters;result.draws=flight.packets.size();
        // The enlarged planes now match these original words.
        if(flight.color_plane>=0){auto& plane=planes[unsigned(flight.color_plane)];plane.snapshot.assign(words+flight.color,words+flight.color+flight.colors);plane.valid=true;plane.shown=flight.image;}
        if(flight.depth_plane>=0 && flight.depths){auto& plane=planes[unsigned(flight.depth_plane)];plane.snapshot.assign(words+flight.depth,words+flight.depth+flight.depths);plane.valid=true;}
        flight.color_plane=flight.depth_plane=-1;
        ++completed;++completed_raster;return result;
    }
    NativeGpuRasterStats raster(std::span<std::uint32_t> colors,std::span<std::uint32_t> depths,
        std::span<const std::uint32_t> texture,std::span<const NativeGpuRasterTriangle> jobs,
        const std::array<std::uint32_t,32>& input){
        if(!precise_raster)throw std::runtime_error("Native raster needs Vulkan int64 support");
        auto point=measure?std::chrono::steady_clock::now():std::chrono::steady_clock::time_point{};
        const auto account=[&](std::uint64_t& total){if(measure){const auto now=std::chrono::steady_clock::now();total+=std::chrono::duration_cast<std::chrono::nanoseconds>(now-point).count();point=now;}};
        auto p=input;const auto left=p[21],top=p[22],right=p[23],bottom=p[24];
        if(!p[7] || p[7]>2044 || left>right || top>bottom || right>p[7] || bottom>1024 || p[9]>3 || (p[11]&~127u))throw std::runtime_error("Invalid Vulkan raster state");
        // Encoded jobs apply to this call only, whatever its outcome.
        const auto encoded=encoded_jobs;encoded_jobs={};
        const auto hires=hires_texels;const unsigned hires_factor=hires_scale;const auto hires_key=hires_token|0x80000000u;hires_texels={};hires_scale=0;
        if(left==right || top==bottom || jobs.empty())return {};
        const auto color_need=std::size_t(bottom-1)*p[7]+right;
        if(colors.size()<color_need || ((p[11]&8) && (!p[8] || right>p[8] || depths.size()<std::size_t(bottom-1)*p[8]+right)))throw std::runtime_error("Vulkan raster output exceeds storage");
        if((p[11]&1) && (!p[18] || !p[19] || p[18]>512 || p[19]>512 || texture.size()!=std::size_t(p[18])*p[19]))throw std::runtime_error("Vulkan raster texture range mismatch");
        const bool prevalidated=encoded.size()==jobs.size()*kFixedJobWords;
        if(!prevalidated)for(const auto& job:jobs)validate_raster_job(job,(p[11]&1)!=0,left,top,right,bottom);
        // A batch shades through its global tile table: per-draw bins unused.
        if(batch_active){static const std::vector<unsigned> none;
            // Enlarged texture for the internal-resolution pass (texture cache ring; skipped when it cannot fit now).
            unsigned hires_offset=0;
            if((p[11]&1) && hires_factor && hires.size()==std::size_t(p[18])*p[19]*hires_factor*hires_factor)hires_offset=cached_texture(hires,hires_key);
            return record_raster(colors,depths,texture,jobs,input,none,none,0,prevalidated?encoded:std::span<const std::uint32_t>{},hires_offset,hires_offset?hires_factor:0u);}
        // Counting sort into reused flat arrays: no per-tile heap allocation,
        // and each tile keeps ascending (original) job order.
        const auto tx=(right-left+15)/16,ty=(bottom-top+15)/16;auto& fill=bin_fill;fill.assign(std::size_t(tx)*ty,0);
        const auto tiles_of=[&](const NativeGpuRasterTriangle& job,auto&& visit){const auto& b=job.state;
            for(unsigned y=(b[1]-top)/16;y<=(b[3]-1-top)/16;++y)for(unsigned x=(b[0]-left)/16;x<=(b[2]-1-left)/16;++x)visit(y*tx+x);};
        for(const auto& job:jobs)tiles_of(job,[&](unsigned tile){++fill[tile];});
        auto& bounds=bin_bounds;auto& indices=bin_indices;bounds.resize(fill.size()*2);unsigned total=0;
        for(std::size_t t=0;t<fill.size();++t){bounds[t*2]=total;bounds[t*2+1]=fill[t];total+=fill[t];fill[t]=bounds[t*2];
            const auto tx0=left+unsigned(t%tx)*16,ty0=top+unsigned(t/tx)*16;tile_job_pixels+=std::uint64_t(bounds[t*2+1])*(std::min(tx0+16,right)-tx0)*(std::min(ty0+16,bottom)-ty0);}
        indices.resize(total);for(unsigned n=0;n<jobs.size();++n)tiles_of(jobs[n],[&](unsigned tile){indices[fill[tile]++]=n;});
        if(measure && !completed_raster){unsigned largest=0;for(std::size_t t=0;t<bounds.size();t+=2)largest=std::max(largest,bounds[t+1]);std::cout<<"raster_jobs="<<jobs.size()<<" tile_indices="<<indices.size()<<" bins="<<bounds.size()/2<<" max_jobs_per_tile="<<largest<<" output_pixels="<<std::size_t(right-left)*(bottom-top)<<'\n';}
        constexpr std::size_t stats_count=8;std::size_t cursor=32;
        const auto reserve=[&](std::size_t words){const auto offset=cursor;cursor+=words;return static_cast<unsigned>(offset);};
        p[0]=reserve(colors.size());p[1]=reserve(depths.size());p[2]=reserve(texture.size());if(cursor&1)++cursor;
        p[3]=reserve(jobs.size()*kFixedJobWords);p[4]=reserve(stats_count);p[5]=reserve(bounds.size());p[6]=reserve(indices.size());p[25]=tx;p[26]=static_cast<unsigned>(jobs.size());p[31]=0;
        if(cursor>capacity)throw std::runtime_error("Vulkan raster packet exceeds bounded storage");

        account(times.bin_ns);
        if(batch_inflight)throw std::runtime_error("GPU staging is owned by a submitted batch");
        checked(vkResetCommandPool(device,pool,0),"Reset raster commands");checked(vkResetFences(device,1,&fence),"Reset raster fence");
        auto* words=static_cast<std::uint32_t*>(mapped);std::memcpy(words,p.data(),sizeof(p));
        std::memcpy(words+p[0],colors.data(),colors.size_bytes());if(!depths.empty())std::memcpy(words+p[1],depths.data(),depths.size_bytes());
        if(!texture.empty())std::memcpy(words+p[2],texture.data(),texture.size_bytes());for(std::size_t n=0;n<jobs.size();++n)write_fixed_job(words+p[3]+n*kFixedJobWords,jobs[n],(p[11]&1) && !(p[11]&2));
        std::memset(words+p[4],0,stats_count*4);
        std::memcpy(words+p[5],bounds.data(),bounds.size()*4);std::memcpy(words+p[6],indices.data(),indices.size()*4);
        account(times.upload_ns);
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};begin.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;checked(vkBeginCommandBuffer(command,&begin),"Begin ordered native raster");
        VkMemoryBarrier upload{VK_STRUCTURE_TYPE_MEMORY_BARRIER};upload.srcAccessMask=VK_ACCESS_HOST_WRITE_BIT;upload.dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT;
        vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_HOST_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,1,&upload,0,nullptr,0,nullptr);
        const auto same=[](const auto& cache,const auto& values){return cache.size()==values.size() && kon::bytes_equal(cache.data(),values.data(),values.size()*sizeof(values[0]));};
        const bool keep_colors=colors_valid && same(cached_raster_colors,colors);
        const bool keep_depths=depths_valid && cached_depth_offset==p[1] && same(cached_raster_depths,depths);
        const bool keep_texture=texture_valid && cached_texture_offset==p[2] && same(cached_raster_texture,texture);
        std::vector<VkBufferCopy> inputs{{0,0,32u*4}};
        if(!keep_colors)inputs.push_back({VkDeviceSize(p[0])*4,VkDeviceSize(p[0])*4,colors.size_bytes()});
        if(!keep_depths && !depths.empty())inputs.push_back({VkDeviceSize(p[1])*4,VkDeviceSize(p[1])*4,depths.size_bytes()});
        if(!keep_texture && !texture.empty())inputs.push_back({VkDeviceSize(p[2])*4,VkDeviceSize(p[2])*4,texture.size_bytes()});
        inputs.push_back({VkDeviceSize(p[3])*4,VkDeviceSize(p[3])*4,VkDeviceSize(cursor-p[3])*4});
        colors_valid=depths_valid=texture_valid=false;
        vkCmdCopyBuffer(command,buffer,raster_buffer,inputs.size(),inputs.data());
        VkMemoryBarrier ready{VK_STRUCTURE_TYPE_MEMORY_BARRIER};ready.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;ready.dstAccessMask=VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_SHADER_WRITE_BIT;
        vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,0,1,&ready,0,nullptr,0,nullptr);
        vkCmdBindPipeline(command,VK_PIPELINE_BIND_POINT_COMPUTE,raster_pipeline);vkCmdBindDescriptorSets(command,VK_PIPELINE_BIND_POINT_COMPUTE,pipeline_layout,0,1,&raster_set,0,nullptr);
        const std::array<unsigned,12> push{};vkCmdPushConstants(command,pipeline_layout,VK_SHADER_STAGE_COMPUTE_BIT,0,sizeof(push),push.data());vkCmdDispatch(command,(right-left+7)/8,(bottom-top+7)/8,1);
        VkMemoryBarrier copy{VK_STRUCTURE_TYPE_MEMORY_BARRIER};copy.srcAccessMask=VK_ACCESS_SHADER_WRITE_BIT;copy.dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT;
        vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,1,&copy,0,nullptr,0,nullptr);
        std::vector<VkBufferCopy> outputs{{31u*4,31u*4,4},{VkDeviceSize(p[0])*4,VkDeviceSize(p[0])*4,colors.size_bytes()},{VkDeviceSize(p[4])*4,VkDeviceSize(p[4])*4,stats_count*4}};
        if(!depths.empty())outputs.push_back({VkDeviceSize(p[1])*4,VkDeviceSize(p[1])*4,depths.size_bytes()});
        vkCmdCopyBuffer(command,raster_buffer,buffer,outputs.size(),outputs.data());
        VkMemoryBarrier readback{VK_STRUCTURE_TYPE_MEMORY_BARRIER};readback.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT|VK_ACCESS_SHADER_WRITE_BIT;readback.dstAccessMask=VK_ACCESS_HOST_READ_BIT;
        vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_TRANSFER_BIT|VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,1,&readback,0,nullptr,0,nullptr);checked(vkEndCommandBuffer(command),"Finish native raster");account(times.record_ns);
        VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};submit.commandBufferCount=1;submit.pCommandBuffers=&command;
        checked(vkQueueSubmit(queue,1,&submit,fence),"Submit native raster");account(times.submit_ns);checked(vkWaitForFences(device,1,&fence,VK_TRUE,5000000000ull),"Wait exact native raster");account(times.wait_ns);
        if(words[31])throw std::runtime_error("Vulkan raster rejected texture coordinate before VRAM commit");
        const auto count=[&](unsigned n){return std::uint64_t(words[p[4]+n*2])|(std::uint64_t(words[p[4]+n*2+1])<<32u);};
        NativeGpuRasterStats result{count(0),count(1),count(2),count(3)};
        std::memcpy(colors.data(),words+p[0],colors.size_bytes());if(!depths.empty())std::memcpy(depths.data(),words+p[1],depths.size_bytes());
        cached_raster_colors.assign(colors.begin(),colors.end());cached_raster_depths.assign(depths.begin(),depths.end());cached_raster_texture.assign(texture.begin(),texture.end());
        cached_depth_offset=p[1];cached_texture_offset=p[2];colors_valid=depths_valid=texture_valid=true;
        account(times.download_ns);++completed;++completed_raster;return result;
    }

    NativeGpuTriangleStats triangles(std::span<std::uint32_t> pixels,unsigned stride,
        std::span<const NativeGpuFlatTriangle> jobs,unsigned left,unsigned top,unsigned right,unsigned bottom,std::uint32_t mask){
        if(pixels.empty() || !stride || stride>2044u || left>right || top>bottom || right>stride || bottom>1024u ||
           (left<right && top<bottom && (std::uint64_t(bottom-1)*stride+right)>pixels.size()) ||
           pixels.size()*3ull+jobs.size()*16ull>capacity)throw std::runtime_error("Invalid GPU triangle storage range");
        // Restrict coordinates so every signed 32-bit edge product/sum is
        // bounded below INT32_MAX, including the entire 1024-pixel viewport.
        // Validation covers the complete batch before touching mapped storage.
        for(const auto& t:jobs){
            for(auto v:t.xy)if(v< -8192 || v>8191)throw std::runtime_error("GPU triangle coordinate outside exact integer range");
            const auto& v=t.xy;
            const auto area=std::int64_t(v[2]-v[0])*(v[5]-v[1])-std::int64_t(v[3]-v[1])*(v[4]-v[0]);
            unsigned inclusive=0;for(unsigned n=0;n<3;++n){const unsigned a=n*2,b=((n+1)%3)*2;if(v[b+1]<v[a+1] || (v[b+1]==v[a+1] && v[b]>v[a]))inclusive|=1u<<n;}
            if(area<=0 || t.edge_mask!=inclusive || t.alpha_pass>1 || t.left<left || t.top<top || t.right>right || t.bottom>bottom || t.left>t.right || t.top>t.bottom)throw std::runtime_error("Invalid normalized GPU triangle");
        }
        if(jobs.empty() || left==right || top==bottom)return {};
        if(batch_inflight)throw std::runtime_error("GPU staging is owned by a submitted batch");
        checked(vkResetCommandPool(device,pool,0),"Reset triangle commands");checked(vkResetFences(device,1,&fence),"Reset triangle fence");
        std::memcpy(mapped,pixels.data(),pixels.size_bytes());
        const auto n=static_cast<unsigned>(pixels.size());
        auto* words=static_cast<std::uint32_t*>(mapped);
        std::memcpy(words+3u*n,jobs.data(),jobs.size_bytes());
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};begin.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        checked(vkBeginCommandBuffer(command,&begin),"Begin ordered native triangles");
        VkMemoryBarrier upload{VK_STRUCTURE_TYPE_MEMORY_BARRIER};upload.srcAccessMask=VK_ACCESS_HOST_WRITE_BIT;upload.dstAccessMask=VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_SHADER_WRITE_BIT;
        vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_HOST_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,0,1,&upload,0,nullptr,0,nullptr);
        vkCmdBindPipeline(command,VK_PIPELINE_BIND_POINT_COMPUTE,triangle_pipeline);vkCmdBindDescriptorSets(command,VK_PIPELINE_BIND_POINT_COMPUTE,pipeline_layout,0,1,&descriptor_set,0,nullptr);
        const std::array<unsigned,8> params{n,stride,left,top,right,bottom,static_cast<unsigned>(jobs.size()),mask};
        vkCmdPushConstants(command,pipeline_layout,VK_SHADER_STAGE_COMPUTE_BIT,0,sizeof(params),params.data());
        vkCmdDispatch(command,(right-left+63u)/64u,bottom-top,1);
        VkMemoryBarrier readback{VK_STRUCTURE_TYPE_MEMORY_BARRIER};readback.srcAccessMask=VK_ACCESS_SHADER_WRITE_BIT;readback.dstAccessMask=VK_ACCESS_HOST_READ_BIT;
        vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,1,&readback,0,nullptr,0,nullptr);
        checked(vkEndCommandBuffer(command),"End native triangles");
        VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};submit.commandBufferCount=1;submit.pCommandBuffers=&command;
        checked(vkQueueSubmit(queue,1,&submit,fence),"Submit native triangles");checked(vkWaitForFences(device,1,&fence,VK_TRUE,5000000000ull),"Wait native triangles");
        NativeGpuTriangleStats stats;for(unsigned y=top;y<bottom;++y)for(unsigned x=left;x<right;++x){const auto offset=n+2u*(y*stride+x);stats.covered+=words[offset];stats.alpha_rejected+=words[offset+1u];}
        std::memcpy(pixels.data(),mapped,pixels.size_bytes());++completed;++completed_triangles;return stats;
    }

};
NativeVulkanClear::NativeVulkanClear(bool measure,bool prefer_cached_reads,std::string adapter_selector):impl_(std::make_unique<Impl>()){impl_->measure=measure;impl_->prefer_cached_reads=prefer_cached_reads;impl_->selector=native_gpu_selector(std::move(adapter_selector));impl_->initialize();}
NativeVulkanClear::~NativeVulkanClear()=default;
void NativeVulkanClear::clear(std::span<std::uint32_t> pixels,unsigned stride,std::span<const NativeGpuClearRectangle> rectangles){impl_->clear(pixels,stride,rectangles);}
NativeGpuTriangleStats NativeVulkanClear::triangles(std::span<std::uint32_t> pixels,unsigned stride,std::span<const NativeGpuFlatTriangle> jobs,unsigned left,unsigned top,unsigned right,unsigned bottom,std::uint32_t mask){return impl_->triangles(pixels,stride,jobs,left,top,right,bottom,mask);}
void NativeVulkanClear::begin_batch(){impl_->begin_batch();}
NativeGpuRasterBatch NativeVulkanClear::end_batch(){return impl_->end_batch();}
void NativeVulkanClear::submit_batch(){impl_->submit_batch();}
std::array<std::uint64_t,6> NativeVulkanClear::profile()const{return {impl_->raster_upload_ns,impl_->raster_shade_ns,impl_->raster_readback_ns,impl_->raster_dispatches,impl_->raster_barriers,impl_->tile_job_pixels};}
NativeGpuRasterBatch NativeVulkanClear::collect_batch(){return impl_->collect_batch();}
bool NativeVulkanClear::batch_ready(){return !impl_->batch_inflight || impl_->vkGetFenceStatus(impl_->device,impl_->batch_fences[impl_->flight.slot])==VK_SUCCESS;}
void NativeVulkanClear::set_encoded_jobs(std::span<const std::uint32_t> words){impl_->encoded_jobs=words;}
bool NativeVulkanClear::set_internal_scale(unsigned scale){
    if(scale<1 || scale>Impl::kMaxScale || !impl_->precise_raster)return false;
    if(scale!=impl_->scale){impl_->scale=scale;for(auto& plane:impl_->planes){plane.key=0;plane.valid=false;plane.snapshot.clear();}}
    return true;
}
void NativeVulkanClear::prepare_batch(){impl_->prepare_batch_now();}
unsigned NativeVulkanClear::internal_scale()const{return impl_->scale;}
void NativeVulkanClear::set_hires_texture(std::span<const std::uint32_t> texels,unsigned scale,std::uint32_t token){impl_->hires_texels=texels;impl_->hires_scale=scale;impl_->hires_token=token;}
void NativeVulkanClear::note_display_address(std::uint32_t address){
    auto& d=impl_->display_addresses;address&=0x1FFFFFF0u;
    if(!address || d[0].load(std::memory_order_relaxed)==address || d[1].load(std::memory_order_relaxed)==address)return;
    d[impl_->display_next.fetch_add(1,std::memory_order_relaxed)&1u].store(address,std::memory_order_relaxed);
}
NativeHiresView NativeVulkanClear::hires_view(std::uint32_t address){
    address&=0x1FFFFFF0u;if(impl_->scale<2)return {};
    for(unsigned n=0;n<Impl::kPlanes;++n){const auto& plane=impl_->planes[n];
        if(plane.key==address && plane.valid && plane.rows)
            return {static_cast<const std::uint32_t*>(impl_->mapped)+Impl::bgra_base(n,plane.shown),plane.snapshot,impl_->scale,plane.stride,plane.width,plane.rows,&impl_->plane_readers[n*Impl::kImages+plane.shown]};}
    return {};
}
NativeGpuRasterStats NativeVulkanClear::raster(std::span<std::uint32_t> colors,std::span<std::uint32_t> depths,std::span<const std::uint32_t> texture,std::span<const NativeGpuRasterTriangle> jobs,const std::array<std::uint32_t,32>& parameters){return impl_->raster(colors,depths,texture,jobs,parameters);}
std::uint64_t NativeVulkanClear::raster_submissions()const{return impl_->completed_raster;}
bool NativeVulkanClear::raster_supported()const{return impl_->precise_raster;}
std::uint64_t NativeVulkanClear::triangle_submissions()const{return impl_->completed_triangles;}
const std::string& NativeVulkanClear::adapter()const{return impl_->name;}
std::uint64_t NativeVulkanClear::submissions()const{return impl_->completed;}
const NativeVulkanClear::Timings& NativeVulkanClear::timings()const{return impl_->times;}
unsigned NativeVulkanClear::memory_flags()const{return impl_->allocation_flags;}
}
