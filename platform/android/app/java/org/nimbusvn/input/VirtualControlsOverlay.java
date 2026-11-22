package org.nimbusvn.input;

import android.app.Activity;
import android.content.Context;
import android.text.InputType;
import android.util.Log;
import android.view.Gravity;
import android.view.MotionEvent;
import android.view.ViewGroup;
import android.widget.FrameLayout;

import org.libsdl.app.SDLActivity;

/**
 * 所有内核共用的虚拟输入层。
 *
 * <p>挂在 {@code SDLActivity} 的根布局上，由 KRKRActivity / GameActivity /
 * ONSActivity 自动继承；以后新增的 SDL 内核也只要继承 SDLActivity 即可。</p>
 *
 * <p>虚拟键盘（手柄）和虚拟鼠标是两套独立开关，互不影响，可以同时开启：
 * 手柄按键优先，鼠标只接管按键之外的区域。鼠标开启时触摸被完全接管，
 * 内核收不到 FINGER 事件；鼠标关闭时，按键之外仍然透传给内核的触摸逻辑。</p>
 */
public class VirtualControlsOverlay extends FrameLayout {

    private static final String TAG = "NimbusInput";
    private static final String VIEW_TAG = "nimbus.virtual.controls";

    private final InputPrefs prefs;

    private GamepadView gamepad;
    private MouseView mouse;
    private ToolbarView toolbar;
    private SettingsPanel settings;
    private LayoutBar layoutBar;

    private boolean keyboardOn;
    private boolean mouseOn;
    private boolean collapsed;
    private boolean mouseHiddenForEdit;

    private static VirtualControlsOverlay sCurrent;

    /** 切后台/销毁时把按住的键全部放开，避免回到游戏时按键卡死。 */
    public static void releaseInputs() {
        VirtualControlsOverlay overlay = sCurrent;
        if (overlay == null) return;
        if (overlay.gamepad != null) overlay.gamepad.releaseAllKeys();
        if (overlay.mouse != null) overlay.mouse.releaseAllButtons();
    }

    public static void attach(Activity activity, ViewGroup root) {
        if (activity == null || root == null) return;
        // 全局总开关：关掉后游戏内不挂任何悬浮控件。
        if (!InputPrefs.get(activity).masterEnabled()) return;
        if (root.findViewWithTag(VIEW_TAG) != null) return;
        try {
            VirtualControlsOverlay overlay = new VirtualControlsOverlay(activity);
            overlay.setTag(VIEW_TAG);
            sCurrent = overlay;
            ViewGroup.LayoutParams params;
            if (root instanceof android.widget.RelativeLayout) {
                params = new android.widget.RelativeLayout.LayoutParams(
                        ViewGroup.LayoutParams.MATCH_PARENT,
                        ViewGroup.LayoutParams.MATCH_PARENT);
            } else if (root instanceof FrameLayout) {
                params = new FrameLayout.LayoutParams(
                        ViewGroup.LayoutParams.MATCH_PARENT,
                        ViewGroup.LayoutParams.MATCH_PARENT);
            } else if (root instanceof android.widget.LinearLayout) {
                params = new android.widget.LinearLayout.LayoutParams(
                        ViewGroup.LayoutParams.MATCH_PARENT,
                        ViewGroup.LayoutParams.MATCH_PARENT);
            } else {
                params = new ViewGroup.LayoutParams(
                        ViewGroup.LayoutParams.MATCH_PARENT,
                        ViewGroup.LayoutParams.MATCH_PARENT);
            }
            root.addView(overlay, params);
        } catch (Throwable t) {
            Log.w(TAG, "virtual controls attach failed", t);
        }
    }

