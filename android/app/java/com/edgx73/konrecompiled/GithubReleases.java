package com.edgx73.konrecompiled;

import android.content.Context;
import android.content.pm.PackageInfo;
import android.content.pm.PackageManager;
import android.content.pm.Signature;
import android.content.pm.SigningInfo;
import android.os.Build;

import org.json.JSONArray;
import org.json.JSONException;
import org.json.JSONObject;

import java.io.BufferedInputStream;
import java.io.BufferedOutputStream;
import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.net.HttpURLConnection;
import java.net.URI;
import java.nio.charset.StandardCharsets;
import java.security.MessageDigest;
import java.security.NoSuchAlgorithmException;
import java.util.Locale;

/**
 * GitHub release lookup and APK download for the public app.
 *
 * <p>Only stable releases (draft=false, prerelease=false) that publish both a
 * {@code *-android.apk} asset and {@code manifest-android.json} are considered.
 * All network work runs on a caller-provided thread; this class never touches
 * the UI thread and never deletes filesDir.
 */
final class GithubReleases {
    /** Same repository as KON_GITHUB_REPO in src/kon_version.h (checked by the build). */
    static final String API =
            "https://api.github.com/repos/EDGX73/KOn-Houkago-Live-Recompiled/releases?per_page=20";

    private static final int CONNECT_TIMEOUT_MS = 20_000;
    private static final int READ_TIMEOUT_MS = 60_000;
    private static final int MAX_RELEASES_JSON = 8 * 1024 * 1024;
    private static final int MAX_MANIFEST_JSON = 1024 * 1024;
    private static final long MAX_APK_BYTES = 256L * 1024 * 1024;

    interface Progress {
        void onProgress(int percent);
    }

    static final class Release {
        final String tag;
        final String apkUrl;
        final String apkName;
        final String manifestUrl;

        Release(String tag, String apkUrl, String apkName, String manifestUrl) {
            this.tag = tag;
            this.apkUrl = apkUrl;
            this.apkName = apkName;
            this.manifestUrl = manifestUrl;
        }
    }

    private GithubReleases() {
    }

    /** Newest stable release that has both Android assets, or null. */
    static Release findLatest() throws IOException, JSONException {
        final String body = httpText(API, MAX_RELEASES_JSON);
        final JSONArray releases = new JSONArray(body);
        for (int i = 0; i < releases.length(); i++) {
            final JSONObject release = releases.optJSONObject(i);
            if (release == null) {
                continue;
            }
            if (release.optBoolean("draft", false) || release.optBoolean("prerelease", false)) {
                continue;
            }
            final String tag = release.optString("tag_name", "");
            final JSONArray assets = release.optJSONArray("assets");
            if (assets == null) {
                continue;
            }
            String apkUrl = null;
            String apkName = null;
            String manifestUrl = null;
            for (int a = 0; a < assets.length(); a++) {
                final JSONObject asset = assets.optJSONObject(a);
                if (asset == null) {
                    continue;
                }
                final String name = asset.optString("name", "");
                final String url = asset.optString("browser_download_url", "");
                if (url.isEmpty()) {
                    continue;
                }
                if (name.endsWith("-android.apk") && apkUrl == null) {
                    apkUrl = url;
                    apkName = name;
                } else if (name.equals("manifest-android.json")) {
                    manifestUrl = url;
                }
            }
            if (apkUrl != null && manifestUrl != null) {
                return new Release(tag, apkUrl, apkName, manifestUrl);
            }
        }
        return null;
    }

    static UpdateManifest fetchManifest(String url) throws IOException, JSONException {
        return UpdateManifest.parse(httpText(url, MAX_MANIFEST_JSON));
    }

    /**
     * Downloads the APK into cacheDir and verifies its size and SHA-256 against
     * the manifest. The package name, versionCode and signing certificate are
     * verified afterwards by {@link #verifyApk}.
     */
    static File downloadApk(Context context, Release release, UpdateManifest manifest,
                            Progress progress) throws IOException {
        final File target = new File(context.getCacheDir(),
                "kon-update-" + manifest.versionCode + ".apk");
        final File part = new File(context.getCacheDir(),
                "kon-update-" + manifest.versionCode + ".apk.part");
        if (part.exists() && !part.delete()) {
            throw new IOException("no se pudo limpiar la descarga anterior");
        }
        if (target.exists() && !target.delete()) {
            throw new IOException("no se pudo reemplazar el APK anterior en la caché");
        }
        if (manifest.size > 0 && context.getCacheDir().getUsableSpace() < manifest.size + (8L << 20)) {
            throw new IOException("no hay espacio suficiente en la caché para el APK");
        }

        final HttpURLConnection connection = open(release.apkUrl);
        try {
            final int code = connection.getResponseCode();
            if (code != HttpURLConnection.HTTP_OK) {
                throw new IOException(describeHttp(code));
            }
            final long expected = connection.getContentLengthLong();
            if (expected > 0 && expected != manifest.size) {
                throw new IOException("el tamaño del APK no coincide con el manifiesto");
            }
            final MessageDigest digest = sha256();
            long done = 0L;
            int lastPercent = -1;
            try (InputStream in = new BufferedInputStream(connection.getInputStream());
                 OutputStream out = new BufferedOutputStream(new FileOutputStream(part))) {
                final byte[] buffer = new byte[1 << 16];
                int read;
                while ((read = in.read(buffer)) > 0) {
                    out.write(buffer, 0, read);
                    digest.update(buffer, 0, read);
                    done += read;
                    if (done > MAX_APK_BYTES) {
                        throw new IOException("el APK descargado es demasiado grande");
                    }
                    if (manifest.size > 0) {
                        final int percent = (int) Math.min(100L, done * 100L / manifest.size);
                        if (percent != lastPercent) {
                            lastPercent = percent;
                            progress.onProgress(percent);
                        }
                    }
                }
            }
            if (done != manifest.size) {
                throw new IOException("la descarga está incompleta");
            }
            final String actual = toHex(digest.digest());
            if (!actual.equalsIgnoreCase(manifest.sha256)) {
                throw new IOException("el SHA-256 del APK no coincide con el manifiesto");
            }
            if (!part.renameTo(target)) {
                throw new IOException("no se pudo mover el APK descargado");
            }
            return target;
        } catch (IOException | RuntimeException error) {
            // A partially written file must never survive a failed check.
            //noinspection ResultOfMethodCallIgnored
            part.delete();
            throw error;
        } finally {
            connection.disconnect();
        }
    }

