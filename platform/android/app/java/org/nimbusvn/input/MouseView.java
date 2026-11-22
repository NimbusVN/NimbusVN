package org.nimbusvn.input;

import android.content.Context;
import android.graphics.Canvas;
import android.graphics.Paint;
import android.graphics.Path;
import android.view.MotionEvent;
import android.view.View;

/**
 * 虚拟鼠标：只负责定位和 hover，点击全部由手势完成。
 *
 * <ul>
 *   <li>单指拖动 = 移动光标（持续发 MouseMove，触发 hover）</li>
 *   <li>单指轻点 = 在光标当前位置按左键</li>
 *   <li>双指轻点 = 在光标当前位置按右键</li>
 *   <li>双指上下滑 = 滚轮</li>
 * </ul>
 *
 * <p>拖动过程中不会按下任何鼠标键，所以不会出现“拖一下就点了一下”；
 * 屏幕上也没有 L/R/M 圆钮，和笔记本触控板的手感一致。</p>
 */
public class MouseView extends View {

    public interface ControlHitTest {
        boolean isOverControl(float x, float y);
    }

    private static final float TAP_SLOP_DP = 8f;
    private static final long TAP_MAX_MS = 320;
    private static final long MULTI_TAP_MAX_MS = 420;
    private static final long LONG_PRESS_MS = 500;
    private static final float DRAG_START_DP = 6f;

    private final InputPrefs prefs;
    private final SdlInjector injector = new SdlInjector();

    private final Paint fillPaint = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint strokePaint = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint hintPaint = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Path cursorPath = new Path();

    private final float density;
    private float viewWidth;
    private float viewHeight;
    private float mouseScale;
    private boolean hintVisible;

    private float cursorX;
    private float cursorY;
    private boolean cursorPlaced;

    private ControlHitTest controlHitTest;

    private int trackPointer = -1;
    private int multiPointer = -1;
    private float downX;
    private float downY;
    private float lastX;
    private float lastY;
    private long downTime;
    private long multiDownTime;
    private boolean moved;
    private boolean dragCancelled;
    private boolean dragHeld;
    private boolean twoFinger;
    private boolean multiMoved;
    private float wheelAnchor;
    private float wheelAccum;

    private long flashUntil;
    private int flashButton;

    public MouseView(Context context) {
        super(context);
        prefs = InputPrefs.get(context);
        density = getResources().getDisplayMetrics().density;
        mouseScale = prefs.mouseScale();
        hintVisible = prefs.showHint();

        setFocusable(false);
        setClickable(false);

        strokePaint.setStyle(Paint.Style.STROKE);
        hintPaint.setTextAlign(Paint.Align.CENTER);
    }

    public void setControlHitTest(ControlHitTest hitTest) {
        this.controlHitTest = hitTest;
    }

    public void refresh() {
        mouseScale = prefs.mouseScale();
        hintVisible = prefs.showHint();
        invalidate();
    }

    public void setHintVisible(boolean visible) {
        if (hintVisible == visible) return;
        hintVisible = visible;
        invalidate();
    }

    public void releaseAllButtons() {
        endDrag();
        resetGesture();
        injector.releaseAll();
        invalidate();
    }

    @Override
    protected void onSizeChanged(int w, int h, int oldw, int oldh) {
        super.onSizeChanged(w, h, oldw, oldh);
        viewWidth = w;
        viewHeight = h;

        if (!cursorPlaced) {
            cursorX = w * 0.5f;
            cursorY = h * 0.5f;
            cursorPlaced = true;
        }
        clampCursor();
    }

    private float dp(float value) {
        return value * density;
    }

    @Override
    protected void onDraw(Canvas canvas) {
        super.onDraw(canvas);
        if (viewWidth <= 0 || viewHeight <= 0) return;

        // 手指在屏幕上时收起提示，避免一直挡视野。
        if (hintVisible && trackPointer == -1) {
            hintPaint.setColor(0x99E8EDF5);
            hintPaint.setTextSize(dp(11));
            canvas.drawText("拖动移动 · 长按后拖动=按住左键 · 轻点左键 · 双指右键/滚轮",
                    viewWidth / 2f, viewHeight - dp(12), hintPaint);
        }

        if (dragHeld) {
            fillPaint.setColor(0x337FB6FF);
            canvas.drawCircle(cursorX, cursorY, dp(16), fillPaint);
            strokePaint.setStrokeWidth(dp(2));
            strokePaint.setColor(0xCC7FB6FF);
            canvas.drawCircle(cursorX, cursorY, dp(16), strokePaint);
        }

        drawCursor(canvas);
        drawClickFlash(canvas);
    }

