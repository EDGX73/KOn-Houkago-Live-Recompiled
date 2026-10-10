#pragma once
#include "native_gamepad_reports.hpp"
#include "native_button_map.hpp"
#include <atomic>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <vector>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <xinput.h>
#include <hidsdi.h>
#include <hidpi.h>
#include <algorithm>
#include <cstdlib>
#include <span>
#pragma comment(lib,"hid.lib")
namespace kon {
// Every game controller Windows exposes: XInput devices (Xbox and compatible)
// polled directly, everything else (PlayStation, Nintendo, generic
// DirectInput/HID pads) through Raw Input on the game window. Devices may be
// connected or removed at any time. The style of the device used last decides
// which button glyphs the game shows.
class GamepadHub {
public:
    static GamepadHub& instance(){static GamepadHub hub;return hub;}
    // Window thread: receive WM_INPUT / WM_INPUT_DEVICE_CHANGE for joysticks and gamepads.
    void register_window(HWND window);
    void handle_input(HRAWINPUT input);
    void device_changed(WPARAM change,HANDLE device);
    // Game thread: the union of all controllers (XInput polled here).
    PadState read();
    // Last device that was used (pressed a button or moved a stick).
    InputStyle style()const{return style_.load(std::memory_order_relaxed);}
    void note_keyboard(){style_.store(InputStyle::Keyboard,std::memory_order_relaxed);}
    struct DeviceInfo {std::string name;std::uint16_t vendor=0,product=0;InputStyle style=InputStyle::Generic;};
    std::vector<DeviceInfo> devices();
    bool xinput_connected()const{return xinput_connected_.load(std::memory_order_relaxed);}
private:
    struct Device {
        DeviceInfo info;KnownPad known=KnownPad::None;std::vector<std::uint8_t> preparsed;
        struct Axis {int min=0,max=255;bool present=false;};Axis x,y;int hat_min=0,hat_max=7;bool hat=false;
        PadState state;
    };
    GamepadHub()=default;
    Device* device(HANDLE handle);
    void note_activity(InputStyle style,const PadState& before,const PadState& after);
    std::mutex mutex_;std::map<HANDLE,Device> devices_;std::atomic<InputStyle> style_{InputStyle::Keyboard};
    std::atomic<bool> xinput_connected_{false};PadState last_xinput_;
};

namespace gamepad_detail {
inline bool active(const PadState& s){
    const auto off=[](std::uint8_t v){return v<64 || v>192;};
    return s.buttons || off(s.x) || off(s.y);
}
inline std::string narrow(const std::wstring& w){std::string s;s.reserve(w.size());for(const auto c:w)s.push_back(c<128?static_cast<char>(c):'?');return s;}
}
inline void GamepadHub::register_window(HWND window){
    // Generic desktop page: joystick (4) and gamepad (5); also notified of arrivals/removals.
    RAWINPUTDEVICE devices[2]{{1,4,RIDEV_INPUTSINK|RIDEV_DEVNOTIFY,window},{1,5,RIDEV_INPUTSINK|RIDEV_DEVNOTIFY,window}};
    RegisterRawInputDevices(devices,2,sizeof(RAWINPUTDEVICE));
}
inline GamepadHub::Device* GamepadHub::device(HANDLE handle){
    if(const auto it=devices_.find(handle);it!=devices_.end())return &it->second;
    Device d;UINT size=0;
    GetRawInputDeviceInfoW(handle,RIDI_DEVICENAME,nullptr,&size);std::wstring name(size,L'\0');
    if(size)GetRawInputDeviceInfoW(handle,RIDI_DEVICENAME,name.data(),&size);
    while(!name.empty() && !name.back())name.pop_back();
    // XInput devices also appear here (path contains "IG_"); they are read through XInput.
    if(name.find(L"IG_")!=std::wstring::npos || name.find(L"ig_")!=std::wstring::npos)return nullptr;
    RID_DEVICE_INFO info{};info.cbSize=sizeof(info);size=sizeof(info);
    if(GetRawInputDeviceInfoW(handle,RIDI_DEVICEINFO,&info,&size)==UINT(-1) || info.dwType!=RIM_TYPEHID)return nullptr;
    d.info.name=gamepad_detail::narrow(name);d.info.vendor=static_cast<std::uint16_t>(info.hid.dwVendorId);d.info.product=static_cast<std::uint16_t>(info.hid.dwProductId);
    d.known=known_pad(d.info.vendor,d.info.product);d.info.style=style_of(d.known);
    size=0;GetRawInputDeviceInfoW(handle,RIDI_PREPARSEDDATA,nullptr,&size);d.preparsed.resize(size);
    if(size && GetRawInputDeviceInfoW(handle,RIDI_PREPARSEDDATA,d.preparsed.data(),&size)!=UINT(-1)){
        const auto pp=reinterpret_cast<PHIDP_PREPARSED_DATA>(d.preparsed.data());HIDP_CAPS caps{};
        if(HidP_GetCaps(pp,&caps)==HIDP_STATUS_SUCCESS && caps.NumberInputValueCaps){
            std::vector<HIDP_VALUE_CAPS> values(caps.NumberInputValueCaps);USHORT count=caps.NumberInputValueCaps;
            if(HidP_GetValueCaps(HidP_Input,values.data(),&count,pp)==HIDP_STATUS_SUCCESS)for(USHORT n=0;n<count;++n){
                const auto& v=values[n];if(v.UsagePage!=1)continue;const auto usage=v.IsRange?v.Range.UsageMin:v.NotRange.Usage;
                if(usage==0x30){d.x={int(v.LogicalMin),int(v.LogicalMax),true};}
                else if(usage==0x31){d.y={int(v.LogicalMin),int(v.LogicalMax),true};}
                else if(usage==0x39){d.hat=true;d.hat_min=int(v.LogicalMin);d.hat_max=int(v.LogicalMax);}
            }
        }
    }else d.preparsed.clear();
    // The Switch Pro Controller sends input over USB only after this handshake
    // (Bluetooth needs none; the writes then simply fail).
    if(d.known==KnownPad::SwitchPro){
        const HANDLE file=CreateFileW(name.c_str(),GENERIC_READ|GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_EXISTING,0,nullptr);
        if(file!=INVALID_HANDLE_VALUE){for(const std::uint8_t command:{std::uint8_t(0x02),std::uint8_t(0x04)}){std::uint8_t buffer[64]{0x80,command};DWORD written=0;WriteFile(file,buffer,sizeof(buffer),&written,nullptr);}CloseHandle(file);}
    }
    return &devices_.emplace(handle,std::move(d)).first->second;
}
inline void GamepadHub::note_activity(InputStyle style,const PadState& before,const PadState& after){
    if(gamepad_detail::active(after) && (after.buttons!=before.buttons || !gamepad_detail::active(before)))style_.store(style,std::memory_order_relaxed);
}
inline void GamepadHub::handle_input(HRAWINPUT input){
    UINT size=0;GetRawInputData(input,RID_INPUT,nullptr,&size,sizeof(RAWINPUTHEADER));if(!size)return;
    std::vector<std::uint8_t> buffer(size);
    if(GetRawInputData(input,RID_INPUT,buffer.data(),&size,sizeof(RAWINPUTHEADER))==UINT(-1))return;
    const auto* raw=reinterpret_cast<const RAWINPUT*>(buffer.data());if(raw->header.dwType!=RIM_TYPEHID)return;
    std::lock_guard lock(mutex_);auto* d=device(raw->header.hDevice);if(!d)return;
    const auto reports=raw->data.hid.dwCount,length=raw->data.hid.dwSizeHid;
    for(DWORD n=0;n<reports;++n){
        const std::span<const std::uint8_t> report(raw->data.hid.bRawData+std::size_t(n)*length,length);
        PadState s;
        if(d->known==KnownPad::DualShock4)s=parse_dualshock4(report);
        else if(d->known==KnownPad::DualSense)s=parse_dualsense(report);
        else if(d->known==KnownPad::SwitchPro)s=parse_switch_pro(report);
        if(!s.valid && !d->preparsed.empty()){
            const auto pp=reinterpret_cast<PHIDP_PREPARSED_DATA>(d->preparsed.data());
            auto* data=const_cast<PCHAR>(reinterpret_cast<const char*>(report.data()));
            USAGE usages[64];ULONG count=64;std::vector<unsigned> pressed;
            if(HidP_GetUsages(HidP_Input,HID_USAGE_PAGE_BUTTON,0,usages,&count,pp,data,length)==HIDP_STATUS_SUCCESS)for(ULONG k=0;k<count;++k)pressed.push_back(usages[k]);
            ULONG x=0,y=0,hat=8;
            const bool has_x=d->x.present && HidP_GetUsageValue(HidP_Input,1,0,0x30,&x,pp,data,length)==HIDP_STATUS_SUCCESS;
            const bool has_y=d->y.present && HidP_GetUsageValue(HidP_Input,1,0,0x31,&y,pp,data,length)==HIDP_STATUS_SUCCESS;
            if(d->hat && HidP_GetUsageValue(HidP_Input,1,0,0x39,&hat,pp,data,length)==HIDP_STATUS_SUCCESS)hat=hat>=ULONG(d->hat_min) && hat<=ULONG(d->hat_max)?hat-d->hat_min:8;
            s=generic_pad(pressed,hat,has_x?int(x):1,has_y?int(y):1,has_x?d->x.min:0,has_x?d->x.max:2,has_y?d->y.min:0,has_y?d->y.max:2,ButtonMap::instance().generic());
        }
        if(!s.valid)continue;
        s.buttons=ButtonMap::instance().route(d->info.style,s.buttons); // config/controls.ini
        note_activity(d->info.style,d->state,s);d->state=s;
    }
}
inline void GamepadHub::device_changed(WPARAM change,HANDLE handle){
    std::lock_guard lock(mutex_);
    if(change==GIDC_REMOVAL)devices_.erase(handle);else device(handle);
}
inline std::vector<GamepadHub::DeviceInfo> GamepadHub::devices(){
    std::lock_guard lock(mutex_);std::vector<DeviceInfo> out;for(const auto& [handle,d]:devices_)out.push_back(d.info);return out;
}
inline PadState GamepadHub::read(){
    PadState combined;
    {std::lock_guard lock(mutex_);
     for(const auto& [handle,d]:devices_)if(d.state.valid){combined.buttons|=d.state.buttons;if(d.state.x<64 || d.state.x>192)combined.x=d.state.x;if(d.state.y<64 || d.state.y>192)combined.y=d.state.y;}}
    using Poll=DWORD(WINAPI*)(DWORD,XINPUT_STATE*);
    static const Poll poll=[]{const auto library=LoadLibraryExW(L"xinput1_4.dll",nullptr,LOAD_LIBRARY_SEARCH_SYSTEM32);return library?reinterpret_cast<Poll>(GetProcAddress(library,"XInputGetState")):nullptr;}();
    bool any=false;PadState xinput;xinput.valid=true;
    if(poll)for(DWORD n=0;n<XUSER_MAX_COUNT;++n){
        XINPUT_STATE state{};if(poll(n,&state)!=ERROR_SUCCESS)continue;any=true;
        using namespace psp_button;
        const std::array<std::pair<WORD,std::uint32_t>,12> map{{{XINPUT_GAMEPAD_BACK,select},{XINPUT_GAMEPAD_START,start},{XINPUT_GAMEPAD_DPAD_UP,up},{XINPUT_GAMEPAD_DPAD_RIGHT,right},{XINPUT_GAMEPAD_DPAD_DOWN,down},{XINPUT_GAMEPAD_DPAD_LEFT,left},{XINPUT_GAMEPAD_LEFT_SHOULDER,l},{XINPUT_GAMEPAD_RIGHT_SHOULDER,r},{XINPUT_GAMEPAD_Y,triangle},{XINPUT_GAMEPAD_A,circle},{XINPUT_GAMEPAD_B,cross},{XINPUT_GAMEPAD_X,square}}};
        for(const auto& [button,bit]:map)if(state.Gamepad.wButtons&button)xinput.buttons|=bit;
        const auto axis=[](int value){return static_cast<std::uint8_t>((value+32768)*255/65535);};
        if(std::abs(static_cast<int>(state.Gamepad.sThumbLX))>XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE)xinput.x=axis(state.Gamepad.sThumbLX);
        if(std::abs(static_cast<int>(state.Gamepad.sThumbLY))>XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE)xinput.y=axis(-static_cast<int>(state.Gamepad.sThumbLY)-1);
    }
    xinput_connected_.store(any,std::memory_order_relaxed);
    xinput.buttons=ButtonMap::instance().route(InputStyle::Xbox,xinput.buttons);
    if(any){note_activity(InputStyle::Xbox,last_xinput_,xinput);last_xinput_=xinput;combined.buttons|=xinput.buttons;if(xinput.x!=128)combined.x=xinput.x;if(xinput.y!=128)combined.y=xinput.y;}
    combined.valid=true;return combined;
}
}
