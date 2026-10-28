package com.edgx73.konrecompiled;

import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.content.pm.PackageInstaller;
import android.widget.Toast;

/**
 * Receives PackageInstaller's result and reports it in Spanish.
 *
 * <p>For STATUS_PENDING_USER_ACTION Android includes the system confirmation
 * intent in EXTRA_INTENT; it must be started from here.
 */
public final class InstallResultReceiver extends BroadcastReceiver {
    public static final String ACTION = "com.edgx73.konrecompiled.INSTALL_RESULT";

    @Override
    public void onReceive(Context context, Intent intent) {
        if (intent == null || !ACTION.equals(intent.getAction())) {
            return;
        }
        final int status = intent.getIntExtra(
                PackageInstaller.EXTRA_STATUS, PackageInstaller.STATUS_FAILURE);
        final String detail = intent.getStringExtra(PackageInstaller.EXTRA_STATUS_MESSAGE);

        switch (status) {
            case PackageInstaller.STATUS_PENDING_USER_ACTION: {
                final Intent confirmation = intent.getParcelableExtra(Intent.EXTRA_INTENT);
                if (confirmation != null) {
                    confirmation.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
                    try {
                        context.startActivity(confirmation);
                    } catch (RuntimeException error) {
                        toast(context, "No se pudo abrir el instalador del sistema.");
                    }
                } else {
                    toast(context, "El instalador del sistema no devolvió el diálogo de confirmación.");
                }
                break;
            }
            case PackageInstaller.STATUS_SUCCESS:
                toast(context, "Actualización instalada. Abre de nuevo K-On! Hokago Live!! Recompiled.");
                break;
            case PackageInstaller.STATUS_FAILURE_ABORTED:
                toast(context, "Instalación cancelada.");
                break;
            case PackageInstaller.STATUS_FAILURE_BLOCKED:
                toast(context, "El sistema bloqueó la instalación. Revisa el permiso para instalar apps desconocidas.");
                break;
            case PackageInstaller.STATUS_FAILURE_CONFLICT:
                toast(context, "El APK entra en conflicto con la app instalada (firma o versión distinta).");
                break;
            case PackageInstaller.STATUS_FAILURE_INCOMPATIBLE:
                toast(context, "La actualización no es compatible con este dispositivo.");
                break;
            case PackageInstaller.STATUS_FAILURE_INVALID:
                toast(context, "El APK de la actualización no es válido.");
                break;
            case PackageInstaller.STATUS_FAILURE_STORAGE:
                toast(context, "No hay espacio suficiente para instalar la actualización.");
                break;
            case PackageInstaller.STATUS_FAILURE:
            default:
                toast(context, "No se pudo instalar la actualización"
                        + (detail == null || detail.isEmpty() ? "." : ": " + detail));
                break;
        }
    }

    private static void toast(Context context, String text) {
        Toast.makeText(context.getApplicationContext(), text, Toast.LENGTH_LONG).show();
    }
}
