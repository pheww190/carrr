package com.example.blerover;

import android.content.Context;
import android.graphics.Canvas;
import android.graphics.Paint;
import android.graphics.Path;
import android.graphics.RectF;
import android.util.AttributeSet;
import android.view.MotionEvent;
import android.view.View;

/**
 * Sectored, snapping joystick.
 *
 * The dial is divided into 8 sectors of 45 degrees. Each of the four
 * directions owns TWO adjacent sectors - a 90 degree cone - so a small
 * angular error does not flip you into a neighbouring direction:
 *
 *            FRONT  (225..315)
 *      LEFT             RIGHT
 *    (135..225)        (315..45)
 *            BACK   (45..135)
 *
 * (angles are Android canvas angles: 0 = right, increasing clockwise)
 *
 * Behaviour:
 *   - inside DEADZONE of the centre  -> NONE (neutral)
 *   - outside it                     -> the nearest cone, with magnitude
 *                                       remapped from 0 (edge of deadzone)
 *                                       to 1 (full deflection)
 *
 * So pushing 30 degrees off-forward gives PURE forward, not forward+left.
 */
public class JoystickView extends View {

    public enum Dir { NONE, FRONT, RIGHT, BACK, LEFT }

    public interface Listener {
        void onMove(Dir dir, float magnitude);   // magnitude 0..1
    }

    /** fraction of the radius that counts as "centred" */
    private static final float DEADZONE = 0.18f;

    private final Paint basePaint  = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint linePaint  = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint wedgePaint = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint arrowPaint = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint knobPaint  = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Path  tri        = new Path();
    private final RectF arcRect    = new RectF();

    private float cx, cy, radius;
    private float kx, ky;
    private Dir dir = Dir.NONE;
    private float mag = 0f;
    private Listener listener;

    public JoystickView(Context c) { super(c); init(); }
    public JoystickView(Context c, AttributeSet a) { super(c, a); init(); }

    private void init() {
        basePaint.setColor(0xFF16202B);
        basePaint.setStyle(Paint.Style.FILL);

        linePaint.setColor(0xFF22303F);
        linePaint.setStyle(Paint.Style.STROKE);
        linePaint.setStrokeWidth(2f);

        wedgePaint.setColor(0xFF38E0A5);
        wedgePaint.setStyle(Paint.Style.FILL);

        arrowPaint.setStyle(Paint.Style.FILL);

        knobPaint.setColor(0xFF38E0A5);
        knobPaint.setStyle(Paint.Style.FILL);

        setFocusable(true);
    }

    public void setListener(Listener l) { this.listener = l; }

    public Dir getDir() { return dir; }
    public float getMagnitude() { return mag; }

    @Override
    protected void onSizeChanged(int w, int h, int ow, int oh) {
        cx = w / 2f;
        cy = h / 2f;
        radius = Math.min(w, h) / 2f * 0.82f;
        arcRect.set(cx - radius, cy - radius, cx + radius, cy + radius);
        kx = cx;
        ky = cy;
    }

    @Override
    protected void onDraw(Canvas canvas) {
        // dial
        canvas.drawCircle(cx, cy, radius, basePaint);

        // active 90-degree cone
        if (dir != Dir.NONE) {
            wedgePaint.setAlpha(46);
            canvas.drawArc(arcRect, coneStart(dir), 90f, true, wedgePaint);
        }

        // 8 sector dividers (every 45 degrees)
        for (int a = 0; a < 360; a += 45) {
            double r = Math.toRadians(a);
            float ex = cx + (float) (Math.cos(r) * radius);
            float ey = cy + (float) (Math.sin(r) * radius);
            canvas.drawLine(cx, cy, ex, ey, linePaint);
        }

        // centre ring
        canvas.drawCircle(cx, cy, radius * DEADZONE, linePaint);

        // 4 arrows at the cone centres
        drawArrow(canvas, 270f, dir == Dir.FRONT);   // up
        drawArrow(canvas, 0f,   dir == Dir.RIGHT);   // right
        drawArrow(canvas, 90f,  dir == Dir.BACK);    // down
        drawArrow(canvas, 180f, dir == Dir.LEFT);    // left

        // knob
        canvas.drawCircle(kx, ky, radius * 0.26f, knobPaint);
    }

    private static float coneStart(Dir d) {
        switch (d) {
            case FRONT: return 225f;   // 225..315, centred on up
            case RIGHT: return 315f;   // 315..45,  centred on right
            case BACK:  return 45f;    // 45..135,  centred on down
            case LEFT:  return 135f;   // 135..225, centred on left
            default:    return 0f;
        }
    }

    private void drawArrow(Canvas c, float angleDeg, boolean active) {
        c.save();
        c.rotate(angleDeg, cx, cy);
        float tip  = cx + radius * 0.94f;
        float base = cx + radius * 0.74f;
        float half = radius * 0.10f;
        tri.reset();
        tri.moveTo(tip, cy);
        tri.lineTo(base, cy - half);
        tri.lineTo(base, cy + half);
        tri.close();
        arrowPaint.setColor(active ? 0xFF38E0A5 : 0xFF5D7288);
        c.drawPath(tri, arrowPaint);
        c.restore();
    }

    @Override
    public boolean onTouchEvent(MotionEvent e) {
        switch (e.getActionMasked()) {
            case MotionEvent.ACTION_DOWN:
            case MotionEvent.ACTION_MOVE:
                apply(e.getX(), e.getY());
                return true;
            case MotionEvent.ACTION_UP:
            case MotionEvent.ACTION_CANCEL:
                reset();
                return true;
            default:
                return super.onTouchEvent(e);
        }
    }

    private void apply(float x, float y) {
        float dx = x - cx;
        float dy = y - cy;
        float d  = (float) Math.hypot(dx, dy);

        // clamp the knob to the dial
        float cl = Math.min(d, radius);
        if (d > 0f) {
            kx = cx + dx / d * cl;
            ky = cy + dy / d * cl;
        }

        float frac = cl / radius;
        if (frac < DEADZONE) {
            dir = Dir.NONE;
            mag = 0f;
        } else {
            double ang = Math.toDegrees(Math.atan2(dy, dx));   // -180..180
            if (ang < 0) ang += 360;                            // 0..360

            if (ang >= 225 && ang < 315)      dir = Dir.FRONT;
            else if (ang >= 315 || ang < 45)  dir = Dir.RIGHT;
            else if (ang >= 45  && ang < 135) dir = Dir.BACK;
            else                              dir = Dir.LEFT;

            mag = (frac - DEADZONE) / (1f - DEADZONE);
            if (mag > 1f) mag = 1f;
        }

        invalidate();
        if (listener != null) listener.onMove(dir, mag);
    }

    private void reset() {
        kx = cx;
        ky = cy;
        dir = Dir.NONE;
        mag = 0f;
        invalidate();
        if (listener != null) listener.onMove(Dir.NONE, 0f);
    }
}
