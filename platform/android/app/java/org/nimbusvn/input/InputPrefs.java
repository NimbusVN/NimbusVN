package org.nimbusvn.input;

import android.content.Context;
import android.content.SharedPreferences;

import org.json.JSONArray;
import org.json.JSONException;
import org.json.JSONObject;

import java.util.ArrayList;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;

/**
 * 虚拟输入的持久化配置：开关、模式、透明度、每个按钮的键位、每个控件组的位置。
 *
 * <p>全部存成一个 JSON 字符串，读写都经过 {@link SharedPreferences}，
 * 升级时缺字段一律回落到默认值，所以旧配置不会因为新增按钮而失效。</p>
 */
public final class InputPrefs {

    private static final String FILE = "nimbus_vinput";
    private static final String STATE = "state";

    /** 虚拟按钮 -> 默认键位。键位 id 见 {@link KeyCatalog}。 */
    public static final Map<String, String> DEFAULT_KEYS = new LinkedHashMap<>();

    static {
        DEFAULT_KEYS.put("UP", "UP");
        DEFAULT_KEYS.put("DOWN", "DOWN");
        DEFAULT_KEYS.put("LEFT", "LEFT");
        DEFAULT_KEYS.put("RIGHT", "RIGHT");
        DEFAULT_KEYS.put("A", "ENTER");
        DEFAULT_KEYS.put("B", "ESCAPE");
        DEFAULT_KEYS.put("X", "CTRL");
        DEFAULT_KEYS.put("Y", "SPACE");
        DEFAULT_KEYS.put("L", "PAGE_UP");
        DEFAULT_KEYS.put("R", "PAGE_DOWN");
        DEFAULT_KEYS.put("MENU", "TAB");
        DEFAULT_KEYS.put("SKIP", "CTRL");
    }

    private static InputPrefs sInstance;

    private final SharedPreferences sp;
    private JSONObject data;

    public static synchronized InputPrefs get(Context context) {
        if (sInstance == null) {
            sInstance = new InputPrefs(context.getApplicationContext());
        }
        return sInstance;
    }

    private InputPrefs(Context context) {
        sp = context.getSharedPreferences(FILE, Context.MODE_PRIVATE);
        String raw = sp.getString(STATE, null);
        try {
            data = raw != null && !raw.isEmpty() ? new JSONObject(raw) : new JSONObject();
        } catch (JSONException e) {
            data = new JSONObject();
        }
    }

    private void save() {
        sp.edit().putString(STATE, data.toString()).apply();
    }

    /** 全局总开关：关闭后游戏内不挂载任何悬浮控件。 */
    public boolean masterEnabled() {
        return data.optBoolean("master", true);
    }

    public void setMasterEnabled(boolean enabled) {
        put("master", enabled);
    }

    /** 虚拟键盘（手柄）是否显示，和虚拟鼠标完全独立。 */
    public boolean keyboardOn() {
        if (data.has("keyboard_on")) return data.optBoolean("keyboard_on", true);
        // 兼容旧配置：以前 mode=1 表示鼠标，键盘隐藏。
        return data.optBoolean("enabled", true) && data.optInt("mode", 0) == 0;
    }

    public void setKeyboardOn(boolean enabled) {
        put("keyboard_on", enabled);
    }

    /** 虚拟鼠标是否显示；开启时触摸由鼠标接管，和虚拟键盘互不影响。 */
    public boolean mouseOn() {
        if (data.has("mouse_on")) return data.optBoolean("mouse_on", false);
        return data.optBoolean("enabled", true) && data.optInt("mode", 0) == 1;
    }

    public void setMouseOn(boolean enabled) {
        put("mouse_on", enabled);
    }

    public float opacity() {
        return (float) data.optDouble("opacity", 0.85);
    }

    public void setOpacity(float opacity) {
        try {
            data.put("opacity", (double) opacity);
        } catch (JSONException ignored) {
        }
        save();
    }

    public float scale() {
        return (float) data.optDouble("scale", 1.0);
    }

    /** 虚拟鼠标（光标 + 三个圆钮）的独立缩放。 */
    public float mouseScale() {
        return (float) data.optDouble("mouse_scale", 1.0);
    }

    public void setMouseScale(float scale) {
        try {
            data.put("mouse_scale", (double) scale);
        } catch (JSONException ignored) {
        }
        save();
    }

    /** 工具条是否收起；收起只影响工具条，不影响手柄/鼠标。 */
    public boolean collapsed() {
        return data.optBoolean("collapsed", false);
    }

    public void setCollapsed(boolean collapsed) {
        put("collapsed", collapsed);
    }

    /** 是否显示鼠标模式底部那行操作提示。 */
    public boolean showHint() {
        return data.optBoolean("show_hint", true);
    }

