#pragma once
// ABI between the public Android runtime and the private game module.
//
// The runtime contains no generated game code. It loads a module built from
// the player's own ISO on their PC; the module exports kon_game_register and
// fills this pointer before kon_main starts.
namespace psprecomp { class Runtime; }

// The module must export this string; the runtime rejects any other value, so a
// module from a different build (or ABI) cannot be loaded silently.
inline constexpr const char* kGameModuleAbi = "kon-android-game-v1";
#ifdef __ANDROID__
extern "C" void (*kon_android_game_register)(psprecomp::Runtime&);
#endif
