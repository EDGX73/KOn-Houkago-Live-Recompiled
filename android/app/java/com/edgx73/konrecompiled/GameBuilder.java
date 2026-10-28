package com.edgx73.konrecompiled;

import android.content.Context;
import android.content.pm.PackageInfo;
import android.content.res.AssetManager;
import android.os.Build;
import android.os.StatFs;
import android.util.Log;

import org.json.JSONObject;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.nio.charset.StandardCharsets;
import java.security.MessageDigest;
import java.util.ArrayList;
import java.util.Collections;
import java.util.LinkedHashSet;
import java.util.List;
import java.util.Locale;
import java.util.Set;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.concurrent.atomic.AtomicInteger;

/**
 * Builds the private game module on the phone from the player's own ISO.
 *
 * <p>Phases: toolchain, decrypt (EBOOT.BIN → pkon.elf), codegen (psp_recomp),
 * compile (clang, one process per unit) and link/install. Everything runs from
 * {@code nativeLibraryDir} because Android denies exec from {@code files/}.
 *
 * <p>The frame-rate patch is <b>not</b> applied: Android runs the original
 * 30 FPS logic, where that patch is a no-op (tools/apply_logic_rate_patch.py).
 * Consequently host_symbols.cpp is not compiled either.
 *
 * <p>One build at a time. Cancelling keeps finished objects; the next run skips
 * units whose {@code .key} still matches (same cache design as
 * src/native_build.hpp).
 */
final class GameBuilder {
    interface Listener {
        void onStatus(BuildStatus status);
    }

    static final String MODULE_ABI = "kon-android-game-v1";
    /** sha256 of the decrypted pkon.elf (src/boot_probe.cpp, kElfHash). */
    static final String ELF_SHA256 =
            "eda890135ea4aee1a8d0a82b2f00a7159060c58498a6705df7518b36e46b85b1";

    private static final String TAG = "kon-build";
    private static final long MIN_FREE_BYTES = 700L * 1024L * 1024L;
    private static final AtomicBoolean RUNNING = new AtomicBoolean();

    private final Context context;
    private final File gameRoot;
    private final File cache;
    private final File corpus;
    private final File objects;
    private File toolsDir;
    private final File logFile;
    private final File statusFile;
    private final File cancelFlag;
    private final File modules;
    private final Set<Process> active = Collections.synchronizedSet(new LinkedHashSet<>());
    private final Object statusLock = new Object();
    private final Object logLock = new Object();
    private final String abi;
    private final String arch;

    private volatile boolean cancelled;
    private volatile Listener listener;
    private String toolchainId = "none";
    private String appVersion = "?";

    GameBuilder(Context context) {
        this.context = context.getApplicationContext();
        this.gameRoot = new File(this.context.getFilesDir(), "root");
        this.cache = new File(gameRoot, "recomp_cache");
        this.corpus = new File(cache, "corpus");
        this.objects = new File(cache, "obj");
        this.toolsDir = new File(this.context.getFilesDir(), "toolchain");
        this.logFile = new File(cache, "compilacion.log");
        this.statusFile = new File(cache, "status.json");
        this.cancelFlag = new File(cache, "cancel.flag");
        this.modules = new File(gameRoot, "modules");

        String selectedAbi = null;
        for (String candidate : Build.SUPPORTED_ABIS) {
            if ("arm64-v8a".equals(candidate) || "x86_64".equals(candidate)) {
                selectedAbi = candidate;
                break;
            }
        }
        if (selectedAbi == null) {
            throw new IllegalStateException("ABI no soportada: " + String.join(",", Build.SUPPORTED_ABIS));
        }
        this.abi = selectedAbi;
        this.arch = "arm64-v8a".equals(abi) ? "aarch64" : "x86_64";
    }

    static boolean isRunning() {
        return RUNNING.get();
    }

    private String triple() {
        return arch + "-linux-android29";
    }

    /** Missing executable names, or null when the APK carries them all. */
    static String missingBinaries(Context context) {
        final File dir = new File(context.getApplicationInfo().nativeLibraryDir);
        final List<String> missing = new ArrayList<>();
        for (String name : new String[]{"libkon_pspdecrypt.so", "libkon_psprecomp.so",
                "libclang.so", "libld.lld.so"}) {
            final File file = new File(dir, name);
            if (!file.isFile()) {
                missing.add(name);
            }
        }
        return missing.isEmpty() ? null : String.join(", ", missing);
    }

