package com.edgx73.konrecompiled;

import android.content.Context;
import android.view.View;
import android.widget.FrameLayout;

/** Centres one child at a fixed aspect ratio (the PSP's 480x272 screen). */
final class AspectRatioLayout extends FrameLayout {
    private final float ratio;

    AspectRatioLayout(Context context, float ratio) {
        super(context);
        this.ratio = ratio;
    }

    @Override
    protected void onMeasure(int widthMeasureSpec, int heightMeasureSpec) {
        final int width = MeasureSpec.getSize(widthMeasureSpec);
        final int height = MeasureSpec.getSize(heightMeasureSpec);
        int childWidth = width;
        int childHeight = Math.round(width / ratio);
        if (childHeight > height) {
            childHeight = height;
            childWidth = Math.round(height * ratio);
        }
        final int childSpecW = MeasureSpec.makeMeasureSpec(childWidth, MeasureSpec.EXACTLY);
        final int childSpecH = MeasureSpec.makeMeasureSpec(childHeight, MeasureSpec.EXACTLY);
        for (int i = 0; i < getChildCount(); i++) {
            getChildAt(i).measure(childSpecW, childSpecH);
        }
        setMeasuredDimension(width, height);
    }

    @Override
    protected void onLayout(boolean changed, int left, int top, int right, int bottom) {
        final int width = right - left;
        final int height = bottom - top;
        for (int i = 0; i < getChildCount(); i++) {
            final View child = getChildAt(i);
            final int childWidth = child.getMeasuredWidth();
            final int childHeight = child.getMeasuredHeight();
            final int x = (width - childWidth) / 2;
            final int y = (height - childHeight) / 2;
            child.layout(x, y, x + childWidth, y + childHeight);
        }
    }
}
