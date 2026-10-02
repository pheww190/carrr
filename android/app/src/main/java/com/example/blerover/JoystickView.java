package com.example.blerover;

import android.content.Context;
import android.graphics.Canvas;
import android.graphics.Paint;
import android.util.AttributeSet;
import android.view.MotionEvent;
import android.view.View;

/**
 * Simple on-screen joystick.
 *
 * Outputs two axes in the range -1..+1:
 *   x : -1 = left,  +1 = right
 *   y : -1 = down,  +1 = up   (screen up = forward)
 *
 * The view is self-contained and reports movement through a Listener; the
 * activity samples getXAxis()/getYAxis() at its own send rate.
 */
public class JoystickView extends View {

    public interface Listener {
        void onMove(float x, float y);
    }

    private final Paint basePaint = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint ringPaint = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint knobPaint = new Paint(Paint.ANTI_ALIAS_FLAG);

    private float cx, cy, radius;
    private float kx, ky;
    private float nx, ny;
    private Listener listener;

    public JoystickView(Context c) {
        super(c);
        init();
    }

    public JoystickView(Context c, AttributeSet a) {
        super(c, a);
        init();
    }

    private void init() {
        basePaint.setColor(0xFF16202B);
        basePaint.setStyle(Paint.Style.FILL);

        ringPaint.setColor(0xFF2B4058);
        ringPaint.setStyle(Paint.Style.STROKE);
        ringPaint.setStrokeWidth(4f);

        knobPaint.setColor(0xFF38E0A5);
        knobPaint.setStyle(Paint.Style.FILL);

        setFocusable(true);
    }

    public void setListener(Listener l) {
        this.listener = l;
    }

    public float getXAxis() {
        return nx;
    }

    public float getYAxis() {
        return ny;
    }

    @Override
    protected void onSizeChanged(int w, int h, int oldw, int oldh) {
        cx = w / 2f;
        cy = h / 2f;
        radius = Math.min(w, h) / 2f * 0.85f;
        kx = cx;
        ky = cy;
    }

    @Override
    protected void onDraw(Canvas canvas) {
        canvas.drawCircle(cx, cy, radius, basePaint);
        canvas.drawCircle(cx, cy, radius, ringPaint);
        canvas.drawCircle(cx, cy, radius * 0.35f, ringPaint);
        canvas.drawCircle(kx, ky, radius * 0.28f, knobPaint);
    }

    @Override
    public boolean onTouchEvent(MotionEvent e) {
        switch (e.getActionMasked()) {
            case MotionEvent.ACTION_DOWN:
            case MotionEvent.ACTION_MOVE:
                move(e.getX(), e.getY());
                return true;
            case MotionEvent.ACTION_UP:
            case MotionEvent.ACTION_CANCEL:
                reset();
                return true;
            default:
                return super.onTouchEvent(e);
        }
    }

    private void move(float x, float y) {
        float dx = x - cx;
        float dy = y - cy;
        float d = (float) Math.hypot(dx, dy);
        if (d > radius) {
            dx = dx / d * radius;
            dy = dy / d * radius;
        }
        kx = cx + dx;
        ky = cy + dy;
        nx = dx / radius;       // right = +x
        ny = -dy / radius;      // up    = +y
        invalidate();
        if (listener != null) {
            listener.onMove(nx, ny);
        }
    }

    private void reset() {
        kx = cx;
        ky = cy;
        nx = 0f;
        ny = 0f;
        invalidate();
        if (listener != null) {
            listener.onMove(0f, 0f);
        }
    }
}