    void cancel() {
        cancelled = true;
        try (OutputStream ignored = new FileOutputStream(cancelFlag)) {
            // The flag survives an app restart; the next run deletes it.
        } catch (IOException ignored) {
            // The in-memory flag is enough while this process lives.
        }
        final List<Process> running;
        synchronized (active) {
            running = new ArrayList<>(active);
        }
        for (Process process : running) {
            destroyTree(process);
        }
    }

    /** Kills a process and, when the runtime has ProcessHandle, its children. */
    private static void destroyTree(Process process) {
        try {
            final Object handle = Process.class.getMethod("toHandle").invoke(process);
            final Object descendants = handle.getClass().getMethod("descendants").invoke(handle);
            if (descendants instanceof java.util.stream.Stream) {
                ((java.util.stream.Stream<?>) descendants).forEach(child -> {
                    try {
                        child.getClass().getMethod("destroyForcibly").invoke(child);
                    } catch (Throwable ignored) {
                        // Already gone or not supported on this API level.
                    }
                });
            }
        } catch (Throwable ignored) {
            // ProcessHandle is not guaranteed on every Android release.
        }
        process.destroyForcibly();
    }

    void run(Listener listener) {
        if (!RUNNING.compareAndSet(false, true)) {
            throw new IllegalStateException("Ya hay una preparación en curso");
        }
        this.listener = listener;
        final BuildStatus status = new BuildStatus();
        status.started = System.currentTimeMillis() / 1000L;
        try {
            readAppVersion();
            ensureDirectories();
            cancelFlag.delete();

            publish(status, BuildStatus.PHASE_TOOLCHAIN, 0, 0, "Preparando el toolchain…");
            ensureToolchain(status);

            final File elf = new File(gameRoot, "analysis/executables/pkon.elf");
            final String elfSha = prepareElf(status, elf);

            prepareCorpus(status, elf, elfSha);
            compileCorpus(status);
            linkModule(status);
            installModule(status, elfSha);

            publish(status, BuildStatus.PHASE_DONE, 1, 1, "Juego preparado");
        } catch (CancelledException error) {
            BuildStatus.appendLog(logFile, "\ncancelado\n");
            publish(status, BuildStatus.PHASE_CANCELLED, status.done, status.total, "Cancelado");
        } catch (Exception error) {
            final String detail = describe(error);
            BuildStatus.appendLog(logFile, "\nerror: " + detail + "\n");
            Log.w(TAG, "prepare failed", error);
            publish(status, BuildStatus.PHASE_ERROR, status.done, status.total, detail);
        } finally {
            this.listener = null;
            RUNNING.set(false);
        }
    }

    // ------------------------------------------------------------------ setup

    private void readAppVersion() {
        try {
            final PackageInfo info = context.getPackageManager().getPackageInfo(context.getPackageName(), 0);
            final long code = Build.VERSION.SDK_INT >= Build.VERSION_CODES.P
                    ? info.getLongVersionCode() : info.versionCode;
            appVersion = (info.versionName == null ? "?" : info.versionName) + "/" + code;
        } catch (Exception ignored) {
            appVersion = "?";
        }
    }

    private void ensureDirectories() throws IOException {
        for (File dir : new File[]{cache, corpus, objects, modules,
                new File(gameRoot, "analysis/executables"), new File(cache, "tmp")}) {
            if (!dir.isDirectory() && !dir.mkdirs()) {
                throw new IOException("no se pudo crear " + dir);
            }
        }
    }

