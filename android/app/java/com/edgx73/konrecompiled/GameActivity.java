package com.edgx73.konrecompiled;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.Context;
import android.content.res.Configuration;
import android.hardware.input.InputManager;
import android.os.Build;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.os.VibrationEffect;
import android.os.Vibrator;
import android.text.InputFilter;
import android.util.SparseBooleanArray;
import android.view.InputDevice;
import android.view.KeyEvent;
import android.view.MotionEvent;
import android.view.Surface;
import android.view.SurfaceHolder;
import android.view.SurfaceView;
import android.view.View;
import android.view.WindowInsets;
import android.view.WindowInsetsController;
import android.view.WindowManager;
import android.widget.EditText;
import android.widget.FrameLayout;
import android.widget.TextView;

/**
 * Fullscreen game window.
 *
 * <p>The native runtime renders the 480x272 PSP frame into a SurfaceView. The
 * touch overlay sends a PSP pad bitmask; a connected controller does the same
 * through Android's key/motion events and hides the overlay so it never covers
 * the game.
 */
public final class GameActivity extends Activity implements SurfaceHolder.Callback {
    static {
        System.loadLibrary("kon_runtime");
    }

    public static final String EXTRA_ROOT = "root";
    public static final String EXTRA_LANGUAGE = "language";
    public static final String EXTRA_MODULE = "module";

    private static final int PSP_SELECT = 0x1;
    private static final int PSP_START = 0x8;
    private static final int PSP_UP = 0x10;
    private static final int PSP_RIGHT = 0x20;
    private static final int PSP_DOWN = 0x40;
    private static final int PSP_LEFT = 0x80;
    private static final int PSP_L = 0x100;
    private static final int PSP_R = 0x200;
    private static final int PSP_TRIANGLE = 0x1000;
    private static final int PSP_CIRCLE = 0x2000;
    private static final int PSP_CROSS = 0x4000;
    private static final int PSP_SQUARE = 0x8000;

    private SurfaceView surfaceView;
    private TouchControlsView touchControls;
    private TextView status;
    private String root;
    private String language;
    private String modulePath;
    private boolean surfaceReady;
    private boolean gameStarted;
    private boolean resumed;
    private InputManager inputManager;
    private final SparseBooleanArray pressedKeys = new SparseBooleanArray();
    private float axisX = 0f;
    private float axisY = 0f;
    private float hatX = 0f;
    private float hatY = 0f;
    private final Handler main = new Handler(Looper.getMainLooper());

