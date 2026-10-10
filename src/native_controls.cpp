#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include "native_controls.hpp"
#include "native_gamepads.hpp"
#include "native_glyphs.hpp"
namespace kon {
// Keyboard plus every connected controller (XInput, PlayStation, Nintendo and
// generic HID pads, see native_gamepads.hpp): the union of their states.
HostPad read_host_pad(bool focused){
    HostPad pad;if(!focused)return pad;
    const auto key=[](int code){return (GetAsyncKeyState(code)&0x8000)!=0;};
    pad=keyboard_pad(key);
    if(pad.buttons || pad.x!=128 || pad.y!=128)GamepadHub::instance().note_keyboard();
    const auto controllers=GamepadHub::instance().read();
    pad.buttons|=controllers.buttons;
    if(controllers.x!=128)pad.x=controllers.x;
    if(controllers.y!=128)pad.y=controllers.y;
    // The last device used chooses the button glyphs drawn on screen.
    glyph_style().store(static_cast<std::uint8_t>(GamepadHub::instance().style()),std::memory_order_relaxed);
    return pad;
}
}