    /**
     * Extracts {@code assets/toolchain} into {@code files/toolchain} on first use.
     * The packager writes {@code toolchain_id.txt}; without it the APK predates the
     * on-device build and the pipeline stops with a clear message.
     */
    private void ensureToolchain(BuildStatus status) throws Exception {
        final AssetManager assets = context.getAssets();
        final String packaged = readAssetText(assets, "toolchain/toolchain_id.txt").trim();
        final File base = new File(context.getFilesDir(), "toolchain");

        // Preferred: files/toolchain/<toolchain_id>/, pushed or extracted once.
        File selected = null;
        if (!packaged.isEmpty()) {
            final File versioned = new File(base, packaged);
            if (packaged.equals(readText(new File(versioned, "toolchain_id.txt")).trim())
                    && toolchainLooksComplete(versioned)) {
                selected = versioned;
            } else {
                // First use or interrupted extraction: rebuild the versioned copy
                // from assets/toolchain (executables, libs, sysroot, headers).
                deleteTree(versioned);
                copyAssetTree(assets, "toolchain", versioned);
                selected = versioned;
            }
        } else {
            // Legacy flat installation (no assets and no id): files/toolchain/.
            final File flat = base;
            if (!toolchainLooksComplete(flat)) {
                throw new IOException("faltan los binarios del toolchain en esta APK");
            }
            selected = flat;
        }

        toolsDir = selected;
        if (!toolchainLooksComplete(toolsDir)) {
            throw new IOException("toolchain incompleto en " + toolsDir);
        }
        toolchainId = readText(new File(toolsDir, "toolchain_id.txt")).trim();
        if (toolchainId.isEmpty()) {
            toolchainId = "none";
        }
        final String missing = missingBinaries(context);
        if (missing != null) {
            throw new IOException("faltan ejecutables en lib/: " + missing);
        }
        // Fails fast (and shows the exact exit code) when the compiler cannot run
        // in the app sandbox, instead of failing later in the first unit.
        runProcess("clang version", new String[]{
                binary("libclang.so").getAbsolutePath(), "--version"}, env(), status);
    }

    private boolean toolchainLooksComplete(File dir) {
        return resourceDir(dir) != null
                && new File(dir, "sysroot/usr/include").isDirectory()
                && new File(dir, "psprecomp/include").isDirectory()
                && new File(dir, "lib/" + abi).isDirectory();
    }

    /** clang resource dir; accepts clang/21/include and include/clang/21/include. */
    private File clangResourceDir() {
        return resourceDir(toolsDir);
    }

    private static File resourceDir(File dir) {
        final File direct = new File(dir, "clang/21");
        if (new File(direct, "include").isDirectory()) {
            return direct;
        }
        final File nested = new File(dir, "include/clang/21");
        if (new File(nested, "include").isDirectory()) {
            return nested;
        }
        return null;
    }

    private static String readAssetText(AssetManager assets, String path) {
        try (InputStream in = assets.open(path)) {
            final ByteArrayOutputStream out = new ByteArrayOutputStream();
            final byte[] buffer = new byte[4096];
            int read;
            while ((read = in.read(buffer)) > 0) {
                out.write(buffer, 0, read);
            }
            return new String(out.toByteArray(), StandardCharsets.UTF_8);
        } catch (IOException error) {
            return "";
        }
    }

    private static void copyAssetTree(AssetManager assets, String path, File target) throws IOException {
        final String[] children = assets.list(path);
        if (children == null || children.length == 0) {
            if (!target.isDirectory() && !target.mkdirs()) {
                throw new IOException("no se pudo crear " + target);
            }
            return;
        }
        if (!target.isDirectory() && !target.mkdirs()) {
            throw new IOException("no se pudo crear " + target);
        }
        for (String child : children) {
            final String assetPath = path + "/" + child;
            final String[] grandChildren = assets.list(assetPath);
            final File destination = new File(target, child);
            if (grandChildren != null && grandChildren.length > 0) {
                copyAssetTree(assets, assetPath, destination);
            } else {
                try (InputStream in = assets.open(assetPath);
                     OutputStream out = new FileOutputStream(destination)) {
                    final byte[] buffer = new byte[1 << 20];
                    int read;
                    while ((read = in.read(buffer)) > 0) {
                        out.write(buffer, 0, read);
                    }
                }
            }
        }
    }

    // ---------------------------------------------------------------- decrypt

    private String prepareElf(BuildStatus status, File elf) throws Exception {
        if (elf.isFile() && ELF_SHA256.equalsIgnoreCase(sha256(elf))) {
            publish(status, BuildStatus.PHASE_DECRYPT, 1, 1, "pkon.elf ya descifrado");
            return ELF_SHA256;
        }
        final File eboot = findEboot();
        if (eboot == null) {
            throw new IOException("falta la ISO: elige tu ISO primero");
        }
        final long total = Math.max(1L, eboot.length());
        publish(status, BuildStatus.PHASE_DECRYPT, 0, (int) Math.min(Integer.MAX_VALUE, total),
                "Descifrando EBOOT.BIN");
        final File part = new File(elf.getPath() + ".part");
        part.delete();
        runProcess("decrypt", new String[]{
                binary("libkon_pspdecrypt.so").getAbsolutePath(),
                eboot.getAbsolutePath(), "-o", part.getAbsolutePath()}, null, status);
        if (!part.isFile() || part.length() < 1024L * 1024L) {
            part.delete();
            throw new IOException("el descifrado no produjo un ELF");
        }
        final String hash = sha256(part);
        if (!ELF_SHA256.equalsIgnoreCase(hash)) {
            part.delete();
            throw new IOException("el ELF descifrado no coincide con esta ISO (SHA-256 " + hash + ")");
        }
        elf.delete();
        if (!part.renameTo(elf)) {
            throw new IOException("no se pudo instalar pkon.elf");
        }
        publish(status, BuildStatus.PHASE_DECRYPT, (int) Math.min(Integer.MAX_VALUE, total),
                (int) Math.min(Integer.MAX_VALUE, total), "pkon.elf verificado");
        return ELF_SHA256;
    }