    private void drawCursor(Canvas canvas) {
        float s = dp(1.6f) * mouseScale;
        cursorPath.reset();
        cursorPath.moveTo(cursorX, cursorY);
        cursorPath.lineTo(cursorX, cursorY + 18 * s);
        cursorPath.lineTo(cursorX + 4.6f * s, cursorY + 13.4f * s);
        cursorPath.lineTo(cursorX + 7.4f * s, cursorY + 19.6f * s);
        cursorPath.lineTo(cursorX + 10.2f * s, cursorY + 18.2f * s);
        cursorPath.lineTo(cursorX + 7.4f * s, cursorY + 12.2f * s);
        cursorPath.lineTo(cursorX + 13.2f * s, cursorY + 12.2f * s);
        cursorPath.close();

        fillPaint.setColor(0xFFFFFFFF);
        canvas.drawPath(cursorPath, fillPaint);

        strokePaint.setStrokeWidth(dp(1.6f));
        strokePaint.setColor(0xCC1A1D22);
        canvas.drawPath(cursorPath, strokePaint);
    }

    private void drawClickFlash(Canvas canvas) {
        long now = System.currentTimeMillis();
        if (now >= flashUntil) return;

        float t = 1f - (flashUntil - now) / 180f;
        float radius = dp(9) + dp(20) * t;
        int alpha = (int) (150 * (1f - t));
        int color = flashButton == MotionEvent.BUTTON_SECONDARY
                ? 0xFFFF7A7A : 0xFF7FB6FF;

        strokePaint.setStrokeWidth(dp(2));
        strokePaint.setColor((color & 0x00FFFFFF) | (alpha << 24));
        canvas.drawCircle(cursorX, cursorY, radius, strokePaint);

        postInvalidateDelayed(30);
    }

    @Override
    public boolean onTouchEvent(MotionEvent event) {
        int action = event.getActionMasked();

        if (action == MotionEvent.ACTION_DOWN || action == MotionEvent.ACTION_POINTER_DOWN) {
            int index = event.getActionIndex();
            float x = event.getX(index);
            float y = event.getY(index);

            if (controlHitTest != null && controlHitTest.isOverControl(x, y)) {
                // 落点在虚拟键盘的按键上：让给键盘，鼠标不吃这一指。
                return false;
            }

            if (trackPointer == -1) {
                trackPointer = event.getPointerId(index);
                downX = x;
                downY = y;
                lastX = x;
                lastY = y;
                downTime = System.currentTimeMillis();
                moved = false;
                dragCancelled = false;
                dragHeld = false;
                twoFinger = false;
                multiPointer = -1;
                multiMoved = false;
                wheelAccum = 0f;
                wheelAnchor = y;
            } else if (!twoFinger) {
                // 进入双指模式前，如果已经长按拖拽，先松开左键。
                endDrag();
                twoFinger = true;
                multiPointer = event.getPointerId(index);
                multiDownTime = System.currentTimeMillis();
                // 第一根手指如果已经在移动，第二根加入后不能算双指轻点。
                multiMoved = moved;
                // 以两指平均位置为锚点，避免只跟第二根手指导致“滚了却不算滚”。
                wheelAnchor = (lastY + y) * 0.5f;
                wheelAccum = 0f;
            } else {
                // 第三根及以上手指忽略，避免把双指状态重置掉。
                return true;
            }
            invalidate();
            return true;
        }

        if (action == MotionEvent.ACTION_MOVE) {
            if (twoFinger) {
                int firstIndex = findPointerIndex(event, trackPointer);
                int secondIndex = findPointerIndex(event, multiPointer);
                if (firstIndex < 0 && secondIndex < 0) return true;

                float y;
                if (firstIndex >= 0 && secondIndex >= 0) {
                    y = (event.getY(firstIndex) + event.getY(secondIndex)) * 0.5f;
                } else {
                    y = event.getY(firstIndex >= 0 ? firstIndex : secondIndex);
                }

                float dy = y - wheelAnchor;
                wheelAnchor = y;
                // 两指平均位移超过 4dp 就认为用户在做滚动，抬手不再合成右键。
                if (Math.abs(dy) > dp(4)) multiMoved = true;
                wheelAccum += dy;
                float step = dp(38);
                while (wheelAccum >= step) {
                    injector.scroll(1f);
                    wheelAccum -= step;
                    multiMoved = true;
                }
                while (wheelAccum <= -step) {
                    injector.scroll(-1f);
                    wheelAccum += step;
                    multiMoved = true;
                }
                return true;
            }

            int index = findPointerIndex(event, trackPointer);
            if (index < 0) return true;

            float x = event.getX(index);
            float y = event.getY(index);
            float dx = x - lastX;
            float dy = y - lastY;
            lastX = x;
            lastY = y;

            float slop = dp(TAP_SLOP_DP);
            float totalX = x - downX;
            float totalY = y - downY;
            float total2 = totalX * totalX + totalY * totalY;
            float dragStart = dp(DRAG_START_DP);
            long held = System.currentTimeMillis() - downTime;

            // krkrsdl3 的触屏逻辑：先按住不动超过 LONG_PRESS_MS，再移动才进入
            // “按住左键拖拽”（滑块/滚动条）。提前移动过的手指不再触发长按。
            if (!dragHeld && !dragCancelled && !moved
                    && held >= LONG_PRESS_MS && total2 > dragStart * dragStart) {
                dragHeld = true;
                moved = true;
                injector.buttonAt(MotionEvent.BUTTON_PRIMARY, true, cursorX, cursorY);
                performHapticFeedback(android.view.HapticFeedbackConstants.LONG_PRESS);
            } else if (!dragHeld && !moved && total2 > dragStart * dragStart) {
                // 先动过手指：本次不可能是长按，也不算轻点（避免小拖动误点）。
                dragCancelled = true;
                moved = true;
            }

            // 只要离开按下点超过触控板抖动范围，就不再算轻点。
            if (total2 > slop * slop) {
                moved = true;
            }

            cursorX += dx * 1.55f;
            cursorY += dy * 1.55f;
            clampCursor();
            // 拖动只发 MouseMove，不按任何键 —— 这就是 hover。
            injector.moveTo(cursorX, cursorY);

            invalidate();
            return true;
        }

        if (action == MotionEvent.ACTION_UP || action == MotionEvent.ACTION_POINTER_UP) {
            int index = event.getActionIndex();
            int pointerId = event.getPointerId(index);

            if (pointerId == multiPointer) {
                // 双指中的第二根先抬起：短促且没滚动过就补右键。
                long multiHeld = System.currentTimeMillis() - multiDownTime;
                if (!multiMoved && multiHeld <= MULTI_TAP_MAX_MS) {
                    click(MotionEvent.BUTTON_SECONDARY);
                }
                multiPointer = -1;
                twoFinger = false;
                moved = true; // 防止剩下那根手指抬起时再合成左键
                invalidate();
                return true;
            }

            if (pointerId == trackPointer) {
                finishTrackGesture();
                invalidate();
            }
            return true;
        }

        if (action == MotionEvent.ACTION_CANCEL) {
            releaseAllButtons();
            return true;
        }

        return true;
    }

