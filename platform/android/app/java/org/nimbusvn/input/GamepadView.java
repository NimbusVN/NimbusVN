package org.nimbusvn.input;

import android.content.Context;
import android.graphics.Canvas;
import android.graphics.DashPathEffect;
import android.graphics.Paint;
import android.graphics.Path;
import android.graphics.RectF;
import android.graphics.Typeface;
import android.view.MotionEvent;
import android.view.View;

import java.util.ArrayList;
import java.util.List;

/**
 * 虚拟键盘：一套可自由增删、拖动、缩放的按键。
 *
 * <p>默认只加载一套内置键位（方向盘 + ABXY + L/R + 菜单/跳过），
 * 之后每个键都是独立的 {@link VirtualKey}：位置、大小、映射的物理键
 * 都在设置页里单独调整。鼠标层的触控板不会抢手柄按键的触点。</p>
 */
public class GamepadView extends View {

    public interface Listener {
        void onButtonSelected(VirtualKey key);

        void onSelectionCleared();
    }

    private static final class Button {
        final VirtualKey spec;
        float x;
        float y;
        float r;

        int pointer = -1;
        boolean pressed;
        int heldKey;

        Button(VirtualKey spec) {
            this.spec = spec;
        }
    }

    private final InputPrefs prefs;
    private final SdlInjector injector = new SdlInjector();
    private final List<Button> buttons = new ArrayList<>();

    private final Paint fillPaint = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint strokePaint = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint textPaint = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint dashPaint = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint hintPaint = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Path path = new Path();
    private final RectF rect = new RectF();

    private final float density;
    private float viewWidth;
    private float viewHeight;
    private float minSize;
    private float scale;

    private boolean editMode;
    private Listener listener;
    private Button selected;
    private float dragLastX;
    private float dragLastY;
    private String dpadDirection;
    private int dpadHeldKey;

    public GamepadView(Context context) {
        super(context);
        prefs = InputPrefs.get(context);
        density = getResources().getDisplayMetrics().density;
        scale = prefs.scale();

        setFocusable(false);
        setClickable(false);

        textPaint.setTypeface(Typeface.create(Typeface.DEFAULT, Typeface.BOLD));
        textPaint.setTextAlign(Paint.Align.CENTER);
        strokePaint.setStyle(Paint.Style.STROKE);
        dashPaint.setStyle(Paint.Style.STROKE);
        dashPaint.setPathEffect(new DashPathEffect(new float[]{dp(7), dp(5)}, 0));
        hintPaint.setTextAlign(Paint.Align.CENTER);

        reloadKeys();
    }

    public void setListener(Listener listener) {
        this.listener = listener;
    }

    /** 重新从配置读取整套按键（增删、改键位、恢复默认后调用）。 */
    public void reloadKeys() {
        releaseAll();
        scale = prefs.scale();
        buttons.clear();
        for (VirtualKey key : prefs.keys()) {
            buttons.add(new Button(key));
        }
        boolean hadSelection = selected != null;
        selected = null;
        updatePositions();
        invalidate();
        if (hadSelection && listener != null) listener.onSelectionCleared();
    }

    public void refresh() {
        scale = prefs.scale();
        updatePositions();
        invalidate();
    }

    public void setEditMode(boolean enabled) {
        editMode = enabled;
        if (enabled) {
            releaseAll();
        } else {
            selected = null;
            if (listener != null) listener.onSelectionCleared();
        }
        invalidate();
    }

    public boolean isEditMode() {
        return editMode;
    }

    public boolean isOverControl(float x, float y) {
        return hitTest(x, y) != null;
    }

    public void releaseAllKeys() {
        releaseAll();
    }

    public void setSelectedScale(float value) {
        if (selected == null) return;
        selected.spec.scale = clamp(value, 0.4f, 2.5f);
        updatePositions();
        saveKeys();
        invalidate();
        if (listener != null) listener.onButtonSelected(selected.spec);
    }

    public void deleteSelectedKey() {
        if (selected == null) return;
        buttons.remove(selected);
        selected = null;
        updatePositions();
        saveKeys();
        invalidate();
        if (listener != null) listener.onSelectionCleared();
    }

    /** 从设置页添加一个按键，并直接选中它方便马上拖动。 */
    public void addKey(String keyId) {
        VirtualKey spec = VirtualKey.create(keyId, buttons.size());
        Button button = new Button(spec);
        buttons.add(button);
        updatePositions();
        saveKeys();
        selected = button;
        invalidate();
        if (listener != null) listener.onButtonSelected(spec);
    }