    private File findEboot() {
        for (String language : new String[]{"jp", "en"}) {
            final File candidate = new File(gameRoot,
                    "analysis/extracted/" + language + "/PSP_GAME/SYSDIR/EBOOT.BIN");
            if (candidate.isFile()) {
                return candidate;
            }
        }
        return null;
    }

    // ---------------------------------------------------------------- codegen

    private void prepareCorpus(BuildStatus status, File elf, String elfSha) throws Exception {
        final String identity = elfSha + " " + sha256(binary("libkon_psprecomp.so"));
        if (isCorpusCurrent(identity)) {
            publish(status, BuildStatus.PHASE_CODEGEN, 1, 1, "código ya traducido");
            return;
        }
        final File staging = new File(cache, "corpus.nuevo");
        deleteTree(staging);
        publish(status, BuildStatus.PHASE_CODEGEN, 0, 0, "Traduciendo el código del juego");

        final AtomicBoolean finished = new AtomicBoolean(false);
        final Thread watcher = new Thread(() -> {
            while (!finished.get()) {
                final int count = countGenerated(staging);
                publish(status, BuildStatus.PHASE_CODEGEN, count, 0,
                        "Traduciendo el código del juego (" + count + " unidades)");
                sleep(500);
            }
        }, "kon-codegen-progress");
        watcher.setDaemon(true);
        watcher.start();
        try {
            runProcess("codegen", new String[]{
                    binary("libkon_psprecomp.so").getAbsolutePath(),
                    elf.getAbsolutePath(), "--auto", staging.getAbsolutePath(),
                    "0x08804000", "16384"}, env(), status);
        } finally {
            finished.set(true);
            try {
                watcher.join(2000);
            } catch (InterruptedException ignored) {
                Thread.currentThread().interrupt();
            }
        }

        final String report = readText(new File(staging, "auto_codegen_report.json"));
        if (!report.contains("\"import_wrappers\": 238")) {
            throw new IOException("la traducción no es la esperada (import_wrappers != 238)");
        }
        writeText(new File(staging, ".identity"), identity);
        deleteTree(corpus);
        if (!staging.renameTo(corpus)) {
            throw new IOException("no se pudo instalar el corpus generado");
        }
        final int units = countGenerated(corpus);
        publish(status, BuildStatus.PHASE_CODEGEN, units, units,
                "Código traducido (" + units + " unidades)");
    }

    private boolean isCorpusCurrent(String identity) {
        return new File(corpus, "generated_registry.cpp").isFile()
                && identity.equals(readText(new File(corpus, ".identity")).trim());
    }

    private static int countGenerated(File dir) {
        final File[] files = dir.listFiles((parent, name) -> name.startsWith("generated_unit_")
                && name.endsWith(".cpp"));
        return files == null ? 0 : files.length;
    }

    // ---------------------------------------------------------------- compile

