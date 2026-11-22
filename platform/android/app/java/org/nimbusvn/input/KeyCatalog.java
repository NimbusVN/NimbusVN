package org.nimbusvn.input;

import android.view.KeyEvent;

import java.util.ArrayList;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;

/**
 * 虚拟按键可映射的物理键目录。
 *
 * <p>虚拟手柄的每个按钮最终都映射到这里的一个 Android keycode；
 * SDL 的 Android 后端会把它翻译成对应的 SDL scancode，因此同一个键位表
 * 对 krkrsdl3 / openartemis / onscripter（以及以后的 RPG 内核）都成立。</p>
 */
public final class KeyCatalog {

    public static final class Key {
        public final String id;
        public final String label;
        public final String group;
        public final int keycode;

        Key(String id, String label, String group, int keycode) {
            this.id = id;
            this.label = label;
            this.group = group;
            this.keycode = keycode;
        }
    }

    private static final Map<String, Key> BY_ID = new LinkedHashMap<>();
    private static final List<Key> ALL = new ArrayList<>();
    private static final List<String> GROUPS = new ArrayList<>();

    private static void add(String group, String id, String label, int keycode) {
        if (!GROUPS.contains(group)) GROUPS.add(group);
        Key key = new Key(id, label, group, keycode);
        BY_ID.put(id, key);
        ALL.add(key);
    }

    static {
        add("方向", "UP", "↑", KeyEvent.KEYCODE_DPAD_UP);
        add("方向", "DOWN", "↓", KeyEvent.KEYCODE_DPAD_DOWN);
        add("方向", "LEFT", "←", KeyEvent.KEYCODE_DPAD_LEFT);
        add("方向", "RIGHT", "→", KeyEvent.KEYCODE_DPAD_RIGHT);

        add("确认 / 取消", "ENTER", "Enter", KeyEvent.KEYCODE_ENTER);
        add("确认 / 取消", "SPACE", "Space", KeyEvent.KEYCODE_SPACE);
        add("确认 / 取消", "ESCAPE", "Esc", KeyEvent.KEYCODE_ESCAPE);
        add("确认 / 取消", "TAB", "Tab", KeyEvent.KEYCODE_TAB);

        add("功能", "CTRL", "Ctrl", KeyEvent.KEYCODE_CTRL_LEFT);
        add("功能", "SHIFT", "Shift", KeyEvent.KEYCODE_SHIFT_LEFT);
        add("功能", "ALT", "Alt", KeyEvent.KEYCODE_ALT_LEFT);
        add("功能", "PAGE_UP", "PgUp", KeyEvent.KEYCODE_PAGE_UP);
        add("功能", "PAGE_DOWN", "PgDn", KeyEvent.KEYCODE_PAGE_DOWN);
        add("功能", "HOME", "Home", KeyEvent.KEYCODE_MOVE_HOME);
        add("功能", "END", "End", KeyEvent.KEYCODE_MOVE_END);
        add("功能", "INSERT", "Ins", KeyEvent.KEYCODE_INSERT);
        add("功能", "DELETE", "Del", KeyEvent.KEYCODE_FORWARD_DEL);
        add("功能", "BACKSPACE", "退格", KeyEvent.KEYCODE_DEL);

        for (int i = 1; i <= 12; i++) {
            add("F 键", "F" + i, "F" + i, KeyEvent.KEYCODE_F1 + (i - 1));
        }

        for (char ch = 'A'; ch <= 'Z'; ch++) {
            add("字母", String.valueOf(ch), String.valueOf(ch),
                    KeyEvent.KEYCODE_A + (ch - 'A'));
        }

        for (char ch = '0'; ch <= '9'; ch++) {
            add("数字", String.valueOf(ch), String.valueOf(ch),
                    KeyEvent.KEYCODE_0 + (ch - '0'));
        }
    }

    public static Key get(String id) {
        return id == null ? null : BY_ID.get(id);
    }

    public static List<Key> all() {
        return ALL;
    }

    public static List<String> groups() {
        return GROUPS;
    }

    public static List<Key> group(String group) {
        List<Key> list = new ArrayList<>();
        for (Key key : ALL) {
            if (key.group.equals(group)) list.add(key);
        }
        return list;
    }

    public static String label(String id) {
        Key key = get(id);
        return key != null ? key.label : "—";
    }

    public static int keycode(String id) {
        Key key = get(id);
        return key != null ? key.keycode : 0;
    }

    private KeyCatalog() {
    }
}
