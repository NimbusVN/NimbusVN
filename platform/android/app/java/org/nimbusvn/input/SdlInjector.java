package org.nimbusvn.input;

import android.view.MotionEvent;

import org.libsdl.app.SDLActivity;

import java.util.HashSet;
import java.util.Set;

/**
 * 把虚拟控件的动作注入 SDL。
 *
 * <p>只依赖 {@link SDLActivity} 已经暴露的三个 JNI 入口：
 * {@code onNativeMouse} / {@code onNativeKeyDown} / {@code onNativeKeyUp}。
 * 因此这一层对内核零侵入，也不需要新增 .so。</p>
 *
 * <p>注意 SDL 的 Android 鼠标实现把第二个参数当作“按键位图”，并用
 * {@code 新状态 & ~旧状态} 推导按下/抬起。所以移动时必须带上当前按住的键，
 * 抬起时必须先把该位清掉，否则左键会一直处于按下状态。</p>
 */
public final class SdlInjector {

    private final Set<Integer> held = new HashSet<>();
    private int buttonState;
    private float lastX = 0f;
    private float lastY = 0f;

    public void keyDown(int keycode) {
        if (keycode <= 0) return;
        if (held.add(keycode)) {
            SDLActivity.onNativeKeyDown(keycode);
        }
    }

    public void keyUp(int keycode) {
        if (keycode <= 0) return;
        if (held.remove(keycode)) {
            SDLActivity.onNativeKeyUp(keycode);
        }
    }

    /** 切后台/失焦/切模式时调用，避免按键卡住。 */
    public void releaseAll() {
        if (buttonState != 0) {
            SDLActivity.onNativeMouse(0, MotionEvent.ACTION_UP,
                    lastX, lastY, false);
            buttonState = 0;
        }
        if (held.isEmpty()) return;
        for (Integer code : new HashSet<>(held)) {
            SDLActivity.onNativeKeyUp(code);
        }
        held.clear();
    }

    public void moveTo(float x, float y) {
        lastX = x;
        lastY = y;
        SDLActivity.onNativeMouse(buttonState, MotionEvent.ACTION_MOVE, x, y, false);
    }

    public void buttonAt(int button, boolean down, float x, float y) {
        int next = down ? (buttonState | button) : (buttonState & ~button);
        lastX = x;
        lastY = y;
        SDLActivity.onNativeMouse(next,
                down ? MotionEvent.ACTION_DOWN : MotionEvent.ACTION_UP, x, y, false);
        buttonState = next;
    }

    public void button(int button, boolean down) {
        buttonAt(button, down, lastX, lastY);
    }

    public void click(int button, float x, float y) {
        buttonAt(button, true, x, y);
        buttonAt(button, false, x, y);
    }

    /** notches > 0 向下滚，< 0 向上滚。 */
    public void scroll(float notches) {
        if (notches == 0f) return;
        SDLActivity.onNativeMouse(0, MotionEvent.ACTION_SCROLL, 0f, notches, false);
    }

    public float lastX() {
        return lastX;
    }

    public float lastY() {
        return lastY;
    }
}