    public void setShowHint(boolean enabled) {
        put("show_hint", enabled);
    }

    public void setScale(float scale) {
        try {
            data.put("scale", (double) scale);
        } catch (JSONException ignored) {
        }
        save();
    }

    public String keyFor(String button) {
        JSONObject mapping = data.optJSONObject("mapping");
        String value = mapping != null ? mapping.optString(button, null) : null;
        if (value != null && KeyCatalog.get(value) != null) return value;
        String fallback = DEFAULT_KEYS.get(button);
        return fallback != null ? fallback : "ENTER";
    }

    public void setKeyFor(String button, String keyId) {
        try {
            JSONObject mapping = data.optJSONObject("mapping");
            if (mapping == null) {
                mapping = new JSONObject();
                data.put("mapping", mapping);
            }
            mapping.put(button, keyId);
        } catch (JSONException ignored) {
        }
        save();
    }

    /** 控件组中心点，归一化到 0..1。 */
    public float[] center(String cluster, float defaultX, float defaultY) {
        JSONObject layout = data.optJSONObject("layout");
        JSONObject node = layout != null ? layout.optJSONObject(cluster) : null;
        if (node == null) return new float[]{defaultX, defaultY};
        return new float[]{
                (float) node.optDouble("x", defaultX),
                (float) node.optDouble("y", defaultY)
        };
    }

    public void setCenter(String cluster, float x, float y) {
        try {
            JSONObject layout = data.optJSONObject("layout");
            if (layout == null) {
                layout = new JSONObject();
                data.put("layout", layout);
            }
            JSONObject node = new JSONObject();
            node.put("x", (double) x);
            node.put("y", (double) y);
            layout.put(cluster, node);
        } catch (JSONException ignored) {
        }
        save();
    }

    /** 当前这套虚拟按键。没有自定义过时返回内置默认键位。 */
    public List<VirtualKey> keys() {
        JSONArray array = data.optJSONArray("keys");
        if (array == null) return VirtualKey.defaults();
        List<VirtualKey> list = new ArrayList<>();
        for (int i = 0; i < array.length(); i++) {
            VirtualKey key = VirtualKey.fromJson(array.optJSONObject(i));
            if (key != null) list.add(key);
        }
        return list;
    }

    public void saveKeys(List<VirtualKey> keys) {
        JSONArray array = new JSONArray();
        for (VirtualKey key : keys) array.put(key.toJson());
        try {
            data.put("keys", array);
        } catch (JSONException ignored) {
        }
        save();
    }

    public void resetKeys() {
        data.remove("keys");
        // 方向盘的四个方向映射也一起回到默认（普通按键的映射在 keys 里）。
        data.remove("mapping");
        save();
    }

    /** 单个虚拟按键的绝对位置（归一化）；没有自定义过返回 null。 */
    public float[] buttonCenter(String id) {
        JSONObject node = buttonNodeIfExists(id);
        if (node == null || !node.has("x") || !node.has("y")) return null;
        return new float[]{
                (float) node.optDouble("x", 0.5),
                (float) node.optDouble("y", 0.5)
        };
    }

    public void setButtonCenter(String id, float x, float y) {
        try {
            JSONObject node = buttonNode(id);
            node.put("x", (double) x);
            node.put("y", (double) y);
        } catch (JSONException ignored) {
        }
        save();
    }

    /** 单个按键相对全局手柄大小的倍率。 */
    public float buttonScale(String id) {
        JSONObject node = buttonNodeIfExists(id);
        return node != null ? (float) node.optDouble("scale", 1.0) : 1.0f;
    }

    public void setButtonScale(String id, float scale) {
        try {
            JSONObject node = buttonNode(id);
            node.put("scale", (double) scale);
        } catch (JSONException ignored) {
        }
        save();
    }

    public void clearButtonLayout(String id) {
        JSONObject buttons = data.optJSONObject("buttons");
        if (buttons != null) {
            buttons.remove(id);
            save();
        }
    }

    private JSONObject buttonNodeIfExists(String id) {
        JSONObject buttons = data.optJSONObject("buttons");
        return buttons != null ? buttons.optJSONObject(id) : null;
    }

    private JSONObject buttonNode(String id) throws JSONException {
        JSONObject buttons = data.optJSONObject("buttons");
        if (buttons == null) {
            buttons = new JSONObject();
            data.put("buttons", buttons);
        }
        JSONObject node = buttons.optJSONObject(id);
        if (node == null) {
            node = new JSONObject();
            buttons.put(id, node);
        }
        return node;
    }

    public void reset() {
        data = new JSONObject();
        save();
    }

    private void put(String name, Object value) {
        try {
            data.put(name, value);
        } catch (JSONException ignored) {
        }
        save();
    }
}