    private void compileCorpus(BuildStatus status) throws Exception {
        final List<File> sources = unitSources();
        final List<String> expected = new ArrayList<>();
        for (File source : sources) {
            expected.add(stem(source));
        }
        cleanStaleObjects(expected);

        final String flagsKey = "-O2 -DNDEBUG -std=c++20 -fno-sanitize=undefined -g0 -fPIC "
                + "-nostdinc++ " + triple();
        final String headerKey = hashTree(new File[]{
                new File(toolsDir, "psprecomp/include"),
                new File(clangResourceDir(), "include"),
                new File(toolsDir, "sysroot/usr/include/c++/v1"),
                new File(toolsDir, "sysroot/usr/include/" + arch + "-linux-android"),
                new File(toolsDir, "sysroot/usr/include")});

        final int jobs = Math.max(2, Math.min(4, Runtime.getRuntime().availableProcessors() / 2));
        publish(status, BuildStatus.PHASE_COMPILE, 0, sources.size(), "Compilando (0/" + sources.size() + ")");

        final AtomicInteger done = new AtomicInteger(0);
        final AtomicInteger failed = new AtomicInteger(0);
        final String[] failure = new String[1];
        final ExecutorService pool = Executors.newFixedThreadPool(jobs);
        try {
            for (File source : sources) {
                pool.execute(() -> {
                    if (cancelled || failed.get() != 0) {
                        return;
                    }
                    final String stem = stem(source);
                    final File object = new File(objects, stem + ".o");
                    final File keyFile = new File(objects, stem + ".key");
                    final String key;
                    try {
                        key = sha256(sha256(source) + flagsKey + headerKey);
                    } catch (IOException error) {
                        failure[0] = describe(error);
                        failed.incrementAndGet();
                        return;
                    }
                    if (object.isFile() && key.equals(readText(keyFile).trim())) {
                        publish(status, BuildStatus.PHASE_COMPILE, done.incrementAndGet(), sources.size(),
                                "Compilando");
                        return;
                    }
                    try {
                        keyFile.delete();
                        final List<String> command = new ArrayList<>();
                        command.add(binary("libclang.so").getAbsolutePath());
                        command.add("--target=" + triple());
                        command.add("--sysroot=" + new File(toolsDir, "sysroot").getAbsolutePath());
                        command.add("-resource-dir");
                        command.add(clangResourceDir().getAbsolutePath());
                        for (String flag : new String[]{"-O2", "-DNDEBUG", "-std=c++20",
                                "-fno-sanitize=undefined", "-g0", "-fPIC"}) {
                            command.add(flag);
                        }
                        command.add("-nostdinc++");
                        for (String include : new String[]{
                                new File(toolsDir, "sysroot/usr/include/c++/v1").getAbsolutePath(),
                                new File(toolsDir, "sysroot/usr/include/" + arch + "-linux-android").getAbsolutePath(),
                                new File(toolsDir, "sysroot/usr/include").getAbsolutePath()}) {
                            command.add("-isystem");
                            command.add(include);
                        }
                        command.add("-I");
                        command.add(new File(toolsDir, "psprecomp/include").getAbsolutePath());
                        command.add("-I");
                        command.add(corpus.getAbsolutePath());
                        command.add("-c");
                        command.add(source.getAbsolutePath());
                        command.add("-o");
                        command.add(object.getAbsolutePath());
                        runProcess("compile " + stem, command.toArray(new String[0]), env(), status);
                        if (!object.isFile()) {
                            throw new IOException("no se generó " + object.getName());
                        }
                        writeText(keyFile, key);
                        publish(status, BuildStatus.PHASE_COMPILE, done.incrementAndGet(), sources.size(),
                                "Compilando");
                    } catch (CancelledException error) {
                        // cancel() already reported the final state
                    } catch (Throwable error) {
                        failure[0] = describe(error);
                        failed.incrementAndGet();
                    }
                });
            }
        } finally {
            pool.shutdown();
            while (!pool.awaitTermination(1, TimeUnit.MINUTES)) {
                if (cancelled) {
                    pool.shutdownNow();
                }
            }
        }
        if (cancelled) {
            throw new CancelledException();
        }
        if (failed.get() != 0) {
            throw new IOException(failure[0] == null ? "la compilación ha fallado" : failure[0]);
        }
        if (done.get() < sources.size()) {
            throw new IOException("faltan unidades compiladas (" + done.get() + "/" + sources.size() + ")");
        }
        publish(status, BuildStatus.PHASE_COMPILE, sources.size(), sources.size(), "Compilado");
    }

    /** Corpus + psprecomp core + the module entry point, in a stable order. */
    private List<File> unitSources() throws IOException {
        final File moduleSource = new File(cache, "module/game_module.cpp");
        writeText(moduleSource, moduleSource());
        final List<File> sources = new ArrayList<>();
        final File[] generated = corpus.listFiles((parent, name) -> name.startsWith("generated_")
                && name.endsWith(".cpp"));
        if (generated == null || generated.length == 0) {
            throw new IOException("el corpus generado está vacío");
        }
        Collections.addAll(sources, generated);
        final File[] core = new File(toolsDir, "psprecomp/src").listFiles((parent, name) -> name.endsWith(".cpp"));
        if (core == null || core.length < 8) {
            throw new IOException("faltan las fuentes de psprecomp_core");
        }
        Collections.addAll(sources, core);
        sources.add(moduleSource);
        sources.sort((a, b) -> a.getName().compareTo(b.getName()));
        return sources;
    }

