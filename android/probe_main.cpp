// Headless Android probe for phase 0 of the port.
//
// It links the portable PSP core (psprecomp_core) and the complete generated
// corpus (126 translated units + registry), then executes corpus code on the
// device without any window, Activity or JNI:
//
//   1. the core decodes the game's own MADD/BREAK encodings;
//   2. psprecomp::register_generated_functions() runs the generated registry and
//      every unit's register_generated_unit_N(), which is generated game code;
//   3. the probe asserts the runtime now resolves the game module entry at
//      0x08804000 and the translated block count is plausible.
//
// Running an actual gameplay block still needs the decrypted EBOOT, the PSP
// module relocations and the mounted std.cpk/snd.cpk (phase 1): the corpus is
// only the translated instruction stream, not a loadable program image.
//
// Build target: android_probe_test. Push with
//   adb push build/android_arm64/android_probe_test /data/local/tmp/
//   adb shell /data/local/tmp/android_probe_test

#include "psprecomp/decoder.hpp"
#include "psprecomp/runtime.hpp"

#include <cstddef>
#include <cstdio>
#include <exception>

namespace {

int failures = 0;

void check(bool condition, const char* description) {
    std::printf("%s %s\n", condition ? "PASS" : "FAIL", description);
    if (!condition) {
        ++failures;
    }
}

} // namespace

int main() {
    try {
        using namespace psprecomp;

        // 0x010B001Cu is the MADD the game's own code uses; 0x0000000Du is BREAK.
        check(decode_allegrex(0x010B001Cu).kind == OpcodeKind::Madd,
              "psprecomp_core decodes the game MADD instruction");
        check(decode_allegrex(0x0000000Du).kind == OpcodeKind::Break,
              "psprecomp_core decodes the game BREAK instruction");

        Runtime runtime;
        const std::size_t before = runtime.function_count();
        register_generated_functions(runtime);
        const std::size_t registered = runtime.function_count();
        std::printf("INFO generated corpus registered %zu functions (was %zu)\n",
                    registered, before);

        check(registered > before + 1000u,
              "the generated registry installed the translated blocks");
        check(runtime.has_function(0x08804000u),
              "the corpus resolves the game module entry at 0x08804000");

        if (failures == 0) {
            std::printf("PASS: headless Android probe executed the generated corpus registry\n");
            return 0;
        }
        std::printf("FAIL: %d probe checks failed\n", failures);
        return 1;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL: %s\n", error.what());
        return 1;
    }
}
