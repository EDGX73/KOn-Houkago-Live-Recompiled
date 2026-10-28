// Private Android game module: generated from the player's own ISO on the PC
// or on the phone. The public runtime loads this file at run time; it is never
// part of the public APK.
#include "psprecomp/runtime.hpp"
#include "game_module_abi.hpp"

extern "C" __attribute__((visibility("default")))
void kon_game_register(psprecomp::Runtime& runtime) {
    psprecomp::register_generated_functions(runtime);
}

extern "C" __attribute__((visibility("default")))
const char* kon_game_abi() {
    return kGameModuleAbi;
}
