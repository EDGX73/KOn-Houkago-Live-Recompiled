package com.edgx73.konrecompiled;

import org.json.JSONException;
import org.json.JSONObject;

import java.io.IOException;
import java.util.Locale;

/**
 * manifest-android.json published next to the APK in each GitHub release.
 *
 * <p>tools/make_android_apk.py writes it. The updater trusts it only after the
 * APK itself has been hashed and its signing certificate compared with the
 * installed app.
 */
final class UpdateManifest {
    final int versionCode;
    final String versionName;
    final String applicationId;
    final String sha256;
    final String signature;
    final long size;

    private UpdateManifest(int versionCode, String versionName, String applicationId,
                           String sha256, String signature, long size) {
        this.versionCode = versionCode;
        this.versionName = versionName;
        this.applicationId = applicationId;
        this.sha256 = sha256;
        this.signature = signature;
        this.size = size;
    }

    static UpdateManifest parse(String text) throws JSONException, IOException {
        final JSONObject object = new JSONObject(text);
        final int versionCode = object.optInt("versionCode", -1);
        final String versionName = object.optString("versionName", "").trim();
        final String applicationId = object.optString("applicationId", "").trim();
        final String sha256 = object.optString("sha256", "").trim().toLowerCase(Locale.ROOT);
        final String signature = object.optString("signature", "").trim().toLowerCase(Locale.ROOT);
        final long size = object.optLong("size", -1L);

        if (versionCode <= 0) {
            throw new IOException("el manifiesto no declara versionCode");
        }
        if (versionName.isEmpty()) {
            throw new IOException("el manifiesto no declara versionName");
        }
        if (applicationId.isEmpty()) {
            throw new IOException("el manifiesto no declara applicationId");
        }
        if (!sha256.matches("[0-9a-f]{64}")) {
            throw new IOException("el manifiesto no declara un SHA-256 válido");
        }
        if (!signature.isEmpty() && !signature.matches("[0-9a-f]{64}")) {
            throw new IOException("el manifiesto no declara una firma válida");
        }
        if (size <= 0) {
            throw new IOException("el manifiesto no declara el tamaño del APK");
        }
        return new UpdateManifest(versionCode, versionName, applicationId, sha256, signature, size);
    }
}
