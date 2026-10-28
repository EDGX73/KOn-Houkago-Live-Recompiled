package com.edgx73.konrecompiled;

/**
 * JNI bridge to libkon_probe.so.
 *
 * <p>The native library links only psprecomp_core (the MIT decoder/runtime in
 * third_party/PSPRecomp). It does not contain, load or read any game data, and
 * it never links the generated corpus.
 */
public final class KonProbe {
    static {
        System.loadLibrary("kon_probe");
    }

    private KonProbe() {
    }

    /**
     * Runs the native checks and returns a report whose lines start with PASS or
     * FAIL. {@code workDir} is used only for the SHA-256 test file.
     */
    public static native String run(String workDir);
}