    private void saveKeys() {
        List<VirtualKey> list = new ArrayList<>();
        for (Button button : buttons) {
            list.add(button.spec);
        }
        prefs.saveKeys(list);
    }

    @Override
    protected void onSizeChanged(int w, int h, int oldw, int oldh) {
        super.onSizeChanged(w, h, oldw, oldh);
        viewWidth = w;
        viewHeight = h;
        minSize = Math.min(w, h);
        updatePositions();
    }

    private void updatePositions() {
        if (viewWidth <= 0 || minSize <= 0) return;
        for (Button button : buttons) {
            button.x = button.spec.x * viewWidth;
            button.y = button.spec.y * viewHeight;
            button.r = baseRadius(button.spec) * minSize * scale * button.spec.scale;
        }
    }

    private static float baseRadius(VirtualKey spec) {
        if (spec.shape == VirtualKey.SHAPE_DPAD) return 0.165f;
        if (spec.shape == VirtualKey.SHAPE_PILL) return 0.034f;
        return 0.060f;
    }

    private float pillHalfWidth(Button button) {
        return button.r * 2.05f;
    }

    private float dp(float value) {
        return value * density;
    }

    @Override
    protected void onDraw(Canvas canvas) {
        super.onDraw(canvas);
        if (minSize <= 0) return;

        for (Button button : buttons) {
            if (editMode) drawEditBounds(canvas, button);
            if (button.spec.shape == VirtualKey.SHAPE_DPAD) {
                drawDpad(canvas, button);
            } else if (button.spec.shape == VirtualKey.SHAPE_PILL) {
                drawPill(canvas, button);
            } else {
                drawCircleButton(canvas, button);
            }
        }

        if (editMode) {
            hintPaint.setColor(0xCCFFFFFF);
            hintPaint.setTextSize(dp(12));
            canvas.drawText("点选按键后拖动位置；底部滑杆调大小 / 删除",
                    viewWidth / 2f, dp(58), hintPaint);
        }
    }

    private void drawCircleButton(Canvas canvas, Button button) {
        float r = button.r;
        int accent = button.spec.color;

        fillPaint.setColor(button.pressed ? withAlpha(accent, 0x88) : 0x66000000);
        canvas.drawCircle(button.x, button.y, r, fillPaint);

        strokePaint.setStrokeWidth(dp(2));
        strokePaint.setColor(withAlpha(accent, button.pressed ? 255 : 205));
        canvas.drawCircle(button.x, button.y, r, strokePaint);

        textPaint.setColor(0xFFF2F5FA);
        drawCenteredText(canvas, button.spec.label, button.x, button.y, r * 0.86f);
    }

    private void drawPill(Canvas canvas, Button button) {
        float hw = pillHalfWidth(button);
        float hh = button.r;
        rect.set(button.x - hw, button.y - hh, button.x + hw, button.y + hh);

        fillPaint.setColor(button.pressed
                ? withAlpha(button.spec.color, 0x88) : 0x66000000);
        canvas.drawRoundRect(rect, hh, hh, fillPaint);

        strokePaint.setStrokeWidth(dp(1.5f));
        strokePaint.setColor(withAlpha(button.spec.color, button.pressed ? 255 : 190));
        canvas.drawRoundRect(rect, hh, hh, strokePaint);

        textPaint.setColor(0xFFE8EDF5);
        float size = hh * 0.95f;
        if (button.spec.label.length() > 5) size = hh * 0.72f;
        drawCenteredText(canvas, button.spec.label, button.x, button.y, size);
    }

    private void drawDpad(Canvas canvas, Button button) {
        float r = button.r;

        fillPaint.setColor(0x55000000);
        canvas.drawCircle(button.x, button.y, r, fillPaint);

        strokePaint.setStrokeWidth(dp(2));
        strokePaint.setColor(withAlpha(button.spec.color, 190));
        canvas.drawCircle(button.x, button.y, r, strokePaint);

        float s = r * 0.30f;
        float offset = r * 0.52f;
        drawArrow(canvas, button.x, button.y - offset, s, "UP", button);
        drawArrow(canvas, button.x, button.y + offset, s, "DOWN", button);
        drawArrow(canvas, button.x - offset, button.y, s, "LEFT", button);
        drawArrow(canvas, button.x + offset, button.y, s, "RIGHT", button);

        fillPaint.setColor(withAlpha(button.spec.color, 70));
        canvas.drawCircle(button.x, button.y, r * 0.15f, fillPaint);
    }

