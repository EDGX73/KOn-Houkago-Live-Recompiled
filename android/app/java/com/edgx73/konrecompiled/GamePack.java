package com.edgx73.konrecompiled;

import android.content.Context;
import android.net.Uri;

import java.io.BufferedInputStream;
import java.io.BufferedOutputStream;
import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.util.zip.ZipEntry;
import java.util.zip.ZipInputStream;

/**
 * Imports the private game pack (libkon_game.so + pkon.elf) generated on the
 * user's PC from their own ISO. The APK never contains this module.
 */
final class GamePack {
    private static final long MAX_MODULE = 128L * 1024L * 1024L;
    private static final long MAX_ELF = 16L * 1024L * 1024L;

    private GamePack() {
    }

    static int importFrom(Context context, Uri uri, File root) throws IOException {
        final File modules = new File(root, "modules");
        final File executables = new File(root, "analysis/executables");
        if (!modules.isDirectory() && !modules.mkdirs()) {
            throw new IOException("no se pudo crear la carpeta de módulos");
        }
        if (!executables.isDirectory() && !executables.mkdirs()) {
            throw new IOException("no se pudo crear la carpeta del ejecutable");
        }
        final File modulePart = new File(modules, "libkon_game.so.part");
        final File elfPart = new File(executables, "pkon.elf.part");
        boolean moduleSeen = false;
        boolean elfSeen = false;
        int files = 0;
        try (InputStream raw = context.getContentResolver().openInputStream(uri);
             ZipInputStream zip = new ZipInputStream(new BufferedInputStream(raw))) {
            if (raw == null) {
                throw new IOException("no se pudo abrir el paquete");
            }
            ZipEntry entry;
            while ((entry = zip.getNextEntry()) != null) {
                if (entry.isDirectory()) {
                    continue;
                }
                final String name = new File(entry.getName()).getName();
                if ("libkon_game.so".equals(name)) {
                    copy(zip, modulePart, MAX_MODULE);
                    moduleSeen = true;
                    files++;
                } else if ("pkon.elf".equals(name)) {
                    copy(zip, elfPart, MAX_ELF);
                    elfSeen = true;
                    files++;
                }
            }
        } catch (IOException error) {
            modulePart.delete();
            elfPart.delete();
            throw error;
        }
        if (!moduleSeen || !elfSeen) {
            modulePart.delete();
            elfPart.delete();
            throw new IOException("el paquete debe contener libkon_game.so y pkon.elf");
        }
        if (!isElf(modulePart)) {
            modulePart.delete();
            elfPart.delete();
            throw new IOException("libkon_game.so no es un ELF válido");
        }
        if (!isElf(elfPart)) {
            modulePart.delete();
            elfPart.delete();
            throw new IOException("pkon.elf no es un ELF válido");
        }
        final File module = new File(modules, "libkon_game.so");
        final File elf = new File(executables, "pkon.elf");
        replace(modulePart, module);
        replace(elfPart, elf);
        module.setReadOnly();
        elf.setReadOnly();
        return files;
    }

    private static void copy(ZipInputStream in, File out, long limit) throws IOException {
        long total = 0L;
        try (BufferedOutputStream stream = new BufferedOutputStream(new FileOutputStream(out))) {
            final byte[] buffer = new byte[1 << 20];
            int read;
            while ((read = in.read(buffer)) > 0) {
                total += read;
                if (total > limit) {
                    throw new IOException("entrada del paquete demasiado grande");
                }
                stream.write(buffer, 0, read);
            }
        }
    }

    private static boolean isElf(File file) throws IOException {
        if (!file.isFile() || file.length() < 1024L * 1024L) {
            return false;
        }
        try (InputStream in = new java.io.FileInputStream(file)) {
            return in.read() == 0x7f && in.read() == 'E' && in.read() == 'L' && in.read() == 'F';
        }
    }

    private static void replace(File source, File target) throws IOException {
        if (target.exists() && !target.delete()) {
            throw new IOException("no se pudo reemplazar " + target.getName());
        }
        if (!source.renameTo(target)) {
            throw new IOException("no se pudo instalar " + target.getName());
        }
    }
}