    public VirtualControlsOverlay(Context context) {
        super(context);
        prefs = InputPrefs.get(context);
        keyboardOn = prefs.keyboardOn();
        mouseOn = prefs.mouseOn();
        collapsed = prefs.collapsed();

        setClipChildren(false);
        setClipToPadding(false);
        setFocusable(false);
        setFocusableInTouchMode(false);
        setClickable(false);

        gamepad = new GamepadView(context);
        gamepad.setListener(new GamepadView.Listener() {
            @Override
            public void onButtonSelected(VirtualKey key) {
                layoutBar.setTarget(key.label, key.scale);
            }

            @Override
            public void onSelectionCleared() {
                layoutBar.clearTarget();
            }
        });
        addView(gamepad, matchParent());

        // 鼠标层盖在手柄上方，这样光标不会被按键挡住；
        // 触点落在手柄按键上时鼠标主动让位（ControlHitTest）。
        mouse = new MouseView(context);
        mouse.setControlHitTest((x, y) -> keyboardOn && gamepad.isOverControl(x, y));
        addView(mouse, matchParent());

        toolbar = new ToolbarView(context, new ToolbarView.Listener() {
            @Override
            public void onToggleMouse() {
                setMouseOn(!mouseOn);
            }

            @Override
            public void onToggleKeyboard() {
                setKeyboardOn(!keyboardOn);
            }

            @Override
            public void onShowIme() {
                showIme();
            }

            @Override
            public void onOpenSettings() {
                openSettings();
            }

            @Override
            public void onToggleCollapsed() {
                setCollapsed(!collapsed);
            }
        });
        toolbar.setStates(keyboardOn, mouseOn);
        toolbar.setCollapsed(collapsed);
        LayoutParams toolbarParams = new LayoutParams(
                LayoutParams.WRAP_CONTENT, LayoutParams.WRAP_CONTENT);
        toolbarParams.gravity = Gravity.TOP | Gravity.END;
        toolbarParams.topMargin = dp(10);
        toolbarParams.rightMargin = dp(12);
        addView(toolbar, toolbarParams);

        settings = new SettingsPanel(context, new SettingsPanel.Listener() {
            @Override
            public void onClose() {
                settings.setVisibility(GONE);
                if (gamepad.isEditMode()) {
                    layoutBar.setVisibility(VISIBLE);
                    layoutBar.bringToFront();
                }
                applyPrefs();
            }

            @Override
            public void onEditLayout() {
                startEditLayout();
            }

            @Override
            public void onSettingsChanged() {
                applyPrefs();
            }

            @Override
            public void onAddKey(String keyId) {
                settings.setVisibility(GONE);
                startEditLayout();
                gamepad.addKey(keyId);
            }
        });
        settings.setVisibility(GONE);
        addView(settings, matchParent());

        layoutBar = new LayoutBar(context, new LayoutBar.Listener() {
            @Override
            public void onScale(float scale) {
                gamepad.setSelectedScale(scale);
            }

            @Override
            public void onDelete() {
                gamepad.deleteSelectedKey();
                layoutBar.clearTarget();
            }

            @Override
            public void onDone() {
                finishEditLayout();
            }
        });
        layoutBar.setVisibility(GONE);
        LayoutParams barParams = new LayoutParams(
                LayoutParams.WRAP_CONTENT, LayoutParams.WRAP_CONTENT);
        barParams.gravity = Gravity.BOTTOM | Gravity.CENTER_HORIZONTAL;
        barParams.bottomMargin = dp(14);
        addView(layoutBar, barParams);

        applyPrefs();
    }

    /**
     * 鼠标开启时，所有触摸都归虚拟鼠标：即使某个触摸没被 MouseView 吃掉，
     * 也在这里截断，不再落到 SDLSurface。
     */
    @Override
    public boolean dispatchTouchEvent(MotionEvent event) {
        boolean handled = super.dispatchTouchEvent(event);
        if (!handled && mouseOn) {
            return true;
        }
        return handled;
    }

    private void setKeyboardOn(boolean enabled) {
        if (keyboardOn == enabled) return;
        keyboardOn = enabled;
        if (!enabled) gamepad.releaseAllKeys();
        prefs.setKeyboardOn(enabled);
        toolbar.setStates(keyboardOn, mouseOn);
        applyVisibility();
    }

    private void setMouseOn(boolean enabled) {
        if (mouseOn == enabled) return;
        mouseOn = enabled;
        if (!enabled) mouse.releaseAllButtons();
        prefs.setMouseOn(enabled);
        toolbar.setStates(keyboardOn, mouseOn);
        mouse.setHintVisible(prefs.showHint() && !collapsed && mouseOn);
        applyVisibility();
    }

