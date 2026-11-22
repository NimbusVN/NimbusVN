package org.nimbusvn.input;

import android.content.Context;
import android.graphics.drawable.GradientDrawable;
import android.util.TypedValue;
import android.view.Gravity;
import android.widget.LinearLayout;
import android.widget.SeekBar;
import android.widget.TextView;

/**
 * 布局编辑条：显示当前选中的按键，提供缩小 / 放大 / 删除 / 完成。
 * 只在「调整布局」模式出现，贴在屏幕底部。
 */
public class LayoutBar extends LinearLayout {

    public interface Listener {
        void onScale(float scale);

        void onDelete();

        void onDone();
    }

    private static final float MIN_SCALE = 0.4f;
    private static final float MAX_SCALE = 2.5f;

    private final Listener listener;
    private final float density;

    private TextView title;
    private SeekBar sizeBar;
    private TextView minus;
    private TextView plus;
    private TextView delete;
    private boolean updating;
    private float currentScale = 1f;

    public LayoutBar(Context context, Listener listener) {
        super(context);
        this.listener = listener;
        density = getResources().getDisplayMetrics().density;

        setOrientation(HORIZONTAL);
        setGravity(Gravity.CENTER_VERTICAL);
        setPadding(dp(12), dp(6), dp(12), dp(6));
        setBackground(rounded(0xE61A1E24, 0x33FFFFFF, 14));
        setClickable(true);
        setFocusable(false);

        build();
        clearTarget();
    }

    private void build() {
        title = text("点选一个按键", 12, 0xFFE7EBF2);
        title.setMinWidth(dp(64));
        addView(title);

        minus = text("−", 18, 0xFFE7EBF2);
        minus.setPadding(dp(9), 0, dp(9), 0);
        minus.setOnClickListener(v -> adjust(-0.05f));
        addView(minus);

        sizeBar = new SeekBar(getContext());
        sizeBar.setMax(Math.round((MAX_SCALE - MIN_SCALE) * 100f));
        sizeBar.setProgress(Math.round((1f - MIN_SCALE) * 100f));
        sizeBar.setOnSeekBarChangeListener(new SeekBar.OnSeekBarChangeListener() {
            @Override
            public void onProgressChanged(SeekBar seekBar, int progress, boolean fromUser) {
                if (!fromUser || updating) return;
                currentScale = MIN_SCALE + progress / 100f;
                listener.onScale(currentScale);
            }

            @Override
            public void onStartTrackingTouch(SeekBar seekBar) {
            }

            @Override
            public void onStopTrackingTouch(SeekBar seekBar) {
            }
        });
        addView(sizeBar, new LayoutParams(dp(120), dp(30)));

        plus = text("+", 18, 0xFFE7EBF2);
        plus.setPadding(dp(9), 0, dp(9), 0);
        plus.setOnClickListener(v -> adjust(0.05f));
        addView(plus);

        delete = text("删除", 12, 0xFFFF9F9F);
        delete.setPadding(dp(10), dp(6), dp(10), dp(6));
        delete.setOnClickListener(v -> listener.onDelete());
        addView(delete);

        TextView done = text("完成", 12, 0xFFFFFFFF);
        done.setPadding(dp(12), dp(6), dp(12), dp(6));
        done.setBackground(rounded(0xFF2E6BD6, 0x00000000, 10));
        done.setOnClickListener(v -> listener.onDone());
        LayoutParams doneParams = new LayoutParams(
                LayoutParams.WRAP_CONTENT, LayoutParams.WRAP_CONTENT);
        doneParams.setMargins(dp(8), 0, 0, 0);
        addView(done, doneParams);
    }

    public void setTarget(String label, float scale) {
        updating = true;
        title.setText(label);
        currentScale = scale;
        sizeBar.setProgress(Math.round((clamp(scale, MIN_SCALE, MAX_SCALE) - MIN_SCALE) * 100f));
        updating = false;
        setControlsEnabled(true);
    }

    public void clearTarget() {
        updating = true;
        title.setText("点选一个按键");
        sizeBar.setProgress(Math.round((1f - MIN_SCALE) * 100f));
        updating = false;
        setControlsEnabled(false);
    }

    private void setControlsEnabled(boolean enabled) {
        sizeBar.setEnabled(enabled);
        minus.setEnabled(enabled);
        plus.setEnabled(enabled);
        delete.setEnabled(enabled);
        float alpha = enabled ? 1f : 0.4f;
        title.setAlpha(alpha);
        sizeBar.setAlpha(alpha);
        minus.setAlpha(alpha);
        plus.setAlpha(alpha);
        delete.setAlpha(alpha);
    }

    private void adjust(float delta) {
        if (!sizeBar.isEnabled()) return;
        int progress = Math.round(sizeBar.getProgress() + delta * 100f);
        progress = Math.max(0, Math.min(sizeBar.getMax(), progress));
        sizeBar.setProgress(progress);
        currentScale = MIN_SCALE + progress / 100f;
        listener.onScale(currentScale);
    }

    private TextView text(String value, float sizeSp, int color) {
        TextView view = new TextView(getContext());
        view.setText(value);
        view.setTextSize(TypedValue.COMPLEX_UNIT_SP, sizeSp);
        view.setTextColor(color);
        view.setGravity(Gravity.CENTER);
        view.setIncludeFontPadding(false);
        view.setFocusable(false);
        view.setFocusableInTouchMode(false);
        return view;
    }

    private GradientDrawable rounded(int fill, int stroke, int radiusDp) {
        GradientDrawable drawable = new GradientDrawable();
        drawable.setColor(fill);
        drawable.setCornerRadius(dp(radiusDp));
        if ((stroke & 0xFF000000) != 0) {
            drawable.setStroke(Math.max(1, (int) dp(1)), stroke);
        }
        return drawable;
    }

    private int dp(float value) {
        return Math.round(value * density);
    }

    private static float clamp(float value, float min, float max) {
        return value < min ? min : (value > max ? max : value);
    }
}
