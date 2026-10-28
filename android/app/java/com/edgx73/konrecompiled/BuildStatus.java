package com.edgx73.konrecompiled;

import android.content.Context;

import org.json.JSONObject;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.RandomAccessFile;
import java.nio.charset.StandardCharsets;

/**
 * State of the on-device "prepare game" pipeline.
 *
 * <p>Written atomically to {@code files/root/recomp_cache/status.json} after every
 * phase and every compiled unit, so the Activity and the foreground service can
 * show real progress and a killed process can be resumed.
 */
final class BuildStatus {
    static final String PHASE_IDLE = "idle";
    static final String PHASE_TOOLCHAIN = "toolchain";
    static final String PHASE_DECRYPT = "decrypt";
    static final String PHASE_CODEGEN = "codegen";
    static final String PHASE_COMPILE = "compile";
    static final String PHASE_LINK = "link";
    static final String PHASE_INSTALL = "install";
    static final String PHASE_DONE = "done";
    static final String PHASE_CANCELLED = "cancelled";
    static final String PHASE_ERROR = "error";

    String phase = PHASE_IDLE;
    int done;
    int total;
    String message = "";
    long started;
    long updated;
    String error;

    static BuildStatus idle() {
        return new BuildStatus();
    }

    boolean active() {
        return !PHASE_IDLE.equals(phase) && !PHASE_DONE.equals(phase)
                && !PHASE_CANCELLED.equals(phase) && !PHASE_ERROR.equals(phase);
    }

    int percent() {
        if (PHASE_DONE.equals(phase)) {
            return 100;
        }
        if (total <= 0) {
            return 0;
        }
        return Math.max(0, Math.min(100, (int) (done * 100L / total)));
    }

    String phaseLabel(Context context) {
        switch (phase) {
            case PHASE_TOOLCHAIN:
                return context.getString(R.string.build_phase_toolchain);
            case PHASE_DECRYPT:
                return context.getString(R.string.build_phase_decrypt);
            case PHASE_CODEGEN:
                return context.getString(R.string.build_phase_codegen, done);
            case PHASE_COMPILE:
                return context.getString(R.string.build_phase_compile, done, Math.max(total, done));
            case PHASE_LINK:
                return context.getString(R.string.build_phase_link);
            case PHASE_INSTALL:
                return context.getString(R.string.build_phase_install);
            case PHASE_DONE:
                return context.getString(R.string.build_done);
            case PHASE_CANCELLED:
                return context.getString(R.string.build_cancelled);
            case PHASE_ERROR:
                return context.getString(R.string.build_error, error == null ? "" : error);
            default:
                return "";
        }
    }

    JSONObject toJson() {
        final JSONObject json = new JSONObject();
        try {
            json.put("phase", phase);
            json.put("done", done);
            json.put("total", total);
            json.put("message", message == null ? "" : message);
            json.put("started", started);
            json.put("updated", updated);
            if (error != null) {
                json.put("error", error);
            }
        } catch (Exception ignored) {
            // JSONObject only throws for invalid keys; the keys are constants.
        }
        return json;
    }

    static BuildStatus fromJson(String text) {
        final BuildStatus status = new BuildStatus();
        try {
            final JSONObject json = new JSONObject(text);
            status.phase = json.optString("phase", PHASE_IDLE);
            status.done = json.optInt("done", 0);
            status.total = json.optInt("total", 0);
            status.message = json.optString("message", "");
            status.started = json.optLong("started", 0L);
            status.updated = json.optLong("updated", 0L);
            status.error = json.has("error") ? json.optString("error", null) : null;
        } catch (Exception error) {
            status.phase = PHASE_ERROR;
            status.error = "estado ilegible";
        }
        return status;
    }

    static BuildStatus read(File file) {
        if (!file.isFile() || file.length() == 0 || file.length() > 64 * 1024) {
            return idle();
        }
        try (InputStream in = new FileInputStream(file)) {
            final ByteArrayOutputStream out = new ByteArrayOutputStream();
            final byte[] buffer = new byte[4096];
            int read;
            while ((read = in.read(buffer)) > 0) {
                out.write(buffer, 0, read);
            }
            return fromJson(new String(out.toByteArray(), StandardCharsets.UTF_8));
        } catch (IOException error) {
            return idle();
        }
    }

    /** Writes {@code .tmp}, fsyncs it and renames it, so readers never see half a file. */
    static void write(File file, BuildStatus status) {
        final File parent = file.getParentFile();
        if (parent != null && !parent.isDirectory() && !parent.mkdirs()) {
            return;
        }
        status.updated = System.currentTimeMillis() / 1000L;
        final File temporary = new File(file.getPath() + ".tmp");
        try (FileOutputStream out = new FileOutputStream(temporary)) {
            out.write(status.toJson().toString().getBytes(StandardCharsets.UTF_8));
            out.flush();
            out.getFD().sync();
        } catch (IOException error) {
            temporary.delete();
            return;
        }
        if (!temporary.renameTo(file)) {
            file.delete();
            if (!temporary.renameTo(file)) {
                temporary.delete();
            }
        }
    }

    static void appendLog(File file, String text) {
        final File parent = file.getParentFile();
        if (parent != null && !parent.isDirectory()) {
            parent.mkdirs();
        }
        try (FileOutputStream out = new FileOutputStream(file, true)) {
            out.write(text.getBytes(StandardCharsets.UTF_8));
        } catch (IOException ignored) {
            // Diagnostics only; the pipeline reports failures through its exit codes.
        }
    }

    /** Last bytes of the build log, for the small on-screen tail. */
    static String tail(File file, int maxBytes) {
        if (!file.isFile() || file.length() == 0) {
            return "";
        }
        try (RandomAccessFile input = new RandomAccessFile(file, "r")) {
            final long length = input.length();
            final int size = (int) Math.min(length, Math.max(256, maxBytes));
            final byte[] data = new byte[size];
            input.seek(length - size);
            input.readFully(data);
            return new String(data, StandardCharsets.UTF_8);
        } catch (IOException error) {
            return "";
        }
    }
}
