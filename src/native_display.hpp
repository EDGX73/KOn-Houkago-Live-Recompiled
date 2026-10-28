#pragma once
#include "native_gpu_raster.hpp"
#include "psprecomp/guest_memory.hpp"
#include "native_controls.hpp"
#include "native_gamepads.hpp"
#include "native_memory_view.hpp"
#include "kon_version.h"
#ifdef KON_HAS_NATIVE_VULKAN
#include "native_vulkan_present.hpp"
#endif
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif
#include <algorithm>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cwchar>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <functional>
#include <stdexcept>
#include <vector>
#ifndef _WIN32
#include "native_display_android.hpp"
#endif
namespace kon {
inline std::uint32_t psp_pixel_bgra(std::uint32_t pixel,unsigned format) {
    unsigned r,g,b,a;
    const auto five=[](unsigned c){return (c<<3u)|(c>>2u);};
    if(format==0){r=five(pixel&31);g=((pixel>>5)&63);g=(g<<2u)|(g>>4u);b=five((pixel>>11)&31);a=255;}
    else if(format==1){r=five(pixel&31);g=five((pixel>>5)&31);b=five((pixel>>10)&31);a=(pixel&0x8000u)?255:0;}
    else if(format==2){r=(pixel&15)*17;g=((pixel>>4)&15)*17;b=((pixel>>8)&15)*17;a=((pixel>>12)&15)*17;}
    else if(format==3){r=pixel&255;g=(pixel>>8)&255;b=(pixel>>16)&255;a=pixel>>24;}
    else throw std::invalid_argument("Unknown framebuffer pixel format");
    return b|(g<<8u)|(r<<16u)|(a<<24u);
}
// The window, its message loop and all GDI painting run on a dedicated
// presentation thread, so painting never delays original game code. The game
// thread only hands over finished pixel buffers and reads keyboard bits.
#ifdef _WIN32
class NativeWindow {
public:
    // Process-wide presentation settings, set once before the window exists.
    static std::string& preferred_adapter(){static std::string name;return name;}
    static bool& gdi_only(){static bool value=false;return value;}
    // Silent test runs: the window still presents, but off screen, unfocused and off the taskbar.
    static bool& hidden(){static bool value=false;return value;}
    // Full screen when the window opens (the saved setting); F11 toggles it.
    static bool& start_fullscreen(){static bool value=false;return value;}
    // FPS counter in the corner: F3 shows/hides it at any time; the services
    // set the saved state before the window opens and save each change.
    static std::atomic<bool>& show_fps(){static std::atomic<bool> value{true};return value;}
    static std::function<void(bool)>& fps_toggled(){static std::function<void(bool)> f;return f;}
    std::string presenter_name()const{std::lock_guard lock(mutex_);return presenter_name_;}
    // Borderless full screen on the window's monitor (any thread).
    bool fullscreen()const{return fullscreen_.load();}
    void set_fullscreen(bool on){if(const auto w=window_.load())PostMessageW(w,WM_APP+2,on?1:0,0);}
    NativeWindow(){
        thread_=std::thread([this]{run();});
        std::unique_lock lock(mutex_);ready_cv_.wait(lock,[this]{return ready_;});
        if(!window_){lock.unlock();thread_.join();throw std::runtime_error("Cannot create native game window");}
    }
    ~NativeWindow(){if(const auto w=window_.load())PostMessageW(w,WM_APP+1,0,0);if(thread_.joinable())thread_.join();release_overlay();}
    void pump(){} // the presentation thread owns the message loop
    bool closed() const{return closed_.load();}
    bool minimized() const{const auto w=window_.load();return w && IsIconic(w);}
    // Frames handed to the window (the title's FPS counts these).
    std::uint64_t window_frames() const{return frames_submitted_.load(std::memory_order_relaxed);}
    bool focused() const{const auto w=window_.load();return w && GetForegroundWindow()==w;}
    std::uint32_t consume_key_presses(){if(!focused())keyboard_.lose_focus();return keyboard_.consume();}
    std::uint32_t peek_key_presses(){if(!focused())keyboard_.lose_focus();return keyboard_.peek();}
    void acknowledge_key_presses(std::uint32_t delivered){keyboard_.acknowledge(delivered);}
    // On-screen keyboard (sceUtilityOsk): a text box over the game window. The
    // game keeps running; the result is polled until the player accepts or cancels.
    struct TextInput {std::wstring text;bool accepted=false;};
    void begin_text_input(std::wstring description,std::wstring initial,unsigned limit){
        {std::lock_guard lock(mutex_);text_description_=std::move(description);text_initial_=std::move(initial);text_limit_=limit;text_result_.reset();}
        if(const auto w=window_.load())PostMessageW(w,WM_APP+3,0,0);
        else{std::lock_guard lock(mutex_);text_result_=TextInput{text_initial_,false};}
    }
    std::optional<TextInput> text_input_result(){std::lock_guard lock(mutex_);auto result=std::move(text_result_);text_result_.reset();return result;}
    // Online play window (F8, see native_online.hpp): the services carry out
    // each action and answer with the text to show (Code: the room code).
    enum class OnlineAction {Status,Host,Join,Leave,Code};
    using OnlineHandler=std::function<std::wstring(OnlineAction,const std::wstring&)>;
    void set_online_handler(OnlineHandler handler){
        {std::lock_guard lock(mutex_);online_handler_=std::move(handler);}
        // Diagnostic (KON_OPEN_ONLINE): open the window at once, as F8 does.
        if(std::getenv("KON_OPEN_ONLINE"))if(const auto w=window_.load())PostMessageW(w,WM_KEYDOWN,VK_F8,0);
    }
    static std::uint64_t host_us(){return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());}
    // Input latency inside the port: from the controller read used by a frame
    // to that frame's hand-off to Vulkan (DWM and the monitor come after).
    struct Latency {std::uint64_t frames=0,total_us=0,max_us=0;};
    Latency take_latency(){std::lock_guard lock(mutex_);const auto result=latency_;latency_={};return result;}
    // Hands `pixels` over and returns the previous unconsumed buffer in it, so
    // a caller presenting every frame reuses two buffers instead of allocating.
    void present_swap(std::vector<std::uint32_t>& pixels,unsigned width,unsigned height,bool new_frame,std::uint64_t input_us=0) {
        {std::lock_guard lock(mutex_);incoming_compose_={};incoming_.swap(pixels);incoming_width_=width;incoming_height_=height;if(new_frame)incoming_input_us_=input_us;}
        if(new_frame)frames_submitted_.fetch_add(1,std::memory_order_relaxed);
        if(const auto w=window_.load())InvalidateRect(w,nullptr,FALSE);
    }
    // Deferred image: `compose` fills the pixels on the presentation thread
    // (large internal-resolution frames are built there, off the render thread).
    // A newer submission replaces one not yet shown; it is then never called.
    using Compose=std::function<void(std::uint32_t*)>; // fills width*height BGRA words
    void present_compose(Compose compose,unsigned width,unsigned height,bool new_frame,std::uint64_t input_us=0){
        {std::lock_guard lock(mutex_);incoming_compose_=std::move(compose);incoming_.clear();incoming_width_=width;incoming_height_=height;if(new_frame)incoming_input_us_=input_us;}
        if(new_frame)frames_submitted_.fetch_add(1,std::memory_order_relaxed);
        if(const auto w=window_.load())InvalidateRect(w,nullptr,FALSE);
    }
    void present(std::vector<std::uint32_t> pixels,unsigned width,unsigned height,bool new_frame,std::uint64_t input_us=0) {
        {std::lock_guard lock(mutex_);incoming_compose_={};incoming_=std::move(pixels);incoming_width_=width;incoming_height_=height;if(new_frame)incoming_input_us_=input_us;}
        if(new_frame)frames_submitted_.fetch_add(1,std::memory_order_relaxed);
        if(const auto w=window_.load())InvalidateRect(w,nullptr,FALSE);
    }
private:
    std::thread thread_;mutable std::mutex mutex_;std::string presenter_name_="gdi";std::condition_variable ready_cv_;bool ready_=false;
    std::atomic<HWND> window_{nullptr};std::atomic<bool> closed_{false},fullscreen_{false};std::atomic<std::uint64_t> frames_submitted_{0};
    WINDOWPLACEMENT placement_{sizeof(WINDOWPLACEMENT)}; // windowed position, kept while full screen
    void apply_fullscreen(HWND hwnd,bool on){
        if(on==fullscreen_.load() || hidden())return;
        if(on){
            GetWindowPlacement(hwnd,&placement_);MONITORINFO monitor{sizeof(monitor)};GetMonitorInfoW(MonitorFromWindow(hwnd,MONITOR_DEFAULTTONEAREST),&monitor);
            SetWindowLongPtrW(hwnd,GWL_STYLE,WS_POPUP|WS_VISIBLE);
            const auto& r=monitor.rcMonitor;SetWindowPos(hwnd,HWND_TOP,r.left,r.top,r.right-r.left,r.bottom-r.top,SWP_FRAMECHANGED|SWP_NOOWNERZORDER);
        }else{
            SetWindowLongPtrW(hwnd,GWL_STYLE,WS_OVERLAPPEDWINDOW|WS_VISIBLE);SetWindowPlacement(hwnd,&placement_);
            SetWindowPos(hwnd,nullptr,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOZORDER|SWP_NOOWNERZORDER|SWP_FRAMECHANGED);
        }
        fullscreen_=on;
#ifdef KON_HAS_NATIVE_VULKAN
        if(vulkan_)vulkan_->resized();
#endif
        InvalidateRect(hwnd,nullptr,FALSE);
    }
    NativeKeyboard keyboard_;
    std::wstring text_description_,text_initial_;unsigned text_limit_=0;std::optional<TextInput> text_result_; // guarded by mutex_
    HWND text_box_=nullptr,text_edit_=nullptr;HFONT text_font_=nullptr; // presentation thread
    void open_text_input(HWND parent){
        std::wstring description,initial;unsigned limit;
        {std::lock_guard lock(mutex_);description=text_description_;initial=text_initial_;limit=text_limit_;}
        if(text_box_)finish_text_input(false);
        if(hidden()){std::lock_guard lock(mutex_);text_result_=TextInput{initial,false};return;} // silent tests keep the original text
        static bool registered=false;const auto instance=GetModuleHandleW(nullptr);
        if(!registered){WNDCLASSW wc{};wc.lpfnWndProc=text_procedure;wc.hInstance=instance;wc.lpszClassName=L"KonTextInput";wc.hbrBackground=GetSysColorBrush(COLOR_BTNFACE);wc.hCursor=LoadCursorW(nullptr,MAKEINTRESOURCEW(32512));registered=RegisterClassW(&wc) || GetLastError()==ERROR_CLASS_ALREADY_EXISTS;}
        using GetDpi=UINT(WINAPI*)(HWND);unsigned dpi=96;
        if(const auto get=reinterpret_cast<GetDpi>(GetProcAddress(GetModuleHandleW(L"user32.dll"),"GetDpiForWindow")))dpi=std::max(96u,get(parent));
        const auto px=[dpi](int v){return MulDiv(v,int(dpi),96);};
        const int width=px(460),height=px(150);
        RECT game{};GetWindowRect(parent,&game);
        text_box_=CreateWindowExW(WS_EX_TOPMOST|WS_EX_DLGMODALFRAME,L"KonTextInput",L"Teclado de la PSP",WS_POPUP|WS_CAPTION|WS_SYSMENU,
            (game.left+game.right-width)/2,(game.top+game.bottom-height)/2,width,height,parent,nullptr,instance,this);
        if(!text_box_){std::lock_guard lock(mutex_);text_result_=TextInput{initial,false};return;}
        RECT client{};GetClientRect(text_box_,&client);
        text_font_=CreateFontW(-px(15),0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Segoe UI");
        const auto make=[&](const wchar_t* type,const wchar_t* text,DWORD style,int x,int y,int w,int h,int id){
            const auto control=CreateWindowExW(std::wcscmp(type,L"EDIT")==0?WS_EX_CLIENTEDGE:0,type,text,WS_CHILD|WS_VISIBLE|style,x,y,w,h,text_box_,reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),instance,nullptr);
            SendMessageW(control,WM_SETFONT,reinterpret_cast<WPARAM>(text_font_),TRUE);return control;};
        const int margin=px(12),inner=client.right-2*margin;
        make(L"STATIC",description.empty()?L"Escribe el texto:":description.c_str(),SS_LEFT|SS_NOPREFIX,margin,px(10),inner,px(22),0);
        text_edit_=make(L"EDIT",initial.c_str(),ES_AUTOHSCROLL|WS_TABSTOP,margin,px(38),inner,px(28),100);
        if(limit)SendMessageW(text_edit_,EM_SETLIMITTEXT,limit,0);
        make(L"BUTTON",L"Aceptar (Intro)",BS_DEFPUSHBUTTON|WS_TABSTOP,client.right-margin-px(250),client.bottom-px(42),px(130),px(30),IDOK);
        make(L"BUTTON",L"Cancelar (Esc)",BS_PUSHBUTTON|WS_TABSTOP,client.right-margin-px(112),client.bottom-px(42),px(112),px(30),IDCANCEL);
        const auto previous=SetWindowLongPtrW(text_edit_,GWLP_WNDPROC,reinterpret_cast<LONG_PTR>(edit_procedure));SetPropW(text_edit_,L"KonPrevious",reinterpret_cast<HANDLE>(previous));
        EnableWindow(parent,FALSE);ShowWindow(text_box_,SW_SHOW);SetForegroundWindow(text_box_);SetFocus(text_edit_);SendMessageW(text_edit_,EM_SETSEL,0,-1);
    }
    void finish_text_input(bool accepted){
        if(!text_box_)return;
        std::wstring text;
        if(text_edit_){text.resize(std::size_t(GetWindowTextLengthW(text_edit_))+1u);text.resize(std::size_t(GetWindowTextW(text_edit_,text.data(),int(text.size()))));}
        const auto box=text_box_;text_box_=nullptr;text_edit_=nullptr;
        if(const auto parent=GetParent(box)){EnableWindow(parent,TRUE);SetForegroundWindow(parent);SetFocus(parent);}
        DestroyWindow(box);if(text_font_){DeleteObject(text_font_);text_font_=nullptr;}
        std::lock_guard lock(mutex_);if(!accepted)text=text_initial_;text_result_=TextInput{std::move(text),accepted};
    }
    OnlineHandler online_handler_; // guarded by mutex_
    HWND online_box_=nullptr,online_status_=nullptr,online_edit_=nullptr;HFONT online_font_=nullptr; // presentation thread
    std::wstring online_call(OnlineAction action,const std::wstring& text={}){
        OnlineHandler handler;{std::lock_guard lock(mutex_);handler=online_handler_;}
        return handler?handler(action,text):std::wstring(L"El modo online no está disponible en esta ejecución.");
    }
    void open_online(HWND parent){
        if(online_box_){ShowWindow(online_box_,SW_SHOW);SetForegroundWindow(online_box_);return;}
        static bool registered=false;const auto instance=GetModuleHandleW(nullptr);
        if(!registered){WNDCLASSW wc{};wc.lpfnWndProc=online_procedure;wc.hInstance=instance;wc.lpszClassName=L"KonOnline";wc.hbrBackground=GetSysColorBrush(COLOR_BTNFACE);wc.hCursor=LoadCursorW(nullptr,MAKEINTRESOURCEW(32512));registered=RegisterClassW(&wc) || GetLastError()==ERROR_CLASS_ALREADY_EXISTS;}
        using GetDpi=UINT(WINAPI*)(HWND);unsigned dpi=96;
        if(const auto get=reinterpret_cast<GetDpi>(GetProcAddress(GetModuleHandleW(L"user32.dll"),"GetDpiForWindow")))dpi=std::max(96u,get(parent));
        const auto px=[dpi](int v){return MulDiv(v,int(dpi),96);};
        const int width=px(560),height=px(430);RECT game{};GetWindowRect(parent,&game);
        online_box_=CreateWindowExW(WS_EX_TOPMOST|WS_EX_DLGMODALFRAME,L"KonOnline",L"Jugar online (F8) - v" KON_VERSION_WIDE,WS_POPUP|WS_CAPTION|WS_SYSMENU,
            (game.left+game.right-width)/2,(game.top+game.bottom-height)/2,width,height,parent,nullptr,instance,this);
        if(!online_box_)return;
        RECT client{};GetClientRect(online_box_,&client);
        online_font_=CreateFontW(-px(15),0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Segoe UI");
        const auto make=[&](const wchar_t* type,const wchar_t* text,DWORD style,int x,int y,int w,int h,int id){
            const auto control=CreateWindowExW(std::wcscmp(type,L"EDIT")==0?WS_EX_CLIENTEDGE:0,type,text,WS_CHILD|WS_VISIBLE|style,x,y,w,h,online_box_,reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),instance,nullptr);
            SendMessageW(control,WM_SETFONT,reinterpret_cast<WPARAM>(online_font_),TRUE);return control;};
        const int m=px(12),inner=client.right-2*m;
        make(L"STATIC",L"Crea una sala y pasa el código a tu amigo; tu amigo lo escribe abajo y pulsa Unirse. Después entrad los dos en つうしん! (Local) dentro del juego: uno crea la banda y el otro se une.",SS_LEFT,m,px(8),inner,px(60),0);
        make(L"BUTTON",L"Crear sala",BS_PUSHBUTTON|WS_TABSTOP,m,px(74),px(150),px(30),301);
        make(L"BUTTON",L"Copiar código",BS_PUSHBUTTON|WS_TABSTOP,m+px(160),px(74),px(150),px(30),304);
        make(L"BUTTON",L"Salir de la sala",BS_PUSHBUTTON|WS_TABSTOP,m+px(320),px(74),inner-px(320),px(30),305);
        make(L"STATIC",L"Código de la sala de tu amigo:",SS_LEFT,m,px(116),inner,px(22),0);
        online_edit_=make(L"EDIT",L"",ES_AUTOHSCROLL|ES_UPPERCASE|WS_TABSTOP,m,px(140),inner-px(130),px(28),302);
        make(L"BUTTON",L"Unirse",BS_DEFPUSHBUTTON|WS_TABSTOP,client.right-m-px(120),px(139),px(120),px(30),303);
        online_status_=make(L"STATIC",L"",SS_LEFT|SS_NOPREFIX,m,px(182),inner,client.bottom-px(190),306);
        refresh_online();SetTimer(online_box_,1,1000,nullptr);
        ShowWindow(online_box_,SW_SHOW);SetForegroundWindow(online_box_);SetFocus(online_edit_);
    }
    void refresh_online(){if(online_status_)SetWindowTextW(online_status_,online_call(OnlineAction::Status).c_str());}
    void close_online(){
        if(!online_box_)return;const auto box=online_box_;online_box_=online_status_=online_edit_=nullptr;
        KillTimer(box,1);if(const auto parent=GetParent(box)){SetForegroundWindow(parent);SetFocus(parent);}
        DestroyWindow(box);if(online_font_){DeleteObject(online_font_);online_font_=nullptr;}
    }
    static void copy_to_clipboard(HWND owner,const std::wstring& text){
        if(text.empty() || !OpenClipboard(owner))return;EmptyClipboard();
        if(const auto memory=GlobalAlloc(GMEM_MOVEABLE,(text.size()+1)*sizeof(wchar_t))){
            std::memcpy(GlobalLock(memory),text.c_str(),(text.size()+1)*sizeof(wchar_t));GlobalUnlock(memory);
            if(!SetClipboardData(CF_UNICODETEXT,memory))GlobalFree(memory);}
        CloseClipboard();
    }
    static LRESULT CALLBACK online_procedure(HWND hwnd,UINT message,WPARAM w,LPARAM l){
        auto* self=reinterpret_cast<NativeWindow*>(GetWindowLongPtrW(hwnd,GWLP_USERDATA));
        if(message==WM_NCCREATE){self=static_cast<NativeWindow*>(reinterpret_cast<CREATESTRUCTW*>(l)->lpCreateParams);SetWindowLongPtrW(hwnd,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(self));}
        if(self && message==WM_TIMER){self->refresh_online();return 0;}
        if(self && message==WM_COMMAND){
            const auto id=LOWORD(w);
            if(id==301)self->online_call(OnlineAction::Host);
            else if(id==303 || id==IDOK){std::wstring code(std::size_t(GetWindowTextLengthW(self->online_edit_))+1u,L'\0');code.resize(std::size_t(GetWindowTextW(self->online_edit_,code.data(),int(code.size()))));
                self->online_call(OnlineAction::Join,code);}
            else if(id==304)copy_to_clipboard(hwnd,self->online_call(OnlineAction::Code));
            else if(id==305)self->online_call(OnlineAction::Leave);
            else if(id==IDCANCEL){self->close_online();return 0;}
            else return DefWindowProcW(hwnd,message,w,l);
            self->refresh_online();return 0;
        }
        if(self && message==WM_CLOSE){self->close_online();return 0;}
        return DefWindowProcW(hwnd,message,w,l);
    }
    // Enter and Esc in the single-line box accept or cancel.
    static LRESULT CALLBACK edit_procedure(HWND edit,UINT message,WPARAM w,LPARAM l){
        const auto previous=reinterpret_cast<WNDPROC>(GetPropW(edit,L"KonPrevious"));
        if((message==WM_KEYDOWN || message==WM_CHAR) && (w==VK_RETURN || w==VK_ESCAPE)){
            // A key still held from the game (Enter is START) repeats; only a new press counts.
            if(message==WM_KEYDOWN && !(l&(1ll<<30)))PostMessageW(GetParent(edit),WM_COMMAND,w==VK_RETURN?IDOK:IDCANCEL,0);return 0;}
        if(message==WM_NCDESTROY)RemovePropW(edit,L"KonPrevious");
        return CallWindowProcW(previous,edit,message,w,l);
    }
    static LRESULT CALLBACK text_procedure(HWND hwnd,UINT message,WPARAM w,LPARAM l){
        auto* self=reinterpret_cast<NativeWindow*>(GetWindowLongPtrW(hwnd,GWLP_USERDATA));
        if(message==WM_NCCREATE){self=static_cast<NativeWindow*>(reinterpret_cast<CREATESTRUCTW*>(l)->lpCreateParams);SetWindowLongPtrW(hwnd,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(self));}
        if(self && message==WM_COMMAND && (LOWORD(w)==IDOK || LOWORD(w)==IDCANCEL)){self->finish_text_input(LOWORD(w)==IDOK);return 0;}
        if(self && message==WM_CLOSE){self->finish_text_input(false);return 0;}
        if(self && message==WM_ACTIVATE && LOWORD(w)!=WA_INACTIVE && self->text_edit_){SetFocus(self->text_edit_);return 0;}
        return DefWindowProcW(hwnd,message,w,l);
    }
    std::vector<std::uint32_t> incoming_;unsigned incoming_width_=480,incoming_height_=272; // guarded by mutex_
    std::uint64_t incoming_input_us_=0;Latency latency_;Compose incoming_compose_; // guarded by mutex_
    // Owned by the presentation thread only.
    std::vector<std::uint32_t> pixels_;unsigned width_=480,height_=272;bool show_controls_=false;
    std::uint64_t fps_frames_=0;std::chrono::steady_clock::time_point fps_started_=std::chrono::steady_clock::now();
    wchar_t fps_text_[64]=L"FPS: --";
    HDC paint_dc_=nullptr;HBITMAP paint_bitmap_=nullptr;HGDIOBJ paint_previous_=nullptr;
    int paint_width_=0,paint_height_=0;
#ifdef KON_HAS_NATIVE_VULKAN
    std::unique_ptr<NativeVulkanPresenter> vulkan_;
    // Composition surface for Vulkan: frame plus GDI overlay text. Original-size
    // frames are doubled (nearest) so the text stays sharp; internal-resolution
    // frames are already large and keep their size.
    HDC compose_dc_=nullptr;HBITMAP compose_bitmap_=nullptr;HGDIOBJ compose_previous_=nullptr;std::uint32_t* compose_bits_=nullptr;int compose_width_=0,compose_height_=0;
    void release_compose(){if(compose_dc_ && compose_previous_)SelectObject(compose_dc_,compose_previous_);if(compose_bitmap_)DeleteObject(compose_bitmap_);if(compose_dc_)DeleteDC(compose_dc_);compose_dc_=nullptr;compose_bitmap_=nullptr;compose_previous_=nullptr;compose_bits_=nullptr;compose_width_=compose_height_=0;}
    bool present_vulkan(){
        if(!vulkan_ || pixels_.empty())return false;
        const int factor=width_<960?2:1,width=int(width_)*factor,height=int(height_)*factor;
        if(width!=compose_width_ || height!=compose_height_){
            release_compose();BITMAPINFO info{};info.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);info.bmiHeader.biWidth=width;info.bmiHeader.biHeight=-height;info.bmiHeader.biPlanes=1;info.bmiHeader.biBitCount=32;info.bmiHeader.biCompression=BI_RGB;
            compose_dc_=CreateCompatibleDC(nullptr);void* bits=nullptr;compose_bitmap_=CreateDIBSection(compose_dc_,&info,DIB_RGB_COLORS,&bits,nullptr,0);
            if(!compose_dc_ || !compose_bitmap_ || !bits){release_compose();return false;}
            compose_previous_=SelectObject(compose_dc_,compose_bitmap_);compose_bits_=static_cast<std::uint32_t*>(bits);compose_width_=width;compose_height_=height;
        }
        if(factor==1)std::memcpy(compose_bits_,pixels_.data(),std::size_t(width)*height*4u);
        else for(int y=0;y<height;++y){const auto* in=&pixels_[std::size_t(y/2)*width_];auto* out=compose_bits_+std::size_t(y)*width;for(int x=0;x<width;++x)out[x]=in[x/2];}
        draw_overlay(compose_dc_,width,height);GdiFlush();
        try{vulkan_->present(compose_bits_,unsigned(width),unsigned(height),true);return true;}
        catch(const std::exception& error){vulkan_.reset();std::lock_guard lock(mutex_);presenter_name_=std::string("gdi (vulkan failed: ")+error.what()+")";return false;}
    }