    private static String stem(File source) {
        return source.getName().substring(0, source.getName().length() - 4);
    }

    /** Removes objects that no longer belong to the current corpus. */
    private void cleanStaleObjects(List<String> expected) {
        final File[] files = objects.listFiles();
        if (files == null) {
            return;
        }
        for (File file : files) {
            final String name = file.getName();
            final int dot = name.lastIndexOf('.');
            if (dot < 0) {
                continue;
            }
            final String extension = name.substring(dot + 1);
            if (!"o".equals(extension) && !"key".equals(extension)) {
                continue;
            }
            if (!expected.contains(name.substring(0, dot))) {
                file.delete();
            }
        }
    }

    // ------------------------------------------------------------------- link

    private void linkModule(BuildStatus status) throws Exception {
        publish(status, BuildStatus.PHASE_LINK, 0, 1, "Enlazando el módulo");
        final List<File> objectsToLink = new ArrayList<>();
        for (File source : unitSources()) {
            final File object = new File(objects, stem(source) + ".o");
            if (!object.isFile()) {
                throw new IOException("falta el objeto " + object.getName() + " (vuelve a compilar)");
            }
            objectsToLink.add(object);
        }

        final File sysroot = new File(toolsDir, "sysroot");
        final File api = new File(sysroot, "usr/lib/" + arch + "-linux-android/29");
        final File lib = new File(sysroot, "usr/lib/" + arch + "-linux-android");
        final File runtime = new File(sysroot, "usr/lib/clang/19/lib/linux");

        final List<String> command = new ArrayList<>();
        // lld decides its flavour from argv[0], so invoke the linker directly
        // (validated: libld.lld.so -flavor gnu ...).
        command.add(binary("libld.lld.so").getAbsolutePath());
        command.add("-flavor");
        command.add("gnu");
        command.add("-shared");
        command.add("-m");
        command.add("aarch64".equals(arch) ? "aarch64linux" : "elf_x86_64");
        command.add("--no-undefined");
        command.add("-z");
        command.add("max-page-size=16384");
        for (File object : objectsToLink) {
            command.add(object.getAbsolutePath());
        }
        command.add(new File(api, "crtbegin_so.o").getAbsolutePath());
        command.add(new File(lib, "libc++_static.a").getAbsolutePath());
        command.add(new File(lib, "libc++abi.a").getAbsolutePath());
        command.add(new File(runtime, arch + "/libunwind.a").getAbsolutePath());
        command.add(new File(runtime, "libclang_rt.builtins-" + arch + "-android.a").getAbsolutePath());
        command.add(new File(api, "libc.so").getAbsolutePath());
        command.add(new File(api, "libdl.so").getAbsolutePath());
        command.add(new File(api, "libm.so").getAbsolutePath());
        command.add(new File(api, "crtend_so.o").getAbsolutePath());

        final File output = new File(cache, "libkon_game.so.part");
        output.delete();
        command.add("-o");
        command.add(output.getAbsolutePath());
        runProcess("link", command.toArray(new String[0]), env(), status);

        if (!output.isFile() || output.length() < 1024L * 1024L || !isElfForAbi(output)) {
            throw new IOException("el enlace no produjo un módulo válido para " + arch);
        }
        publish(status, BuildStatus.PHASE_LINK, 1, 1, "Módulo enlazado");
    }

    // ---------------------------------------------------------------- install

