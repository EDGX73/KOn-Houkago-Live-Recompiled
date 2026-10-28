// Native diagnostics for the public Android app (0.2 base).
//
// This library links ONLY psprecomp_core (third_party/PSPRecomp, MIT): the
// portable Allegrex decoder, guest memory, raw DEFLATE, NID registry, runtime
// and SHA-256. It never links libkon_guest_corpus.a, the generated units in
// work/recompiled_corrected, the ISO, std.cpk/snd.cpk or the EBOOT.
//
// The Java UI shows the returned text verbatim. Each check starts with PASS or
// FAIL, so the result is visible without a logcat. The report is ASCII because
// JNI's NewStringUTF uses modified UTF-8; accents live in the Java resources.
//
// Built by tools/make_android_apk.py for arm64-v8a and x86_64.

#include "psprecomp/decoder.hpp"
#include "psprecomp/deflate.hpp"
#include "psprecomp/guest_memory.hpp"
#include "psprecomp/nid_registry.hpp"
#include "psprecomp/runtime.hpp"
#include "psprecomp/sha256.hpp"

#include <jni.h>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <exception>
#include <filesystem>
#include <span>
#include <string>
#include <system_error>

namespace {

std::string g_report;
int g_failures = 0;

void check(bool condition, const char* description) {
    g_report += condition ? "PASS " : "FAIL ";
    g_report += description;
    g_report += '\n';
    if (!condition) {
        ++g_failures;
    }
}

void info(const std::string& line) {
    g_report += "INFO ";
    g_report += line;
    g_report += '\n';
}

std::string abi_name() {
#if defined(__aarch64__)
    return "arm64-v8a";
#elif defined(__x86_64__)
    return "x86_64";
#elif defined(__arm__)
    return "armeabi-v7a";
#elif defined(__i386__)
    return "x86";
#else
    return "unknown";
#endif
}

// A registrable function that does nothing: it proves Runtime accepts the
// translated-block API without linking any generated unit.
void probe_function(psprecomp::Runtime&, psprecomp::AllegrexContext&) {}

// One RFC 1951 raw DEFLATE stream that expands to "hello".
constexpr std::uint8_t kDeflateHello[] = {0xCB, 0x48, 0xCD, 0xC9, 0xC9, 0x07, 0x00};

void run_checks(const std::string& work_dir) {
    using namespace psprecomp;

    g_report += "K-On! Houkago Live!! Recompiled - diagnostico nativo\n";
    info("ABI " + abi_name() + ", punteros de " + std::to_string(sizeof(void*) * 8) + " bits, API " +
         std::to_string(__ANDROID_API__));
    info("PSPRecomp core: decodificador, memoria, DEFLATE, NID, runtime y SHA-256 (MIT)");

    // 1. Allegrex decoder: opcodes that the PSP uses everywhere, including the
    //    exact MADD/BREAK encodings the Windows probe decodes.
    const auto madd = decode_allegrex(0x010B001Cu);
    check(madd.kind == OpcodeKind::Madd, "decodificador: MADD 0x010B001C");
    check(decode_allegrex(0x0000000Du).kind == OpcodeKind::Break, "decodificador: BREAK 0x0000000D");
    check(decode_allegrex(0x24840001u).kind == OpcodeKind::Addiu, "decodificador: ADDIU 0x24840001");
    check(decode_allegrex(0x3C080880u).kind == OpcodeKind::Lui, "decodificador: LUI 0x3C080880");
    check(decode_allegrex(0x0000000Cu).kind == OpcodeKind::Syscall, "decodificador: SYSCALL 0x0000000C");
    check(decode_allegrex(0x00000000u).kind == OpcodeKind::Nop, "decodificador: NOP 0x00000000");
    const auto jal = decode_allegrex(0x0C000000u);
    check(jal.kind == OpcodeKind::Jal && jal.is_control_flow() && jal.has_delay_slot(),
          "decodificador: JAL es salto con delay slot");

    // 2. Guest memory: little-endian 32-bit access and the cached/uncached
    //    aliases (0x08804000 and 0x88804000 address the same byte).
    {
        GuestMemory memory;
        memory.store32(0x08804000u, 0xDEADBEEFu);
        check(memory.load32(0x08804000u) == 0xDEADBEEFu, "memoria: store32/load32 en 0x08804000");
        check(memory.load32(0x88804000u) == 0xDEADBEEFu, "memoria: alias no cacheado 0x88804000");
        check(memory.contains(0x08804000u, 4u), "memoria: contains() del bloque");
    }

    // 3. Raw DEFLATE between guest-memory ranges (same code path used for the
    //    CPK/PSP media payloads, with no zlib dependency).
    {
        GuestMemory memory;
        memory.copy_in(0x08805000u, std::span<const std::uint8_t>(kDeflateHello, sizeof kDeflateHello));
        const auto result = inflate_raw_deflate(memory, 0x08806000u, 64u, 0x08805000u);
        check(result.status == RawDeflateStatus::Ok, "DEFLATE: estado Ok");
        check(result.output_size == 5u, "DEFLATE: 5 bytes de salida");
        std::uint8_t text[8]{};
        memory.copy_out(0x08806000u, std::span<std::uint8_t>(text, 5u));
        check(std::memcmp(text, "hello", 5u) == 0, "DEFLATE: salida \"hello\"");
    }

    // 4. NID registry: the import name map used by the PSP loader.
    {
        NidRegistry registry;
        registry.add("sceProbe", 0x12345678u, "sceProbeFunction");
        const auto name = registry.resolve("sceProbe", 0x12345678u);
        check(name.has_value() && *name == "sceProbeFunction", "NID: alta y resolucion de un simbolo");
    }

    // 5. Runtime: registration bookkeeping without any generated unit.
    {
        Runtime runtime;
        const std::size_t before = runtime.function_count();
        runtime.register_function(0x08804000u, probe_function, "kon_probe");
        check(runtime.has_function(0x08804000u), "runtime: has_function(0x08804000)");
        check(runtime.function_count() == before + 1u, "runtime: function_count aumenta en uno");
    }

    // 6. SHA-256 over a real file in the app cache (the updater depends on the
    //    same implementation being correct).
    {
        const std::filesystem::path file =
            std::filesystem::path(work_dir) / "kon_probe_sha256.txt";
        bool wrote = false;
        {
            std::FILE* out = std::fopen(file.string().c_str(), "wb");
            if (out != nullptr) {
                wrote = std::fwrite("abc", 1u, 3u, out) == 3u;
                std::fclose(out);
            }
        }
        std::string digest;
        if (wrote) {
            try {
                digest = psprecomp::sha256_file(file);
            } catch (const std::exception& error) {
                info(std::string("SHA-256: ") + error.what());
            }
        }
        std::error_code ignored;
        std::filesystem::remove(file, ignored);
        check(digest == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
              "SHA-256: vector \"abc\" de FIPS 180-4");
    }
}

} // namespace

extern "C" JNIEXPORT jstring JNICALL
Java_com_edgx73_konrecompiled_KonProbe_run(JNIEnv* env, jclass, jstring work_dir) {
    g_report.clear();
    g_failures = 0;
    try {
        std::string directory;
        if (work_dir != nullptr) {
            const char* utf = env->GetStringUTFChars(work_dir, nullptr);
            if (utf != nullptr) {
                directory = utf;
                env->ReleaseStringUTFChars(work_dir, utf);
            }
        }
        if (directory.empty()) {
            directory = ".";
        }
        run_checks(directory);
        if (g_failures == 0) {
            g_report += "PASS: diagnostico nativo completo\n";
        } else {
            g_report += "FAIL: " + std::to_string(g_failures) + " comprobaciones fallaron\n";
        }
    } catch (const std::exception& error) {
        g_report += "FAIL: excepcion nativa: ";
        g_report += error.what();
        g_report += '\n';
    } catch (...) {
        g_report += "FAIL: excepcion nativa desconocida\n";
    }
    return env->NewStringUTF(g_report.c_str());
}
