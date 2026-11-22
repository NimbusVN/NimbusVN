package org.nimbusvn.input;

import android.content.Context;
import android.graphics.drawable.GradientDrawable;
import android.util.TypedValue;
import android.view.Gravity;
import android.view.View;
import android.view.ViewGroup;
import android.view.Window;
import android.widget.FrameLayout;
import android.widget.GridLayout;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.SeekBar;
import android.widget.TextView;

import androidx.appcompat.app.AlertDialog;
import androidx.appcompat.widget.SwitchCompat;

import java.util.List;

/**
 * 虚拟按键设置页：开关、布局编辑、透明度/大小、动态键位列表、添加按键、重置。
 *
 * <p>默认键位只是内置的一套；用户可以逐键改映射、拖动位置、缩放、删除，
 * 也可以从键位目录里添加新的按键。</p>
 */
public class SettingsPanel extends FrameLayout {

    public interface Listener {
        void onClose();

        void onEditLayout();

        void onSettingsChanged();

        void onAddKey(String keyId);
    }

    private interface KeyCallback {
        void onKey(String keyId);
    }

    private final InputPrefs prefs;
    private final Listener listener;
    private final float density;

    private SwitchCompat keyboardSwitch;
    private SwitchCompat mouseSwitch;
    private SeekBar opacityBar;
    private SeekBar scaleBar;
    private SeekBar mouseScaleBar;
    private SwitchCompat hintSwitch;
    private LinearLayout mappingContainer;

    public SettingsPanel(Context context, Listener listener) {
        super(context);
        this.prefs = InputPrefs.get(context);
        this.listener = listener;
        this.density = getResources().getDisplayMetrics().density;

        setBackgroundColor(0xE60B0D10);
        setClickable(true);
        setFocusable(false);

        build();
    }

    private void build() {
        removeAllViews();

        ScrollView scroll = new ScrollView(getContext());
        scroll.setFillViewport(true);
        addView(scroll, new LayoutParams(LayoutParams.MATCH_PARENT, LayoutParams.MATCH_PARENT));

        LinearLayout card = new LinearLayout(getContext());
        card.setOrientation(LinearLayout.VERTICAL);
        card.setPadding(dp(18), dp(16), dp(18), dp(16));
        card.setBackground(rounded(0xF21A1E24, 0xFF343B47, 16));

        LinearLayout.LayoutParams cardParams = new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT,
                LinearLayout.LayoutParams.WRAP_CONTENT);
        cardParams.setMargins(dp(18), dp(18), dp(18), dp(18));
        scroll.addView(card, cardParams);

        LinearLayout header = new LinearLayout(getContext());
        header.setOrientation(LinearLayout.HORIZONTAL);
        header.setGravity(Gravity.CENTER_VERTICAL);
        header.addView(title("虚拟按键", 18, 0xFFF2F5FA),
                new LinearLayout.LayoutParams(0,
                        LinearLayout.LayoutParams.WRAP_CONTENT, 1f));

        TextView close = title("关闭", 13, 0xFF9AA4B2);
        close.setPadding(dp(12), dp(6), dp(4), dp(6));
        close.setOnClickListener(v -> listener.onClose());
        header.addView(close);
        card.addView(header);

        card.addView(subtitle("默认只是一套内置键位，位置、大小、增删都可以改", 11, 0xFF8A93A3));
        card.addView(space(8));

        keyboardSwitch = new SwitchCompat(getContext());
        keyboardSwitch.setChecked(prefs.keyboardOn());
        keyboardSwitch.setFocusable(false);
        keyboardSwitch.setOnCheckedChangeListener((buttonView, checked) -> {
            prefs.setKeyboardOn(checked);
            listener.onSettingsChanged();
        });
        card.addView(switchRow("显示虚拟键盘", keyboardSwitch));

        mouseSwitch = new SwitchCompat(getContext());
        mouseSwitch.setChecked(prefs.mouseOn());
        mouseSwitch.setFocusable(false);
        mouseSwitch.setOnCheckedChangeListener((buttonView, checked) -> {
            prefs.setMouseOn(checked);
            listener.onSettingsChanged();
        });
        card.addView(switchRow("显示虚拟鼠标", mouseSwitch));

        card.addView(actionRow("调整布局", "点选按键后拖动位置，底部滑杆调大小 / 删除", v -> {
            setVisibility(GONE);
            listener.onEditLayout();
        }));

