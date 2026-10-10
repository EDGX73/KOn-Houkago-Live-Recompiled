#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <cstdint>
#include <memory>
#include <string>
namespace kon {
// Vulkan swapchain presentation for the native game window. It is owned and
// used only by the window's presentation thread: each call uploads a finished
// BGRA image and scales it into the window (aspect preserved, black borders).
// Construction throws when no device can present to the window; the caller
// then keeps GDI presentation.
class NativeVulkanPresenter {
public:
    NativeVulkanPresenter(HWND window,const std::string& preferred_adapter);
    ~NativeVulkanPresenter();
    NativeVulkanPresenter(const NativeVulkanPresenter&)=delete;
    NativeVulkanPresenter& operator=(const NativeVulkanPresenter&)=delete;
    // Returns false when the window currently has no drawable area.
    bool present(const std::uint32_t* bgra,unsigned width,unsigned height,bool linear_filter);
    // Same as present, in two steps: the caller writes the image straight
    // into the returned upload memory (width*height BGRA words), then submits.
    std::uint32_t* begin_frame(unsigned width,unsigned height);
    bool end_frame(bool linear_filter);
    // The window changed size (full screen toggled): the next present rebuilds the swapchain.
    void resized();
    const std::string& adapter()const;
    const std::string& present_mode()const; // "mailbox" or "fifo"
private:
    struct Impl;std::unique_ptr<Impl> impl_;
};
}
