#include "platform/library.hpp"
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#include "native_gpu_selection.hpp"
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <vector>
#include <string>
#include <array>
#include "native_renderer.hpp"
namespace {
void checked(VkResult value,const char* operation){if(value!=VK_SUCCESS)throw std::runtime_error(std::string(operation)+" VkResult="+std::to_string(value));}
std::string quoted(const char* value){std::string result="\"";for(const unsigned char ch:std::string(value)){if(ch=='"' || ch=='\\')result+='\\';if(ch<32)throw std::runtime_error("Unexpected control byte in Vulkan device name");result+=ch;}return result+'"';}
struct Handles {
    kon::platform::DynamicLibrary loader;VkInstance instance=nullptr;VkDevice device=nullptr;VkBuffer buffer=nullptr;VkDeviceMemory memory=nullptr;VkCommandPool pool=nullptr;VkFence fence=nullptr;bool mapped=false;
    PFN_vkDestroyInstance destroy_instance=nullptr;PFN_vkDestroyDevice destroy_device=nullptr;PFN_vkDeviceWaitIdle idle=nullptr;
    PFN_vkDestroyBuffer destroy_buffer=nullptr;PFN_vkFreeMemory free_memory=nullptr;PFN_vkDestroyCommandPool destroy_pool=nullptr;PFN_vkDestroyFence destroy_fence=nullptr;PFN_vkUnmapMemory unmap=nullptr;
    VkShaderModule shader=nullptr;VkDescriptorSetLayout descriptor_layout=nullptr;VkPipelineLayout pipeline_layout=nullptr;VkPipeline pipeline=nullptr;VkDescriptorPool descriptor_pool=nullptr;
    PFN_vkDestroyShaderModule destroy_shader=nullptr;PFN_vkDestroyDescriptorSetLayout destroy_descriptor_layout=nullptr;PFN_vkDestroyPipelineLayout destroy_pipeline_layout=nullptr;PFN_vkDestroyPipeline destroy_pipeline=nullptr;PFN_vkDestroyDescriptorPool destroy_descriptor_pool=nullptr;
    ~Handles(){if(device){if(idle)idle(device);if(descriptor_pool && destroy_descriptor_pool)destroy_descriptor_pool(device,descriptor_pool,nullptr);if(pipeline && destroy_pipeline)destroy_pipeline(device,pipeline,nullptr);if(pipeline_layout && destroy_pipeline_layout)destroy_pipeline_layout(device,pipeline_layout,nullptr);if(descriptor_layout && destroy_descriptor_layout)destroy_descriptor_layout(device,descriptor_layout,nullptr);if(shader && destroy_shader)destroy_shader(device,shader,nullptr);if(mapped && unmap)unmap(device,memory);if(fence && destroy_fence)destroy_fence(device,fence,nullptr);if(pool && destroy_pool)destroy_pool(device,pool,nullptr);if(buffer && destroy_buffer)destroy_buffer(device,buffer,nullptr);if(memory && free_memory)free_memory(device,memory,nullptr);if(destroy_device)destroy_device(device,nullptr);}if(instance && destroy_instance)destroy_instance(instance,nullptr);}  // the loader member is released last
};
}
int main(int argc,char** argv){try{
    if(argc!=3)throw std::runtime_error("Usage: native_vulkan_shader_test own-clear.spv verification.json");
    Handles h;h.loader=kon::platform::open_vulkan_loader();if(!h.loader)throw std::runtime_error("Installed system Vulkan loader is unavailable");
    const auto get_instance=h.loader.symbol<PFN_vkGetInstanceProcAddr>("vkGetInstanceProcAddr");if(!get_instance)throw std::runtime_error("Missing Vulkan loader entry");
    const auto get=[&](const char* name){auto fn=get_instance(h.instance,name);if(!fn)throw std::runtime_error(std::string("Missing Vulkan entry ")+name);return fn;};
    const auto create_instance=reinterpret_cast<PFN_vkCreateInstance>(get("vkCreateInstance"));
    VkApplicationInfo application{VK_STRUCTURE_TYPE_APPLICATION_INFO};application.pApplicationName="K-On native GPU diagnostic";application.apiVersion=VK_API_VERSION_1_0;
    VkInstanceCreateInfo info{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};info.pApplicationInfo=&application;checked(create_instance(&info,nullptr,&h.instance),"Create Vulkan instance");
    h.destroy_instance=reinterpret_cast<PFN_vkDestroyInstance>(get("vkDestroyInstance"));
    const auto enumerate=reinterpret_cast<PFN_vkEnumeratePhysicalDevices>(get("vkEnumeratePhysicalDevices"));
    const auto properties=reinterpret_cast<PFN_vkGetPhysicalDeviceProperties>(get("vkGetPhysicalDeviceProperties"));
    const auto features=reinterpret_cast<PFN_vkGetPhysicalDeviceFeatures>(get("vkGetPhysicalDeviceFeatures"));
    const auto families=reinterpret_cast<PFN_vkGetPhysicalDeviceQueueFamilyProperties>(get("vkGetPhysicalDeviceQueueFamilyProperties"));
    unsigned count=0;checked(enumerate(h.instance,&count,nullptr),"Enumerate Vulkan adapters");if(!count)throw std::runtime_error("No Vulkan physical device");
    std::vector<VkPhysicalDevice> physical(count);checked(enumerate(h.instance,&count,physical.data()),"Read Vulkan adapters");
    struct Adapter {VkPhysicalDeviceProperties props{};VkPhysicalDeviceFeatures features{};unsigned queue=~0u;};std::vector<Adapter> adapters(count);unsigned selected=~0u;const auto selector=kon::native_gpu_selector();
    for(unsigned n=0;n<count;++n){auto& a=adapters[n];properties(physical[n],&a.props);features(physical[n],&a.features);unsigned queues=0;families(physical[n],&queues,nullptr);std::vector<VkQueueFamilyProperties> values(queues);families(physical[n],&queues,values.data());for(unsigned i=0;i<queues;++i)if(values[i].queueCount && (values[i].queueFlags&VK_QUEUE_COMPUTE_BIT)){a.queue=i;break;}if(kon::native_gpu_matches(n,a.props.vendorID,a.props.deviceType,a.props.deviceName,selector) && a.queue!=~0u && (selected==~0u || a.props.deviceType==VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU))selected=n;}
    if(selected==~0u)throw std::runtime_error("No graphics-capable Vulkan queue");
    const float priority=1;VkDeviceQueueCreateInfo queue_info{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};queue_info.queueFamilyIndex=adapters[selected].queue;queue_info.queueCount=1;queue_info.pQueuePriorities=&priority;
    VkDeviceCreateInfo device_info{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};device_info.queueCreateInfoCount=1;device_info.pQueueCreateInfos=&queue_info;
    const auto create_device=reinterpret_cast<PFN_vkCreateDevice>(get("vkCreateDevice"));checked(create_device(physical[selected],&device_info,nullptr,&h.device),"Create native Vulkan device");
    const auto get_device=reinterpret_cast<PFN_vkGetDeviceProcAddr>(get("vkGetDeviceProcAddr"));
    const auto device_entry=[&](const char* name){auto fn=get_device(h.device,name);if(!fn)throw std::runtime_error(std::string("Missing device entry ")+name);return fn;};
#define VK_ENTRY(name) const auto name=reinterpret_cast<PFN_##name>(device_entry(#name))
    h.destroy_device=reinterpret_cast<PFN_vkDestroyDevice>(device_entry("vkDestroyDevice"));h.idle=reinterpret_cast<PFN_vkDeviceWaitIdle>(device_entry("vkDeviceWaitIdle"));
    h.destroy_buffer=reinterpret_cast<PFN_vkDestroyBuffer>(device_entry("vkDestroyBuffer"));h.free_memory=reinterpret_cast<PFN_vkFreeMemory>(device_entry("vkFreeMemory"));h.destroy_pool=reinterpret_cast<PFN_vkDestroyCommandPool>(device_entry("vkDestroyCommandPool"));h.destroy_fence=reinterpret_cast<PFN_vkDestroyFence>(device_entry("vkDestroyFence"));h.unmap=reinterpret_cast<PFN_vkUnmapMemory>(device_entry("vkUnmapMemory"));
    VK_ENTRY(vkGetDeviceQueue);VK_ENTRY(vkCreateBuffer);VK_ENTRY(vkGetBufferMemoryRequirements);VK_ENTRY(vkAllocateMemory);VK_ENTRY(vkBindBufferMemory);VK_ENTRY(vkCreateCommandPool);VK_ENTRY(vkAllocateCommandBuffers);VK_ENTRY(vkBeginCommandBuffer);VK_ENTRY(vkCmdFillBuffer);VK_ENTRY(vkCmdPipelineBarrier);VK_ENTRY(vkEndCommandBuffer);VK_ENTRY(vkCreateFence);VK_ENTRY(vkQueueSubmit);VK_ENTRY(vkWaitForFences);VK_ENTRY(vkMapMemory);
    VK_ENTRY(vkCreateShaderModule);VK_ENTRY(vkCreateDescriptorSetLayout);VK_ENTRY(vkCreatePipelineLayout);VK_ENTRY(vkCreateComputePipelines);VK_ENTRY(vkCreateDescriptorPool);VK_ENTRY(vkAllocateDescriptorSets);VK_ENTRY(vkUpdateDescriptorSets);VK_ENTRY(vkCmdBindPipeline);VK_ENTRY(vkCmdBindDescriptorSets);VK_ENTRY(vkCmdPushConstants);VK_ENTRY(vkCmdDispatch);
    h.destroy_shader=reinterpret_cast<PFN_vkDestroyShaderModule>(device_entry("vkDestroyShaderModule"));h.destroy_descriptor_layout=reinterpret_cast<PFN_vkDestroyDescriptorSetLayout>(device_entry("vkDestroyDescriptorSetLayout"));h.destroy_pipeline_layout=reinterpret_cast<PFN_vkDestroyPipelineLayout>(device_entry("vkDestroyPipelineLayout"));h.destroy_pipeline=reinterpret_cast<PFN_vkDestroyPipeline>(device_entry("vkDestroyPipeline"));h.destroy_descriptor_pool=reinterpret_cast<PFN_vkDestroyDescriptorPool>(device_entry("vkDestroyDescriptorPool"));
    VkQueue queue=nullptr;vkGetDeviceQueue(h.device,adapters[selected].queue,0,&queue);
    constexpr unsigned words=128u*64u,active_words=words-17u,pattern=0xAB112233u;constexpr VkDeviceSize bytes=words*4u;
    VkBufferCreateInfo buffer_info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};buffer_info.size=bytes;buffer_info.usage=VK_BUFFER_USAGE_TRANSFER_DST_BIT|VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;buffer_info.sharingMode=VK_SHARING_MODE_EXCLUSIVE;checked(vkCreateBuffer(h.device,&buffer_info,nullptr,&h.buffer),"Create GPU clear buffer");
    VkMemoryRequirements requirements{};vkGetBufferMemoryRequirements(h.device,h.buffer,&requirements);VkPhysicalDeviceMemoryProperties memory_props{};
    reinterpret_cast<PFN_vkGetPhysicalDeviceMemoryProperties>(get("vkGetPhysicalDeviceMemoryProperties"))(physical[selected],&memory_props);
    unsigned memory_type=~0u;for(unsigned n=0;n<memory_props.memoryTypeCount;++n)if((requirements.memoryTypeBits&(1u<<n)) && (memory_props.memoryTypes[n].propertyFlags&(VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT))==(VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)){memory_type=n;break;}
    if(memory_type==~0u)throw std::runtime_error("No coherent memory for GPU clear diagnostic");
    VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};allocation.allocationSize=requirements.size;allocation.memoryTypeIndex=memory_type;checked(vkAllocateMemory(h.device,&allocation,nullptr,&h.memory),"Allocate GPU clear storage");checked(vkBindBufferMemory(h.device,h.buffer,h.memory,0),"Bind GPU clear storage");
    std::ifstream shader_file(argv[1],std::ios::binary|std::ios::ate);if(!shader_file)throw std::runtime_error("Cannot read own shader module");
    const auto shader_bytes=shader_file.tellg();if(shader_bytes<20 || shader_bytes>1024*1024 || shader_bytes%4)throw std::runtime_error("Invalid own shader size");
    std::vector<std::uint32_t> shader_words(static_cast<std::size_t>(shader_bytes)/4u);shader_file.seekg(0);shader_file.read(reinterpret_cast<char*>(shader_words.data()),shader_bytes);if(!shader_file || shader_words[0]!=0x07230203u)throw std::runtime_error("Invalid own shader bytes");
    VkShaderModuleCreateInfo shader_info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};shader_info.codeSize=static_cast<std::size_t>(shader_bytes);shader_info.pCode=shader_words.data();checked(vkCreateShaderModule(h.device,&shader_info,nullptr,&h.shader),"Create actual GPU shader");
    VkDescriptorSetLayoutBinding binding{};binding.binding=0;binding.descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;binding.descriptorCount=1;binding.stageFlags=VK_SHADER_STAGE_COMPUTE_BIT;
    VkDescriptorSetLayoutCreateInfo layout_info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};layout_info.bindingCount=1;layout_info.pBindings=&binding;checked(vkCreateDescriptorSetLayout(h.device,&layout_info,nullptr,&h.descriptor_layout),"Create GPU buffer binding");
    VkPushConstantRange push_range{VK_SHADER_STAGE_COMPUTE_BIT,0,32};VkPipelineLayoutCreateInfo pipeline_layout_info{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};pipeline_layout_info.setLayoutCount=1;pipeline_layout_info.pSetLayouts=&h.descriptor_layout;pipeline_layout_info.pushConstantRangeCount=1;pipeline_layout_info.pPushConstantRanges=&push_range;checked(vkCreatePipelineLayout(h.device,&pipeline_layout_info,nullptr,&h.pipeline_layout),"Create shader parameter layout");
    VkComputePipelineCreateInfo pipeline_info{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};pipeline_info.stage.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;pipeline_info.stage.stage=VK_SHADER_STAGE_COMPUTE_BIT;pipeline_info.stage.module=h.shader;pipeline_info.stage.pName="main";pipeline_info.layout=h.pipeline_layout;checked(vkCreateComputePipelines(h.device,nullptr,1,&pipeline_info,nullptr,&h.pipeline),"Compile actual GPU compute pipeline");
    VkDescriptorPoolSize pool_size{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1};VkDescriptorPoolCreateInfo descriptor_pool_info{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};descriptor_pool_info.maxSets=1;descriptor_pool_info.poolSizeCount=1;descriptor_pool_info.pPoolSizes=&pool_size;checked(vkCreateDescriptorPool(h.device,&descriptor_pool_info,nullptr,&h.descriptor_pool),"Create native shader descriptor pool");
    VkDescriptorSetAllocateInfo set_info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};set_info.descriptorPool=h.descriptor_pool;set_info.descriptorSetCount=1;set_info.pSetLayouts=&h.descriptor_layout;VkDescriptorSet descriptor_set=nullptr;checked(vkAllocateDescriptorSets(h.device,&set_info,&descriptor_set),"Allocate native shader binding");
    VkDescriptorBufferInfo descriptor_buffer{h.buffer,0,bytes};VkWriteDescriptorSet descriptor_write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};descriptor_write.dstSet=descriptor_set;descriptor_write.dstBinding=0;descriptor_write.descriptorCount=1;descriptor_write.descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;descriptor_write.pBufferInfo=&descriptor_buffer;vkUpdateDescriptorSets(h.device,1,&descriptor_write,0,nullptr);
    struct ClearJob {unsigned x0,y0,x1,y1,color,mask,clear;};
    const std::array<ClearJob,3> jobs{{{7,9,121,56,0xFF804020u,0x00FFFFFFu,0x101u},{31,17,97,45,0x5F000000u,0xFF000000u,0x201u},{55,7,128,64,0xB07193D5u,0xFFFFFFFFu,0x301u}}};
    // Reference is the production CPU renderer on the same clear sprites.
    psprecomp::GuestMemory reference_memory;kon::NativeRenderer reference(reference_memory,1);kon::GeState state;auto& r=state.registers;
    r[0x12]=0x80011Cu;r[0x9D]=128;r[0xD2]=3;r[0xD5]=(63u<<10u)|127u;
    for(unsigned n=0;n<words;++n)reference_memory.store32(0x04000000u+n*4u,pattern);
    for(const auto& job:jobs){constexpr unsigned vertex=0x08810000u;state.vertex_address=vertex;r[0xD3]=job.clear;
        reference_memory.store16(vertex+4u,job.x0);reference_memory.store16(vertex+6u,job.y0);reference_memory.store32(vertex+12u,job.color);reference_memory.store16(vertex+16u,job.x1);reference_memory.store16(vertex+18u,job.y1);
        // Two actual GE scissor ranges represent the valid buffer prefix.
        // The rounded-up GPU invocation count includes 17 protected words.
        for(unsigned pass=0;pass<2;++pass){state.vertex_address=vertex;
            r[0xD4]=pass?(63u<<10u):0;r[0xD5]=pass?((63u<<10u)|110u):((62u<<10u)|127u);
            if(!reference.draw(state,6,2))throw std::runtime_error("Production CPU clear reference rejected");
        }
    }
    VkCommandPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};pool_info.queueFamilyIndex=adapters[selected].queue;checked(vkCreateCommandPool(h.device,&pool_info,nullptr,&h.pool),"Create native command pool");
    VkCommandBufferAllocateInfo command_info{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};command_info.commandPool=h.pool;command_info.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY;command_info.commandBufferCount=1;VkCommandBuffer command=nullptr;checked(vkAllocateCommandBuffers(h.device,&command_info,&command),"Allocate GPU command buffer");
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};begin.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;checked(vkBeginCommandBuffer(command,&begin),"Begin GPU command buffer");
    // Actual GPU transfer work; no CPU initializes or writes this destination.
    vkCmdFillBuffer(command,h.buffer,0,bytes,pattern);
    VkMemoryBarrier transfer_barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};transfer_barrier.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;transfer_barrier.dstAccessMask=VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_SHADER_WRITE_BIT;
    vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,0,1,&transfer_barrier,0,nullptr,0,nullptr);
    vkCmdBindPipeline(command,VK_PIPELINE_BIND_POINT_COMPUTE,h.pipeline);vkCmdBindDescriptorSets(command,VK_PIPELINE_BIND_POINT_COMPUTE,h.pipeline_layout,0,1,&descriptor_set,0,nullptr);
    for(const auto& job:jobs){const std::array<unsigned,8> parameters{active_words,128,job.x0,job.y0,job.x1,job.y1,job.color,job.mask};
        vkCmdPushConstants(command,h.pipeline_layout,VK_SHADER_STAGE_COMPUTE_BIT,0,sizeof(parameters),parameters.data());vkCmdDispatch(command,(job.x1-job.x0+63u)/64u,job.y1-job.y0,1);
        VkMemoryBarrier dependency{VK_STRUCTURE_TYPE_MEMORY_BARRIER};dependency.srcAccessMask=VK_ACCESS_SHADER_WRITE_BIT;dependency.dstAccessMask=VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_SHADER_WRITE_BIT;
        vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,0,1,&dependency,0,nullptr,0,nullptr);
    }
    VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};barrier.srcAccessMask=VK_ACCESS_SHADER_WRITE_BIT;barrier.dstAccessMask=VK_ACCESS_HOST_READ_BIT;
    vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,1,&barrier,0,nullptr,0,nullptr);checked(vkEndCommandBuffer(command),"Finish GPU command buffer");
    VkFenceCreateInfo fence_info{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};checked(vkCreateFence(h.device,&fence_info,nullptr,&h.fence),"Create GPU completion fence");VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};submit.commandBufferCount=1;submit.pCommandBuffers=&command;checked(vkQueueSubmit(queue,1,&submit,h.fence),"Submit GPU clear");checked(vkWaitForFences(h.device,1,&h.fence,VK_TRUE,5000000000ull),"Wait actual GPU completion");
    void* mapped=nullptr;checked(vkMapMemory(h.device,h.memory,0,bytes,0,&mapped),"Read completed GPU buffer");h.mapped=true;const auto* output=static_cast<const unsigned*>(mapped);for(unsigned n=0;n<words;++n)if(output[n]!=reference_memory.load32(0x04000000u+n*4u))throw std::runtime_error("GPU shader differed from production CPU clear at word "+std::to_string(n));
    std::ofstream report(argv[2]);report<<"{\"native_gpu_rendering_integrated\":false,\"shader_execution_verified\":true,\"words_compared_to_cpu_renderer\":"<<words<<",\"masked_overlapping_clears\":"<<jobs.size()<<",\"initialization_by_gpu_transfer\":true,\"rgb_alpha_preservation_verified\":true,\"rounded_dispatch_guard_words_verified\":17,\"game_fps_verified\":false,\"selected_adapter\":"<<quoted(adapters[selected].props.deviceName)<<"}\n";if(!report)throw std::runtime_error("Cannot save actual shader evidence");
    std::cout<<"PASS: own Vulkan shader matches production CPU renderer for "<<words<<" words, RGB/alpha masks, overlap order and preserved pixels on "<<adapters[selected].props.deviceName<<"; game GPU rendering remains pending\n";return 0;
}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}}
