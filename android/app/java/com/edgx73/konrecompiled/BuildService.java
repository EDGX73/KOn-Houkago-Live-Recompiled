package com.edgx73.konrecompiled;

import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.PendingIntent;
import android.app.Service;
import android.content.Context;
import android.content.Intent;
import android.content.pm.ServiceInfo;
import android.os.Build;
import android.os.IBinder;
import android.util.Log;

/**
 * Foreground service that keeps the on-device build alive while the screen is
 * off or the Activity is in the background, and offers Cancel from the
 * notification. The Activity polls {@code status.json}; this service only
 * mirrors the same state in the notification.
 */
public final class BuildService extends Service {
    private static final String TAG = "kon-build";
    private static final String CHANNEL = "kon_prepare";
    private static final int NOTIFICATION_ID = 4102;

    static final String ACTION_PREPARE = "com.edgx73.konrecompiled.action.PREPARE";
    static final String ACTION_CANCEL = "com.edgx73.konrecompiled.action.CANCEL_PREPARE";

    private volatile GameBuilder builder;
    private Thread worker;
    private static final java.util.concurrent.atomic.AtomicBoolean RUNNING =
            new java.util.concurrent.atomic.AtomicBoolean();

    /** True while this process is actually running a build (not merely resuming a stale status). */
    static boolean isRunning() {
        return RUNNING.get();
    }

    static Intent prepareIntent(Context context) {
        return new Intent(context, BuildService.class).setAction(ACTION_PREPARE);
    }

    static Intent cancelIntent(Context context) {
        return new Intent(context, BuildService.class).setAction(ACTION_CANCEL);
    }

    @Override
    public void onCreate() {
        super.onCreate();
        final NotificationManager manager = getSystemService(NotificationManager.class);
        if (manager != null) {
            final NotificationChannel channel = new NotificationChannel(CHANNEL,
                    getString(R.string.build_notification_channel),
                    NotificationManager.IMPORTANCE_LOW);
            channel.setDescription(getString(R.string.build_notification_channel_description));
            manager.createNotificationChannel(channel);
        }
    }

    @Override
    public int onStartCommand(Intent intent, int flags, int startId) {
        final String action = intent == null ? ACTION_PREPARE : intent.getAction();
        if (ACTION_CANCEL.equals(action)) {
            final GameBuilder current = builder;
            if (current != null) {
                current.cancel();
            }
            return START_NOT_STICKY;
        }
        startInForeground(startNotification(getString(R.string.build_phase_toolchain), 0));
        if (worker != null && worker.isAlive()) {
            return START_NOT_STICKY;
        }
        worker = new Thread(this::prepare, "kon-prepare");
        worker.start();
        return START_NOT_STICKY;
    }

    private void prepare() {
        RUNNING.set(true);
        builder = new GameBuilder(this);
        try {
            builder.run(this::publish);
        } catch (Throwable error) {
            Log.w(TAG, "prepare service failed", error);
        } finally {
            builder = null;
            RUNNING.set(false);
            stopForeground(STOP_FOREGROUND_REMOVE);
            stopSelf();
        }
    }

    private void publish(BuildStatus status) {
        final NotificationManager manager = getSystemService(NotificationManager.class);
        if (manager == null) {
            return;
        }
        final String text = status.phaseLabel(this);
        manager.notify(NOTIFICATION_ID, startNotification(text, status.percent()));
    }

    private void startInForeground(Notification notification) {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
            startForeground(NOTIFICATION_ID, notification, ServiceInfo.FOREGROUND_SERVICE_TYPE_DATA_SYNC);
        } else {
            startForeground(NOTIFICATION_ID, notification);
        }
    }

    private Notification startNotification(String text, int percent) {
        final Intent open = new Intent(this, MainActivity.class);
        open.addFlags(Intent.FLAG_ACTIVITY_SINGLE_TOP);
        final PendingIntent content = PendingIntent.getActivity(this, 0, open,
                PendingIntent.FLAG_UPDATE_CURRENT | PendingIntent.FLAG_IMMUTABLE);
        final PendingIntent cancel = PendingIntent.getService(this, 1, cancelIntent(this),
                PendingIntent.FLAG_UPDATE_CURRENT | PendingIntent.FLAG_IMMUTABLE);
        final Notification.Builder builder = new Notification.Builder(this, CHANNEL)
                .setSmallIcon(android.R.drawable.stat_sys_download)
                .setContentTitle(getString(R.string.build_notification_title))
                .setContentText(text)
                .setContentIntent(content)
                .setOngoing(true)
                .setOnlyAlertOnce(true)
                .setProgress(100, percent, percent <= 0)
                .addAction(android.R.drawable.ic_menu_close_clear_cancel,
                        getString(R.string.button_cancel_build), cancel);
        return builder.build();
    }

    @Override
    public void onDestroy() {
        if (worker != null) {
            worker.interrupt();
        }
        super.onDestroy();
    }

    @Override
    public IBinder onBind(Intent intent) {
        return null;
    }
}