    private void finishTrackGesture() {
        long held = System.currentTimeMillis() - downTime;
        long multiHeld = System.currentTimeMillis() - multiDownTime;

        if (twoFinger) {
            if (!multiMoved && multiHeld <= MULTI_TAP_MAX_MS) {
                click(MotionEvent.BUTTON_SECONDARY);
            }
        } else if (dragHeld) {
            // 长按拖拽收尾：只松开左键，不再合成一次点击。
            endDrag();
        } else if (!moved && held <= TAP_MAX_MS) {
            // 轻点：在光标当前位置点左键，不移动光标。
            click(MotionEvent.BUTTON_PRIMARY);
        }

        resetGesture();
    }

    private void click(int button) {
        injector.moveTo(cursorX, cursorY);
        injector.click(button, cursorX, cursorY);
        flashButton = button;
        flashUntil = System.currentTimeMillis() + 180;
        invalidate();
    }

    private void endDrag() {
        if (!dragHeld) return;
        injector.buttonAt(MotionEvent.BUTTON_PRIMARY, false, cursorX, cursorY);
        dragHeld = false;
    }

    private void resetGesture() {
        trackPointer = -1;
        multiPointer = -1;
        twoFinger = false;
        moved = false;
        dragCancelled = false;
        dragHeld = false;
        multiMoved = false;
        wheelAccum = 0f;
    }

    private static int findPointerIndex(MotionEvent event, int pointerId) {
        if (pointerId < 0) return -1;
        for (int i = 0; i < event.getPointerCount(); i++) {
            if (event.getPointerId(i) == pointerId) return i;
        }
        return -1;
    }

    private void clampCursor() {
        cursorX = Math.max(0f, Math.min(viewWidth, cursorX));
        cursorY = Math.max(0f, Math.min(viewHeight, cursorY));
    }

    @Override
    protected void onWindowVisibilityChanged(int visibility) {
        super.onWindowVisibilityChanged(visibility);
        if (visibility != VISIBLE) {
            releaseAllButtons();
        }
    }

    @Override
    protected void onDetachedFromWindow() {
        releaseAllButtons();
        super.onDetachedFromWindow();
    }
}
