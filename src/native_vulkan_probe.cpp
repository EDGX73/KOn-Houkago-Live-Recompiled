#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#include "native_gpu_selection.hpp"
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <vector>
#include <string>
namespace {
void checked(VkResult value,const char* operation){if(value!=VK_SUCCESS)throw std::runtime_error(std::string(operation)+" VkResult="+std::to_string(value));}
std::string quoted(const char* value){std::string result="\"";for(const unsigned char ch:std::string(value)){if(ch=='"' || ch=='\\')result+='\\';if(ch<32)throw std::runtime_error("Unexpected control byte in Vulkan device name");result+=ch;}return result+'"';}
struct Handles {
    HMODULE loader=nullptr;VkInstance instance=nullptr;VkDevice device=nullptr;VkBuffer buffer=nullptr;VkDeviceMemory memory=nullptr;VkCommandPool pool=nullptr;VkFence fence=nullptr;bool mapped=false;
    PFN_vkDestroyInstance destroy_instance=nullptr;PFN_vkDestroyDevice destroy_device=nullptr;PFN_vkDeviceWaitIdle idle=nullptr;
    PFN_vkDestroyBuffer destroy_buffer=nullptr;PFN_vkFreeMemory free_memory=nullptr;PFN_vkDestroyCommandPool destroy_pool=nullptr;PFN_vkDestroyFence destroy_fence=nullptr;PFN_vkUnmapMemory unmap=nullptr;
    ~Handles(){if(device){if(idle)idle(device);if(mapped && unmap)unmap(device,memory);if(fence && destroy_fence)destroy_fence(device,fence,nullptr);if(pool && destroy_pool)destroy_pool(device,pool,nullptr);if(buffer && destroy_buffer)destroy_buffer(device,buffer,nullptr);if(memory && free_memory)free_memory(device,memory,nullptr);if(destroy_device)destroy_device(device,nullptr);}if(instance && destroy_instance)destroy_instance(instance,nullptr);if(loader)FreeLibrary(loader);}
};
}
int main(int argc,char** argv){try{
    if(argc!=2)throw std::runtime_error("Usage: native_vulkan_probe capability-report.json");
    Handles h;h.loader=LoadLibraryExW(L"vulkan-1.dll",nullptr,LOAD_LIBRARY_SEARCH_SYSTEM32);if(!h.loader)throw std::runtime_error("Installed system Vulkan loader is unavailable");
    const auto get_instance=reinterpret_cast<PFN_vkGetInstanceProcAddr>(GetProcAddress(h.loader,"vkGetInstanceProcAddr"));if(!get_instance)throw std::runtime_error("Missing Vulkan loader entry");
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
    for(unsigned n=0;n<count;++n){auto& a=adapters[n];properties(physical[n],&a.props);features(physical[n],&a.features);unsigned queues=0;families(physical[n],&queues,nullptr);std::vector<VkQueueFamilyProperties> values(queues);families(physical[n],&queues,values.data());for(unsigned i=0;i<queues;++i)if(values[i].queueCount && (values[i].queueFlags&VK_QUEUE_GRAPHICS_BIT)){a.queue=i;break;}if(kon::native_gpu_matches(n,a.props.vendorID,a.props.deviceType,a.props.deviceName,selector) && a.queue!=~0u && (selected==~0u || a.props.deviceType==VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU))selected=n;}
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
    VkQueue queue=nullptr;vkGetDeviceQueue(h.device,adapters[selected].queue,0,&queue);
    constexpr unsigned words=128u*64u,pattern=0x00804020u;constexpr VkDeviceSize bytes=words*4u;
    VkBufferCreateInfo buffer_info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};buffer_info.size=bytes;buffer_info.usage=VK_BUFFER_USAGE_TRANSFER_DST_BIT;buffer_info.sharingMode=VK_SHARING_MODE_EXCLUSIVE;checked(vkCreateBuffer(h.device,&buffer_info,nullptr,&h.buffer),"Create GPU clear buffer");
    VkMemoryRequirements requirements{};vkGetBufferMemoryRequirements(h.device,h.buffer,&requirements);VkPhysicalDeviceMemoryProperties memory_props{};
    reinterpret_cast<PFN_vkGetPhysicalDeviceMemoryProperties>(get("vkGetPhysicalDeviceMemoryProperties"))(physical[selected],&memory_props);
    unsigned memory_type=~0u;for(unsigned n=0;n<memory_props.memoryTypeCount;++n)if((requirements.memoryTypeBits&(1u<<n)) && (memory_props.memoryTypes[n].propertyFlags&(VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT))==(VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)){memory_type=n;break;}
    if(memory_type==~0u)throw std::runtime_error("No coherent memory for GPU clear diagnostic");
    VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};allocation.allocationSize=requirements.size;allocation.memoryTypeIndex=memory_type;checked(vkAllocateMemory(h.device,&allocation,nullptr,&h.memory),"Allocate GPU clear storage");checked(vkBindBufferMemory(h.device,h.buffer,h.memory,0),"Bind GPU clear storage");
    VkCommandPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};pool_info.queueFamilyIndex=adapters[selected].queue;checked(vkCreateCommandPool(h.device,&pool_info,nullptr,&h.pool),"Create native command pool");
    VkCommandBufferAllocateInfo command_info{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};command_info.commandPool=h.pool;command_info.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY;command_info.commandBufferCount=1;VkCommandBuffer command=nullptr;checked(vkAllocateCommandBuffers(h.device,&command_info,&command),"Allocate GPU command buffer");
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};begin.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;checked(vkBeginCommandBuffer(command,&begin),"Begin GPU command buffer");
    // Actual GPU transfer work; no CPU initializes or writes this destination.
    vkCmdFillBuffer(command,h.buffer,0,bytes,pattern);
    VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};barrier.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;barrier.dstAccessMask=VK_ACCESS_HOST_READ_BIT;
    vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,1,&barrier,0,nullptr,0,nullptr);checked(vkEndCommandBuffer(command),"Finish GPU command buffer");
    VkFenceCreateInfo fence_info{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};checked(vkCreateFence(h.device,&fence_info,nullptr,&h.fence),"Create GPU completion fence");VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};submit.commandBufferCount=1;submit.pCommandBuffers=&command;checked(vkQueueSubmit(queue,1,&submit,h.fence),"Submit GPU clear");checked(vkWaitForFences(h.device,1,&h.fence,VK_TRUE,5000000000ull),"Wait actual GPU completion");
    void* mapped=nullptr;checked(vkMapMemory(h.device,h.memory,0,bytes,0,&mapped),"Read completed GPU buffer");h.mapped=true;const auto* output=static_cast<const unsigned*>(mapped);for(unsigned n=0;n<words;++n)if(output[n]!=pattern)throw std::runtime_error("GPU clear differed from exact expected word");
    std::ofstream report(argv[1]);report<<"{\"native_gpu_rendering_integrated\":false,\"vulkan_device_created\":true,\"gpu_clear_words_verified\":"<<words<<",\"gpu_clear_pattern\":"<<pattern<<",\"game_fps_verified\":false,\"shader_execution_verified\":false,\"selected_adapter\":"<<selected<<",\"adapters\":[";
    for(unsigned n=0;n<count;++n){const auto& a=adapters[n];if(n)report<<',';report<<"{\"name\":"<<quoted(a.props.deviceName)<<",\"device_type\":"<<a.props.deviceType<<",\"api_version\":"<<a.props.apiVersion<<",\"shader_float64\":"<<(a.features.shaderFloat64?"true":"false")<<",\"shader_int64\":"<<(a.features.shaderInt64?"true":"false")<<",\"graphics_queue_available\":"<<(a.queue!=~0u?"true":"false")<<'}';}report<<"]}\n";if(!report)throw std::runtime_error("Cannot save Vulkan capability evidence");
    std::cout<<"PASS: Vulkan device and actual GPU clear verified on "<<adapters[selected].props.deviceName<<"; game GPU renderer remains unimplemented\n";return 0;
}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}}
