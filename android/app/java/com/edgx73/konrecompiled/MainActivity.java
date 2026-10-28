package com.edgx73.konrecompiled;

import android.Manifest;
import android.app.Activity;
import android.app.AlertDialog;
import android.content.ActivityNotFoundException;
import android.content.Intent;
import android.content.pm.PackageInfo;
import android.content.pm.PackageManager;
import android.graphics.Typeface;
import android.graphics.drawable.GradientDrawable;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.os.ParcelFileDescriptor;
import android.provider.Settings;
import android.util.TypedValue;
import android.view.View;
import android.view.ViewGroup;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.ProgressBar;
import android.widget.ScrollView;
import android.widget.TextView;

import org.json.JSONObject;

import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.security.MessageDigest;
import java.text.SimpleDateFormat;
import java.util.ArrayList;
import java.util.Date;
import java.util.List;
import java.util.Locale;

/**
 * Public 0.2 base of K-On! Houkago Live!! Recompiled for Android.
 *
 * <p>It contains no game data: only the updater (GitHub releases, SHA-256 and
 * signature checks, PackageInstaller), SAF export/import of userdata/config,
 * and the native PSPRecomp decoder diagnostic.
 */
public final class MainActivity extends Activity {
    private static final int REQUEST_EXPORT = 1001;
    private static final int REQUEST_IMPORT = 1002;
    private static final int REQUEST_ISO = 1003;
    private static final int REQUEST_GAME_PACK = 1004;
    private static final int REQUEST_NOTIFICATIONS = 1005;
    private static final int COLOR_PRIMARY = 0xFF880E4F;
    private static final int COLOR_TEXT = 0xFF2B2B2B;
    private static final int COLOR_MUTED = 0xFF666666;
    private static final int COLOR_OK = 0xFF1B5E20;
    private static final int COLOR_FAIL = 0xFFB00020;

    private static final boolean gameLibraryLoaded;
    static {
        boolean loaded = false;
        try {
            System.loadLibrary("kon_runtime");
            loaded = true;
        } catch (UnsatisfiedLinkError error) {
            loaded = false;
        }
        gameLibraryLoaded = loaded;
    }

    private final List<Button> actionButtons = new ArrayList<>();

    private TextView statusView;
    private TextView diagnosticsView;
    private TextView playStatusView;
    private ProgressBar progressBar;
    private Button playButton;
    private Button installIsoButton;
    private Button importGameButton;
    private Button prepareButton;
    private Button cancelBuildButton;
    private ProgressBar buildProgress;
    private TextView buildPhaseView;
    private TextView buildLogView;
    private Thread buildPoller;
    private volatile boolean pollingBuild;
    private volatile boolean buildRunning;
    private String elfHash;
    private long elfHashStamp = -1L;
    private File gameRoot;
    private boolean busy;
    private int installedVersionCode;
    private String installedVersionName;