    /** 收起只影响工具条本身，手柄/鼠标保持原样。 */
    private void setCollapsed(boolean value) {
        if (collapsed == value) return;
        collapsed = value;
        prefs.setCollapsed(collapsed);
        toolbar.setCollapsed(collapsed);
        mouse.setHintVisible(prefs.showHint() && !collapsed && mouseOn);
    }

    private void applyPrefs() {
        boolean newKeyboard = prefs.keyboardOn();
        boolean newMouse = prefs.mouseOn();
        if (newKeyboard != keyboardOn) {
            keyboardOn = newKeyboard;
            if (!keyboardOn) gamepad.releaseAllKeys();
        }
        if (newMouse != mouseOn) {
            mouseOn = newMouse;
            if (!mouseOn) mouse.releaseAllButtons();
        }
        boolean savedCollapsed = prefs.collapsed();
        if (savedCollapsed != collapsed) {
            collapsed = savedCollapsed;
            toolbar.setCollapsed(collapsed);
        }
        toolbar.setStates(keyboardOn, mouseOn);

        float alpha = prefs.opacity();
        gamepad.setAlpha(Math.max(0.25f, alpha));
        mouse.setAlpha(Math.max(0.25f, alpha));
        toolbar.setAlpha(Math.max(0.55f, alpha));
        gamepad.reloadKeys();
        mouse.refresh();
        mouse.setHintVisible(prefs.showHint() && !collapsed && mouseOn);
        applyVisibility();
    }

    private void applyVisibility() {
        gamepad.setVisibility(keyboardOn ? VISIBLE : GONE);
        mouse.setVisibility(mouseOn && !mouseHiddenForEdit ? VISIBLE : GONE);
    }

    private void openSettings() {
        gamepad.releaseAllKeys();
        mouse.releaseAllButtons();
        // 布局条在设置页之上，打开设置时先收起来，避免挡住内容。
        if (gamepad.isEditMode()) layoutBar.setVisibility(GONE);
        settings.refresh();
        settings.setVisibility(VISIBLE);
        settings.bringToFront();
    }

    private void startEditLayout() {
        // 编辑时只临时隐藏鼠标层，不改用户真正的开关状态。
        mouseHiddenForEdit = true;
        if (!keyboardOn) setKeyboardOn(true);
        collapsed = false;
        prefs.setCollapsed(false);
        toolbar.setCollapsed(false);
        applyVisibility();
        mouse.setHintVisible(false);
        gamepad.setEditMode(true);
        layoutBar.clearTarget();
        layoutBar.setVisibility(VISIBLE);
        layoutBar.bringToFront();
    }

    private void finishEditLayout() {
        gamepad.setEditMode(false);
        layoutBar.setVisibility(GONE);
        mouseHiddenForEdit = false;
        applyVisibility();
    }

    private void showIme() {
        try {
            // 1x1 的隐藏输入框：只要能把焦点交给输入法即可，避免挡住画面。
            SDLActivity.showTextInput(InputType.TYPE_CLASS_TEXT, 0, 0, 1, 1);
        } catch (Throwable t) {
            Log.w(TAG, "showTextInput failed", t);
        }
    }

    private LayoutParams matchParent() {
        return new LayoutParams(LayoutParams.MATCH_PARENT, LayoutParams.MATCH_PARENT);
    }

    private int dp(float value) {
        return Math.round(value * getResources().getDisplayMetrics().density);
    }

    @Override
    protected void onWindowVisibilityChanged(int visibility) {
        super.onWindowVisibilityChanged(visibility);
        if (visibility != VISIBLE) {
            if (gamepad != null) gamepad.releaseAllKeys();
            if (mouse != null) mouse.releaseAllButtons();
        }
    }

    @Override
    protected void onDetachedFromWindow() {
        if (gamepad != null) gamepad.releaseAllKeys();
        if (mouse != null) mouse.releaseAllButtons();
        if (sCurrent == this) sCurrent = null;
        super.onDetachedFromWindow();
    }
}