    private void drawArrow(Canvas canvas, float x, float y, float s,
                           String direction, Button button) {
        path.reset();
        if ("UP".equals(direction)) {
            path.moveTo(x, y - s);
            path.lineTo(x - s * 0.78f, y + s * 0.55f);
            path.lineTo(x + s * 0.78f, y + s * 0.55f);
        } else if ("DOWN".equals(direction)) {
            path.moveTo(x, y + s);
            path.lineTo(x - s * 0.78f, y - s * 0.55f);
            path.lineTo(x + s * 0.78f, y - s * 0.55f);
        } else if ("LEFT".equals(direction)) {
            path.moveTo(x - s, y);
            path.lineTo(x + s * 0.55f, y - s * 0.78f);
            path.lineTo(x + s * 0.55f, y + s * 0.78f);
        } else {
            path.moveTo(x + s, y);
            path.lineTo(x - s * 0.55f, y - s * 0.78f);
            path.lineTo(x - s * 0.55f, y + s * 0.78f);
        }
        path.close();

        boolean active = button.pressed && direction.equals(dpadDirection);
        fillPaint.setColor(active
                ? withAlpha(button.spec.color, 230) : 0xB3EDF2FA);
        canvas.drawPath(path, fillPaint);
    }

    private void drawEditBounds(Canvas canvas, Button button) {
        boolean active = button == selected;
        float pad = dp(6);
        dashPaint.setStrokeWidth(active ? dp(2f) : dp(1.1f));
        dashPaint.setColor(active ? 0xFFFFD36E : 0x66FFFFFF);

        if (button.spec.shape == VirtualKey.SHAPE_PILL) {
            rect.set(button.x - pillHalfWidth(button) - pad,
                    button.y - button.r - pad,
                    button.x + pillHalfWidth(button) + pad,
                    button.y + button.r + pad);
            canvas.drawRoundRect(rect, dp(10), dp(10), dashPaint);
        } else {
            canvas.drawCircle(button.x, button.y, button.r + pad, dashPaint);
        }
    }

    private void drawCenteredText(Canvas canvas, String value, float x, float y, float size) {
        textPaint.setTextSize(size);
        Paint.FontMetrics metrics = textPaint.getFontMetrics();
        float baseline = y - (metrics.ascent + metrics.descent) / 2f;
        canvas.drawText(value, x, baseline, textPaint);
    }

    private static int withAlpha(int color, int alpha) {
        return (color & 0x00FFFFFF) | ((alpha & 0xFF) << 24);
    }

    @Override
    public boolean onTouchEvent(MotionEvent event) {
        if (editMode) return handleEditTouch(event);

        int action = event.getActionMasked();
        if (action == MotionEvent.ACTION_DOWN || action == MotionEvent.ACTION_POINTER_DOWN) {
            int index = event.getActionIndex();
            float x = event.getX(index);
            float y = event.getY(index);
            Button button = hitTest(x, y);
            if (button == null) {
                return action == MotionEvent.ACTION_POINTER_DOWN;
            }
            if (button.pressed) {
                // 第二根手指按到已按下的键：不重绑指针，保持原来的按下状态。
                return true;
            }
            press(button, event.getPointerId(index), x, y);
            invalidate();
            return true;
        }

        if (action == MotionEvent.ACTION_MOVE) {
            boolean handled = false;
            for (int i = 0; i < event.getPointerCount(); i++) {
                Button button = findPressed(event.getPointerId(i));
                if (button == null) continue;
                handled = true;
                if (button.spec.isDpad()) {
                    updateDpadDirection(button, event.getX(i), event.getY(i));
                }
            }
            return handled;
        }

        if (action == MotionEvent.ACTION_UP || action == MotionEvent.ACTION_POINTER_UP) {
            int index = event.getActionIndex();
            Button button = findPressed(event.getPointerId(index));
            if (button != null) release(button);
            int remaining = event.getPointerCount();
            if (action == MotionEvent.ACTION_POINTER_UP) remaining--;
            if (remaining <= 0) releaseAll();
            invalidate();
            return true;
        }

        if (action == MotionEvent.ACTION_CANCEL) {
            releaseAll();
            invalidate();
            return true;
        }

        return true;
    }