        opacityBar = new SeekBar(getContext());
        opacityBar.setMax(70);
        opacityBar.setProgress(Math.round((prefs.opacity() - 0.3f) * 100f));
        opacityBar.setOnSeekBarChangeListener(new SimpleSeekListener() {
            @Override
            public void onProgressChanged(SeekBar seekBar, int progress, boolean fromUser) {
                if (!fromUser) return;
                prefs.setOpacity(0.3f + progress / 100f);
                listener.onSettingsChanged();
            }
        });
        card.addView(seekRow("透明度", opacityBar));

        scaleBar = new SeekBar(getContext());
        scaleBar.setMax(60);
        scaleBar.setProgress(Math.round((prefs.scale() - 0.8f) * 100f));
        scaleBar.setOnSeekBarChangeListener(new SimpleSeekListener() {
            @Override
            public void onProgressChanged(SeekBar seekBar, int progress, boolean fromUser) {
                if (!fromUser) return;
                prefs.setScale(0.8f + progress / 100f);
                listener.onSettingsChanged();
            }
        });
        card.addView(seekRow("手柄大小", scaleBar));

        mouseScaleBar = new SeekBar(getContext());
        mouseScaleBar.setMax(90);
        mouseScaleBar.setProgress(Math.round((prefs.mouseScale() - 0.7f) * 100f));
        mouseScaleBar.setOnSeekBarChangeListener(new SimpleSeekListener() {
            @Override
            public void onProgressChanged(SeekBar seekBar, int progress, boolean fromUser) {
                if (!fromUser) return;
                prefs.setMouseScale(0.7f + progress / 100f);
                listener.onSettingsChanged();
            }
        });
        card.addView(seekRow("光标大小", mouseScaleBar));

        hintSwitch = new SwitchCompat(getContext());
        hintSwitch.setChecked(prefs.showHint());
        hintSwitch.setFocusable(false);
        hintSwitch.setOnCheckedChangeListener((buttonView, checked) -> {
            prefs.setShowHint(checked);
            listener.onSettingsChanged();
        });
        card.addView(switchRow("显示鼠标操作提示", hintSwitch));

        card.addView(divider());

