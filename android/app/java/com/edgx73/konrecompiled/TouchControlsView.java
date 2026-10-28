package com.edgx73.konrecompiled;

import android.content.Context;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.graphics.RectF;
import android.view.MotionEvent;
import android.view.View;

/**
 * Translucent on-screen PSP pad: d-pad, face buttons, L/R and Start/Select.
 * It is hidden by GameActivity while a physical controller is connected.
 */
final class TouchControlsView extends View {
    interface Callback {
        void onState(int buttons, int x, int y, boolean active);
    }

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

    private final Paint fill = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint stroke = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint text = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final RectF lRect = new RectF();
    private final RectF rRect = new RectF();
    private float dpadX, dpadY, dpadRadius;
    private float faceX, faceY, faceRadius;
    private float startX, startY, selectX, selectY, smallRadius;
    private int buttons;
    private int analogX = 128, analogY = 128;
    private boolean active;
    private Callback callback;

    TouchControlsView(Context context) {
        super(context);
        setFocusable(false);
        setClickable(true);
        fill.setColor(0x55FFFFFF);
        stroke.setColor(0x99FFFFFF);
        stroke.setStyle(Paint.Style.STROKE);
        stroke.setStrokeWidth(dp(2));
        text.setColor(0xCCFFFFFF);
        text.setTextAlign(Paint.Align.CENTER);
        text.setTextSize(dp(18));
        text.setFakeBoldText(true);
    }

    void setCallback(Callback value) {
        callback = value;
    }

    private float dp(float value) {
        return value * getResources().getDisplayMetrics().density;
    }

    @Override
    protected void onSizeChanged(int w, int h, int oldw, int oldh) {
        super.onSizeChanged(w, h, oldw, oldh);
        final float min = Math.min(w, h);
        dpadRadius = min * 0.16f;
        dpadX = w * 0.16f;
        dpadY = h * 0.60f;
        faceRadius = min * 0.085f;
        faceX = w * 0.84f;
        faceY = h * 0.60f;
        smallRadius = min * 0.035f;
        startX = w * 0.56f;
        selectX = w * 0.44f;
        startY = selectY = h * 0.90f;
        final float bar = min * 0.07f;
        lRect.set(w * 0.06f, h * 0.06f, w * 0.06f + bar * 1.7f, h * 0.06f + bar);
        rRect.set(w * 0.94f - bar * 1.7f, h * 0.06f, w * 0.94f, h * 0.06f + bar);
    }

    @Override
    protected void onDraw(Canvas canvas) {
        super.onDraw(canvas);
        // d-pad
        fill.setColor(0x33FFFFFF);
        canvas.drawCircle(dpadX, dpadY, dpadRadius, fill);
        drawDirection(canvas, "UP", dpadX, dpadY - dpadRadius * 0.62f, PSP_UP);
        drawDirection(canvas, "DN", dpadX, dpadY + dpadRadius * 0.62f, PSP_DOWN);
        drawDirection(canvas, "<", dpadX - dpadRadius * 0.62f, dpadY, PSP_LEFT);
        drawDirection(canvas, ">", dpadX + dpadRadius * 0.62f, dpadY, PSP_RIGHT);
        // face buttons
        drawFace(canvas, "T", faceX, faceY - faceRadius * 1.25f, PSP_TRIANGLE);
        drawFace(canvas, "O", faceX + faceRadius * 1.25f, faceY, PSP_CIRCLE);
        drawFace(canvas, "X", faceX, faceY + faceRadius * 1.25f, PSP_CROSS);
        drawFace(canvas, "S", faceX - faceRadius * 1.25f, faceY, PSP_SQUARE);
        // shoulders
        fill.setColor((buttons & PSP_L) != 0 ? 0x99FFFFFF : 0x33FFFFFF);
        canvas.drawRoundRect(lRect, dp(8), dp(8), fill);
        fill.setColor((buttons & PSP_R) != 0 ? 0x99FFFFFF : 0x33FFFFFF);
        canvas.drawRoundRect(rRect, dp(8), dp(8), fill);
        text.setTextSize(dp(13));
        canvas.drawText("L", lRect.centerX(), lRect.centerY() + dp(5), text);
        canvas.drawText("R", rRect.centerX(), rRect.centerY() + dp(5), text);
        // start/select
        drawSmall(canvas, "SELECT", selectX, selectY, PSP_SELECT);
        drawSmall(canvas, "START", startX, startY, PSP_START);
    }