    private final InputManager.InputDeviceListener deviceListener = new InputManager.InputDeviceListener() {
        @Override
        public void onInputDeviceAdded(int deviceId) {
            updateGamepadVisibility();
        }

        @Override
        public void onInputDeviceRemoved(int deviceId) {
            nativeGamepad(deviceId, "", 0, 128, 128, false);
            updateGamepadVisibility();
        }

        @Override
        public void onInputDeviceChanged(int deviceId) {
            updateGamepadVisibility();
        }
    };

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        root = getIntent().getStringExtra(EXTRA_ROOT);
        language = getIntent().getStringExtra(EXTRA_LANGUAGE);
        modulePath = getIntent().getStringExtra(EXTRA_MODULE);
        if (language == null || language.isEmpty()) {
            language = "en";
        }
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        inputManager = (InputManager) getSystemService(Context.INPUT_SERVICE);
        buildUi();
        hideSystemUi();
    }

    private void buildUi() {
        final FrameLayout frame = new FrameLayout(this);
        frame.setBackgroundColor(0xFF000000);

        surfaceView = new SurfaceView(this);
        surfaceView.getHolder().addCallback(this);
        surfaceView.setFocusable(true);
        surfaceView.setFocusableInTouchMode(true);
        final AspectRatioLayout screen = new AspectRatioLayout(this, 480f / 272f);
        screen.addView(surfaceView, new FrameLayout.LayoutParams(
                FrameLayout.LayoutParams.MATCH_PARENT, FrameLayout.LayoutParams.MATCH_PARENT));
        frame.addView(screen, new FrameLayout.LayoutParams(
                FrameLayout.LayoutParams.MATCH_PARENT, FrameLayout.LayoutParams.MATCH_PARENT));

        touchControls = new TouchControlsView(this);
        touchControls.setCallback((buttons, x, y, active) ->
                nativeTouch(buttons, x, y, active));
        final FrameLayout.LayoutParams touchParams = new FrameLayout.LayoutParams(
                FrameLayout.LayoutParams.MATCH_PARENT, FrameLayout.LayoutParams.MATCH_PARENT);
        frame.addView(touchControls, touchParams);

        status = new TextView(this);
        status.setTextColor(0xFFFFFFFF);
        status.setTextSize(16f);
        status.setPadding(dp(20), dp(20), dp(20), dp(20));
        status.setVisibility(View.GONE);
        frame.addView(status, new FrameLayout.LayoutParams(
                FrameLayout.LayoutParams.WRAP_CONTENT, FrameLayout.LayoutParams.WRAP_CONTENT));

        setContentView(frame);
        surfaceView.requestFocus();
    }

    private void hideSystemUi() {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
            final WindowInsetsController controller = getWindow().getInsetsController();
            if (controller != null) {
                controller.hide(WindowInsets.Type.statusBars() | WindowInsets.Type.navigationBars());
                controller.setSystemBarsBehavior(
                        WindowInsetsController.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE);
            }
        } else {
            getWindow().getDecorView().setSystemUiVisibility(
                    View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY
                            | View.SYSTEM_UI_FLAG_FULLSCREEN
                            | View.SYSTEM_UI_FLAG_HIDE_NAVIGATION
                            | View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN
                            | View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION
                            | View.SYSTEM_UI_FLAG_LAYOUT_STABLE);
        }
    }

    @Override
    protected void onResume() {
        super.onResume();
        resumed = true;
        nativeFocus(true);
        if (inputManager != null) {
            inputManager.registerInputDeviceListener(deviceListener, null);
        }
        updateGamepadVisibility();
        startGameIfReady();
    }

    @Override
    protected void onPause() {
        resumed = false;
        nativeFocus(false);
        if (inputManager != null) {
            inputManager.unregisterInputDeviceListener(deviceListener);
        }
        super.onPause();
    }

    @Override
    protected void onDestroy() {
        nativeStop();
        super.onDestroy();
    }

    @Override
    public void onConfigurationChanged(Configuration newConfig) {
        super.onConfigurationChanged(newConfig);
        hideSystemUi();
    }

    @Override
    public void surfaceCreated(SurfaceHolder holder) {
        nativeSurfaceCreated(holder.getSurface());
        surfaceReady = true;
        startGameIfReady();
    }

    @Override
    public void surfaceChanged(SurfaceHolder holder, int format, int width, int height) {
        nativeSurfaceChanged(holder.getSurface(), width, height);
    }

    @Override
    public void surfaceDestroyed(SurfaceHolder holder) {
        surfaceReady = false;
        gameStarted = false;
        nativeSurfaceDestroyed();
        nativeStop();
    }

    private void startGameIfReady() {
        if (!surfaceReady || gameStarted || !resumed || root == null || root.isEmpty()
                || modulePath == null || modulePath.isEmpty()) {
            return;
        }
        gameStarted = nativeStart(root, language, modulePath);
        if (!gameStarted) {
            final String detail = nativeLastError();
            status.setText(detail == null || detail.isEmpty()
                    ? "No se pudo iniciar el juego. Revisa la instalación."
                    : "No se pudo iniciar el juego: " + detail);
            status.setVisibility(View.VISIBLE);
        }
    }

    private void updateGamepadVisibility() {
        boolean connected = false;
        if (inputManager != null) {
            for (int id : inputManager.getInputDeviceIds()) {
                final InputDevice device = inputManager.getInputDevice(id);
                if (isGamepad(device)) {
                    connected = true;
                    break;
                }
            }
        }
        if (connected) {
            touchControls.setVisibility(View.GONE);
            nativeTouch(0, 128, 128, false);
        } else {
            touchControls.setVisibility(View.VISIBLE);
        }
    }

    private static boolean isGamepad(InputDevice device) {
        if (device == null) {
            return false;
        }
        final int sources = device.getSources();
        return (sources & InputDevice.SOURCE_GAMEPAD) == InputDevice.SOURCE_GAMEPAD
                || (sources & InputDevice.SOURCE_JOYSTICK) == InputDevice.SOURCE_JOYSTICK;
    }

    @Override
    public boolean onKeyDown(int keyCode, KeyEvent event) {
        if (isGamepadKey(keyCode)) {
            pressedKeys.put(keyCode, true);
            sendGamepad(event.getDevice());
            return true;
        }
        if (keyCode == KeyEvent.KEYCODE_BACK) {
            nativeStop();
            finish();
            return true;
        }
        return super.onKeyDown(keyCode, event);
    }

    @Override
    public boolean onKeyUp(int keyCode, KeyEvent event) {
        if (isGamepadKey(keyCode)) {
            pressedKeys.delete(keyCode);
            sendGamepad(event.getDevice());
            return true;
        }
        return super.onKeyUp(keyCode, event);
    }

    @Override
    public boolean onGenericMotionEvent(MotionEvent event) {
        final InputDevice device = event.getDevice();
        if (device == null || !isGamepad(device)) {
            return super.onGenericMotionEvent(event);
        }
        if (event.getActionMasked() == MotionEvent.ACTION_MOVE) {
            axisX = event.getAxisValue(MotionEvent.AXIS_X);
            axisY = event.getAxisValue(MotionEvent.AXIS_Y);
            hatX = event.getAxisValue(MotionEvent.AXIS_HAT_X);
            hatY = event.getAxisValue(MotionEvent.AXIS_HAT_Y);
            sendGamepad(device);
            return true;
        }
        return super.onGenericMotionEvent(event);
    }

    private static boolean isGamepadKey(int keyCode) {
        switch (keyCode) {
            case KeyEvent.KEYCODE_BUTTON_A:
            case KeyEvent.KEYCODE_BUTTON_B:
            case KeyEvent.KEYCODE_BUTTON_X:
            case KeyEvent.KEYCODE_BUTTON_Y:
            case KeyEvent.KEYCODE_BUTTON_L1:
            case KeyEvent.KEYCODE_BUTTON_R1:
            case KeyEvent.KEYCODE_BUTTON_L2:
            case KeyEvent.KEYCODE_BUTTON_R2:
            case KeyEvent.KEYCODE_BUTTON_START:
            case KeyEvent.KEYCODE_BUTTON_SELECT:
            case KeyEvent.KEYCODE_DPAD_UP:
            case KeyEvent.KEYCODE_DPAD_DOWN:
            case KeyEvent.KEYCODE_DPAD_LEFT:
            case KeyEvent.KEYCODE_DPAD_RIGHT:
                return true;
            default:
                return false;
        }
    }

    private void sendGamepad(InputDevice device) {
        if (device == null) {
            return;
        }
        int buttons = 0;
        if (pressedKeys.get(KeyEvent.KEYCODE_BUTTON_A)) buttons |= PSP_CIRCLE;
        if (pressedKeys.get(KeyEvent.KEYCODE_BUTTON_B)) buttons |= PSP_CROSS;
        if (pressedKeys.get(KeyEvent.KEYCODE_BUTTON_X)) buttons |= PSP_SQUARE;
        if (pressedKeys.get(KeyEvent.KEYCODE_BUTTON_Y)) buttons |= PSP_TRIANGLE;
        if (pressedKeys.get(KeyEvent.KEYCODE_BUTTON_L1) || pressedKeys.get(KeyEvent.KEYCODE_BUTTON_L2)) buttons |= PSP_L;
        if (pressedKeys.get(KeyEvent.KEYCODE_BUTTON_R1) || pressedKeys.get(KeyEvent.KEYCODE_BUTTON_R2)) buttons |= PSP_R;
        if (pressedKeys.get(KeyEvent.KEYCODE_BUTTON_START)) buttons |= PSP_START;
        if (pressedKeys.get(KeyEvent.KEYCODE_BUTTON_SELECT)) buttons |= PSP_SELECT;
        if (pressedKeys.get(KeyEvent.KEYCODE_DPAD_UP) || hatY < -0.5f) buttons |= PSP_UP;
        if (pressedKeys.get(KeyEvent.KEYCODE_DPAD_DOWN) || hatY > 0.5f) buttons |= PSP_DOWN;
        if (pressedKeys.get(KeyEvent.KEYCODE_DPAD_LEFT) || hatX < -0.5f) buttons |= PSP_LEFT;
        if (pressedKeys.get(KeyEvent.KEYCODE_DPAD_RIGHT) || hatX > 0.5f) buttons |= PSP_RIGHT;
        final int x = axis(axisX);
        final int y = axis(axisY);
        nativeGamepad(device.getId(), device.getName(), buttons, x, y, true);
        touchControls.setVisibility(View.GONE);
    }

    private static int axis(float value) {
        final float dead = 0.18f;
        if (Math.abs(value) < dead) {
            return 128;
        }
        final int scaled = Math.round(128f + Math.max(-1f, Math.min(1f, value)) * 127f);
        return Math.max(0, Math.min(255, scaled));
    }

    /** Called from the native thread when the game asks for text (sceUtilityOsk). */
    public void requestTextInput(final String description, final String initial, final int limit) {
        runOnUiThread(() -> {
            final EditText input = new EditText(this);
            input.setText(initial == null ? "" : initial);
            input.setSelection(input.getText().length());
            if (limit > 0 && limit < 1024) {
                input.setFilters(new InputFilter[]{new InputFilter.LengthFilter(limit)});
            }
            final AlertDialog dialog = new AlertDialog.Builder(this)
                    .setTitle(description == null || description.isEmpty() ? "Texto" : description)
                    .setView(input)
                    .setPositiveButton("Aceptar", (d, w) ->
                            nativeTextResult(input.getText().toString(), true))
                    .setNegativeButton("Cancelar", (d, w) -> nativeTextResult("", false))
                    .create();
            dialog.show();
            input.requestFocus();
        });
    }

    private int dp(int value) {
        return Math.round(value * getResources().getDisplayMetrics().density);
    }

    // ---------------------------------------------------------------- native

    private native boolean nativeStart(String root, String language, String module);

    private native String nativeLastError();

    private native void nativeStop();

    private native void nativeSurfaceCreated(Surface surface);

    private native void nativeSurfaceChanged(Surface surface, int width, int height);

    private native void nativeSurfaceDestroyed();

    private native void nativeFocus(boolean focused);

    private native void nativeTouch(int buttons, int x, int y, boolean active);

    private native void nativeGamepad(int deviceId, String name, int buttons, int x, int y, boolean connected);

    private native void nativeKey(int keyCode, boolean down);

    private native void nativeTextResult(String text, boolean accepted);
}