    private void installModule(BuildStatus status, String elfSha) throws Exception {
        publish(status, BuildStatus.PHASE_INSTALL, 0, 1, "Instalando el módulo");
        final File source = new File(cache, "libkon_game.so.part");
        final File staging = new File(modules, "libkon_game.so.new");
        final File target = new File(modules, "libkon_game.so");

        if (!source.isFile() || !isElfForAbi(source)) {
            throw new IOException("falta el módulo enlazado");
        }
        if (!source.renameTo(staging)) {
            copyFile(source, staging);
            source.delete();
        }
        staging.setReadOnly();
        target.delete();
        if (!staging.renameTo(target)) {
            throw new IOException("no se pudo instalar libkon_game.so");
        }
        target.setReadOnly();

        final JSONObject metadata = new JSONObject();
        metadata.put("abi", MODULE_ABI);
        metadata.put("elf_sha256", elfSha);
        metadata.put("toolchain_id", toolchainId);
        metadata.put("app_version", appVersion);
        metadata.put("built_at", System.currentTimeMillis() / 1000L);
        metadata.put("fps_patch", false);
        writeText(new File(modules, "libkon_game.json.tmp"), metadata.toString(2));
        final File metadataFile = new File(modules, "libkon_game.json");
        final File temporary = new File(modules, "libkon_game.json.tmp");
        metadataFile.delete();
        if (!temporary.renameTo(metadataFile)) {
            throw new IOException("no se pudo escribir libkon_game.json");
        }
        publish(status, BuildStatus.PHASE_INSTALL, 1, 1, "Módulo instalado");
    }

    // --------------------------------------------------------------- process

    private File binary(String name) throws IOException {
        final File file = new File(context.getApplicationInfo().nativeLibraryDir, name);
        if (!file.isFile()) {
            throw new IOException("falta " + name + " en nativeLibraryDir");
        }
        return file;
    }

    private java.util.Map<String, String> env() {
        final java.util.Map<String, String> env = new java.util.HashMap<>();
        env.put("TMPDIR", new File(cache, "tmp").getAbsolutePath());
        env.put("HOME", cache.getAbsolutePath());
        env.put("LD_LIBRARY_PATH", new File(toolsDir, "lib/" + abi).getAbsolutePath());
        env.put("PSPRECOMP_PLAIN_AUTO", "1");
        return env;
    }

    private void runProcess(String label, String[] command, java.util.Map<String, String> environment,
                            BuildStatus status) throws Exception {
        if (cancelled) {
            throw new CancelledException();
        }
        final ProcessBuilder builder = new ProcessBuilder(command);
        builder.directory(gameRoot);
        builder.redirectErrorStream(true);
        if (environment != null) {
            builder.environment().putAll(environment);
        }
        BuildStatus.appendLog(logFile, "\n$ " + label + "\n  " + String.join(" ", command) + "\n");
        final Process process = builder.start();
        active.add(process);
        final Thread pump = new Thread(() -> {
            try (java.io.BufferedReader reader = new java.io.BufferedReader(
                    new java.io.InputStreamReader(process.getInputStream(), StandardCharsets.UTF_8))) {
                String line;
                while ((line = reader.readLine()) != null) {
                    synchronized (logLock) {
                        BuildStatus.appendLog(logFile, line + "\n");
                    }
                }
            } catch (IOException ignored) {
                // The exit code below is the authoritative result.
            }
        }, "kon-log-" + label);
        pump.setDaemon(true);
        pump.start();
        try {
            final int code = process.waitFor();
            try {
                pump.join(2000);
            } catch (InterruptedException interrupted) {
                Thread.currentThread().interrupt();
            }
            if (cancelled) {
                throw new CancelledException();
            }
            if (code != 0) {
                throw new IOException(label + " terminó con código " + code + " (ver compilacion.log)");
            }
        } finally {
            active.remove(process);
        }
    }

    private void publish(BuildStatus status, String phase, int done, int total, String message) {
        synchronized (statusLock) {
            status.phase = phase;
            status.done = done;
            status.total = total;
            status.message = message;
            if (BuildStatus.PHASE_ERROR.equals(phase)) {
                status.error = message;
            }
            BuildStatus.write(statusFile, status);
        }
        final Listener current = listener;
        if (current != null) {
            current.onStatus(status);
        }
    }

    // ----------------------------------------------------------------- utils

    private static String moduleSource() {
        return "#include \"psprecomp/runtime.hpp\"\n"
                + "extern \"C\" __attribute__((visibility(\"default\")))\n"
                + "void kon_game_register(psprecomp::Runtime& runtime) {\n"
                + "    psprecomp::register_generated_functions(runtime);\n"
                + "}\n"
                + "extern \"C\" __attribute__((visibility(\"default\")))\n"
                + "const char* kon_game_abi() { return \"kon-android-game-v1\"; }\n";
    }

    private static boolean isElf(File file) throws IOException {
        if (!file.isFile() || file.length() < 1024) {
            return false;
        }
        try (InputStream in = new FileInputStream(file)) {
            return in.read() == 0x7f && in.read() == 'E' && in.read() == 'L' && in.read() == 'F';
        }
    }