#endif
    // Overlay boxes copied onto an image. The layout scales with the target:
    // the original fixed 372x248 bitmap left 5 px text at 1088p.
    HDC overlay_dc_=nullptr;HBITMAP overlay_bitmap_=nullptr;std::uint32_t* overlay_bits_=nullptr;
    int overlay_width_=0,overlay_height_=0;HFONT overlay_font_=nullptr;int overlay_font_height_=0;
    bool direct_frame_=false; // the last frame went straight to the Vulkan upload memory
    struct OverlayRects{RECT fps{};RECT help{};};
    static const wchar_t* overlay_line(int index){
        static const wchar_t* lines[]{L"CONTROLES PSP - F1: mostrar / ocultar",L"Flechas: cruceta   |   Stick: I arriba, K abajo, J izquierda, L derecha",L"S: Triángulo    X: Círculo",L"Z: Cruz         A: Cuadrado",L"Q: L            W: R",L"Enter: START    Retroceso: SELECT",L"Cruz (Z) y Círculo (X): confirmar / volver; lo indica el juego.",L"F11 o Alt+Intro: pantalla completa",L"F8: jugar online (crear sala o unirse con un código)",L"F3: mostrar / ocultar los FPS",L"La ventana debe tener el foco.",L"K-On! Houkago Live!! Recompiled v" KON_VERSION_WIDE};
        return index>=0 && index<12?lines[index]:L"";
    }
    static std::wstring overlay_text(){std::wstring text;for(int i=0;i<12;++i){if(i)text+=L'\n';text+=overlay_line(i);}return text;}
    static double overlay_scale(int width,int height){return std::max(1.0,std::min(width/480.0,height/272.0));}
    static int overlay_font_px(double scale,int height){return std::max(11,std::min(int(11*scale),int(height/18)));}
    HFONT overlay_font(int height){
        if(overlay_font_ && overlay_font_height_==height)return overlay_font_;
        if(overlay_font_){DeleteObject(overlay_font_);overlay_font_=nullptr;}
        overlay_font_=CreateFontW(-height,0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Segoe UI");
        overlay_font_height_=height;return overlay_font_;
    }
    OverlayRects overlay_layout(HDC dc,int width,int height){
        const double scale=overlay_scale(width,height);
        const int margin=std::max(4,int(8*scale)),pad=std::max(4,int(8*scale));
        const int font=overlay_font_px(scale,height),fps_w=std::min(width-2*margin,int(122*scale)),fps_h=font+std::max(4,int(6*scale));
        OverlayRects rects;
        rects.fps={margin,margin,margin+fps_w,margin+fps_h};
        const int help_top=rects.fps.bottom+std::max(4,int(6*scale));
        const int panel_w=std::min(width-2*margin,std::max(int(372*scale),width*3/5));
        RECT calc{0,0,panel_w-2*pad,0};
        const auto font_handle=overlay_font(font);
        const auto old_font=font_handle?SelectObject(dc,font_handle):nullptr; // measured with the final font
        const auto text=overlay_text();
        DrawTextW(dc,text.c_str(),-1,&calc,DT_CALCRECT|DT_WORDBREAK|DT_NOPREFIX);
        if(old_font)SelectObject(dc,old_font);
        const int panel_h=std::max(font,int(std::min<long>(height-help_top-margin,calc.bottom+2*pad)));
        rects.help={margin,help_top,margin+panel_w,help_top+panel_h};
        return rects;
    }
    OverlayRects draw_overlay(HDC dc,int width,int height){
        SetBkMode(dc,TRANSPARENT);
        const double scale=overlay_scale(width,height);const int font=overlay_font_px(scale,height),pad=std::max(4,int(8*scale));
        const auto old_font=overlay_font(font)?SelectObject(dc,overlay_font_):nullptr;
        const auto rects=overlay_layout(dc,width,height);
        if(show_fps().load()){
            FillRect(dc,&rects.fps,static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
            SetTextColor(dc,RGB(120,255,120));
            RECT text=rects.fps;text.left+=pad;
            DrawTextW(dc,fps_text_,-1,&text,DT_LEFT|DT_VCENTER|DT_SINGLELINE|DT_NOPREFIX);
        }
        if(show_controls_){
            FillRect(dc,&rects.help,static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
            FrameRect(dc,&rects.help,static_cast<HBRUSH>(GetStockObject(GRAY_BRUSH)));
            RECT area=rects.help;area.left+=pad;area.top+=pad;area.right-=pad;area.bottom-=pad;
            SetTextColor(dc,RGB(255,255,255));
            const auto text=overlay_text();
            DrawTextW(dc,text.c_str(),-1,&area,DT_LEFT|DT_TOP|DT_WORDBREAK|DT_NOPREFIX);
        }
        if(old_font)SelectObject(dc,old_font);
        return rects;
    }
    void release_overlay(){
        if(overlay_dc_ && overlay_bitmap_){SelectObject(overlay_dc_,GetStockObject(DEFAULT_GUI_FONT));DeleteObject(overlay_bitmap_);}
        if(overlay_dc_)DeleteDC(overlay_dc_);
        overlay_dc_=nullptr;overlay_bitmap_=nullptr;overlay_bits_=nullptr;overlay_width_=overlay_height_=0;
        if(overlay_font_){DeleteObject(overlay_font_);overlay_font_=nullptr;overlay_font_height_=0;}
    }
    void overlay_into(std::uint32_t* image,unsigned width,unsigned height){
        if(int(width)!=overlay_width_ || int(height)!=overlay_height_){
            release_overlay();
            BITMAPINFO info{};info.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);info.bmiHeader.biWidth=int(width);info.bmiHeader.biHeight=-int(height);info.bmiHeader.biPlanes=1;info.bmiHeader.biBitCount=32;info.bmiHeader.biCompression=BI_RGB;
            overlay_dc_=CreateCompatibleDC(nullptr);void* bits=nullptr;overlay_bitmap_=CreateDIBSection(overlay_dc_,&info,DIB_RGB_COLORS,&bits,nullptr,0);
            if(!overlay_dc_ || !overlay_bitmap_ || !bits){release_overlay();return;}
            SelectObject(overlay_dc_,overlay_bitmap_);overlay_bits_=static_cast<std::uint32_t*>(bits);
            overlay_width_=int(width);overlay_height_=int(height);
        }
        if(!overlay_bits_)return;
        const auto rects=draw_overlay(overlay_dc_,int(width),int(height));GdiFlush();
        const auto copy=[&](const RECT& r){
            const int left=std::max<long>(0,r.left),top=std::max<long>(0,r.top);
            const int right=std::min<long>(long(width),r.right),bottom=std::min<long>(long(height),r.bottom);
            for(int y=top;y<bottom;++y)if(right>left)std::memcpy(image+std::size_t(y)*width+left,overlay_bits_+std::size_t(y)*width+left,std::size_t(right-left)*4u);
        };
        if(show_fps().load())copy(rects.fps);
        if(show_controls_)copy(rects.help);
    }
    void run(){
        const auto instance=GetModuleHandleW(nullptr);
        WNDCLASSW wc{};wc.lpfnWndProc=procedure;wc.hInstance=instance;wc.lpszClassName=L"KonNativePortWindow";wc.hIcon=LoadIconW(instance,MAKEINTRESOURCEW(1)); // src/app.rc
        wc.hCursor=LoadCursorW(nullptr,MAKEINTRESOURCEW(32512));
        HWND created=nullptr;
        // Real pixels: without DPI awareness Windows stretches the whole window
        // (125% scaling blurs every frame). The window keeps its usual size.
        using SetContext=BOOL(WINAPI*)(HANDLE);using GetDpi=UINT(WINAPI*)();
        const auto user32=GetModuleHandleW(L"user32.dll");unsigned dpi=96;
        if(const auto set=reinterpret_cast<SetContext>(GetProcAddress(user32,"SetProcessDpiAwarenessContext"));set && set(reinterpret_cast<HANDLE>(-4))) // per monitor v2
            if(const auto get=reinterpret_cast<GetDpi>(GetProcAddress(user32,"GetDpiForSystem")))dpi=std::max(96u,get());
        if(RegisterClassW(&wc) || GetLastError()==ERROR_CLASS_ALREADY_EXISTS){
            RECT rect{0,0,LONG(960*dpi/96),LONG(544*dpi/96)};AdjustWindowRect(&rect,WS_OVERLAPPEDWINDOW,FALSE);
            const bool off=hidden();
            created=CreateWindowExW(off?WS_EX_TOOLWINDOW|WS_EX_NOACTIVATE:0,wc.lpszClassName,L"K-On! Houkago Live!! Recompiled " KON_VERSION_WIDE,WS_OVERLAPPEDWINDOW,
                off?-32000:CW_USEDEFAULT,off?-32000:CW_USEDEFAULT,rect.right-rect.left,rect.bottom-rect.top,nullptr,nullptr,instance,this);
        }
        if(created){ShowWindow(created,hidden()?SW_SHOWNOACTIVATE:SW_SHOW);UpdateWindow(created);SetTimer(created,1,1000,nullptr);GamepadHub::instance().register_window(created);}
#ifdef KON_HAS_NATIVE_VULKAN
        if(created && !gdi_only()){
            try{vulkan_=std::make_unique<NativeVulkanPresenter>(created,preferred_adapter());std::lock_guard lock(mutex_);presenter_name_="vulkan:"+vulkan_->adapter()+":"+vulkan_->present_mode();}
            catch(const std::exception& error){vulkan_.reset();std::lock_guard lock(mutex_);presenter_name_=std::string("gdi (vulkan unavailable: ")+error.what()+")";}
        }
#endif
        if(created && start_fullscreen())apply_fullscreen(created,true);
        {std::lock_guard lock(mutex_);window_=created;ready_=true;}ready_cv_.notify_all();
        if(!created)return;
        MSG message{};while(GetMessageW(&message,nullptr,0,0)>0){TranslateMessage(&message);DispatchMessageW(&message);}
        release_paint_buffer();
#ifdef KON_HAS_NATIVE_VULKAN
        vulkan_.reset(); // destroy on the thread that owns the window
#endif
    }
    void update_fps(HWND hwnd){
        const auto now=std::chrono::steady_clock::now();const auto seconds=std::chrono::duration<double>(now-fps_started_).count();
        const auto frames=frames_submitted_.load(std::memory_order_relaxed);const auto fps=(frames-fps_frames_)/seconds;fps_frames_=frames;fps_started_=now;
        std::swprintf(fps_text_,64,L"FPS: %.1f",fps);
        // Rendered resolution: the size of the frames being shown (272p original, 272p x N enlarged).
        wchar_t title[200];std::swprintf(title,200,L"K-On! Houkago Live!! Recompiled " KON_VERSION_WIDE L" | FPS: %.1f | %ux%u (%up)",fps,width_,height_,height_);SetWindowTextW(hwnd,title);
        RECT overlay{8,8,130,36};InvalidateRect(hwnd,&overlay,FALSE);
    }
    void release_paint_buffer(){
        if(paint_dc_ && paint_previous_)SelectObject(paint_dc_,paint_previous_);
        if(paint_bitmap_)DeleteObject(paint_bitmap_);if(paint_dc_)DeleteDC(paint_dc_);
        paint_dc_=nullptr;paint_bitmap_=nullptr;paint_previous_=nullptr;paint_width_=paint_height_=0;
    }
    HDC prepare_paint_buffer(HDC target,int width,int height){
        if(width<=0 || height<=0)return target;
        if(width!=paint_width_ || height!=paint_height_){
            release_paint_buffer();paint_dc_=CreateCompatibleDC(target);paint_bitmap_=CreateCompatibleBitmap(target,width,height);
            if(!paint_dc_ || !paint_bitmap_){release_paint_buffer();return target;}
            paint_previous_=SelectObject(paint_dc_,paint_bitmap_);paint_width_=width;paint_height_=height;
        }
        return paint_dc_?paint_dc_:target;
    }
    static LRESULT CALLBACK procedure(HWND hwnd,UINT message,WPARAM w,LPARAM l) {
        NativeWindow* self=reinterpret_cast<NativeWindow*>(GetWindowLongPtrW(hwnd,GWLP_USERDATA));
        if(message==WM_NCCREATE){self=static_cast<NativeWindow*>(reinterpret_cast<CREATESTRUCTW*>(l)->lpCreateParams);SetWindowLongPtrW(hwnd,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(self));}
        if(self && (message==WM_CLOSE || message==WM_APP+1)){self->closed_=true;KillTimer(hwnd,1);self->window_=nullptr;DestroyWindow(hwnd);PostQuitMessage(0);return 0;}
        if(self && message==WM_TIMER){self->update_fps(hwnd);return 0;}
        // Controllers other than XInput (PlayStation, Nintendo, generic HID), and their arrival/removal.
        if(message==WM_INPUT){GamepadHub::instance().handle_input(reinterpret_cast<HRAWINPUT>(l));return DefWindowProcW(hwnd,message,w,l);}
        if(message==WM_INPUT_DEVICE_CHANGE){GamepadHub::instance().device_changed(w,reinterpret_cast<HANDLE>(l));return 0;}
        // Full screen: F11 or Alt+Enter at any time, or the Settings Video row.
        if(self && message==WM_APP+2){self->apply_fullscreen(hwnd,w!=0);return 0;}
        if(self && message==WM_APP+3){self->open_text_input(hwnd);return 0;}
        if(self && message==WM_SYSKEYDOWN && w==VK_RETURN && !(l&(1ll<<30))){self->apply_fullscreen(hwnd,!self->fullscreen_.load());return 0;}
        if(self && message==WM_KEYDOWN && w==VK_F11){if(!(l&(1ll<<30)))self->apply_fullscreen(hwnd,!self->fullscreen_.load());return 0;}
#ifdef KON_HAS_NATIVE_VULKAN
        if(self && message==WM_SIZE && self->vulkan_)self->vulkan_->resized();
#endif
        if(self && message==WM_KEYDOWN){
            if(w==VK_F1 && !(l&(1ll<<30))){self->show_controls_=!self->show_controls_;InvalidateRect(hwnd,nullptr,FALSE);return 0;}
            if(w==VK_F8 && !(l&(1ll<<30))){self->open_online(hwnd);return 0;}
            if(w==VK_F3 && !(l&(1ll<<30))){const bool on=!show_fps().load();show_fps()=on;if(fps_toggled())fps_toggled()(on);InvalidateRect(hwnd,nullptr,FALSE);return 0;}
            self->keyboard_.down(static_cast<unsigned>(w),(l&(1ll<<30))!=0);GamepadHub::instance().note_keyboard();
        }
        if(self && message==WM_KILLFOCUS)self->keyboard_.lose_focus();
        if(self && message==WM_PAINT) {
            std::uint64_t input_us=0;
            Compose compose;
            {std::lock_guard lock(self->mutex_);
             if(self->incoming_compose_){compose=std::move(self->incoming_compose_);self->incoming_compose_={};self->width_=self->incoming_width_;self->height_=self->incoming_height_;input_us=self->incoming_input_us_;self->incoming_input_us_=0;}
             else if(!self->incoming_.empty()){self->pixels_.swap(self->incoming_);self->incoming_.resize(0);self->width_=self->incoming_width_;self->height_=self->incoming_height_;input_us=self->incoming_input_us_;self->incoming_input_us_=0;self->direct_frame_=false;}}
            PAINTSTRUCT paint{};const auto target=BeginPaint(hwnd,&paint);RECT client{};GetClientRect(hwnd,&client);
            // Diagnostic (KON_SKIP_WINDOW_PRESENT): compose on the CPU as usual, but show nothing (GPU cost of presenting).
            static const bool skip_present=std::getenv("KON_SKIP_WINDOW_PRESENT")!=nullptr;
            if(skip_present && compose){self->pixels_.resize(std::size_t(self->width_)*self->height_);compose(self->pixels_.data());EndPaint(hwnd,&paint);return 0;}
#ifdef KON_HAS_NATIVE_VULKAN
            // Large composed frames go straight into the Vulkan upload memory
            // (one copy instead of three); the overlay is drawn on top.
            // A repaint without a new frame shows the direct frame again (its
            // upload memory keeps it), with the overlay refreshed.
            bool shown=false;
            if((compose || self->direct_frame_) && self->vulkan_)try{
                if(auto* image=self->vulkan_->begin_frame(self->width_,self->height_)){
                    if(compose){compose(image);compose={};self->direct_frame_=true;self->pixels_.clear();}
                    self->overlay_into(image,self->width_,self->height_);shown=self->vulkan_->end_frame(true);}
            }catch(const std::exception& error){self->vulkan_.reset();std::lock_guard lock(self->mutex_);self->presenter_name_=std::string("gdi (vulkan failed: ")+error.what()+")";}
#endif
            if(compose){self->pixels_.resize(std::size_t(self->width_)*self->height_);compose(self->pixels_.data());}
#ifdef KON_HAS_NATIVE_VULKAN
            if(shown || self->present_vulkan()){
                if(input_us){const auto now=host_us();if(now>input_us){std::lock_guard lock(self->mutex_);auto& l=self->latency_;++l.frames;l.total_us+=now-input_us;l.max_us=std::max(l.max_us,now-input_us);}}
                EndPaint(hwnd,&paint);return 0;}
#endif
            const auto dc=self->prepare_paint_buffer(target,client.right,client.bottom);
            FillRect(dc,&client,static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
            if(!self->pixels_.empty()) {
                BITMAPINFO info{};info.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);info.bmiHeader.biWidth=self->width_;
                info.bmiHeader.biHeight=-static_cast<LONG>(self->height_);info.bmiHeader.biPlanes=1;info.bmiHeader.biBitCount=32;info.bmiHeader.biCompression=BI_RGB;
                const auto scale=std::min(static_cast<double>(client.right)/self->width_,static_cast<double>(client.bottom)/self->height_);
                const auto width=static_cast<int>(scale*self->width_),height=static_cast<int>(scale*self->height_);
                SetStretchBltMode(dc,COLORONCOLOR);
                StretchDIBits(dc,(client.right-width)/2,(client.bottom-height)/2,width,height,0,0,self->width_,self->height_,self->pixels_.data(),&info,DIB_RGB_COLORS,SRCCOPY);
            }
            self->draw_overlay(dc,client.right,client.bottom);
            if(dc!=target)BitBlt(target,0,0,client.right,client.bottom,dc,0,0,SRCCOPY);
            EndPaint(hwnd,&paint);return 0;
        }
        return DefWindowProcW(hwnd,message,w,l);
    }
};
#endif
class NativeDisplay {
public:
    struct Buffer {std::uint32_t address=0,stride=512,format=3;std::uint64_t input_us=0;};
    // Host time of the controller read the game used for the frame being drawn.
    void set_input_time(std::uint64_t us){input_us_=us;}
    NativeWindow::Latency take_latency(){return window_?window_->take_latency():NativeWindow::Latency{};}
    explicit NativeDisplay(psprecomp::GuestMemory& memory,bool show_window=false):memory_(memory),show_window_(show_window){}
    std::uint32_t set_mode(std::uint32_t mode,unsigned width,unsigned height) {
        if(mode!=0 || width!=480 || height!=272)return 0x80000107u;
        width_=width;height_=height;mode_=mode;
        if(show_window_ && !window_){window_=std::make_unique<NativeWindow>();if(online_handler_)window_->set_online_handler(online_handler_);}return 0;
    }
    std::uint32_t set_buffer(std::uint32_t address,unsigned stride,unsigned format,unsigned sync) {
        if(sync>1 || format>3)return 0x80000107u;
        if(address && ((address&15u) || stride<width_ || (stride&(stride-1u)) || stride>1024))return 0x800001FEu;
        if(address && !memory_.contains(address,static_cast<std::size_t>(stride)*height_*(format==3?4u:2u)))return 0x80000103u;
        // PC frame-rate modes: an immediate (sync 0) change also waits for the
        // frame end, and a frame still pending is shown before it is replaced.
        if(pc_presentation_ && pending_update_)latch_now();
        const Buffer next{address,stride,format,input_us_};pending_=next;
        if(sync==0 && !pc_presentation_){current_=next;pending_update_=false;present(address!=0);}else pending_update_=true;
        return 0;
    }
    Buffer buffer(unsigned sync) const{return sync?pending_:current_;}
    // Vblank timeline: the PSP refreshes at 60000/1001 Hz. The PC frame-rate
    // modes raise the refresh to `hz` (same 1000/1001 factor); a change starts
    // a new epoch at `now` so frame numbers stay monotonic.
    std::uint64_t frame_count(std::uint64_t microseconds) const{
        if(microseconds<epoch_us_)return epoch_frame_;
        return epoch_frame_+(microseconds-epoch_us_)*refresh_hz_/1001000u;
    }
    void set_refresh_hz(unsigned hz,std::uint64_t now){
        if(!hz || hz==refresh_hz_)return;
        epoch_frame_=frame_count(now);epoch_us_=now;refresh_hz_=hz;
    }
    unsigned refresh_hz()const{return refresh_hz_;}
    std::uint64_t vblank_time(std::uint64_t frame) const{
        return epoch_us_+((frame-epoch_frame_)*1001000u+refresh_hz_-1u)/refresh_hz_;
    }
    std::uint32_t until_next_vblank(std::uint64_t microseconds) const {
        return static_cast<std::uint32_t>(vblank_time(frame_count(microseconds)+1u)-microseconds);
    }
    std::uint64_t until_vblank(std::uint64_t microseconds,std::uint32_t cycles) const {
        if(!cycles || cycles>0x7FFFFFFFu)throw std::invalid_argument("Vblank count must be positive");
        return vblank_time(frame_count(microseconds)+cycles)-microseconds;
    }
    // PC frame-rate modes: show the pending buffer now instead of at the next
    // 60 Hz vblank, so every logic frame is presented as soon as it is done.
    void latch_now(){
        pending_ticks_=0;
        if(!pending_update_)return;
        const bool new_frame=pending_.address!=0;current_=pending_;pending_update_=false;present(new_frame);
    }
    void set_pc_presentation(bool enabled){if(!enabled && pending_update_ && pc_presentation_)latch_now();pc_presentation_=enabled;}
    void update(std::uint64_t microseconds) {
        const auto frame=frame_count(microseconds);
        if(frame>last_frame_ && pc_presentation_){
            // PC modes present at frame ends; a buffer left pending for three
            // 60 Hz ticks (the game stopped ending frames) is still shown, and
            // the unchanged scanout is not re-read (it may be mid-render).
            last_frame_=frame;
            if(pending_update_ && ++pending_ticks_>=3){pending_ticks_=0;latch_now();}else if(window_)window_->pump();
        }
        else if(frame>last_frame_) {
            last_frame_=frame;const bool new_frame=pending_update_ && pending_.address;
            if(pending_update_){current_=pending_;pending_update_=false;}
            present(new_frame);
        }
        if(microseconds>=fps_sample_us_+1000000ull){fps_=double(submitted_frames_-fps_sample_frames_)*1000000.0/double(microseconds-fps_sample_us_);fps_sample_us_=microseconds;fps_sample_frames_=submitted_frames_;fps_available_=true;}
        if(window_)window_->pump();
    }
    double measured_fps()const{return fps_;}
    std::uint64_t present_us()const{return present_us_;}
    std::string presenter()const{return window_?window_->presenter_name():"none";}
    bool fps_available()const{return fps_available_;}
    bool closed() const{return window_ && window_->closed();}
    bool minimized() const{return window_ && window_->minimized();}
    std::uint64_t window_frames() const{return window_?window_->window_frames():0;}
    std::uint64_t submitted_frames() const{return submitted_frames_;}
    bool focused() const{return window_ && window_->focused();}
    bool fullscreen() const{return window_ && window_->fullscreen();}
    void set_fullscreen(bool on){NativeWindow::start_fullscreen()=on;if(window_)window_->set_fullscreen(on);}
    std::uint32_t consume_key_presses(){return window_?window_->consume_key_presses():0;}
    std::uint32_t peek_key_presses(){return window_?window_->peek_key_presses():0;}
    void acknowledge_key_presses(std::uint32_t delivered){if(window_)window_->acknowledge_key_presses(delivered);}
    // On-screen keyboard; without a window the original text is kept at once.
    void begin_text_input(std::wstring description,std::wstring initial,unsigned limit){
        if(window_)window_->begin_text_input(std::move(description),std::move(initial),limit);else no_window_text_=NativeWindow::TextInput{std::move(initial),false};}
    // Kept until the window exists (it opens when the game sets its display mode).
    void set_online_handler(NativeWindow::OnlineHandler handler){online_handler_=std::move(handler);if(window_)window_->set_online_handler(online_handler_);}
    NativeWindow::OnlineHandler online_handler_;
    std::optional<NativeWindow::TextInput> text_input_result(){
        if(window_)return window_->text_input_result();auto result=std::move(no_window_text_);no_window_text_.reset();return result;}
    // Asynchronous rendering: presentations go to the render thread, which
    // shows the buffer once its draws are done (present_buffer).
    void set_present_hook(std::function<void(const Buffer&,bool)> hook){present_hook_=std::move(hook);}
    void present_buffer(const Buffer& buffer,bool new_frame){
        if(!window_)return;
        const auto start=std::chrono::steady_clock::now();
        static const char* capture_dir=std::getenv("KON_PRESENT_CAPTURE_DIR"); // diagnostic: presented image once per second
        const bool capture=capture_dir && start>=next_present_capture_;
        unsigned scale=1;NativeWindow::Compose compose;
        if(hires_source_)if(const auto view=hires_source_(buffer.address))if((compose=hires_compose(buffer,view)))scale=view.scale;
        if(scale>1){
            ++hires_presents_;
            // The window thread copies the enlarged image when it paints: the
            // backend fills its other image meanwhile, so batches do not wait.
            if(capture){present_pixels_.resize(std::size_t(width_*scale)*height_*scale);compose(present_pixels_.data());last_capture_=present_pixels_;compose={};
                window_->present_swap(present_pixels_,width_*scale,height_*scale,new_frame,new_frame?buffer.input_us:0);}
            else window_->present_compose(std::move(compose),width_*scale,height_*scale,new_frame,new_frame?buffer.input_us:0);
        }else{pixels_into(buffer,present_pixels_);if(capture)write_present_capture(start,1);window_->present_swap(present_pixels_,width_,height_,new_frame,new_frame?buffer.input_us:0);}
        if(capture && scale>1)write_present_capture(start,scale,true);
        present_us_+=std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now()-start).count();
    }
    void write_present_capture(std::chrono::steady_clock::time_point now,unsigned scale,bool swapped=false){
        static const auto every=std::chrono::milliseconds([]{const char* v=std::getenv("KON_PRESENT_CAPTURE_MS");return v?std::max(10,std::atoi(v)):1000;}());next_present_capture_=now+every; // KON_PRESENT_CAPTURE_MS: interval (default 1 s)
        const auto ms=std::chrono::duration_cast<std::chrono::milliseconds>(now-capture_origin_).count();
        static const long long begin=[]{const char* v=std::getenv("KON_PRESENT_CAPTURE_BEGIN_MS");return v?std::atoll(v):0ll;}(); // diagnostic window start
        if(ms<begin)return;
        // After present_swap the composed image is the window's; capture the copy kept before.
        if(swapped)write_bitmap(std::filesystem::path(std::getenv("KON_PRESENT_CAPTURE_DIR"))/("present_"+std::to_string(ms)+"ms.bmp"),last_capture_,width_*scale,height_*scale);
        else write_bitmap(std::filesystem::path(std::getenv("KON_PRESENT_CAPTURE_DIR"))/("present_"+std::to_string(ms)+"ms.bmp"),present_pixels_,width_*scale,height_*scale);
    }
    std::vector<std::uint32_t> last_capture_;
    // Internal resolution: the render thread's source of enlarged buffers.
    void set_hires_source(std::function<NativeHiresView(std::uint32_t)> source){hires_source_=std::move(source);}
    std::uint64_t hires_presents()const{return hires_presents_;}
    std::uint64_t hires_fallback_tiles()const{return hires_fallback_tiles_;}
    std::uint64_t hires_bad_presents()const{return hires_bad_presents_;}
    // An enlarged buffer, 16x16 original pixels at a time: where the guest's
    // buffer still equals the snapshot its last batch left, the enlarged
    // samples (already BGRA); elsewhere (written since by the guest or the CPU
    // renderer) the original pixel, repeated. The comparison runs here; the
    // returned function copies the samples while the view's plane stays
    // reserved. Empty when the view does not cover the screen.
    NativeWindow::Compose hires_compose(const Buffer& buffer,const NativeHiresView& view)const{
        const unsigned n=view.scale,bytes=buffer.format==3?4u:2u;
        if(n<2 || view.stride!=buffer.stride || view.width<width_ || view.rows<height_ || view.snapshot.size()<std::size_t(height_-1)*view.stride+width_)return {};
        NativeMemoryView scanout(memory_,buffer.address,(static_cast<std::size_t>(height_-1)*buffer.stride+width_)*bytes);
        if(!scanout.direct())return {};
        const auto& table=bgra_tables()[std::min(buffer.format,2u)];
        const auto* in=scanout.data();
        const unsigned columns=(width_+15)/16,rows=(height_+15)/16;
        auto changed=std::make_shared<std::vector<std::uint32_t>>(); // BGRA original pixels of changed tiles (or empty)
        std::vector<std::uint8_t> same(std::size_t(columns)*rows,1);unsigned changed_tiles=0;
        for(unsigned ty=0;ty<height_;ty+=16)for(unsigned tx=0;tx<width_;tx+=16){
            const unsigned x1=std::min(width_,tx+16),y1=std::min(height_,ty+16);bool equal=true;
            for(unsigned y=ty;y<y1 && equal;++y){
                const auto* snap=view.snapshot.data()+std::size_t(y)*view.stride;const auto* row=in+std::size_t(y)*buffer.stride*bytes;
                if(bytes==4)equal=std::memcmp(row+tx*4u,snap+tx,std::size_t(x1-tx)*4u)==0;
                else for(unsigned x=tx;x<x1;++x){std::uint16_t v;std::memcpy(&v,row+x*2u,2);if(v!=std::uint16_t(snap[x])){equal=false;break;}}
            }
            if(equal)continue;
            ++changed_tiles;same[std::size_t(ty/16)*columns+tx/16]=0;
            if(changed->empty())changed->assign(std::size_t(width_)*height_,0);
            for(unsigned y=ty;y<y1;++y)for(unsigned x=tx;x<x1;++x){std::uint32_t v=0;std::memcpy(&v,in+(std::size_t(y)*buffer.stride+x)*bytes,bytes);
                (*changed)[std::size_t(y)*width_+x]=bytes==4?((v&0x0000FF00u)|((v&0xFFu)<<16u)|((v>>16u)&0xFFu)|0xFF000000u):table[v&0xFFFFu];}
        }
        // Diagnostic: original tiles shown instead of enlarged ones.
        hires_fallback_tiles_+=changed_tiles;if(changed_tiles*4>columns*rows)++hires_bad_presents_;
        if(view.readers)view.readers->fetch_add(1);
        // Releases the plane when the image is built or replaced unbuilt.
        auto reserved=std::shared_ptr<void>(nullptr,[readers=view.readers](void*){if(readers)readers->fetch_sub(1);});
        const unsigned width=width_,height=height_,source=view.stride*n;const auto* samples=view.pixels;
        return [=,same=std::move(same)](std::uint32_t* pixels){
            const unsigned wide=width*n;
            for(unsigned ty=0;ty<height;ty+=16)for(unsigned tx=0;tx<width;tx+=16){
                const unsigned x1=std::min(width,tx+16),y1=std::min(height,ty+16);const bool equal=same[std::size_t(ty/16)*columns+tx/16];
                for(unsigned y=ty;y<y1;++y)for(unsigned dy=0;dy<n;++dy){
                    auto* out=pixels+(std::size_t(y)*n+dy)*wide;
                    if(equal)std::memcpy(out+tx*n,samples+(std::size_t(y)*n+dy)*source+tx*n,std::size_t(x1-tx)*n*4u);
                    else for(unsigned x=tx;x<x1;++x){const auto c=(*changed)[std::size_t(y)*width+x];for(unsigned i=0;i<n;++i)out[x*n+i]=c;}
                }
            }
            (void)reserved;
        };
    }
    std::vector<std::uint32_t> present_pixels_; // render thread only
    std::function<NativeHiresView(std::uint32_t)> hires_source_;std::uint64_t hires_presents_=0;mutable std::uint64_t hires_fallback_tiles_=0,hires_bad_presents_=0;
    std::chrono::steady_clock::time_point next_present_capture_{},capture_origin_=std::chrono::steady_clock::now();
    static void bmp_u16(std::ofstream& out,unsigned value){const char bytes[2]={char(value&0xFFu),char((value>>8)&0xFFu)};out.write(bytes,2);}
    static void bmp_u32(std::ofstream& out,std::uint32_t value){const char bytes[4]={char(value&0xFFu),char((value>>8)&0xFFu),char((value>>16)&0xFFu),char((value>>24)&0xFFu)};out.write(bytes,4);}
    static void write_bitmap(const std::filesystem::path& path,const std::vector<std::uint32_t>& data,unsigned width,unsigned height){
        const auto bytes=static_cast<std::uint32_t>(data.size()*4u);
        std::ofstream out(path,std::ios::binary);
        // 14-byte BMP file header + 40-byte BITMAPINFOHEADER, top-down 32-bit BGRA.
        out.put('B');out.put('M');bmp_u32(out,14u+40u+bytes);bmp_u16(out,0);bmp_u16(out,0);bmp_u32(out,14u+40u);
        bmp_u32(out,40u);bmp_u32(out,width);bmp_u32(out,0u-static_cast<std::uint32_t>(height));bmp_u16(out,1);bmp_u16(out,32);
        bmp_u32(out,0);bmp_u32(out,bytes);bmp_u32(out,2835);bmp_u32(out,2835);bmp_u32(out,0);bmp_u32(out,0);
        out.write(reinterpret_cast<const char*>(data.data()),bytes);
        if(!out)throw std::runtime_error("Cannot write bitmap");
    }
    // 16-bit guest pixel to BGRA, per format; built once, thread-safely (the
    // render thread and captures both convert).
    static const std::array<std::vector<std::uint32_t>,3>& bgra_tables(){
        static const auto tables=[]{std::array<std::vector<std::uint32_t>,3> t;for(unsigned f=0;f<3;++f){t[f].resize(65536);for(std::uint32_t v=0;v<65536;++v)t[f][v]=psp_pixel_bgra(v,f)|0xFF000000u;}return t;}();
        return tables;
    }
    std::vector<std::uint32_t> pixels() const {return pixels(current_);}
    std::vector<std::uint32_t> pixels(const Buffer& buffer) const {std::vector<std::uint32_t> result;pixels_into(buffer,result);return result;}
    void pixels_into(const Buffer& current_,std::vector<std::uint32_t>& pixels) const {
        pixels.assign(static_cast<std::size_t>(width_)*height_,0xFF000000u);
        if(!current_.address)return;
        const unsigned bytes=current_.format==3?4u:2u;
        NativeMemoryView scanout(memory_,current_.address,(static_cast<std::size_t>(height_-1)*current_.stride+width_)*bytes);
        if(scanout.direct() && bytes==4){
            // 8888 scanout: swap R/B and force alpha, one row at a time.
            std::vector<std::uint32_t> row(width_);
            for(unsigned y=0;y<height_;y++){const auto offset=std::size_t(y)*current_.stride*4u;for(unsigned x=0;x<width_;x++)row[x]=scanout.load32(offset+x*4u);
                auto* out=&pixels[static_cast<std::size_t>(y)*width_];for(unsigned x=0;x<width_;x++){const auto v=row[x];out[x]=(v&0x0000FF00u)|((v&0xFFu)<<16u)|((v>>16u)&0xFFu)|0xFF000000u;}}
            return;
        }
        if(scanout.direct()){
            // 16-bit scanout: bulk read, then one table lookup per pixel (presented every frame).
            // Built once, thread-safely: the render thread and captures both convert.
            const auto& table=bgra_tables()[current_.format];
            const auto* in=scanout.data();
            for(unsigned y=0;y<height_;y++){const auto* row=in+std::size_t(y)*current_.stride*2u;auto* out=&pixels[std::size_t(y)*width_];
                for(unsigned x=0;x<width_;x++){std::uint16_t v;std::memcpy(&v,row+x*2u,2);out[x]=table[v];}}
            return;
        }
        for(unsigned y=0;y<height_;y++)for(unsigned x=0;x<width_;x++) {
            const auto offset=(y*current_.stride+x)*bytes;
            const auto raw=bytes==4?scanout.load32(offset):scanout.load16(offset);
            pixels[static_cast<std::size_t>(y)*width_+x]=psp_pixel_bgra(raw,current_.format)|0xFF000000u;
        }
        return;
    }
    void save_bitmap(const std::filesystem::path& path,bool fps_overlay=false) const {
        auto data=pixels();
#ifdef _WIN32
        if(fps_overlay){
            const auto bytes=static_cast<std::uint32_t>(data.size()*4u);
            BITMAPINFOHEADER info{};info.biSize=sizeof(info);info.biWidth=width_;info.biHeight=-static_cast<LONG>(height_);info.biPlanes=1;info.biBitCount=32;info.biCompression=BI_RGB;info.biSizeImage=bytes;
            BITMAPINFO dib{};dib.bmiHeader=info;HDC dc=CreateCompatibleDC(nullptr);void* pixels=nullptr;HBITMAP bitmap=CreateDIBSection(dc,&dib,DIB_RGB_COLORS,&pixels,nullptr,0);
            if(!dc || !bitmap || !pixels){if(bitmap)DeleteObject(bitmap);if(dc)DeleteDC(dc);throw std::runtime_error("Cannot create FPS capture overlay");}
            const auto old=SelectObject(dc,bitmap);std::memcpy(pixels,data.data(),bytes);
            RECT box{8,8,160,34};FillRect(dc,&box,static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));SetTextColor(dc,RGB(100,255,170));SetBkMode(dc,TRANSPARENT);
            wchar_t label[64];if(fps_available_)std::swprintf(label,64,L"FPS juego: %.1f",fps_);else std::swprintf(label,64,L"FPS juego: --");
            const auto font=CreateFontW(-17,0,0,0,FW_BOLD,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Segoe UI");const auto oldfont=SelectObject(dc,font);
            TextOutW(dc,14,11,label,std::wcslen(label));GdiFlush();std::memcpy(data.data(),pixels,bytes);SelectObject(dc,oldfont);if(font)DeleteObject(font);SelectObject(dc,old);DeleteObject(bitmap);DeleteDC(dc);
        }
#else
        (void)fps_overlay;
#endif
        write_bitmap(path,data,width_,height_);
    }
private:
    psprecomp::GuestMemory& memory_;bool show_window_=false;
    std::unique_ptr<NativeWindow> window_;
    unsigned mode_=0,width_=480,height_=272;Buffer current_,pending_;bool pending_update_=false,pc_presentation_=false;std::uint64_t input_us_=0;unsigned pending_ticks_=0;
    std::uint64_t last_frame_=0,epoch_us_=0,epoch_frame_=0;unsigned refresh_hz_=60;
    std::uint64_t submitted_frames_=0,fps_sample_us_=0,fps_sample_frames_=0;
    double fps_=0;bool fps_available_=false;std::atomic<std::uint64_t> present_us_{0};
    std::function<void(const Buffer&,bool)> present_hook_;
    // Count buffer submissions actually presented, never repeated scanout or WM_PAINT.
    // Convert and repaint only new frames; an unchanged scanout is refreshed at
    // 15 Hz (covers guests that draw into the displayed buffer directly).
    void present(bool new_frame){
        if(new_frame)++submitted_frames_;if(!window_)return;
        if(!new_frame && (++stale_presents_&3u)){window_->pump();return;}
        if(present_hook_){present_hook_(current_,new_frame);return;}
        const auto start=std::chrono::steady_clock::now();window_->present(pixels(),width_,height_,new_frame,new_frame?current_.input_us:0);
        present_us_+=std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now()-start).count();
    }
    unsigned stale_presents_=0;std::optional<NativeWindow::TextInput> no_window_text_;
};
}
