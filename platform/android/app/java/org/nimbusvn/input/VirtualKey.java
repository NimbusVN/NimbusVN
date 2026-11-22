package org.nimbusvn.input;

import org.json.JSONException;
import org.json.JSONObject;

import java.util.ArrayList;
import java.util.List;

/**
 * 一个虚拟按键实例：位置、大小、外观、映射到的物理键都是独立的。
 *
 * <p>默认只提供一套内置键位（方向盘 + ABXY + L/R + 菜单/跳过），
 * 之后用户可以任意增删、拖动、缩放。</p>
 */
public final class VirtualKey {

    public static final int SHAPE_CIRCLE = 0;
    public static final int SHAPE_PILL = 1;
    public static final int SHAPE_DPAD = 2;

    private static final int[] PALETTE = {
            0xFF62D68B, 0xFFFF7A7A, 0xFF5AB0FF, 0xFFFFC94D,
            0xFFB98CFF, 0xFF6FE3D6, 0xFFFF9F6B, 0xFF9AA8FF
    };

    public String id;
    public String keyId;
    public String label;
    public float x;
    public float y;
    public float scale;
    public int shape;
    public int color;

    public VirtualKey(String id, String keyId, String label,
                      float x, float y, float scale, int shape, int color) {
        this.id = id;
        this.keyId = keyId;
        this.label = label;
        this.x = x;
        this.y = y;
        this.scale = scale;
        this.shape = shape;
        this.color = color;
    }

    public boolean isDpad() {
        return shape == SHAPE_DPAD;
    }

    public JSONObject toJson() {
        JSONObject object = new JSONObject();
        try {
            object.put("id", id);
            object.put("key", keyId);
            object.put("label", label);
            object.put("x", (double) x);
            object.put("y", (double) y);
            object.put("scale", (double) scale);
            object.put("shape", shape);
            object.put("color", color);
        } catch (JSONException ignored) {
        }
        return object;
    }

    public static VirtualKey fromJson(JSONObject object) {
        if (object == null) return null;
        try {
            String id = object.optString("id", null);
            if (id == null || id.isEmpty()) id = "k" + System.nanoTime();
            return new VirtualKey(
                    id,
                    object.optString("key", "ENTER"),
                    object.optString("label", "?"),
                    (float) object.optDouble("x", 0.5),
                    (float) object.optDouble("y", 0.5),
                    (float) object.optDouble("scale", 1.0),
                    object.optInt("shape", SHAPE_CIRCLE),
                    object.optInt("color", PALETTE[0]));
        } catch (Throwable t) {
            return null;
        }
    }

    /** 内置默认键位：与录屏里的布局一致。 */
    public static List<VirtualKey> defaults() {
        List<VirtualKey> list = new ArrayList<>();
        list.add(new VirtualKey("dpad", "", "方向盘",
                0.105f, 0.620f, 1f, SHAPE_DPAD, 0xFF4A9EFF));
        list.add(new VirtualKey("A", "ENTER", "A",
                0.953f, 0.640f, 1f, SHAPE_CIRCLE, 0xFF62D68B));
        list.add(new VirtualKey("B", "ESCAPE", "B",
                0.875f, 0.755f, 1f, SHAPE_CIRCLE, 0xFFFF7A7A));
        list.add(new VirtualKey("X", "CTRL", "X",
                0.797f, 0.640f, 1f, SHAPE_CIRCLE, 0xFF5AB0FF));
        list.add(new VirtualKey("Y", "SPACE", "Y",
                0.875f, 0.525f, 1f, SHAPE_CIRCLE, 0xFFFFC94D));
        list.add(new VirtualKey("L", "PAGE_UP", "L",
                0.787f, 0.425f, 1f, SHAPE_CIRCLE, 0xFF6FA8FF));
        list.add(new VirtualKey("R", "PAGE_DOWN", "R",
                0.963f, 0.425f, 1f, SHAPE_CIRCLE, 0xFF6FA8FF));
        list.add(new VirtualKey("MENU", "TAB", "菜单",
                0.120f, 0.100f, 1f, SHAPE_PILL, 0xFFB9C4D4));
        list.add(new VirtualKey("SKIP", "CTRL", "跳过",
                0.205f, 0.100f, 1f, SHAPE_PILL, 0xFFB9C4D4));
        return list;
    }

    /** 新建一个按键：按标签长度自动选圆形或胶囊，位置从屏幕中间略微错开。 */
    public static VirtualKey create(String keyId, int count) {
        String label = KeyCatalog.label(keyId);
        int shape = label.length() > 2 ? SHAPE_PILL : SHAPE_CIRCLE;
        float x = 0.42f + (count % 4) * 0.045f;
        float y = 0.50f + (count % 3) * 0.05f;
        return new VirtualKey(
                "k" + System.currentTimeMillis() + "_" + count,
                keyId,
                label,
                x,
                y,
                1f,
                shape,
                PALETTE[Math.abs(keyId.hashCode()) % PALETTE.length]);
    }
}
