package com.edgx73.konrecompiled;

import android.content.Context;
import android.net.Uri;

import java.io.BufferedInputStream;
import java.io.BufferedOutputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.text.SimpleDateFormat;
import java.util.Date;
import java.util.Locale;
import java.util.zip.Deflater;
import java.util.zip.ZipEntry;
import java.util.zip.ZipInputStream;
import java.util.zip.ZipOutputStream;

/**
 * Export/import of the player's data through the Storage Access Framework.
 *
 * <p>The ZIP contains only {@code filesDir/userdata} and {@code filesDir/config}.
 * Importing replaces those two trees (that is what the confirmation dialog
 * warns about) after writing a safety copy to cacheDir. filesDir itself and any
 * other files in it are never deleted.
 */
final class SaveArchive {
    private static final String[] ROOTS = {"userdata", "config"};
    private static final long MAX_IMPORT_BYTES = 512L * 1024 * 1024;
    private static final int MAX_ENTRIES = 50_000;

    static final class ImportResult {
        final int files;
        final File backup;

        ImportResult(int files, File backup) {
            this.files = files;
            this.backup = backup;
        }
    }

    private SaveArchive() {
    }

    /** Writes the ZIP to the document the user chose; returns the file count. */
    static int exportTo(Context context, Uri uri) throws IOException {
        try (OutputStream raw = context.getContentResolver().openOutputStream(uri, "w")) {
            if (raw == null) {
                throw new IOException("no se pudo abrir el archivo de destino");
            }
            return zipRoots(context, raw);
        }
    }

    /**
     * Replaces userdata/config with the ZIP contents. The document is read twice:
     * first to validate every entry, then to extract after the old data is gone.
     */
    static ImportResult importFrom(Context context, Uri uri) throws IOException {
        int entries = 0;
        try (InputStream raw = context.getContentResolver().openInputStream(uri)) {
            if (raw == null) {
                throw new IOException("no se pudo abrir el archivo elegido");
            }
            try (ZipInputStream zip = new ZipInputStream(new BufferedInputStream(raw))) {
                ZipEntry entry;
                while ((entry = zip.getNextEntry()) != null) {
                    if (++entries > MAX_ENTRIES) {
                        throw new IOException("el ZIP tiene demasiadas entradas");
                    }
                    validateEntry(entry.getName());
                    zip.closeEntry();
                }
            }
        }
        if (entries == 0) {
            throw new IOException("el archivo no es un ZIP válido o está vacío");
        }

        final String stamp = new SimpleDateFormat("yyyyMMdd-HHmmss", Locale.US).format(new Date());
        final File backup = new File(context.getCacheDir(), "kon-backup-" + stamp + ".zip");
        try (OutputStream out = new BufferedOutputStream(new FileOutputStream(backup))) {
            zipRoots(context, out);
        }

        for (String root : ROOTS) {
            deleteContents(new File(context.getFilesDir(), root));
        }

        int files = 0;
        long total = 0L;
        try (InputStream raw = context.getContentResolver().openInputStream(uri)) {
            if (raw == null) {
                throw new IOException("no se pudo volver a abrir el archivo elegido");
            }
            try (ZipInputStream zip = new ZipInputStream(new BufferedInputStream(raw))) {
                ZipEntry entry;
                while ((entry = zip.getNextEntry()) != null) {
                    final File target = new File(context.getFilesDir(), entry.getName());
                    final String basePath = context.getFilesDir().getCanonicalPath() + File.separator;
                    if (!target.getCanonicalPath().startsWith(basePath)) {
                        throw new IOException("el ZIP intenta escribir fuera del almacenamiento de la app");
                    }
                    if (entry.isDirectory()) {
                        if (!target.isDirectory() && !target.mkdirs()) {
                            throw new IOException("no se pudo crear " + entry.getName());
                        }
                    } else {
                        final File parent = target.getParentFile();
                        if (parent != null && !parent.isDirectory() && !parent.mkdirs()) {
                            throw new IOException("no se pudo crear la carpeta de " + entry.getName());
                        }
                        try (OutputStream out = new BufferedOutputStream(new FileOutputStream(target))) {
                            final byte[] buffer = new byte[1 << 16];
                            int read;
                            while ((read = zip.read(buffer)) > 0) {
                                total += read;
                                if (total > MAX_IMPORT_BYTES) {
                                    throw new IOException("el ZIP supera el tamaño máximo permitido");
                                }
                                out.write(buffer, 0, read);
                            }
                        }
                        files++;
                    }
                    zip.closeEntry();
                }
            }
        } catch (IOException error) {
            throw new IOException("la importación falló; copia de seguridad en "
                    + backup.getAbsolutePath() + ": " + error.getMessage(), error);
        }
        return new ImportResult(files, backup);
    }

    private static int zipRoots(Context context, OutputStream raw) throws IOException {
        int files = 0;
        try (ZipOutputStream zip = new ZipOutputStream(new BufferedOutputStream(raw))) {
            zip.setLevel(Deflater.DEFAULT_COMPRESSION);
            for (String root : ROOTS) {
                files += addTree(zip, new File(context.getFilesDir(), root), root);
            }
            zip.finish();
        }
        return files;
    }

    private static int addTree(ZipOutputStream zip, File file, String entryName) throws IOException {
        if (!file.exists()) {
            return 0;
        }
        if (file.isDirectory()) {
            final File[] children = file.listFiles();
            int files = 0;
            if (children != null) {
                for (File child : children) {
                    files += addTree(zip, child, entryName + "/" + child.getName());
                }
            }
            return files;
        }
        if (!file.isFile()) {
            return 0;
        }
        zip.putNextEntry(new ZipEntry(entryName));
        try (InputStream in = new BufferedInputStream(new FileInputStream(file))) {
            final byte[] buffer = new byte[1 << 16];
            int read;
            while ((read = in.read(buffer)) > 0) {
                zip.write(buffer, 0, read);
            }
        }
        zip.closeEntry();
        return 1;
    }

    private static void validateEntry(String name) throws IOException {
        if (name == null || name.isEmpty()) {
            throw new IOException("el ZIP contiene una entrada sin nombre");
        }
        final String normalized = name.replace('\\', '/');
        if (normalized.startsWith("/")) {
            throw new IOException("el ZIP contiene una ruta absoluta");
        }
        final String[] parts = normalized.split("/");
        if (parts.length == 0 || !("userdata".equals(parts[0]) || "config".equals(parts[0]))) {
            throw new IOException("el ZIP contiene archivos fuera de userdata/ o config/");
        }
        for (String part : parts) {
            if ("..".equals(part) || ".".equals(part)) {
                throw new IOException("el ZIP contiene una ruta no permitida");
            }
        }
        if (normalized.indexOf('\0') >= 0) {
            throw new IOException("el ZIP contiene un nombre no válido");
        }
    }

    /** Deletes the children of userdata or config, never the root or filesDir. */
    private static void deleteContents(File directory) throws IOException {
        if (!directory.exists()) {
            return;
        }
        final File[] children = directory.listFiles();
        if (children == null) {
            return;
        }
        for (File child : children) {
            deleteRecursively(child);
        }
    }

    private static void deleteRecursively(File file) throws IOException {
        if (file.isDirectory()) {
            final File[] children = file.listFiles();
            if (children != null) {
                for (File child : children) {
                    deleteRecursively(child);
                }
            }
        }
        if (!file.delete() && file.exists()) {
            throw new IOException("no se pudo borrar " + file.getName());
        }
    }
}