    private native String nativeInstallIso(int fd, String root);

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        readInstalledVersion();
        gameRoot = new File(getFilesDir(), "root");
        prepareGameRoot();
        setContentView(buildUi());
        handleIntentAction(getIntent());
    }

    @Override
    protected void onNewIntent(Intent intent) {
        super.onNewIntent(intent);
        setIntent(intent);
        handleIntentAction(intent);
    }

    @Override
    protected void onResume() {
        super.onResume();
        refreshBuildState();
        if (buildRunning) {
            startBuildPolling();
        }
        refreshPlayState();
    }

    @Override
    protected void onPause() {
        pollingBuild = false;
        super.onPause();
    }

    /** Allows the emulator/CI to trigger an action with ``--es action diagnostics|update``. */
    private void handleIntentAction(Intent intent) {
        final String action = intent == null ? null : intent.getStringExtra("action");
        if ("diagnostics".equals(action)) {
            diagnosticsView.post(this::runDiagnostics);
        } else if ("update".equals(action)) {
            statusView.post(this::checkForUpdates);
        }
    }

    private void readInstalledVersion() {
        try {
            final PackageInfo info = getPackageManager().getPackageInfo(getPackageName(), 0);
            installedVersionName = info.versionName == null ? "?" : info.versionName;
            installedVersionCode = Build.VERSION.SDK_INT >= Build.VERSION_CODES.P
                    ? (int) info.getLongVersionCode()
                    : info.versionCode;
        } catch (PackageManager.NameNotFoundException error) {
            installedVersionName = "?";
            installedVersionCode = 0;
        }
    }

    private View buildUi() {
        final int pad = dp(20);
        final ScrollView scroll = new ScrollView(this);
        scroll.setFillViewport(true);

        final LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        root.setPadding(pad, pad, pad, pad);
        root.setBackgroundColor(0xFFFFF7FA);
        scroll.addView(root);

        root.setOnApplyWindowInsetsListener((view, insets) -> {
            view.setPadding(pad, pad + insets.getSystemWindowInsetTop(),
                    pad, pad + insets.getSystemWindowInsetBottom());
            return insets;
        });

        final TextView title = new TextView(this);
        title.setText(R.string.app_name);
        title.setTextSize(TypedValue.COMPLEX_UNIT_SP, 25);
        title.setTypeface(Typeface.DEFAULT_BOLD);
        title.setTextColor(COLOR_PRIMARY);
        root.addView(title);

        addText(root, getString(R.string.header_subtitle), 14, COLOR_MUTED, false);
        final String abi = Build.SUPPORTED_ABIS.length == 0 ? "?" : Build.SUPPORTED_ABIS[0];
        addText(root, getString(R.string.version_format, installedVersionName,
                installedVersionCode, Build.VERSION.RELEASE, Build.VERSION.SDK_INT, abi),
                13, COLOR_MUTED, false);

        addSection(root, R.string.section_play);
        addText(root, getString(R.string.play_description), 14, COLOR_TEXT, false);
        installIsoButton = addButton(root, R.string.button_install_iso, view -> pickIso());
        importGameButton = addButton(root, R.string.button_import_game, view -> pickGamePack());
        playButton = addButton(root, R.string.button_play, view -> startGame());
        prepareButton = addButton(root, R.string.button_prepare_game, view -> startPrepare());
        actionButtons.add(installIsoButton);
        actionButtons.add(importGameButton);
        actionButtons.add(playButton);
        actionButtons.add(prepareButton);

        buildProgress = new ProgressBar(this, null, android.R.attr.progressBarStyleHorizontal);
        buildProgress.setIndeterminate(false);
        buildProgress.setMax(100);
        buildProgress.setVisibility(View.GONE);
        final LinearLayout.LayoutParams buildProgressParams = new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT);
        buildProgressParams.topMargin = dp(10);
        root.addView(buildProgress, buildProgressParams);

        buildPhaseView = addText(root, "", 13, COLOR_TEXT, false);
        buildPhaseView.setVisibility(View.GONE);
        buildLogView = addText(root, "", 11, COLOR_MUTED, false);
        buildLogView.setTypeface(Typeface.MONOSPACE);
        buildLogView.setTextIsSelectable(true);
        buildLogView.setVisibility(View.GONE);

        cancelBuildButton = addButton(root, R.string.button_cancel_build, view -> cancelPrepare());
        cancelBuildButton.setVisibility(View.GONE);

        playStatusView = addText(root, "", 13, COLOR_MUTED, false);
        playStatusView.setPadding(dp(12), dp(10), dp(12), dp(10));
        playStatusView.setBackground(rounded(0xFFFFE4EE, dp(10)));
        refreshPlayState();

        addSection(root, R.string.section_updates);
        addText(root, getString(R.string.updates_description), 14, COLOR_TEXT, false);
        actionButtons.add(addButton(root, R.string.button_check_updates, view -> checkForUpdates()));

        addSection(root, R.string.section_saves);
        addText(root, getString(R.string.saves_description), 14, COLOR_TEXT, false);
        actionButtons.add(addButton(root, R.string.button_export, view -> startExport()));
        actionButtons.add(addButton(root, R.string.button_import, view -> startImportPicker()));

        addSection(root, R.string.section_diagnostics);
        addText(root, getString(R.string.diagnostics_description), 14, COLOR_TEXT, false);
        actionButtons.add(addButton(root, R.string.button_diagnostics, view -> runDiagnostics()));

        progressBar = new ProgressBar(this, null, android.R.attr.progressBarStyleHorizontal);
        progressBar.setIndeterminate(true);
        progressBar.setVisibility(View.GONE);
        final LinearLayout.LayoutParams progressParams =
                new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT,
                        ViewGroup.LayoutParams.WRAP_CONTENT);
        progressParams.topMargin = dp(12);
        root.addView(progressBar, progressParams);

        statusView = addText(root, getString(R.string.status_idle), 14, COLOR_TEXT, false);
        statusView.setPadding(dp(12), dp(12), dp(12), dp(12));
        statusView.setBackground(rounded(0xFFFFE4EE, dp(10)));

        diagnosticsView = addText(root, "", 12, COLOR_TEXT, false);
        diagnosticsView.setTypeface(Typeface.MONOSPACE);
        diagnosticsView.setTextIsSelectable(true);
        diagnosticsView.setPadding(dp(12), dp(12), dp(12), dp(12));
        diagnosticsView.setBackground(rounded(0xFFF1F1F1, dp(10)));

        addText(root, getString(R.string.footer_no_game_data), 12, COLOR_MUTED, false);
        return scroll;
    }

    private TextView addText(LinearLayout parent, String text, float sizeSp, int color, boolean bold) {
        final TextView view = new TextView(this);
        view.setText(text);
        view.setTextSize(TypedValue.COMPLEX_UNIT_SP, sizeSp);
        view.setTextColor(color);
        if (bold) {
            view.setTypeface(Typeface.DEFAULT_BOLD);
        }
        final LinearLayout.LayoutParams params = new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT);
        params.topMargin = dp(6);
        parent.addView(view, params);
        return view;
    }

    private void addSection(LinearLayout parent, int label) {
        final TextView view = addText(parent, getString(label), 18, COLOR_PRIMARY, true);
        view.setPadding(0, dp(22), 0, 0);
    }

    private Button addButton(LinearLayout parent, int label, View.OnClickListener listener) {
        final Button button = new Button(this);
        button.setText(label);
        button.setAllCaps(false);
        button.setTextSize(TypedValue.COMPLEX_UNIT_SP, 15);
        button.setOnClickListener(listener);
        final LinearLayout.LayoutParams params = new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT);
        params.topMargin = dp(8);
        parent.addView(button, params);
        return button;
    }

    private static GradientDrawable rounded(int color, int radius) {
        final GradientDrawable drawable = new GradientDrawable();
        drawable.setColor(color);
        drawable.setCornerRadius(radius);
        return drawable;
    }

    private void setBusy(boolean value) {
        busy = value;
        for (Button button : actionButtons) {
            button.setEnabled(!value);
        }
        progressBar.setVisibility(value ? View.VISIBLE : View.GONE);
        progressBar.setIndeterminate(value);
        progressBar.setProgress(0);
    }

    private void setStatus(int text) {
        setStatus(getString(text));
    }

    private void setStatus(CharSequence text) {
        statusView.setText(text);
        statusView.setTextColor(COLOR_TEXT);
    }

    private void ui(Runnable action) {
        runOnUiThread(action);
    }

    // ---------------------------------------------------------------- updates

    private void checkForUpdates() {
        if (busy) {
            return;
        }
        if (gameLibraryLoaded) {
            new AlertDialog.Builder(this)
                    .setTitle(R.string.section_updates)
                    .setMessage("Esta es la APK jugable personal. Se actualiza reinstalando "
                            + "una nueva APK jugable generada en el PC; la actualización "
                            + "pública no incluye el juego y lo desactivaría. Descarga la "
                            + "nueva APK personal cuando exista.")
                    .setPositiveButton(android.R.string.ok, null)
                    .show();
            return;
        }
        setBusy(true);
        setStatus(R.string.status_checking);
        new Thread(() -> {
            try {
                final GithubReleases.Release release = GithubReleases.findLatest();
                if (release == null) {
                    ui(() -> {
                        setBusy(false);
                        setStatus(R.string.status_no_release);
                    });
                    return;
                }
                final UpdateManifest manifest = GithubReleases.fetchManifest(release.manifestUrl);
                if (!getPackageName().equals(manifest.applicationId)) {
                    throw new IllegalStateException("el manifiesto pertenece a otra aplicación");
                }
                if (manifest.versionCode <= installedVersionCode) {
                    ui(() -> {
                        setBusy(false);
                        setStatus(getString(R.string.status_up_to_date,
                                installedVersionCode, release.tag, manifest.versionCode));
                    });
                    return;
                }
                ui(() -> {
                    setBusy(false);
                    showUpdateDialog(release, manifest);
                });
            } catch (Exception error) {
                final String detail = describe(error);
                ui(() -> {
                    setBusy(false);
                    setStatus(getString(R.string.status_error, detail));
                });
            }
        }, "kon-update-check").start();
    }

    private void showUpdateDialog(GithubReleases.Release release, UpdateManifest manifest) {
        setStatus(getString(R.string.update_title, manifest.versionName));
        new AlertDialog.Builder(this)
                .setTitle(getString(R.string.update_title, manifest.versionName))
                .setMessage(getString(R.string.update_message, release.tag,
                        manifest.versionName, manifest.versionCode, formatSize(manifest.size)))
                .setPositiveButton(R.string.button_download_install,
                        (dialog, which) -> downloadAndInstall(release, manifest))
                .setNegativeButton(R.string.cancel, null)
                .show();
    }

    private void downloadAndInstall(GithubReleases.Release release, UpdateManifest manifest) {
        setBusy(true);
        progressBar.setIndeterminate(false);
        progressBar.setProgress(0);
        setStatus(getString(R.string.status_downloading, manifest.versionName));
        new Thread(() -> {
            try {
                final File apk = GithubReleases.downloadApk(this, release, manifest,
                        percent -> ui(() -> progressBar.setProgress(percent)));
                ui(() -> setStatus(R.string.status_verifying));
                GithubReleases.verifyApk(this, apk, manifest);
                ui(() -> {
                    setBusy(false);
                    setStatus(R.string.status_verified);
                    installVerifiedApk(apk);
                });
            } catch (Exception error) {
                final String detail = describe(error);
                ui(() -> {
                    setBusy(false);
                    setStatus(getString(R.string.status_error, detail));
                });
            }
        }, "kon-update-download").start();
    }

    private void installVerifiedApk(File apk) {
        if (!getPackageManager().canRequestPackageInstalls()) {
            setStatus(R.string.status_install_permission);
            new AlertDialog.Builder(this)
                    .setTitle(R.string.install_permission_title)
                    .setMessage(R.string.install_permission_message)
                    .setPositiveButton(R.string.button_open_settings, (dialog, which) -> {
                        try {
                            startActivity(new Intent(Settings.ACTION_MANAGE_UNKNOWN_APP_SOURCES,
                                    Uri.parse("package:" + getPackageName())));
                        } catch (ActivityNotFoundException error) {
                            setStatus(getString(R.string.status_error, describe(error)));
                        }
                    })
                    .setNegativeButton(R.string.cancel, null)
                    .show();
            return;
        }
        try {
            UpdateInstaller.install(this, apk);
            setStatus(R.string.status_install_prompt);
        } catch (Exception error) {
            setStatus(getString(R.string.status_error, describe(error)));
        }
    }

    // ----------------------------------------------------------- saves (SAF)

    private void startExport() {
        if (busy) {
            return;
        }
        final String stamp = new SimpleDateFormat("yyyyMMdd-HHmm", Locale.US).format(new Date());
        final Intent intent = new Intent(Intent.ACTION_CREATE_DOCUMENT);
        intent.addCategory(Intent.CATEGORY_OPENABLE);
        intent.setType("application/zip");
        intent.putExtra(Intent.EXTRA_TITLE, "kon-houkago-saves-" + stamp + ".zip");
        try {
            startActivityForResult(intent, REQUEST_EXPORT);
        } catch (ActivityNotFoundException error) {
            setStatus(getString(R.string.status_error,
                    "no hay ninguna app para elegir el archivo de destino"));
        }
    }

    private void startImportPicker() {
        if (busy) {
            return;
        }
        final Intent intent = new Intent(Intent.ACTION_OPEN_DOCUMENT);
        intent.addCategory(Intent.CATEGORY_OPENABLE);
        intent.setType("*/*");
        intent.putExtra(Intent.EXTRA_MIME_TYPES, new String[]{
                "application/zip", "application/x-zip-compressed", "application/octet-stream"});
        try {
            startActivityForResult(intent, REQUEST_IMPORT);
        } catch (ActivityNotFoundException error) {
            setStatus(getString(R.string.status_error,
                    "no hay ninguna app para elegir el archivo de partidas"));
        }
    }

    @Override
    protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        super.onActivityResult(requestCode, resultCode, data);
        if (resultCode != RESULT_OK || data == null || data.getData() == null) {
            if (requestCode == REQUEST_IMPORT) {
                setStatus(R.string.status_import_cancelled);
            }
            return;
        }
        final Uri uri = data.getData();
        if (requestCode == REQUEST_EXPORT) {
            runExport(uri);
        } else if (requestCode == REQUEST_GAME_PACK) {
            installGamePack(uri);
        } else if (requestCode == REQUEST_ISO) {
            installIso(uri);
        } else if (requestCode == REQUEST_IMPORT) {
            new AlertDialog.Builder(this)
                    .setTitle(R.string.import_confirm_title)
                    .setMessage(R.string.import_confirm_message)
                    .setPositiveButton(R.string.button_import_confirm,
                            (dialog, which) -> runImport(uri))
                    .setNegativeButton(R.string.cancel,
                            (dialog, which) -> setStatus(R.string.status_import_cancelled))
                    .show();
        }
    }

    private void runExport(Uri uri) {
        setBusy(true);
        setStatus(R.string.status_exporting);
        new Thread(() -> {
            try {
                final int files = SaveArchive.exportTo(this, uri);
                ui(() -> {
                    setBusy(false);
                    setStatus(getString(R.string.status_export_done, files));
                });
            } catch (Exception error) {
                final String detail = describe(error);
                ui(() -> {
                    setBusy(false);
                    setStatus(getString(R.string.status_error, detail));
                });
            }
        }, "kon-export").start();
    }

    private void runImport(Uri uri) {
        setBusy(true);
        setStatus(R.string.status_importing);
        new Thread(() -> {
            try {
                final SaveArchive.ImportResult result = SaveArchive.importFrom(this, uri);
                ui(() -> {
                    setBusy(false);
                    setStatus(getString(R.string.status_import_done, result.files,
                            result.backup.getAbsolutePath()));
                });
            } catch (Exception error) {
                final String detail = describe(error);
                ui(() -> {
                    setBusy(false);
                    setStatus(getString(R.string.status_error, detail));
                });
            }
        }, "kon-import").start();
    }

    // ----------------------------------------------------------- diagnostics

    private void runDiagnostics() {
        if (busy) {
            return;
        }
        setBusy(true);
        setStatus(R.string.status_diagnostics_running);
        diagnosticsView.setText("...");
        new Thread(() -> {
            String report;
            try {
                report = KonProbe.run(getCacheDir().getAbsolutePath());
            } catch (Throwable error) {
                report = "FAIL: no se pudo cargar libkon_probe.so\n" + describe(error);
            }
            final String text = report;
            final boolean failed = text.contains("FAIL");
            ui(() -> {
                diagnosticsView.setText(text);
                diagnosticsView.setTextColor(failed ? COLOR_FAIL : COLOR_OK);
                setBusy(false);
                setStatus(failed ? R.string.status_diagnostics_fail
                        : R.string.status_diagnostics_pass);
            });
        }, "kon-diagnostics").start();
    }

    // ---------------------------------------------------------------- game

    private void prepareGameRoot() {
        new File(gameRoot, "config").mkdirs();
        new File(gameRoot, "userdata/ms0").mkdirs();
        final File executables = new File(gameRoot, "analysis/executables");
        executables.mkdirs();
        final File elf = new File(executables, "pkon.elf");
        if (!elf.isFile()) {
            try (InputStream in = getAssets().open("pkon.elf");
                 OutputStream out = new FileOutputStream(elf)) {
                final byte[] buffer = new byte[1 << 20];
                int read;
                while ((read = in.read(buffer)) > 0) {
                    out.write(buffer, 0, read);
                }
            } catch (IOException ignored) {
                // Public APK: the module and the executable arrive in game.kon.
            }
        }
        final File modules = new File(gameRoot, "modules");
        modules.mkdirs();
        final File module = new File(modules, "libkon_game.so");
        if (!module.isFile()) {
            try (InputStream in = getAssets().open("libkon_game.so");
                 OutputStream out = new FileOutputStream(module)) {
                final byte[] buffer = new byte[1 << 20];
                int read;
                while ((read = in.read(buffer)) > 0) {
                    out.write(buffer, 0, read);
                }
            } catch (IOException ignored) {
                // Public APK: the module arrives in game.kon.
            }
        }
    }

    private boolean elfInstalled() {
        final File elf = new File(gameRoot, "analysis/executables/pkon.elf");
        return elf.isFile() && elf.length() > 1024L * 1024L;
    }

    private boolean isoInstalled() {
        return new File(gameRoot, "analysis/extracted/en/PSP_GAME/USRDIR/std.cpk").isFile()
                || new File(gameRoot, "analysis/extracted/jp/PSP_GAME/USRDIR/std.cpk").isFile();
    }

    private boolean moduleInstalled() {
        final File module = new File(gameRoot, "modules/libkon_game.so");
        return module.isFile() && module.length() > 1024L * 1024L;
    }

    private String modulePath() {
        return new File(gameRoot, "modules/libkon_game.so").getAbsolutePath();
    }

    private String gameLanguage() {
        final File settings = new File(gameRoot, "config/settings.ini");
        if (settings.isFile()) {
            try {
                final List<String> lines = java.nio.file.Files.readAllLines(settings.toPath());
                for (String line : lines) {
                    if (line.startsWith("language=")) {
                        final String value = line.substring("language=".length()).trim();
                        if ("ja".equals(value) || "en".equals(value)) {
                            return value;
                        }
                    }
                }
            } catch (IOException ignored) {
            }
        }
        return new File(gameRoot, "analysis/extracted/en/PSP_GAME/USRDIR/std.cpk").isFile() ? "en" : "ja";
    }

    private void writeLanguage(String language) {
        final File config = new File(gameRoot, "config");
        config.mkdirs();
        final File settings = new File(config, "settings.ini");
        final StringBuilder text = new StringBuilder();
        if (settings.isFile()) {
            try {
                for (String line : java.nio.file.Files.readAllLines(settings.toPath())) {
                    if (!line.startsWith("language=")) {
                        text.append(line).append('\n');
                    }
                }
            } catch (IOException ignored) {
            }
        }
        text.append("language=").append(language).append('\n');
        try {
            java.nio.file.Files.write(settings.toPath(), text.toString().getBytes("UTF-8"));
        } catch (IOException ignored) {
        }
    }

    private void refreshPlayState() {
        if (playStatusView == null) {
            return;
        }
        if (!gameLibraryLoaded) {
            playStatusView.setText(R.string.play_no_library);
            playButton.setEnabled(false);
            installIsoButton.setEnabled(false);
            prepareButton.setEnabled(false);
            return;
        }
        installIsoButton.setEnabled(!busy && !buildRunning);
        importGameButton.setEnabled(!busy && !buildRunning);
        prepareButton.setEnabled(!busy && !buildRunning);
        if (!isoInstalled()) {
            playStatusView.setText(R.string.play_missing_iso);
            playButton.setEnabled(false);
            return;
        }
        if (!moduleInstalled()) {
            playStatusView.setText(R.string.play_missing_module);
            playButton.setEnabled(false);
            return;
        }
        final String problem = moduleProblem();
        if (problem != null) {
            playStatusView.setText(problem);
            playButton.setEnabled(false);
            return;
        }
        playStatusView.setText(R.string.play_ready);
        playButton.setEnabled(!busy && !buildRunning);
    }

    /**
     * Why the installed module cannot be used, or null when it is fine.
     * A module imported as game.kon has no metadata and is accepted (legacy path);
     * a module built here records its ABI, ISO hash and toolchain.
     */
    private String moduleProblem() {
        final File metadata = new File(gameRoot, "modules/libkon_game.json");
        if (!metadata.isFile()) {
            return null;
        }
        try {
            if (metadata.length() > 64L * 1024L) {
                return getString(R.string.module_invalid);
            }
            final String text = new String(java.nio.file.Files.readAllBytes(metadata.toPath()), "UTF-8");
            final JSONObject json = new JSONObject(text);
            if (!GameBuilder.MODULE_ABI.equals(json.optString("abi", ""))) {
                return getString(R.string.module_abi_mismatch);
            }
            final String actual = currentElfHash();
            if (actual == null) {
                return getString(R.string.play_missing_elf);
            }
            if (!actual.equalsIgnoreCase(json.optString("elf_sha256", ""))) {
                return getString(R.string.module_stale);
            }
            return null;
        } catch (Exception error) {
            return getString(R.string.module_invalid);
        }
    }

    private String currentElfHash() {
        final File elf = new File(gameRoot, "analysis/executables/pkon.elf");
        if (!elf.isFile()) {
            return null;
        }
        final long stamp = elf.length() * 31L + elf.lastModified();
        if (elfHash != null && stamp == elfHashStamp) {
            return elfHash;
        }
        try (InputStream in = new FileInputStream(elf)) {
            final MessageDigest digest = MessageDigest.getInstance("SHA-256");
            final byte[] buffer = new byte[1 << 20];
            int read;
            while ((read = in.read(buffer)) > 0) {
                digest.update(buffer, 0, read);
            }
            final StringBuilder text = new StringBuilder();
            for (byte value : digest.digest()) {
                text.append(String.format(Locale.ROOT, "%02x", value & 0xFF));
            }
            elfHash = text.toString();
            elfHashStamp = stamp;
            return elfHash;
        } catch (Exception error) {
            return null;
        }
    }

    private void startGame() {
        if (!gameLibraryLoaded) {
            new AlertDialog.Builder(this).setMessage(R.string.play_no_library)
                    .setPositiveButton(android.R.string.ok, null).show();
            return;
        }
        if (!moduleInstalled() || !elfInstalled() || !isoInstalled()) {
            refreshPlayState();
            new AlertDialog.Builder(this)
                    .setMessage(!moduleInstalled() ? R.string.play_missing_module
                            : !elfInstalled() ? R.string.play_missing_elf : R.string.play_missing_iso)
                    .setPositiveButton(android.R.string.ok, null).show();
            return;
        }
        final String problem = moduleProblem();
        if (problem != null) {
            new AlertDialog.Builder(this).setTitle(R.string.button_play)
                    .setMessage(problem)
                    .setPositiveButton(android.R.string.ok, null).show();
            refreshPlayState();
            return;
        }
        final Intent intent = new Intent(this, GameActivity.class)
                .putExtra(GameActivity.EXTRA_ROOT, gameRoot.getAbsolutePath())
                .putExtra(GameActivity.EXTRA_LANGUAGE, gameLanguage())
                .putExtra(GameActivity.EXTRA_MODULE, modulePath());
        startActivity(intent);
    }

    // ------------------------------------------------------- prepare on device

    private File buildStatusFile() {
        return new File(gameRoot, "recomp_cache/status.json");
    }

    private File buildLogFile() {
        return new File(gameRoot, "recomp_cache/compilacion.log");
    }

    private void startPrepare() {
        if (busy || buildRunning) {
            return;
        }
        if (!isoInstalled()) {
            setStatus(R.string.play_missing_iso);
            return;
        }
        final String missing = GameBuilder.missingBinaries(this);
        if (missing != null) {
            new AlertDialog.Builder(this)
                    .setTitle(R.string.button_prepare_game)
                    .setMessage(getString(R.string.prepare_missing_tools, missing))
                    .setPositiveButton(android.R.string.ok, null)
                    .show();
            return;
        }
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU
                && checkSelfPermission(Manifest.permission.POST_NOTIFICATIONS)
                != PackageManager.PERMISSION_GRANTED) {
            requestPermissions(new String[]{Manifest.permission.POST_NOTIFICATIONS}, REQUEST_NOTIFICATIONS);
        }
        setStatus(R.string.prepare_started);
        buildRunning = true;
        buildProgress.setIndeterminate(true);
        buildProgress.setProgress(0);
        buildProgress.setVisibility(View.VISIBLE);
        buildPhaseView.setText(R.string.build_phase_toolchain);
        buildPhaseView.setVisibility(View.VISIBLE);
        cancelBuildButton.setVisibility(View.VISIBLE);
        refreshPlayState();
        try {
            startForegroundService(BuildService.prepareIntent(this));
        } catch (RuntimeException error) {
            buildRunning = false;
            renderBuild(BuildStatus.read(buildStatusFile()), "");
            setStatus(getString(R.string.build_error, describe(error)));
            return;
        }
        startBuildPolling();
    }

    private void cancelPrepare() {
        startService(BuildService.cancelIntent(this));
        setStatus(R.string.build_cancelling);
    }

    private void refreshBuildState() {
        final BuildStatus status = BuildStatus.read(buildStatusFile());
        buildRunning = buildActive(status);
        renderBuild(status, BuildStatus.tail(buildLogFile(), 1200));
    }

    /** A status from a killed process is stale, not an active build. */
    private static boolean buildActive(BuildStatus status) {
        return status.active() && (GameBuilder.isRunning() || BuildService.isRunning());
    }

    private void startBuildPolling() {
        pollingBuild = true;
        if (buildPoller != null && buildPoller.isAlive()) {
            return;
        }
        buildPoller = new Thread(() -> {
            try {
                Thread.sleep(800L);
            } catch (InterruptedException interrupted) {
                return;
            }
            while (pollingBuild) {
                final BuildStatus status = BuildStatus.read(buildStatusFile());
                final String tail = BuildStatus.tail(buildLogFile(), 1200);
                final boolean active = buildActive(status);
                ui(() -> renderBuild(status, tail));
                if (!active) {
                    pollingBuild = false;
                }
                try {
                    Thread.sleep(500L);
                } catch (InterruptedException interrupted) {
                    return;
                }
            }
        }, "kon-build-poll");
        buildPoller.setDaemon(true);
        buildPoller.start();
    }

    private void renderBuild(BuildStatus status, String tail) {
        final boolean active = buildActive(status);
        buildRunning = active;
        buildProgress.setIndeterminate(status.total <= 0);
        buildProgress.setVisibility(active ? View.VISIBLE : View.GONE);
        buildPhaseView.setVisibility(active ? View.VISIBLE : View.GONE);
        cancelBuildButton.setVisibility(active ? View.VISIBLE : View.GONE);
        if (active) {
            if (status.total > 0) {
                buildProgress.setProgress(status.percent());
            }
            buildPhaseView.setText(status.phaseLabel(this));
        }
        final boolean showLog = (active || BuildStatus.PHASE_ERROR.equals(status.phase)
                || status.active())
                && tail != null && !tail.trim().isEmpty();
        buildLogView.setVisibility(showLog ? View.VISIBLE : View.GONE);
        if (showLog) {
            buildLogView.setText(tail);
        }
        if (!active) {
            if (status.active()) {
                setStatus(R.string.build_interrupted);
            } else if (BuildStatus.PHASE_DONE.equals(status.phase)) {
                setStatus(R.string.build_done);
            } else if (BuildStatus.PHASE_CANCELLED.equals(status.phase)) {
                setStatus(R.string.build_cancelled);
            } else if (BuildStatus.PHASE_ERROR.equals(status.phase)) {
                setStatus(getString(R.string.build_error,
                        status.error == null ? "" : status.error));
            }
        }
        refreshPlayState();
    }

    private void pickIso() {
        if (!gameLibraryLoaded) {
            new AlertDialog.Builder(this).setMessage(R.string.play_no_library)
                    .setPositiveButton(android.R.string.ok, null).show();
            return;
        }
        final Intent intent = new Intent(Intent.ACTION_OPEN_DOCUMENT);
        intent.addCategory(Intent.CATEGORY_OPENABLE);
        intent.setType("*/*");
        try {
            startActivityForResult(intent, REQUEST_ISO);
        } catch (ActivityNotFoundException error) {
            setStatus(getString(R.string.iso_error, "no hay selector de archivos"));
        }
    }

    private void pickGamePack() {
        if (!gameLibraryLoaded) {
            new AlertDialog.Builder(this).setMessage(R.string.play_no_library)
                    .setPositiveButton(android.R.string.ok, null).show();
            return;
        }
        final Intent intent = new Intent(Intent.ACTION_OPEN_DOCUMENT);
        intent.addCategory(Intent.CATEGORY_OPENABLE);
        intent.setType("*/*");
        try {
            startActivityForResult(intent, REQUEST_GAME_PACK);
        } catch (ActivityNotFoundException error) {
            setStatus(getString(R.string.game_pack_error, "no hay selector de archivos"));
        }
    }

    private void installGamePack(Uri uri) {
        setBusy(true);
        setStatus(R.string.game_pack_importing);
        new Thread(() -> {
            try {
                final int files = GamePack.importFrom(this, uri, gameRoot);
                ui(() -> {
                    setBusy(false);
                    setStatus(getString(R.string.game_pack_done, files));
                    refreshPlayState();
                });
            } catch (Exception error) {
                final String detail = describe(error);
                ui(() -> {
                    setBusy(false);
                    setStatus(getString(R.string.game_pack_error, detail));
                });
            }
        }, "kon-game-pack").start();
    }

    private void installIso(Uri uri) {
        ParcelFileDescriptor descriptor = null;
        try {
            descriptor = getContentResolver().openFileDescriptor(uri, "r");
            if (descriptor == null) {
                throw new IOException("no se pudo abrir la ISO");
            }
            final ParcelFileDescriptor handle = descriptor;
            final int fd = handle.getFd();
            setBusy(true);
            setStatus(R.string.iso_installing);
            new Thread(() -> {
                final String result;
                try {
                    result = nativeInstallIso(fd, gameRoot.getAbsolutePath());
                } finally {
                    try {
                        handle.close();
                    } catch (IOException ignored) {
                    }
                }
                ui(() -> {
                    setBusy(false);
                    if (result != null && result.startsWith("ERROR:")) {
                        setStatus(getString(R.string.iso_error, result.substring(6).trim()));
                    } else {
                        writeLanguage(result == null ? "en" : result);
                        setStatus(getString(R.string.iso_installed, result));
                        refreshPlayState();
                    }
                });
            }, "kon-iso").start();
        } catch (Exception error) {
            if (descriptor != null) {
                try {
                    descriptor.close();
                } catch (IOException ignored) {
                }
            }
            setBusy(false);
            setStatus(getString(R.string.iso_error, describe(error)));
        }
    }

    // ---------------------------------------------------------------- helpers

    private int dp(int value) {
        return (int) (value * getResources().getDisplayMetrics().density + 0.5f);
    }

    private static String formatSize(long bytes) {
        if (bytes >= 1024L * 1024L) {
            return String.format(Locale.ROOT, "%.1f MB", bytes / 1048576.0);
        }
        return String.format(Locale.ROOT, "%d KB", Math.max(1L, bytes / 1024L));
    }

    private static String describe(Throwable error) {
        String message = error.getMessage();
        if (message == null || message.trim().isEmpty()) {
            message = error.getClass().getSimpleName();
        }
        return message;
    }
}