    private void drawDirection(Canvas canvas, String label, float x, float y, int bit) {
        text.setTextSize(dp(17));
        text.setColor((buttons & bit) != 0 ? Color.WHITE : 0xAAFFFFFF);
        canvas.drawText(label, x, y + dp(6), text);
    }

    private void drawFace(Canvas canvas, String label, float x, float y, int bit) {
        fill.setColor((buttons & bit) != 0 ? 0xAAFFFFFF : 0x33FFFFFF);
        canvas.drawCircle(x, y, faceRadius, fill);
        text.setTextSize(dp(20));
        text.setColor((buttons & bit) != 0 ? Color.BLACK : Color.WHITE);
        canvas.drawText(label, x, y + dp(7), text);
    }

    private void drawSmall(Canvas canvas, String label, float x, float y, int bit) {
        fill.setColor((buttons & bit) != 0 ? 0xAAFFFFFF : 0x33FFFFFF);
        canvas.drawCircle(x, y, smallRadius, fill);
        text.setTextSize(dp(8));
        text.setColor((buttons & bit) != 0 ? Color.BLACK : Color.WHITE);
        canvas.drawText(label, x, y + dp(3), text);
    }

    @Override
    public boolean onTouchEvent(MotionEvent event) {
        final int action = event.getActionMasked();
        if (action != MotionEvent.ACTION_DOWN && action != MotionEvent.ACTION_MOVE
                && action != MotionEvent.ACTION_UP && action != MotionEvent.ACTION_POINTER_DOWN
                && action != MotionEvent.ACTION_POINTER_UP && action != MotionEvent.ACTION_CANCEL) {
            return super.onTouchEvent(event);
        }
        buttons = 0;
        analogX = analogY = 128;
        active = action != MotionEvent.ACTION_UP && action != MotionEvent.ACTION_CANCEL
                && event.getPointerCount() > 0;
        for (int i = 0; i < event.getPointerCount(); i++) {
            hit(event.getX(i), event.getY(i), i == 0);
        }
        if (action == MotionEvent.ACTION_UP || action == MotionEvent.ACTION_CANCEL) {
            buttons = 0;
            analogX = analogY = 128;
            active = false;
        }
        invalidate();
        if (callback != null) {
            callback.onState(buttons, analogX, analogY, active);
        }
        return true;
    }

    private void hit(float x, float y, boolean first) {
        // d-pad: direction bits and an analog vector from its centre
        final float dx = x - dpadX, dy = y - dpadY;
        final float distance = (float) Math.hypot(dx, dy);
        if (distance <= dpadRadius) {
            if (distance > dpadRadius * 0.18f) {
                final double angle = Math.atan2(dy, dx);
                final boolean right = Math.cos(angle) > 0.38;
                final boolean left = Math.cos(angle) < -0.38;
                final boolean down = Math.sin(angle) > 0.38;
                final boolean up = Math.sin(angle) < -0.38;
                if (up) buttons |= PSP_UP;
                if (down) buttons |= PSP_DOWN;
                if (left) buttons |= PSP_LEFT;
                if (right) buttons |= PSP_RIGHT;
                analogX = clamp(128 + Math.round((dx / dpadRadius) * 127f));
                analogY = clamp(128 + Math.round((dy / dpadRadius) * 127f));
            }
            return;
        }
        if (circle(x, y, faceX, faceY - faceRadius * 1.25f, faceRadius)) { buttons |= PSP_TRIANGLE; return; }
        if (circle(x, y, faceX + faceRadius * 1.25f, faceY, faceRadius)) { buttons |= PSP_CIRCLE; return; }
        if (circle(x, y, faceX, faceY + faceRadius * 1.25f, faceRadius)) { buttons |= PSP_CROSS; return; }
        if (circle(x, y, faceX - faceRadius * 1.25f, faceY, faceRadius)) { buttons |= PSP_SQUARE; return; }
        if (lRect.contains(x, y)) { buttons |= PSP_L; return; }
        if (rRect.contains(x, y)) { buttons |= PSP_R; return; }
        if (circle(x, y, selectX, selectY, smallRadius)) { buttons |= PSP_SELECT; return; }
        if (circle(x, y, startX, startY, smallRadius)) { buttons |= PSP_START; }
    }

    private static int clamp(int value) {
        return Math.max(0, Math.min(255, value));
    }

    private static boolean circle(float x, float y, float cx, float cy, float radius) {
        return Math.hypot(x - cx, y - cy) <= radius;
    }
}
