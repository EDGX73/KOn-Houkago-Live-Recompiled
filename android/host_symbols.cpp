// Host-side state that the patched generated corpus expects.
//
// The PC frame-rate patch (tools/apply_logic_rate_patch.py) emits references to
// seven symbols from the generated units: kon_logic_frame_ticks, kon_logic_step30,
// kon_logic_fps, kon_logic_frame_ticks_f, kon_pad_hold_before, kon_pad_repeat_due
// and kon_smooth_step(). On Windows they are defined in src/boot_probe.cpp, next
// to the Win32 runtime that publishes them every logic frame.
//
// That file cannot be compiled for Android yet (windows.h, GDI, Vulkan surface),
// so phase 0 mirrors the exact same initial values and smoothing helper here:
// original 30 FPS mode, two 60 Hz ticks per frame, smoothing factor 0.5, and at
// least one unit of movement per step. Keep this in sync with boot_probe.cpp
// until the portable runtime takes over the logic-rate pacing (AND-5).
//
// This is not game code: it is host state the translated code reads.

#include <cstdint>

extern "C" std::uint32_t kon_logic_frame_ticks = 2;
extern "C" std::uint32_t kon_logic_step30 = 1;
extern "C" std::uint32_t kon_logic_fps = 30;
// Pad auto-repeat patch scratch (one button at a time, game thread only).
extern "C" std::uint32_t kon_pad_hold_before = 0;
extern "C" std::uint32_t kon_pad_repeat_due = 0;
extern "C" float kon_logic_frame_ticks_f = 2.0f;
extern "C" float kon_logic_smooth_factor = 0.5f;

// Smoothing `x += d/2` per 30 FPS frame becomes `x += d*factor` per logic frame
// (same convergence per second); at least 1 so small gaps still close.
extern "C" std::uint32_t kon_smooth_step(std::uint32_t difference) {
    const auto d = static_cast<std::int32_t>(difference);
    auto step = static_cast<std::int32_t>(static_cast<double>(d) * kon_logic_smooth_factor);
    if (!step && d) {
        step = d > 0 ? 1 : -1;
    }
    return static_cast<std::uint32_t>(step);
}