        LinearLayout mappingHeader = new LinearLayout(getContext());
        mappingHeader.setOrientation(LinearLayout.HORIZONTAL);
        mappingHeader.setGravity(Gravity.CENTER_VERTICAL);
        mappingHeader.addView(sectionTitle("键位映射"),
                new LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1f));
        TextView resetKeys = title("恢复默认键位", 11, 0xFF7FB6FF);
        resetKeys.setPadding(dp(8), dp(6), 0, dp(6));
        resetKeys.setOnClickListener(v -> {
            prefs.resetKeys();
            refreshMapping();
            listener.onSettingsChanged();
        });
        mappingHeader.addView(resetKeys);
        card.addView(mappingHeader);

        card.addView(subtitle("点一行改目标按键；加号可以新增一个按键", 11, 0xFF8A93A3));

        mappingContainer = new LinearLayout(getContext());
        mappingContainer.setOrientation(LinearLayout.VERTICAL);
        card.addView(mappingContainer);

        TextView addKey = title("＋ 添加按键", 13, 0xFF7FB6FF);
        addKey.setGravity(Gravity.CENTER);
        addKey.setPadding(0, dp(12), 0, dp(12));
        addKey.setBackground(selectable());
        addKey.setOnClickListener(v -> showKeyPicker("添加按键", keyId -> {
            listener.onAddKey(keyId);
        }));
        card.addView(addKey);

        card.addView(space(12));
        LinearLayout footer = new LinearLayout(getContext());
        footer.setOrientation(LinearLayout.HORIZONTAL);

        TextView reset = pillButton("重置全部", 0x33FFFFFF, 0xFFD7DEEA);
        reset.setOnClickListener(v -> {
            prefs.reset();
            refresh();
            listener.onSettingsChanged();
        });
        footer.addView(reset, new LinearLayout.LayoutParams(0,
                LinearLayout.LayoutParams.WRAP_CONTENT, 1f));

        TextView done = pillButton("完成", 0xFF2E6BD6, 0xFFFFFFFF);
        done.setOnClickListener(v -> listener.onClose());
        LinearLayout.LayoutParams doneParams = new LinearLayout.LayoutParams(0,
                LinearLayout.LayoutParams.WRAP_CONTENT, 1f);
        doneParams.setMargins(dp(8), 0, 0, 0);
        footer.addView(done, doneParams);

        card.addView(footer);
    }

    public void refresh() {
        if (keyboardSwitch != null) keyboardSwitch.setChecked(prefs.keyboardOn());
        if (mouseSwitch != null) mouseSwitch.setChecked(prefs.mouseOn());
        if (opacityBar != null) {
            opacityBar.setProgress(Math.round((prefs.opacity() - 0.3f) * 100f));
        }
        if (scaleBar != null) {
            scaleBar.setProgress(Math.round((prefs.scale() - 0.8f) * 100f));
        }
        if (mouseScaleBar != null) {
            mouseScaleBar.setProgress(Math.round((prefs.mouseScale() - 0.7f) * 100f));
        }
        if (hintSwitch != null) {
            hintSwitch.setChecked(prefs.showHint());
        }
        refreshMapping();
    }

    /** 按当前按键列表重建映射行；方向盘的四个方向单独占四行。 */
    private void refreshMapping() {
        if (mappingContainer == null) return;
        mappingContainer.removeAllViews();

        List<VirtualKey> keys = prefs.keys();
        for (VirtualKey key : keys) {
            if (key.isDpad()) {
                addMappingRow("方向上", prefs.keyFor("UP"),
                        id -> prefs.setKeyFor("UP", id));
                addMappingRow("方向下", prefs.keyFor("DOWN"),
                        id -> prefs.setKeyFor("DOWN", id));
                addMappingRow("方向左", prefs.keyFor("LEFT"),
                        id -> prefs.setKeyFor("LEFT", id));
                addMappingRow("方向右", prefs.keyFor("RIGHT"),
                        id -> prefs.setKeyFor("RIGHT", id));
            } else {
                final String keyId = key.id;
                addMappingRow(key.label, KeyCatalog.label(key.keyId), chosen -> {
                    List<VirtualKey> list = prefs.keys();
                    for (VirtualKey item : list) {
                        if (!item.id.equals(keyId)) continue;
                        String oldLabel = KeyCatalog.label(item.keyId);
                        item.keyId = chosen;
                        if (item.label.equals(oldLabel)) {
                            item.label = KeyCatalog.label(chosen);
                        }
                    }
                    prefs.saveKeys(list);
                    refreshMapping();
                    listener.onSettingsChanged();
                });
            }
        }

        if (keys.isEmpty()) {
            TextView empty = subtitle("还没有按键，点下面的「添加按键」开始", 12, 0xFF8A93A3);
            empty.setPadding(0, dp(10), 0, dp(10));
            mappingContainer.addView(empty);
        }
    }

    private void addMappingRow(String label, String value, KeyCallback callback) {
        LinearLayout row = new LinearLayout(getContext());
        row.setOrientation(LinearLayout.HORIZONTAL);
        row.setGravity(Gravity.CENTER_VERTICAL);
        row.setPadding(0, dp(9), 0, dp(9));
        row.setBackground(selectable());

        TextView name = title(label, 14, 0xFFE7EBF2);
        row.addView(name, new LinearLayout.LayoutParams(0,
                LinearLayout.LayoutParams.WRAP_CONTENT, 1f));

        TextView current = title(value, 13, 0xFF7FB6FF);
        current.setPadding(dp(10), dp(4), dp(10), dp(4));
        current.setBackground(rounded(0x332E6BD6, 0x003E7BEA, 8));
        row.addView(current);

        row.setOnClickListener(v -> showKeyPicker(label, callback));
        mappingContainer.addView(row);
    }

    private void showKeyPicker(String titleText, KeyCallback callback) {
        Context context = getContext();
        ScrollView scroll = new ScrollView(context);

        LinearLayout root = new LinearLayout(context);
        root.setOrientation(LinearLayout.VERTICAL);
        root.setPadding(dp(14), dp(8), dp(14), dp(8));
        scroll.addView(root);

        AlertDialog.Builder builder = new AlertDialog.Builder(context)
                .setTitle("选择按键 · " + titleText)
                .setView(scroll)
                .setNegativeButton("取消", null);

        AlertDialog dialog = builder.create();
        dialog.setOnShowListener(d -> {
            Window window = dialog.getWindow();
            if (window != null) {
                window.setLayout((int) (getResources().getDisplayMetrics().widthPixels * 0.9f),
                        ViewGroup.LayoutParams.WRAP_CONTENT);
            }
        });

        for (String group : KeyCatalog.groups()) {
            TextView header = sectionTitle(group);
            root.addView(header);

            GridLayout grid = new GridLayout(context);
            grid.setColumnCount(6);
            root.addView(grid, new LinearLayout.LayoutParams(
                    LinearLayout.LayoutParams.MATCH_PARENT,
                    LinearLayout.LayoutParams.WRAP_CONTENT));

            for (KeyCatalog.Key key : KeyCatalog.group(group)) {
                TextView chip = new TextView(context);
                chip.setText(key.label);
                chip.setTextColor(0xFFE7EBF2);
                chip.setTextSize(TypedValue.COMPLEX_UNIT_SP, 12);
                chip.setGravity(Gravity.CENTER);
                chip.setPadding(dp(4), dp(9), dp(4), dp(9));
                chip.setBackground(rounded(0xFF2A3038, 0xFF3A4250, 8));
                chip.setOnClickListener(v -> {
                    callback.onKey(key.id);
                    dialog.dismiss();
                });

                GridLayout.LayoutParams params = new GridLayout.LayoutParams();
                params.width = 0;
                params.columnSpec = GridLayout.spec(GridLayout.UNDEFINED, 1f);
                params.setMargins(dp(3), dp(3), dp(3), dp(3));
                grid.addView(chip, params);
            }
        }

        dialog.show();
    }

    private View switchRow(String label, SwitchCompat control) {
        LinearLayout row = new LinearLayout(getContext());
        row.setOrientation(LinearLayout.HORIZONTAL);
        row.setGravity(Gravity.CENTER_VERTICAL);
        row.setPadding(0, dp(10), 0, dp(10));

        TextView title = title(label, 14, 0xFFE7EBF2);
        row.addView(title, new LinearLayout.LayoutParams(0,
                LinearLayout.LayoutParams.WRAP_CONTENT, 1f));
        row.addView(control);
        return row;
    }

    private View actionRow(String titleText, String subtitleText, OnClickListener click) {
        LinearLayout row = new LinearLayout(getContext());
        row.setOrientation(LinearLayout.HORIZONTAL);
        row.setGravity(Gravity.CENTER_VERTICAL);
        row.setPadding(0, dp(10), 0, dp(10));
        row.setOnClickListener(click);
        row.setBackground(selectable());

        LinearLayout texts = new LinearLayout(getContext());
        texts.setOrientation(LinearLayout.VERTICAL);
        texts.addView(title(titleText, 14, 0xFFE7EBF2));
        texts.addView(subtitle(subtitleText, 11, 0xFF8A93A3));
        row.addView(texts, new LinearLayout.LayoutParams(0,
                LinearLayout.LayoutParams.WRAP_CONTENT, 1f));

        TextView arrow = title("›", 20, 0xFF8A93A3);
        row.addView(arrow);
        return row;
    }

    private View seekRow(String label, SeekBar bar) {
        LinearLayout row = new LinearLayout(getContext());
        row.setOrientation(LinearLayout.VERTICAL);
        row.setPadding(0, dp(8), 0, dp(2));
        row.addView(title(label, 14, 0xFFE7EBF2));
        row.addView(bar, new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT, dp(30)));
        return row;
    }

    private TextView title(String text, float sizeSp, int color) {
        TextView view = new TextView(getContext());
        view.setText(text);
        view.setTextColor(color);
        view.setTextSize(TypedValue.COMPLEX_UNIT_SP, sizeSp);
        view.setIncludeFontPadding(false);
        return view;
    }

    private TextView subtitle(String text, float sizeSp, int color) {
        TextView view = title(text, sizeSp, color);
        view.setPadding(0, dp(3), 0, dp(3));
        return view;
    }

    private TextView sectionTitle(String text) {
        TextView view = title(text, 12, 0xFF8A93A3);
        view.setPadding(0, dp(12), 0, dp(6));
        return view;
    }

    private TextView pillButton(String text, int fillColor, int textColor) {
        TextView view = new TextView(getContext());
        view.setText(text);
        view.setTextColor(textColor);
        view.setTextSize(TypedValue.COMPLEX_UNIT_SP, 14);
        view.setGravity(Gravity.CENTER);
        view.setPadding(0, dp(11), 0, dp(11));
        view.setBackground(rounded(fillColor, 0x00000000, 10));
        view.setFocusable(false);
        return view;
    }

    private View divider() {
        View view = new View(getContext());
        view.setBackgroundColor(0x1FFFFFFF);
        LinearLayout.LayoutParams params = new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT, Math.max(1, (int) dp(1)));
        params.setMargins(0, dp(10), 0, 0);
        view.setLayoutParams(params);
        return view;
    }

    private View space(int dpValue) {
        View view = new View(getContext());
        view.setLayoutParams(new LinearLayout.LayoutParams(1, dp(dpValue)));
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

    private GradientDrawable selectable() {
        GradientDrawable drawable = new GradientDrawable();
        drawable.setColor(0x00000000);
        drawable.setCornerRadius(dp(8));
        return drawable;
    }

    private int dp(float value) {
        return Math.round(value * density);
    }

    private abstract static class SimpleSeekListener implements SeekBar.OnSeekBarChangeListener {
        @Override
        public void onStartTrackingTouch(SeekBar seekBar) {
        }

        @Override
        public void onStopTrackingTouch(SeekBar seekBar) {
        }
    }
}
