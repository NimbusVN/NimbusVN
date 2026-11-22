package org.nimbusvn.input;

import android.content.Context;
import android.graphics.drawable.GradientDrawable;
import android.util.TypedValue;
import android.view.Gravity;
import android.widget.LinearLayout;
import android.widget.TextView;

/**
 * 悬浮工具条：虚拟鼠标、虚拟键盘、系统输入法、设置、收起。
 *
 * <p>鼠标和键盘是两个独立开关，互不影响；两个都打开时也能同时工作。
 * 收起只把工具条缩成一个圆形「≡」，不会隐藏任何控件。</p>
 */
public class ToolbarView extends LinearLayout {

    public interface Listener {
        void onToggleMouse();

        void onToggleKeyboard();

        void onShowIme();

        void onOpenSettings();

        void onToggleCollapsed();
    }

    private final Listener listener;
    private final float density;
    private boolean collapsed;
    private boolean keyboardOn;
    private boolean mouseOn;

    public ToolbarView(Context context, Listener listener) {
        super(context);
        this.listener = listener;
        density = getResources().getDisplayMetrics().density;

        setOrientation(HORIZONTAL);
        setGravity(Gravity.CENTER_VERTICAL);
        setPadding(dp(6), dp(4), dp(6), dp(4));
        setFocusable(false);
        setFocusableInTouchMode(false);
        rebuild();
    }

    public void setStates(boolean keyboardOn, boolean mouseOn) {
        if (this.keyboardOn == keyboardOn && this.mouseOn == mouseOn) return;
        this.keyboardOn = keyboardOn;
        this.mouseOn = mouseOn;
        if (!collapsed) rebuild();
    }

    public void setCollapsed(boolean collapsed) {
        if (this.collapsed == collapsed) return;
        this.collapsed = collapsed;
        rebuild();
    }

    public boolean isCollapsed() {
        return collapsed;
    }

    private void rebuild() {
        removeAllViews();

        if (collapsed) {
            setBackground(circleBackground());
            TextView handle = makeItem("≡", dp(38), dp(38), 18);
            handle.setOnClickListener(v -> listener.onToggleCollapsed());
            addView(handle);
            return;
        }

        setBackground(pillBackground());
        addToggle("鼠标", mouseOn, v -> listener.onToggleMouse());
        addToggle("键盘", keyboardOn, v -> listener.onToggleKeyboard());
        addItem("输入法", v -> listener.onShowIme());
        addItem("设置", v -> listener.onOpenSettings());
        addItem("收起", v -> listener.onToggleCollapsed());
    }

    private void addToggle(String label, boolean active, OnClickListener click) {
        TextView view = addItem(label, click);
        if (active) {
            view.setTextColor(0xFFA9CCFF);
            GradientDrawable background = new GradientDrawable();
            background.setColor(0x332E6BD6);
            background.setCornerRadius(dp(15));
            view.setBackground(background);
        }
    }

    private TextView addItem(String label, OnClickListener click) {
        TextView view = makeItem(label, LayoutParams.WRAP_CONTENT, dp(30), 12);
        view.setOnClickListener(click);
        LayoutParams params = new LayoutParams(
                LayoutParams.WRAP_CONTENT, dp(30));
        params.setMargins(dp(1), 0, dp(1), 0);
        addView(view, params);
        return view;
    }

    private TextView makeItem(String label, int width, int height, float textSizeSp) {
        TextView view = new TextView(getContext());
        view.setText(label);
        view.setTextColor(0xFFE7EBF2);
        view.setTextSize(TypedValue.COMPLEX_UNIT_SP, textSizeSp);
        view.setGravity(Gravity.CENTER);
        view.setPadding(dp(9), 0, dp(9), 0);
        view.setFocusable(false);
        view.setFocusableInTouchMode(false);
        view.setLayoutParams(new LayoutParams(width, height));
        return view;
    }

    private GradientDrawable pillBackground() {
        GradientDrawable drawable = new GradientDrawable();
        drawable.setColor(0xD9181C22);
        drawable.setCornerRadius(dp(19));
        drawable.setStroke(Math.max(1, (int) dp(1)), 0x33FFFFFF);
        return drawable;
    }

    private GradientDrawable circleBackground() {
        GradientDrawable drawable = new GradientDrawable();
        drawable.setShape(GradientDrawable.OVAL);
        drawable.setColor(0xD9181C22);
        drawable.setStroke(Math.max(1, (int) dp(1)), 0x44FFFFFF);
        return drawable;
    }

    private int dp(float value) {
        return Math.round(value * density);
    }
}