    /**
     * Checks the archive with PackageManager: package name, versionCode and the
     * signing certificate (GET_SIGNING_CERTIFICATES on API 28+, GET_SIGNATURES
     * on 26-27). The certificate must match the installed app, so a different
     * key can never replace this installation.
     */
    static void verifyApk(Context context, File apk, UpdateManifest manifest) throws IOException {
        final PackageManager manager = context.getPackageManager();
        final int flags = Build.VERSION.SDK_INT >= Build.VERSION_CODES.P
                ? PackageManager.GET_SIGNING_CERTIFICATES
                : PackageManager.GET_SIGNATURES;
        final PackageInfo archive = manager.getPackageArchiveInfo(apk.getAbsolutePath(), flags);
        if (archive == null) {
            throw new IOException("Android no pudo leer el APK descargado");
        }
        if (!context.getPackageName().equals(archive.packageName)) {
            throw new IOException("el APK pertenece a otra aplicación");
        }
        final long archiveVersion = Build.VERSION.SDK_INT >= Build.VERSION_CODES.P
                ? archive.getLongVersionCode()
                : archive.versionCode;
        if (archiveVersion != manifest.versionCode) {
            throw new IOException("el versionCode del APK no coincide con el manifiesto");
        }
        final String archiveSigner = firstSignerSha256(archive);
        final PackageInfo installed;
        try {
            installed = manager.getPackageInfo(context.getPackageName(), flags);
        } catch (PackageManager.NameNotFoundException error) {
            throw new IOException("no se pudo leer la firma de la app instalada", error);
        }
        final String installedSigner = firstSignerSha256(installed);
        if (!archiveSigner.equals(installedSigner)) {
            throw new IOException("la firma del APK no coincide con la de esta instalación");
        }
        if (!manifest.signature.isEmpty() && !manifest.signature.equalsIgnoreCase(archiveSigner)) {
            throw new IOException("la firma del APK no coincide con el manifiesto");
        }
    }

    private static String firstSignerSha256(PackageInfo info) throws IOException {
        final Signature[] signatures;
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.P) {
            final SigningInfo signing = info.signingInfo;
            if (signing == null) {
                throw new IOException("el APK no declara firma");
            }
            signatures = signing.hasMultipleSigners()
                    ? signing.getApkContentsSigners()
                    : signing.getSigningCertificateHistory();
        } else {
            signatures = info.signatures;
        }
        if (signatures == null || signatures.length == 0) {
            throw new IOException("el APK no declara firma");
        }
        return toHex(sha256().digest(signatures[0].toByteArray()));
    }

    private static String httpText(String url, int maxBytes) throws IOException {
        final HttpURLConnection connection = open(url);
        try {
            final int code = connection.getResponseCode();
            if (code != HttpURLConnection.HTTP_OK) {
                throw new IOException(describeHttp(code));
            }
            try (InputStream in = connection.getInputStream()) {
                return readAll(in, maxBytes);
            }
        } finally {
            connection.disconnect();
        }
    }

    private static HttpURLConnection open(String url) throws IOException {
        final HttpURLConnection connection =
                (HttpURLConnection) URI.create(url).toURL().openConnection();
        connection.setConnectTimeout(CONNECT_TIMEOUT_MS);
        connection.setReadTimeout(READ_TIMEOUT_MS);
        connection.setInstanceFollowRedirects(true);
        connection.setRequestProperty("Accept", "application/vnd.github+json, application/octet-stream");
        connection.setRequestProperty("User-Agent", "KOn-Houkago-Live-Recompiled-Android");
        return connection;
    }

    private static String readAll(InputStream in, int maxBytes) throws IOException {
        final ByteArrayOutputStream out = new ByteArrayOutputStream();
        final byte[] buffer = new byte[1 << 16];
        int read;
        int total = 0;
        while ((read = in.read(buffer)) > 0) {
            total += read;
            if (total > maxBytes) {
                throw new IOException("la respuesta de GitHub es demasiado grande");
            }
            out.write(buffer, 0, read);
        }
        return new String(out.toByteArray(), StandardCharsets.UTF_8);
    }

    private static String describeHttp(int code) {
        if (code == 403 || code == 429) {
            return "GitHub ha limitado las consultas (HTTP " + code + "). Inténtalo más tarde";
        }
        if (code == 404) {
            return "no se encontró el repositorio de actualizaciones (HTTP 404)";
        }
        return "GitHub respondió HTTP " + code;
    }

    private static MessageDigest sha256() {
        try {
            return MessageDigest.getInstance("SHA-256");
        } catch (NoSuchAlgorithmException error) {
            throw new IllegalStateException("SHA-256 no disponible", error);
        }
    }

    private static String toHex(byte[] bytes) {
        final StringBuilder out = new StringBuilder(bytes.length * 2);
        for (byte value : bytes) {
            out.append(String.format(Locale.ROOT, "%02x", value & 0xFF));
        }
        return out.toString();
    }
}
