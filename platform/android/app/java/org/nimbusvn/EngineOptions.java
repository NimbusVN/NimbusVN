package org.nimbusvn;

import org.json.JSONObject;

import java.util.ArrayList;
import java.util.Collections;
import java.util.HashMap;
import java.util.List;
import java.util.Locale;
import java.util.Map;

/**
 * Per-engine option schema.
 *
 * <p>Each engine declares the option groups it supports and how a choice maps
 * to the engine's command line.  The game-detail UI is generated from this
 * table, and {@code buildLaunchArgs()} consumes the same table, so adding a
 * new engine (or a new option for one engine) only touches one place.</p>
 *
 * <p>Per-game values live in the game's config JSON under the group key, e.g.
 * {@code {"backend":"vulkan","platform":"windows"}}.  Missing values fall back
 * to the group default, so old configs keep working.</p>
 */
public final class EngineOptions {
    public static final class Choice {
        public final String id;
        public final String label;
        /** CLI switch name, e.g. "-render" or "--platform". */
        public final String cliName;
        /** Switch value; null means "let the engine decide" (no argument). */
        public final String cliValue;

        Choice(String id, String label, String cliName, String cliValue) {
            this.id = id;
            this.label = label;
            this.cliName = cliName;
            this.cliValue = cliValue;
        }
    }

    public static final class Group {
        public final String key;
        public final String label;
        /** CLI switch name shared by all choices in this group. */
        public final String cliName;
        public final String defaultId;
        public final List<Choice> choices;

        Group(String key, String label, String cliName, String defaultId, Choice... choices) {
            this.key = key;
            this.label = label;
            this.cliName = cliName;
            this.defaultId = defaultId;
            List<Choice> list = new ArrayList<>();
            Collections.addAll(list, choices);
            this.choices = Collections.unmodifiableList(list);
        }

        public Choice find(String id) {
            if (id != null) {
                String v = id.trim().toLowerCase(Locale.US);
                for (Choice c : choices) {
                    if (c.id.equals(v)) return c;
                }
            }
            for (Choice c : choices) {
                if (c.id.equals(defaultId)) return c;
            }
            return choices.isEmpty() ? null : choices.get(0);
        }
    }

    public static final class Profile {
        public final String engineId;
        public final List<Group> groups;

        Profile(String engineId, Group... groups) {
            this.engineId = engineId;
            List<Group> list = new ArrayList<>();
            Collections.addAll(list, groups);
            this.groups = Collections.unmodifiableList(list);
        }
    }

    private static final Map<String, Profile> PROFILES = new HashMap<>();

    static {
        // krkrsdl3: renderer only; no platform selection.
        PROFILES.put("krkr", new Profile("krkr",
                new Group("backend", "渲染后端", "-render", "auto",
                        new Choice("auto", "自动", "-render", null),
                        new Choice("software", "软件", "-render", "software"),
                        new Choice("opengl", "GL", "-render", "opengl"),
                        new Choice("vulkan", "Vulkan", "-render", "vulkan"))));

        // openartemis: SDL/GL renderer + system.ini platform section.
        PROFILES.put("openartemis", new Profile("openartemis",
                new Group("backend", "渲染后端", "--renderer", "sdl",
                        new Choice("sdl", "SDL", "--renderer", "sdl"),
                        new Choice("gles", "GL", "--renderer", "gles")),
                new Group("platform", "平台", "--platform", "android",
                        new Choice("android", "android", "--platform", "android"),
                        new Choice("windows", "windows", "--platform", "windows"))));

        // ONScripter has no renderer/platform switches.
        PROFILES.put("onscripter", new Profile("onscripter"));
    }

    private EngineOptions() {
    }

    /** Returns null for an unknown/future engine (no generated options). */
    public static Profile forId(String engineId) {
        if (engineId == null) return null;
        return PROFILES.get(engineId.trim().toLowerCase(Locale.US));
    }

    /** Every option key known across all engines (for config cleanup). */
    public static java.util.Set<String> allKeys() {
        java.util.Set<String> keys = new java.util.HashSet<>();
        for (Profile p : PROFILES.values()) {
            for (Group g : p.groups) keys.add(g.key);
        }
        return keys;
    }

    public static String value(JSONObject config, Group group) {
        if (config == null) return group.defaultId;
        String v = config.optString(group.key, group.defaultId);
        if (v == null || v.trim().isEmpty()) v = group.defaultId;
        return group.find(v).id;
    }

    /** Returns e.g. "-render=vulkan", or null for "auto"/no argument. */
    public static String argument(Group group, String valueId) {
        Choice c = group.find(valueId);
        if (c == null || c.cliValue == null || c.cliName == null || c.cliName.isEmpty())
            return null;
        return c.cliName + "=" + c.cliValue;
    }

    public static String choiceLabel(Group group, String valueId) {
        Choice c = group.find(valueId);
        return c != null ? c.label : valueId;
    }
}