    /** ELF magic plus the machine field (AArch64 0xB7, x86-64 0x3E). */
    private boolean isElfForAbi(File file) throws IOException {
        if (!isElf(file)) {
            return false;
        }
        try (java.io.RandomAccessFile input = new java.io.RandomAccessFile(file, "r")) {
            input.seek(18);
            final int machine = input.read() | (input.read() << 8);
            return "aarch64".equals(arch) ? machine == 0xB7 : machine == 0x3E;
        }
    }

    private static String describe(Throwable error) {
        final String message = error.getMessage();
        if (message == null || message.trim().isEmpty()) {
            return error.getClass().getSimpleName();
        }
        return message;
    }

    private static void sleep(long millis) {
        try {
            Thread.sleep(millis);
        } catch (InterruptedException error) {
            Thread.currentThread().interrupt();
        }
    }

    private static String sha256(File file) throws IOException {
        final MessageDigest digest = digest();
        try (InputStream in = new FileInputStream(file)) {
            final byte[] buffer = new byte[1 << 20];
            int read;
            while ((read = in.read(buffer)) > 0) {
                digest.update(buffer, 0, read);
            }
        }
        return hex(digest.digest());
    }

    private static String sha256(String text) {
        final MessageDigest digest = digest();
        digest.update(text.getBytes(StandardCharsets.UTF_8));
        return hex(digest.digest());
    }

    /** Hash of every file under the given trees, sorted, for the per-unit key. */
    private static String hashTree(File[] trees) throws IOException {
        final List<File> files = new ArrayList<>();
        for (File tree : trees) {
            collect(tree, files);
        }
        files.sort((a, b) -> a.getPath().compareTo(b.getPath()));
        final MessageDigest digest = digest();
        for (File file : files) {
            digest.update(file.getPath().getBytes(StandardCharsets.UTF_8));
            try (InputStream in = new FileInputStream(file)) {
                final byte[] buffer = new byte[1 << 16];
                int read;
                while ((read = in.read(buffer)) > 0) {
                    digest.update(buffer, 0, read);
                }
            }
        }
        return hex(digest.digest());
    }

    private static void collect(File file, List<File> out) {
        if (file.isFile()) {
            out.add(file);
            return;
        }
        final File[] children = file.listFiles();
        if (children == null) {
            return;
        }
        for (File child : children) {
            collect(child, out);
        }
    }

    private static MessageDigest digest() {
        try {
            return MessageDigest.getInstance("SHA-256");
        } catch (Exception error) {
            throw new IllegalStateException("SHA-256 no disponible", error);
        }
    }

    private static String hex(byte[] bytes) {
        final StringBuilder text = new StringBuilder(bytes.length * 2);
        for (byte value : bytes) {
            text.append(String.format(Locale.ROOT, "%02x", value & 0xFF));
        }
        return text.toString();
    }

    private static String readText(File file) {
        if (!file.isFile() || file.length() > 1024L * 1024L) {
            return "";
        }
        try (InputStream in = new FileInputStream(file)) {
            final ByteArrayOutputStream out = new ByteArrayOutputStream();
            final byte[] buffer = new byte[8192];
            int read;
            while ((read = in.read(buffer)) > 0) {
                out.write(buffer, 0, read);
            }
            return new String(out.toByteArray(), StandardCharsets.UTF_8);
        } catch (IOException error) {
            return "";
        }
    }

    private static void writeText(File file, String text) throws IOException {
        final File parent = file.getParentFile();
        if (parent != null && !parent.isDirectory() && !parent.mkdirs()) {
            throw new IOException("no se pudo crear " + parent);
        }
        try (FileOutputStream out = new FileOutputStream(file)) {
            out.write(text.getBytes(StandardCharsets.UTF_8));
            out.flush();
            out.getFD().sync();
        }
    }

    private static void copyFile(File source, File target) throws IOException {
        try (InputStream in = new FileInputStream(source);
             OutputStream out = new FileOutputStream(target)) {
            final byte[] buffer = new byte[1 << 20];
            int read;
            while ((read = in.read(buffer)) > 0) {
                out.write(buffer, 0, read);
            }
        }
    }

    private static void deleteTree(File file) {
        if (!file.exists()) {
            return;
        }
        if (file.isDirectory()) {
            final File[] children = file.listFiles();
            if (children != null) {
                for (File child : children) {
                    deleteTree(child);
                }
            }
        }
        file.delete();
    }

    private static final class CancelledException extends Exception {
    }
}
