#define VK_USE_PLATFORM_WIN32_KHR
#include "native_vulkan_present.hpp"
#include <vulkan/vulkan.h>
#include <algorithm>
#include <cstring>
#include <stdexcept>
#include <vector>
namespace kon {
namespace {
void check(VkResult result,const char* what){if(result!=VK_SUCCESS)throw std::runtime_error(std::string(what)+" failed (VkResult "+std::to_string(static_cast<int>(result))+")");}
}
struct NativeVulkanPresenter::Impl {
    HMODULE loader=nullptr;HWND window=nullptr;std::string name,present_mode="fifo";
    VkInstance instance=nullptr;VkSurfaceKHR surface=VK_NULL_HANDLE;VkPhysicalDevice physical=nullptr;VkDevice device=nullptr;VkQueue queue=nullptr;unsigned family=0;
    VkSwapchainKHR swapchain=VK_NULL_HANDLE;VkFormat format=VK_FORMAT_B8G8R8A8_UNORM;VkExtent2D extent{};std::vector<VkImage> images;std::vector<VkSemaphore> rendered;
    VkCommandPool pool=VK_NULL_HANDLE;VkCommandBuffer command=nullptr;VkFence fence=VK_NULL_HANDLE;VkSemaphore acquired=VK_NULL_HANDLE;bool submitted=false,stale=false;
    VkBuffer staging=VK_NULL_HANDLE;VkDeviceMemory staging_memory=VK_NULL_HANDLE;void* mapped=nullptr;VkDeviceSize staging_size=0;
    VkImage source=VK_NULL_HANDLE;VkDeviceMemory source_memory=VK_NULL_HANDLE;unsigned source_width=0,source_height=0;
    VkPhysicalDeviceMemoryProperties memory_properties{};
#define FN(n) PFN_##n n=nullptr
    FN(vkGetInstanceProcAddr);FN(vkCreateInstance);FN(vkDestroyInstance);FN(vkCreateWin32SurfaceKHR);FN(vkDestroySurfaceKHR);
    FN(vkEnumeratePhysicalDevices);FN(vkGetPhysicalDeviceProperties);FN(vkGetPhysicalDeviceQueueFamilyProperties);FN(vkGetPhysicalDeviceMemoryProperties);
    FN(vkGetPhysicalDeviceSurfaceSupportKHR);FN(vkGetPhysicalDeviceSurfaceCapabilitiesKHR);FN(vkGetPhysicalDeviceSurfaceFormatsKHR);FN(vkGetPhysicalDeviceSurfacePresentModesKHR);
    FN(vkCreateDevice);FN(vkDestroyDevice);FN(vkGetDeviceQueue);FN(vkDeviceWaitIdle);
    FN(vkCreateSwapchainKHR);FN(vkDestroySwapchainKHR);FN(vkGetSwapchainImagesKHR);FN(vkAcquireNextImageKHR);FN(vkQueuePresentKHR);
    FN(vkCreateCommandPool);FN(vkDestroyCommandPool);FN(vkAllocateCommandBuffers);FN(vkResetCommandPool);FN(vkBeginCommandBuffer);FN(vkEndCommandBuffer);
    FN(vkCmdPipelineBarrier);FN(vkCmdCopyBufferToImage);FN(vkCmdBlitImage);FN(vkCmdClearColorImage);FN(vkQueueSubmit);
    FN(vkCreateFence);FN(vkDestroyFence);FN(vkWaitForFences);FN(vkResetFences);FN(vkCreateSemaphore);FN(vkDestroySemaphore);
    FN(vkCreateBuffer);FN(vkDestroyBuffer);FN(vkGetBufferMemoryRequirements);FN(vkAllocateMemory);FN(vkFreeMemory);FN(vkBindBufferMemory);FN(vkMapMemory);FN(vkUnmapMemory);
    FN(vkCreateImage);FN(vkDestroyImage);FN(vkGetImageMemoryRequirements);FN(vkBindImageMemory);
#undef FN
    template<class F> void load(F& function,const char* symbol){function=reinterpret_cast<F>(vkGetInstanceProcAddr(instance,symbol));if(!function)throw std::runtime_error(std::string("Missing Vulkan presentation entry ")+symbol);}
    Impl(HWND target,const std::string& preferred):window(target){
        loader=LoadLibraryExW(L"vulkan-1.dll",nullptr,LOAD_LIBRARY_SEARCH_SYSTEM32);
        if(!loader)throw std::runtime_error("System Vulkan loader is unavailable for presentation");
        vkGetInstanceProcAddr=reinterpret_cast<PFN_vkGetInstanceProcAddr>(GetProcAddress(loader,"vkGetInstanceProcAddr"));
        if(!vkGetInstanceProcAddr)throw std::runtime_error("Missing Vulkan loader entry point");
        load(vkCreateInstance,"vkCreateInstance");
        const char* extensions[]{VK_KHR_SURFACE_EXTENSION_NAME,VK_KHR_WIN32_SURFACE_EXTENSION_NAME};
        VkApplicationInfo application{VK_STRUCTURE_TYPE_APPLICATION_INFO};application.pApplicationName="K-On native presentation";application.apiVersion=VK_API_VERSION_1_0;
        VkInstanceCreateInfo instance_info{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};instance_info.pApplicationInfo=&application;instance_info.enabledExtensionCount=2;instance_info.ppEnabledExtensionNames=extensions;
        check(vkCreateInstance(&instance_info,nullptr,&instance),"Create presentation instance");
        load(vkDestroyInstance,"vkDestroyInstance");load(vkCreateWin32SurfaceKHR,"vkCreateWin32SurfaceKHR");load(vkDestroySurfaceKHR,"vkDestroySurfaceKHR");
        load(vkEnumeratePhysicalDevices,"vkEnumeratePhysicalDevices");load(vkGetPhysicalDeviceProperties,"vkGetPhysicalDeviceProperties");
        load(vkGetPhysicalDeviceQueueFamilyProperties,"vkGetPhysicalDeviceQueueFamilyProperties");load(vkGetPhysicalDeviceMemoryProperties,"vkGetPhysicalDeviceMemoryProperties");
        load(vkGetPhysicalDeviceSurfaceSupportKHR,"vkGetPhysicalDeviceSurfaceSupportKHR");load(vkGetPhysicalDeviceSurfaceCapabilitiesKHR,"vkGetPhysicalDeviceSurfaceCapabilitiesKHR");
        load(vkGetPhysicalDeviceSurfaceFormatsKHR,"vkGetPhysicalDeviceSurfaceFormatsKHR");load(vkGetPhysicalDeviceSurfacePresentModesKHR,"vkGetPhysicalDeviceSurfacePresentModesKHR");load(vkCreateDevice,"vkCreateDevice");
        VkWin32SurfaceCreateInfoKHR surface_info{VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR};surface_info.hinstance=GetModuleHandleW(nullptr);surface_info.hwnd=window;
        check(vkCreateWin32SurfaceKHR(instance,&surface_info,nullptr,&surface),"Create window surface");
        // Same adapter as the raster backend when it can present; otherwise
        // any adapter that can present to this window (automatic, no prompt).
        unsigned count=0;check(vkEnumeratePhysicalDevices(instance,&count,nullptr),"Enumerate presentation adapters");
        std::vector<VkPhysicalDevice> devices(count);check(vkEnumeratePhysicalDevices(instance,&count,devices.data()),"Read presentation adapters");
        int best=-1;
        for(unsigned pass=0;pass<2 && !physical;++pass)for(auto candidate:devices){
            VkPhysicalDeviceProperties properties{};vkGetPhysicalDeviceProperties(candidate,&properties);
            if(pass==0 && (preferred.empty() || preferred.find(properties.deviceName)==std::string::npos))continue;
            unsigned families=0;vkGetPhysicalDeviceQueueFamilyProperties(candidate,&families,nullptr);std::vector<VkQueueFamilyProperties> list(families);vkGetPhysicalDeviceQueueFamilyProperties(candidate,&families,list.data());
            for(unsigned n=0;n<families;++n){VkBool32 supported=VK_FALSE;vkGetPhysicalDeviceSurfaceSupportKHR(candidate,n,surface,&supported);
                if(supported && (list[n].queueFlags&(VK_QUEUE_GRAPHICS_BIT|VK_QUEUE_COMPUTE_BIT|VK_QUEUE_TRANSFER_BIT))){physical=candidate;family=n;name=properties.deviceName;best=int(n);break;}}
            if(physical)break;
        }
        if(!physical || best<0)throw std::runtime_error("No Vulkan adapter can present to the game window");
        vkGetPhysicalDeviceMemoryProperties(physical,&memory_properties);
        const float priority=1.0f;VkDeviceQueueCreateInfo queue_info{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};queue_info.queueFamilyIndex=family;queue_info.queueCount=1;queue_info.pQueuePriorities=&priority;
        const char* device_extensions[]{VK_KHR_SWAPCHAIN_EXTENSION_NAME};
        VkDeviceCreateInfo device_info{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};device_info.queueCreateInfoCount=1;device_info.pQueueCreateInfos=&queue_info;device_info.enabledExtensionCount=1;device_info.ppEnabledExtensionNames=device_extensions;
        check(vkCreateDevice(physical,&device_info,nullptr,&device),"Create presentation device");
        load(vkDestroyDevice,"vkDestroyDevice");load(vkGetDeviceQueue,"vkGetDeviceQueue");load(vkDeviceWaitIdle,"vkDeviceWaitIdle");
        load(vkCreateSwapchainKHR,"vkCreateSwapchainKHR");load(vkDestroySwapchainKHR,"vkDestroySwapchainKHR");load(vkGetSwapchainImagesKHR,"vkGetSwapchainImagesKHR");
        load(vkAcquireNextImageKHR,"vkAcquireNextImageKHR");load(vkQueuePresentKHR,"vkQueuePresentKHR");
        load(vkCreateCommandPool,"vkCreateCommandPool");load(vkDestroyCommandPool,"vkDestroyCommandPool");load(vkAllocateCommandBuffers,"vkAllocateCommandBuffers");
        load(vkResetCommandPool,"vkResetCommandPool");load(vkBeginCommandBuffer,"vkBeginCommandBuffer");load(vkEndCommandBuffer,"vkEndCommandBuffer");
        load(vkCmdPipelineBarrier,"vkCmdPipelineBarrier");load(vkCmdCopyBufferToImage,"vkCmdCopyBufferToImage");load(vkCmdBlitImage,"vkCmdBlitImage");
        load(vkCmdClearColorImage,"vkCmdClearColorImage");load(vkQueueSubmit,"vkQueueSubmit");
        load(vkCreateFence,"vkCreateFence");load(vkDestroyFence,"vkDestroyFence");load(vkWaitForFences,"vkWaitForFences");load(vkResetFences,"vkResetFences");
        load(vkCreateSemaphore,"vkCreateSemaphore");load(vkDestroySemaphore,"vkDestroySemaphore");
        load(vkCreateBuffer,"vkCreateBuffer");load(vkDestroyBuffer,"vkDestroyBuffer");load(vkGetBufferMemoryRequirements,"vkGetBufferMemoryRequirements");
        load(vkAllocateMemory,"vkAllocateMemory");load(vkFreeMemory,"vkFreeMemory");load(vkBindBufferMemory,"vkBindBufferMemory");load(vkMapMemory,"vkMapMemory");load(vkUnmapMemory,"vkUnmapMemory");
        load(vkCreateImage,"vkCreateImage");load(vkDestroyImage,"vkDestroyImage");load(vkGetImageMemoryRequirements,"vkGetImageMemoryRequirements");load(vkBindImageMemory,"vkBindImageMemory");
        vkGetDeviceQueue(device,family,0,&queue);
        VkCommandPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};pool_info.queueFamilyIndex=family;check(vkCreateCommandPool(device,&pool_info,nullptr,&pool),"Create presentation command pool");
        VkCommandBufferAllocateInfo command_info{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};command_info.commandPool=pool;command_info.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY;command_info.commandBufferCount=1;
        check(vkAllocateCommandBuffers(device,&command_info,&command),"Allocate presentation commands");
        VkFenceCreateInfo fence_info{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};check(vkCreateFence(device,&fence_info,nullptr,&fence),"Create presentation fence");
        VkSemaphoreCreateInfo semaphore_info{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};check(vkCreateSemaphore(device,&semaphore_info,nullptr,&acquired),"Create acquire semaphore");
        create_swapchain();
    }
    ~Impl(){
        if(device){
            if(vkDeviceWaitIdle)vkDeviceWaitIdle(device);
            destroy_source();destroy_swapchain(swapchain);
            if(acquired)vkDestroySemaphore(device,acquired,nullptr);if(fence)vkDestroyFence(device,fence,nullptr);if(pool)vkDestroyCommandPool(device,pool,nullptr);
            vkDestroyDevice(device,nullptr);
        }
        if(surface && vkDestroySurfaceKHR)vkDestroySurfaceKHR(instance,surface,nullptr);
        if(instance && vkDestroyInstance)vkDestroyInstance(instance,nullptr);
        if(loader)FreeLibrary(loader);
    }
    unsigned memory_type(unsigned bits,VkMemoryPropertyFlags wanted){
        for(unsigned n=0;n<memory_properties.memoryTypeCount;++n)if((bits&(1u<<n)) && (memory_properties.memoryTypes[n].propertyFlags&wanted)==wanted)return n;
        throw std::runtime_error("No suitable presentation memory type");
    }
    void destroy_swapchain(VkSwapchainKHR old){
        for(auto semaphore:rendered)vkDestroySemaphore(device,semaphore,nullptr);rendered.clear();images.clear();
        if(old)vkDestroySwapchainKHR(device,old,nullptr);if(old==swapchain)swapchain=VK_NULL_HANDLE;
    }
    bool create_swapchain(){
        VkSurfaceCapabilitiesKHR capabilities{};check(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical,surface,&capabilities),"Read surface capabilities");
        if(!capabilities.currentExtent.width || !capabilities.currentExtent.height)return false; // minimized
        if(!(capabilities.supportedUsageFlags&VK_IMAGE_USAGE_TRANSFER_DST_BIT))throw std::runtime_error("Window surface cannot receive transfers");
        unsigned formats=0;check(vkGetPhysicalDeviceSurfaceFormatsKHR(physical,surface,&formats,nullptr),"Count surface formats");
        std::vector<VkSurfaceFormatKHR> list(formats);check(vkGetPhysicalDeviceSurfaceFormatsKHR(physical,surface,&formats,list.data()),"Read surface formats");
        if(list.empty())throw std::runtime_error("Window surface has no formats");
        auto chosen=list.front();for(const auto& f:list)if(f.format==VK_FORMAT_B8G8R8A8_UNORM){chosen=f;break;}
        format=chosen.format;extent=capabilities.currentExtent;
        VkSwapchainCreateInfoKHR info{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};info.surface=surface;
        info.minImageCount=std::max(2u,capabilities.minImageCount);if(capabilities.maxImageCount)info.minImageCount=std::min(info.minImageCount,capabilities.maxImageCount);
        info.imageFormat=chosen.format;info.imageColorSpace=chosen.colorSpace;info.imageExtent=extent;info.imageArrayLayers=1;info.imageUsage=VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        info.imageSharingMode=VK_SHARING_MODE_EXCLUSIVE;info.preTransform=capabilities.currentTransform;
        info.compositeAlpha=(capabilities.supportedCompositeAlpha&VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR)?VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR:static_cast<VkCompositeAlphaFlagBitsKHR>(capabilities.supportedCompositeAlpha&-capabilities.supportedCompositeAlpha);
        // Lowest latency first. The game paces its own frames, so presentation
        // must not queue them: MAILBOX shows the newest frame without tearing;
        // IMMEDIATE in a window is composited by DWM and behaves the same (the
        // 780M offers no MAILBOX); FIFO, always available, is the fallback.
        unsigned modes=0;std::vector<VkPresentModeKHR> available;
        if(vkGetPhysicalDeviceSurfacePresentModesKHR(physical,surface,&modes,nullptr)==VK_SUCCESS){available.resize(modes);vkGetPhysicalDeviceSurfacePresentModesKHR(physical,surface,&modes,available.data());available.resize(modes);}
        const auto has=[&](VkPresentModeKHR mode){return std::find(available.begin(),available.end(),mode)!=available.end();};
        info.presentMode=VK_PRESENT_MODE_FIFO_KHR;present_mode="fifo";
        if(has(VK_PRESENT_MODE_MAILBOX_KHR)){info.presentMode=VK_PRESENT_MODE_MAILBOX_KHR;present_mode="mailbox";}
        else if(has(VK_PRESENT_MODE_IMMEDIATE_KHR)){info.presentMode=VK_PRESENT_MODE_IMMEDIATE_KHR;present_mode="immediate";}
        if(info.presentMode!=VK_PRESENT_MODE_FIFO_KHR){info.minImageCount=std::max(3u,capabilities.minImageCount);if(capabilities.maxImageCount)info.minImageCount=std::min(info.minImageCount,capabilities.maxImageCount);}
        info.clipped=VK_TRUE;const auto old=swapchain;info.oldSwapchain=old;
        VkSwapchainKHR created=VK_NULL_HANDLE;check(vkCreateSwapchainKHR(device,&info,nullptr,&created),"Create swapchain");
        if(old){vkDeviceWaitIdle(device);destroy_swapchain(old);}
        swapchain=created;unsigned count=0;check(vkGetSwapchainImagesKHR(device,swapchain,&count,nullptr),"Count swapchain images");
        images.resize(count);check(vkGetSwapchainImagesKHR(device,swapchain,&count,images.data()),"Read swapchain images");
        VkSemaphoreCreateInfo semaphore_info{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};rendered.resize(count);
        for(auto& semaphore:rendered)check(vkCreateSemaphore(device,&semaphore_info,nullptr,&semaphore),"Create present semaphore");
        return true;
    }
    void destroy_source(){
        if(mapped){vkUnmapMemory(device,staging_memory);mapped=nullptr;}
        if(staging)vkDestroyBuffer(device,staging,nullptr);if(staging_memory)vkFreeMemory(device,staging_memory,nullptr);
        if(source)vkDestroyImage(device,source,nullptr);if(source_memory)vkFreeMemory(device,source_memory,nullptr);
        staging=VK_NULL_HANDLE;staging_memory=VK_NULL_HANDLE;source=VK_NULL_HANDLE;source_memory=VK_NULL_HANDLE;source_width=source_height=0;staging_size=0;
    }
    void prepare_source(unsigned width,unsigned height){
        if(width==source_width && height==source_height)return;
        if(submitted){check(vkWaitForFences(device,1,&fence,VK_TRUE,5000000000ull),"Wait presentation");}
        destroy_source();
        staging_size=VkDeviceSize(width)*height*4;
        VkBufferCreateInfo buffer_info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};buffer_info.size=staging_size;buffer_info.usage=VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        check(vkCreateBuffer(device,&buffer_info,nullptr,&staging),"Create presentation staging");
        VkMemoryRequirements requirements{};vkGetBufferMemoryRequirements(device,staging,&requirements);
        VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};allocation.allocationSize=requirements.size;
        allocation.memoryTypeIndex=memory_type(requirements.memoryTypeBits,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        check(vkAllocateMemory(device,&allocation,nullptr,&staging_memory),"Allocate presentation staging");check(vkBindBufferMemory(device,staging,staging_memory,0),"Bind presentation staging");
        check(vkMapMemory(device,staging_memory,0,staging_size,0,&mapped),"Map presentation staging");
        VkImageCreateInfo image_info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};image_info.imageType=VK_IMAGE_TYPE_2D;image_info.format=VK_FORMAT_B8G8R8A8_UNORM;
        image_info.extent={width,height,1};image_info.mipLevels=1;image_info.arrayLayers=1;image_info.samples=VK_SAMPLE_COUNT_1_BIT;image_info.tiling=VK_IMAGE_TILING_OPTIMAL;
        image_info.usage=VK_IMAGE_USAGE_TRANSFER_DST_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT;image_info.initialLayout=VK_IMAGE_LAYOUT_UNDEFINED;
        check(vkCreateImage(device,&image_info,nullptr,&source),"Create presentation image");
        vkGetImageMemoryRequirements(device,source,&requirements);allocation.allocationSize=requirements.size;
        allocation.memoryTypeIndex=memory_type(requirements.memoryTypeBits,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        check(vkAllocateMemory(device,&allocation,nullptr,&source_memory),"Allocate presentation image");check(vkBindImageMemory(device,source,source_memory,0),"Bind presentation image");
        source_width=width;source_height=height;
    }
    static void barrier(Impl& s,VkImage image,VkImageLayout from,VkImageLayout to,VkAccessFlags src,VkAccessFlags dst,VkPipelineStageFlags src_stage,VkPipelineStageFlags dst_stage){
        VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};b.oldLayout=from;b.newLayout=to;b.srcAccessMask=src;b.dstAccessMask=dst;
        b.srcQueueFamilyIndex=b.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;b.image=image;b.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
        s.vkCmdPipelineBarrier(s.command,src_stage,dst_stage,0,0,nullptr,0,nullptr,1,&b);
    }
    bool present(const std::uint32_t* pixels,unsigned width,unsigned height,bool linear){
        auto* target=begin(width,height);if(!target)return false;
        std::memcpy(target,pixels,staging_size);return end(linear);
    }
    std::uint32_t* begin(unsigned width,unsigned height){
        if(!width || !height)return nullptr;
        if(stale && swapchain){stale=false;if(submitted){check(vkWaitForFences(device,1,&fence,VK_TRUE,5000000000ull),"Wait presentation");submitted=false;}vkDeviceWaitIdle(device);destroy_swapchain(swapchain);}
        if(!swapchain && !create_swapchain())return nullptr;
        prepare_source(width,height);
        if(submitted){check(vkWaitForFences(device,1,&fence,VK_TRUE,5000000000ull),"Wait presentation");submitted=false;}
        return static_cast<std::uint32_t*>(mapped);
    }
    bool end(bool linear){
        if(!swapchain || !mapped)return false;
        const unsigned width=source_width,height=source_height;
        unsigned index=0;auto result=vkAcquireNextImageKHR(device,swapchain,UINT64_MAX,acquired,VK_NULL_HANDLE,&index);
        if(result==VK_ERROR_OUT_OF_DATE_KHR){vkDeviceWaitIdle(device);destroy_swapchain(swapchain);create_swapchain();return false;}
        if(result!=VK_SUCCESS && result!=VK_SUBOPTIMAL_KHR)check(result,"Acquire swapchain image");
        check(vkResetFences(device,1,&fence),"Reset presentation fence");check(vkResetCommandPool(device,pool,0),"Reset presentation commands");
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};begin.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;check(vkBeginCommandBuffer(command,&begin),"Begin presentation");
        const auto target=images[index];
        barrier(*this,source,VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,0,VK_ACCESS_TRANSFER_WRITE_BIT,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT);
        VkBufferImageCopy copy{};copy.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};copy.imageExtent={width,height,1};
        vkCmdCopyBufferToImage(command,staging,source,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&copy);
        barrier(*this,source,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,VK_ACCESS_TRANSFER_WRITE_BIT,VK_ACCESS_TRANSFER_READ_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT);
        barrier(*this,target,VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,0,VK_ACCESS_TRANSFER_WRITE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT);
        VkClearColorValue black{};VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
        vkCmdClearColorImage(command,target,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,&black,1,&range);
        barrier(*this,target,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_ACCESS_TRANSFER_WRITE_BIT,VK_ACCESS_TRANSFER_WRITE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT);
        double scale=std::min(double(extent.width)/width,double(extent.height)/height);
        // Slightly larger than the screen (1088 rows on a 1080 one): shown 1:1
        // with the few outer rows/columns cut, instead of a blurring 0.99x resample.
        int sx0=0,sy0=0,sx1=int(width),sy1=int(height);
        if(scale<1.0 && scale>=0.97){
            scale=1.0;const int cw=std::min(int(width),int(extent.width)),ch=std::min(int(height),int(extent.height));
            sx0=(int(width)-cw)/2;sy0=(int(height)-ch)/2;sx1=sx0+cw;sy1=sy0+ch;
        }
        const int w=std::max(1,int((sx1-sx0)*scale)),h=std::max(1,int((sy1-sy0)*scale)),x=(int(extent.width)-w)/2,y=(int(extent.height)-h)/2;
        VkImageBlit blit{};blit.srcSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};blit.srcOffsets[0]={sx0,sy0,0};blit.srcOffsets[1]={sx1,sy1,1};
        blit.dstSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};blit.dstOffsets[0]={x,y,0};blit.dstOffsets[1]={x+w,y+h,1};
        vkCmdBlitImage(command,source,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,target,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&blit,linear?VK_FILTER_LINEAR:VK_FILTER_NEAREST);
        barrier(*this,target,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,VK_ACCESS_TRANSFER_WRITE_BIT,0,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
        check(vkEndCommandBuffer(command),"Finish presentation");
        const VkPipelineStageFlags wait_stage=VK_PIPELINE_STAGE_TRANSFER_BIT;
        VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};submit.waitSemaphoreCount=1;submit.pWaitSemaphores=&acquired;submit.pWaitDstStageMask=&wait_stage;
        submit.commandBufferCount=1;submit.pCommandBuffers=&command;submit.signalSemaphoreCount=1;submit.pSignalSemaphores=&rendered[index];
        check(vkQueueSubmit(queue,1,&submit,fence),"Submit presentation");submitted=true;
        VkPresentInfoKHR present_info{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};present_info.waitSemaphoreCount=1;present_info.pWaitSemaphores=&rendered[index];
        present_info.swapchainCount=1;present_info.pSwapchains=&swapchain;present_info.pImageIndices=&index;
        result=vkQueuePresentKHR(queue,&present_info);
        if(result==VK_ERROR_OUT_OF_DATE_KHR || result==VK_SUBOPTIMAL_KHR){vkDeviceWaitIdle(device);submitted=false;destroy_swapchain(swapchain);create_swapchain();}
        else check(result,"Present swapchain image");
        return true;
    }
};
NativeVulkanPresenter::NativeVulkanPresenter(HWND window,const std::string& preferred):impl_(std::make_unique<Impl>(window,preferred)){}
NativeVulkanPresenter::~NativeVulkanPresenter()=default;
bool NativeVulkanPresenter::present(const std::uint32_t* bgra,unsigned width,unsigned height,bool linear){return impl_->present(bgra,width,height,linear);}
void NativeVulkanPresenter::resized(){impl_->stale=true;}
std::uint32_t* NativeVulkanPresenter::begin_frame(unsigned width,unsigned height){return impl_->begin(width,height);}
bool NativeVulkanPresenter::end_frame(bool linear){return impl_->end(linear);}
const std::string& NativeVulkanPresenter::adapter()const{return impl_->name;}
const std::string& NativeVulkanPresenter::present_mode()const{return impl_->present_mode;}
}