    private Button hitTest(float x, float y) {
        for (int i = buttons.size() - 1; i >= 0; i--) {
            Button button = buttons.get(i);
            if (button.spec.shape == VirtualKey.SHAPE_PILL) {
                float hw = pillHalfWidth(button);
                float hh = button.r;
                if (x >= button.x - hw && x <= button.x + hw
                        && y >= button.y - hh && y <= button.y + hh) {
                    return button;
                }
            } else {
                float dx = x - button.x;
                float dy = y - button.y;
                if (dx * dx + dy * dy <= button.r * button.r) return button;
            }
        }
        return null;
    }

    private Button findPressed(int pointerId) {
        for (Button button : buttons) {
            if (button.pressed && button.pointer == pointerId) return button;
        }
        return null;
    }

    private void press(Button button, int pointerId, float x, float y) {
        button.pointer = pointerId;
        button.pressed = true;
        if (button.spec.isDpad()) {
            dpadDirection = directionAt(button, x, y);
            if (dpadDirection != null) {
                dpadHeldKey = KeyCatalog.keycode(prefs.keyFor(dpadDirection));
                injector.keyDown(dpadHeldKey);
            }
        } else {
            button.heldKey = KeyCatalog.keycode(button.spec.keyId);
            injector.keyDown(button.heldKey);
        }
    }

    private void updateDpadDirection(Button button, float x, float y) {
        String direction = directionAt(button, x, y);
        if (direction == null || direction.equals(dpadDirection)) return;
        if (dpadHeldKey != 0) injector.keyUp(dpadHeldKey);
        dpadDirection = direction;
        dpadHeldKey = KeyCatalog.keycode(prefs.keyFor(direction));
        injector.keyDown(dpadHeldKey);
        invalidate();
    }

    private String directionAt(Button button, float x, float y) {
        float dx = x - button.x;
        float dy = y - button.y;
        float deadZone = button.r * 0.16f;
        if (dx * dx + dy * dy < deadZone * deadZone) return null;
        if (Math.abs(dx) > Math.abs(dy)) {
            return dx > 0 ? "RIGHT" : "LEFT";
        }
        return dy > 0 ? "DOWN" : "UP";
    }

    private void release(Button button) {
        if (button.spec.isDpad()) {
            if (dpadHeldKey != 0) injector.keyUp(dpadHeldKey);
            dpadHeldKey = 0;
            dpadDirection = null;
        } else if (button.heldKey != 0) {
            injector.keyUp(button.heldKey);
            button.heldKey = 0;
        }
        button.pressed = false;
        button.pointer = -1;
    }

    private void releaseAll() {
        for (Button button : buttons) {
            if (button.pressed) release(button);
        }
        injector.releaseAll();
        dpadDirection = null;
        dpadHeldKey = 0;
    }

    private boolean handleEditTouch(MotionEvent event) {
        int action = event.getActionMasked();
        if (action == MotionEvent.ACTION_DOWN) {
            float x = event.getX();
            float y = event.getY();
            selected = hitTest(x, y);
            dragLastX = x;
            dragLastY = y;
            if (selected != null) {
                if (listener != null) listener.onButtonSelected(selected.spec);
            } else if (listener != null) {
                listener.onSelectionCleared();
            }
            invalidate();
            return true;
        }

        if (action == MotionEvent.ACTION_MOVE) {
            if (selected != null) {
                float x = event.getX();
                float y = event.getY();
                float dx = x - dragLastX;
                float dy = y - dragLastY;
                dragLastX = x;
                dragLastY = y;
                selected.spec.x = clamp(selected.spec.x + dx / viewWidth, 0.01f, 0.99f);
                selected.spec.y = clamp(selected.spec.y + dy / viewHeight, 0.01f, 0.99f);
                selected.x = selected.spec.x * viewWidth;
                selected.y = selected.spec.y * viewHeight;
                invalidate();
            }
            return true;
        }

        if (action == MotionEvent.ACTION_UP || action == MotionEvent.ACTION_CANCEL) {
            if (selected != null) saveKeys();
            return true;
        }

        return true;
    }

    private static float clamp(float value, float min, float max) {
        return value < min ? min : (value > max ? max : value);
    }

    @Override
    protected void onWindowVisibilityChanged(int visibility) {
        super.onWindowVisibilityChanged(visibility);
        if (visibility != VISIBLE) releaseAll();
    }

    @Override
    protected void onDetachedFromWindow() {
        releaseAll();
        super.onDetachedFromWindow();
    }
}
