#pragma once
// Android presentation for NativeDisplay. The game renders into its own CPU
// framebuffer; this window copies finished frames to the Activity's
// ANativeWindow. There is no separate message loop: the Activity owns the
// lifecycle and feeds input through the JNI hooks below.
#include <atomic>
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>
#ifdef __ANDROID__
#include <android/native_window.h>
#endif

namespace kon {
class NativeWindow;
namespace android_window {
#ifdef __ANDROID__
ANativeWindow* surface();
void set_surface(ANativeWindow* surface);
void clear_surface();
void present(const std::uint32_t* pixels,unsigned width,unsigned height);
bool focused();
void set_focused(bool focused);
bool closed();
void set_closed(bool closed);
void key_press(std::uint32_t bit);
void request_text(std::wstring description,std::wstring initial,unsigned limit);
void set_active_window(NativeWindow* window);
void clear_active_window(NativeWindow* window);
void deliver_text_result(std::wstring text,bool accepted);
void online_result(std::wstring text);
std::wstring online_action(int action,const std::wstring& text);
#endif
} // namespace android_window

class NativeWindow {
public:
    static std::string& preferred_adapter(){static std::string name;return name;}
    static bool& gdi_only(){static bool value=false;return value;}
    static bool& hidden(){static bool value=false;return value;}
    static bool& start_fullscreen(){static bool value=true;return value;}
    static std::atomic<bool>& show_fps(){static std::atomic<bool> value{true};return value;}
    static std::function<void(bool)>& fps_toggled(){static std::function<void(bool)> f;return f;}
    struct TextInput {std::wstring text;bool accepted=false;};
    enum class OnlineAction {Status,Host,Join,Leave,Code};
    using OnlineHandler=std::function<std::wstring(OnlineAction,const std::wstring&)>;
    using Compose=std::function<void(std::uint32_t*)>;
    struct Latency {std::uint64_t frames=0,total_us=0,max_us=0;};

    NativeWindow(){
#ifdef __ANDROID__
        android_window::set_active_window(this);
#endif
    }
    ~NativeWindow(){
#ifdef __ANDROID__
        android_window::clear_active_window(this);
#endif
    }
    void pump() {}
    bool closed() const {
#ifdef __ANDROID__
        return android_window::closed();
#else
        return false;
#endif
    }
    bool minimized() const {return false;}
    std::uint64_t window_frames() const {return frames_submitted_.load(std::memory_order_relaxed);}
    bool focused() const {
#ifdef __ANDROID__
        return android_window::focused();
#else
        return false;
#endif
    }
    std::uint32_t consume_key_presses(){return keys_.exchange(0);}
    std::uint32_t peek_key_presses(){return keys_.load();}
    void acknowledge_key_presses(std::uint32_t delivered){keys_.fetch_and(~delivered);}
    void begin_text_input(std::wstring description,std::wstring initial,unsigned limit){
        {
            std::lock_guard lock(mutex_);
            text_result_.reset();
        }
#ifdef __ANDROID__
        android_window::request_text(std::move(description),std::move(initial),limit);
#else
        std::lock_guard lock(mutex_);
        text_result_=TextInput{std::move(initial),false};
#endif
    }
    std::optional<TextInput> text_input_result(){
        std::lock_guard lock(mutex_);
        auto result=std::move(text_result_);text_result_.reset();return result;
    }
    void set_online_handler(OnlineHandler handler){std::lock_guard lock(mutex_);online_handler_=std::move(handler);}
    static std::uint64_t host_us(){
        return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
    }
    Latency take_latency(){return {};}
    bool fullscreen() const {return true;}
    void set_fullscreen(bool){}
    std::string presenter_name()const {return "android";}

    void present_swap(std::vector<std::uint32_t>& pixels,unsigned width,unsigned height,bool new_frame,std::uint64_t /*input_us*/=0){
        if(new_frame)frames_submitted_.fetch_add(1,std::memory_order_relaxed);
#ifdef __ANDROID__
        present_pixels(pixels.data(),width,height);
#else
        (void)pixels;(void)width;(void)height;
#endif
    }
    void present_compose(Compose compose,unsigned width,unsigned height,bool new_frame,std::uint64_t input_us=0){
        if(!compose)return;
        std::vector<std::uint32_t> pixels(static_cast<std::size_t>(width)*height);
        compose(pixels.data());
        present_swap(pixels,width,height,new_frame,input_us);
    }
    void present(std::vector<std::uint32_t> pixels,unsigned width,unsigned height,bool new_frame,std::uint64_t input_us=0){
        present_swap(pixels,width,height,new_frame,input_us);
    }

    // JNI entry points.
    void deliver_text(std::wstring text,bool accepted){
        std::lock_guard lock(mutex_);
        text_result_=TextInput{std::move(text),accepted};
    }
    void inject_key(std::uint32_t bit){keys_.fetch_or(bit);}

private:
    std::atomic<std::uint64_t> frames_submitted_{0};
    std::atomic<std::uint32_t> keys_{0};
    std::mutex mutex_;
    std::optional<TextInput> text_result_;
    OnlineHandler online_handler_;

#ifdef __ANDROID__
    void present_pixels(const std::uint32_t* source,unsigned width,unsigned height){
        android_window::present(source,width,height);
    }
#endif
};
}
