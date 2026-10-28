package com.edgx73.konrecompiled;

import android.app.PendingIntent;
import android.content.Context;
import android.content.Intent;
import android.content.pm.PackageInstaller;
import android.os.Build;

import java.io.BufferedInputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.IOException;
import java.io.OutputStream;

/**
 * Installs a verified APK with PackageInstaller in MODE_FULL_INSTALL.
 *
 * <p>The APK stays in cacheDir; this class never deletes filesDir, userdata or
 * config. The result arrives at {@link InstallResultReceiver}, which shows the
 * state in Spanish and launches the system confirmation dialog.
 */
final class UpdateInstaller {
    private UpdateInstaller() {
    }

    static void install(Context context, File apk) throws IOException {
        final PackageInstaller installer = context.getPackageManager().getPackageInstaller();
        final PackageInstaller.SessionParams params =
                new PackageInstaller.SessionParams(PackageInstaller.SessionParams.MODE_FULL_INSTALL);
        params.setAppPackageName(context.getPackageName());
        params.setSize(apk.length());

        final int sessionId = installer.createSession(params);
        try (PackageInstaller.Session session = installer.openSession(sessionId)) {
            try (OutputStream out = session.openWrite("kon-update.apk", 0L, apk.length());
                 BufferedInputStream in =
                         new BufferedInputStream(new FileInputStream(apk))) {
                final byte[] buffer = new byte[1 << 16];
                int read;
                while ((read = in.read(buffer)) > 0) {
                    out.write(buffer, 0, read);
                }
                out.flush();
                session.fsync(out);
            }
            final Intent status = new Intent(context, InstallResultReceiver.class)
                    .setAction(InstallResultReceiver.ACTION);
            final int pendingFlags = PendingIntent.FLAG_UPDATE_CURRENT
                    | (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S ? PendingIntent.FLAG_MUTABLE : 0);
            final PendingIntent callback =
                    PendingIntent.getBroadcast(context, sessionId, status, pendingFlags);
            session.commit(callback.getIntentSender());
        } catch (IOException | RuntimeException error) {
            // Do not leave a half-written session behind.
            try {
                installer.abandonSession(sessionId);
            } catch (RuntimeException ignored) {
                // The session may already be closed or committed.
            }
            throw error;
        }
    }
}
