package org.nimbusvn.launcher;

import android.content.Context;
import android.util.Log;

import org.json.JSONException;
import org.json.JSONObject;

import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.nio.charset.StandardCharsets;
import java.text.SimpleDateFormat;
import java.util.ArrayList;
import java.util.Date;
import java.util.List;
import java.util.Locale;
import java.util.TimeZone;
import java.util.zip.CRC32;
import java.util.zip.ZipEntry;
import java.util.zip.ZipInputStream;
import java.util.zip.ZipOutputStream;

/**
 * 每游戏独立存档目录（隔离存档）的分配、迁移与导入导出。
 *
 * <p>分配规则：{@code <应用私有 files>/saves/<内核>/<游戏名>-<路径哈希>}。</p>
 * <ul>
 *   <li>私有目录路径短、无权限问题，也不在启动器的游戏扫描范围里；</li>
 *   <li>带上完整路径的 CRC32，两个同名游戏目录不会互相踩存档；</li>
 *   <li>目录名保留游戏名，用户导出/排查时能直接认出来。</li>
 * </ul>
 *
 * <p>导入导出都用 zip：导出的包里根目录直接是存档文件，另外附带一个
 * {@link #MANIFEST_NAME} 记录游戏名/内核/来源路径，导入时可以提示是否跨游戏。</p>
 */
public final class SaveStore {
    private static final String TAG = "## nimbusvn.save";

    /** 单人配置里控制隔离存档的键（缺省时跟随全局开关）。 */
    public static final String CONFIG_KEY = "scopedsavedir";
    /** 导出包里的清单文件名。 */
    public static final String MANIFEST_NAME = "nimbus-save.json";

    private static final String ROOT_NAME = "saves";
    private static final int SLUG_MAX = 24;
    private static final int MAX_ENTRIES = 4096;
    /** 存档包一般只有几 MB；上限压住内存占用，也顺手挡掉 zip 炸弹。 */
    private static final long MAX_TOTAL_BYTES = 64L * 1024 * 1024;
    private static final long MAX_ENTRY_BYTES = 16L * 1024 * 1024;
    private static final int COPY_BUFFER = 32 * 1024;

    private SaveStore() {
    }

    // ========== 目录分配 ==========

    /** 存档总根：<files>/saves。 */
    public static File root(Context context) {
        File dir = new File(context.getFilesDir(), ROOT_NAME);
        if (!dir.exists() && !dir.mkdirs()) {
            Log.w(TAG, "cannot create save root: " + dir);
        }
        return dir;
    }

    /**
     * 给一个游戏分配（并创建）独立存档目录。同一个游戏路径每次得到同一个目录。
     *
     * @param engineId 内核 id（krkr / openartemis / onscripter）
     * @param gamePath 游戏目录（绝对路径即可，尾部的 '/' 会被忽略）
     */
    public static File dirFor(Context context, String engineId, String gamePath) {
        return dirFor(context, engineId, gamePath, true);
    }

    /**
     * @param create false 只算路径不建目录（详情页展示用，避免随手点开就
     *               给每个游戏留一个空目录）
     */
    public static File dirFor(Context context, String engineId, String gamePath, boolean create) {
        File engineRoot = new File(root(context), safeSegment(engineId));
        if (create && !engineRoot.exists() && !engineRoot.mkdirs()) {
            Log.w(TAG, "cannot create engine save dir: " + engineRoot);
        }

        String normalized = normalizePath(gamePath);
        String name = baseName(normalized);
        File dir = new File(engineRoot, slug(name) + "-" + pathHash(normalized));
        if (create && !dir.exists() && !dir.mkdirs()) {
            Log.w(TAG, "cannot create game save dir: " + dir);
        }
        return dir;
    }

    /** 单人配置优先，没有配置时用全局开关。 */
    public static boolean isIsolated(JSONObject gameConfig, boolean globalDefault) {
        if (gameConfig != null && gameConfig.has(CONFIG_KEY)) {
            return gameConfig.optBoolean(CONFIG_KEY, globalDefault);
        }
        return globalDefault;
    }

    /** 旧版布局：<external-files>/save/<游戏名>。只用于迁移，不再写入。 */
    public static File legacyDir(Context context, String gamePath) {
        File ext = context.getExternalFilesDir(null);
        if (ext == null) return null;
        String name = baseName(normalizePath(gamePath));
        if (name.isEmpty()) return null;
        return new File(new File(ext, "save"), name);
    }

    /**
     * 把旧布局的存档搬到新的隔离目录。
     *
     * <p>只在目标目录还没有文件时迁移，避免覆盖新存档；复制成功也不删除旧目录，
     * 大不了多占一份空间，用户的存档不会因为一次迁移失败而丢。</p>
     *
     * @return 复制过去的文件数（0 表示没有可迁移内容）
     */
    public static int migrateLegacy(Context context, String gamePath, File target) {
        if (target == null || countFiles(target) > 0) return 0;
        File legacy = legacyDir(context, gamePath);
        if (legacy == null || !legacy.isDirectory()) return 0;
        try {
            int copied = copyTree(legacy, target);
            if (copied > 0) {
                Log.i(TAG, "migrated " + copied + " save file(s) from " + legacy + " to " + target);
            }
            return copied;
        } catch (IOException e) {
            Log.w(TAG, "migrate legacy saves failed: " + e.getMessage());
            return 0;
        }
    }

    // ========== 统计与清空 ==========

    public static int countFiles(File dir) {
        if (dir == null || !dir.isDirectory()) return 0;
        int count = 0;
        File[] children = dir.listFiles();
        if (children == null) return 0;
        for (File child : children) {
            if (child.isDirectory()) count += countFiles(child);
            else count++;
        }
        return count;
    }

    public static long totalBytes(File dir) {
        if (dir == null || !dir.isDirectory()) return 0;
        long total = 0;
        File[] children = dir.listFiles();
        if (children == null) return 0;
        for (File child : children) {
            total += child.isDirectory() ? totalBytes(child) : child.length();
        }
        return total;
    }

    /** 人类可读的大小，例如 "1.2 MB"。 */
    public static String formatBytes(long bytes) {
        if (bytes < 1024) return bytes + " B";
        double kb = bytes / 1024.0;
        if (kb < 1024) return String.format(Locale.US, "%.1f KB", kb);
        double mb = kb / 1024.0;
        if (mb < 1024) return String.format(Locale.US, "%.1f MB", mb);
        return String.format(Locale.US, "%.2f GB", mb / 1024.0);
    }

    /**
     * 清空目录里的存档文件（保留目录本身）。
     *
     * <p>只允许删 {@code storeRoot} 之下的目录，避免参数传错删到游戏目录。</p>
     *
     * @return 删除的文件数
     */
    public static int clear(File dir, File storeRoot) {
        if (dir == null || storeRoot == null || !dir.isDirectory()) return 0;
        try {
            String base = storeRoot.getCanonicalPath() + File.separator;
            String path = dir.getCanonicalPath() + File.separator;
            if (!path.startsWith(base) || path.equals(base)) {
                Log.w(TAG, "refuse to clear outside save root: " + dir);
                return 0;
            }
        } catch (IOException e) {
            return 0;
        }
        return deleteChildren(dir);
    }

    private static int deleteChildren(File dir) {
        int removed = 0;
        File[] children = dir.listFiles();
        if (children == null) return 0;
        for (File child : children) {
            if (child.isDirectory()) {
                removed += deleteChildren(child);
                // 空目录也一并删掉
                //noinspection ResultOfMethodCallIgnored
                child.delete();
            } else if (child.delete()) {
                removed++;
            }
        }
        return removed;
    }

    // ========== 导出 ==========

    /** 生成导出包的清单（放在 zip 根目录，不写进存档目录）。 */
    public static JSONObject makeManifest(String engineId, String gamePath, String gameName,
                                          File dir) {
        JSONObject manifest = new JSONObject();
        try {
            manifest.put("format", 1);
            manifest.put("engine", engineId == null ? "" : engineId);
            manifest.put("game", gameName == null ? baseName(normalizePath(gamePath)) : gameName);
            manifest.put("path", normalizePath(gamePath));
            manifest.put("files", countFiles(dir));
            manifest.put("bytes", totalBytes(dir));
            SimpleDateFormat fmt = new SimpleDateFormat("yyyy-MM-dd'T'HH:mm:ssXXX", Locale.US);
            fmt.setTimeZone(TimeZone.getDefault());
            manifest.put("exportedAt", fmt.format(new Date()));
        } catch (JSONException ignored) {
        }
        return manifest;
    }

    /**
     * 把存档目录打包成 zip。包内条目是相对存档目录的路径，根目录直接是文件。
     *
     * @return 写进包里的文件数
     */
    public static int exportZip(File dir, JSONObject manifest, OutputStream out)
            throws IOException {
        if (dir == null || !dir.isDirectory()) {
            throw new IOException("存档目录不存在");
        }
        List<File> files = new ArrayList<>();
        collectFiles(dir, files);
        if (files.isEmpty()) {
            throw new IOException("这个游戏还没有存档");
        }

        ZipOutputStream zos = new ZipOutputStream(out);
        byte[] buffer = new byte[COPY_BUFFER];
        if (manifest != null) {
            zos.putNextEntry(new ZipEntry(MANIFEST_NAME));
            zos.write(manifest.toString().getBytes(StandardCharsets.UTF_8));
            zos.closeEntry();
        }
        for (File file : files) {
            String rel = relativePath(dir, file);
            if (rel.isEmpty()) continue;
            zos.putNextEntry(new ZipEntry(rel));
            try (InputStream in = new FileInputStream(file)) {
                int n;
                while ((n = in.read(buffer)) > 0) zos.write(buffer, 0, n);
            }
            zos.closeEntry();
        }
        zos.finish();
        zos.flush();
        return files.size();
    }

    // ========== 导入 ==========

    /** 导入结果：写入了多少文件、包里的来源信息（用于跨游戏提示）。 */
    public static final class ImportResult {
        public int files;
        public long bytes;
        public String engine = "";
        public String game = "";
        public String path = "";

        public boolean hasSource() {
            return !game.isEmpty() || !path.isEmpty();
        }
    }

    /**
     * 把 zip 解到存档目录（同名文件覆盖）。
     *
     * <p>只接受普通文件条目：绝对路径、含 {@code ..} 的条目一律跳过。带
     * {@link #MANIFEST_NAME} 清单的包（本启动器导出的）按原样落盘；没有清单的包
     * 才把唯一的顶层目录剥掉——那是“用户自己压缩整个文件夹”的常见形态。</p>
     */
    public static ImportResult importZip(File dir, File storeRoot, InputStream in)
            throws IOException {
        if (dir == null || !dir.isDirectory()) {
            throw new IOException("存档目录不存在");
        }
        String base;
        try {
            base = storeRoot.getCanonicalPath() + File.separator;
        } catch (IOException e) {
            throw new IOException("存档根目录不可用");
        }

        List<Entry> entries = readEntries(in);
        if (entries.isEmpty()) {
            throw new IOException("压缩包里没有文件");
        }

        ImportResult result = new ImportResult();
        boolean hasManifest = false;
        for (Entry entry : entries) {
            if (MANIFEST_NAME.equals(entry.name)) {
                hasManifest = true;
                readManifest(entry, result);
            }
        }

        // 带清单的包是自己导出的，目录结构就是存档结构，绝不能剥层——
        // 有些内核的存档本来就全部放在一个子目录里（ONS 的 savedir 命令）。
        // 没有清单才当成“用户自己压缩整个文件夹”，剥掉唯一的顶层目录。
        String prefix = hasManifest ? null : commonRoot(entries);
        long total = 0;
        for (Entry entry : entries) {
            String name = entry.name;
            if (MANIFEST_NAME.equals(name)) continue;
            if (prefix != null && name.startsWith(prefix)) {
                name = name.substring(prefix.length());
            }
            String rel = safeRelative(name);
            if (rel.isEmpty()) continue;

            File out = new File(dir, rel);
            String canonical = out.getCanonicalPath();
            if (!canonical.startsWith(base)) {
                Log.w(TAG, "skip unsafe zip entry: " + entry.name);
                continue;
            }
            File parent = out.getParentFile();
            if (parent != null && !parent.exists() && !parent.mkdirs()) {
                continue;
            }
            long written = 0;
            try (OutputStream os = new FileOutputStream(out)) {
                InputStream entryIn = new java.io.ByteArrayInputStream(entry.data);
                byte[] buffer = new byte[COPY_BUFFER];
                int n;
                while ((n = entryIn.read(buffer)) > 0) {
                    written += n;
                    total += n;
                    if (written > MAX_ENTRY_BYTES || total > MAX_TOTAL_BYTES) {
                        throw new IOException("压缩包解压后过大");
                    }
                    os.write(buffer, 0, n);
                }
            }
            result.files++;
            result.bytes += written;
        }
        if (result.files == 0) {
            throw new IOException("压缩包里没有可导入的存档");
        }
        return result;
    }

    // ========== 内部工具 ==========

    private static final class Entry {
        final String name;
        final byte[] data;

        Entry(String name, byte[] data) {
            this.name = name;
            this.data = data;
        }
    }

    /**
     * 先把整个 zip 读进内存（存档包一般很小），顺便做条目数/大小上限检查。
     * 这样在真正落盘前就能确定“是否只有一个顶层目录”，不用二次读取。
     */
    private static List<Entry> readEntries(InputStream in) throws IOException {
        List<Entry> entries = new ArrayList<>();
        ZipInputStream zis = new ZipInputStream(in);
        long total = 0;
        ZipEntry ze;
        while ((ze = zis.getNextEntry()) != null) {
            if (!ze.isDirectory()) {
                String name = ze.getName();
                if (name != null && !name.isEmpty()) {
                    if (entries.size() >= MAX_ENTRIES) throw new IOException("压缩包条目过多");

                    java.io.ByteArrayOutputStream buffer = new java.io.ByteArrayOutputStream();
                    byte[] chunk = new byte[COPY_BUFFER];
                    int n;
                    while ((n = zis.read(chunk)) > 0) {
                        total += n;
                        if (total > MAX_TOTAL_BYTES || buffer.size() + (long) n > MAX_ENTRY_BYTES) {
                            throw new IOException("压缩包解压后过大");
                        }
                        buffer.write(chunk, 0, n);
                    }
                    entries.add(new Entry(name, buffer.toByteArray()));
                }
            }
            zis.closeEntry();
        }
        return entries;
    }

    private static void readManifest(Entry entry, ImportResult result) {
        try {
            JSONObject manifest = new JSONObject(new String(entry.data, StandardCharsets.UTF_8));
            result.engine = manifest.optString("engine", "");
            result.game = manifest.optString("game", "");
            result.path = manifest.optString("path", "");
        } catch (JSONException | RuntimeException ignored) {
        }
    }

    /** 所有条目共用的第一层目录名（以 '/' 结尾）；没有就返回 null。 */
    private static String commonRoot(List<Entry> entries) {
        String first = null;
        for (Entry entry : entries) {
            if (MANIFEST_NAME.equals(entry.name)) continue;
            String name = entry.name.replace('\\', '/');
            int slash = name.indexOf('/');
            if (slash <= 0) return null; // 根目录直接有文件
            String root = name.substring(0, slash + 1);
            if (first == null) first = root;
            else if (!first.equals(root)) return null;
        }
        return first;
    }

    private static String safeRelative(String name) {
        String n = name.replace('\\', '/');
        while (n.startsWith("/")) n = n.substring(1);
        if (n.isEmpty() || n.contains(":")) return "";
        String[] parts = n.split("/");
        StringBuilder sb = new StringBuilder();
        for (String part : parts) {
            if (part.isEmpty() || ".".equals(part)) continue;
            if ("..".equals(part)) return ""; // 拒绝路径穿越，不做“消毒后继续写”
            if (sb.length() > 0) sb.append('/');
            sb.append(part);
        }
        return sb.toString();
    }

    private static void collectFiles(File dir, List<File> out) {
        File[] children = dir.listFiles();
        if (children == null) return;
        List<File> sorted = new ArrayList<>();
        for (File child : children) sorted.add(child);
        sorted.sort((a, b) -> a.getName().compareToIgnoreCase(b.getName()));
        for (File child : sorted) {
            if (child.isDirectory()) collectFiles(child, out);
            else out.add(child);
        }
    }

    private static String relativePath(File root, File file) {
        try {
            String base = root.getCanonicalPath();
            String path = file.getCanonicalPath();
            if (!path.startsWith(base)) return "";
            String rel = path.substring(base.length());
            while (rel.startsWith(File.separator) || rel.startsWith("/")) {
                rel = rel.substring(1);
            }
            return rel.replace(File.separatorChar, '/');
        } catch (IOException e) {
            return "";
        }
    }

    private static int copyTree(File from, File to) throws IOException {
        int copied = 0;
        File[] children = from.listFiles();
        if (children == null) return 0;
        for (File child : children) {
            File target = new File(to, child.getName());
            if (child.isDirectory()) {
                if (!target.exists() && !target.mkdirs()) continue;
                copied += copyTree(child, target);
            } else {
                try (InputStream in = new FileInputStream(child);
                     OutputStream out = new FileOutputStream(target)) {
                    byte[] buffer = new byte[COPY_BUFFER];
                    int n;
                    while ((n = in.read(buffer)) > 0) out.write(buffer, 0, n);
                }
                copied++;
            }
        }
        return copied;
    }

    private static String normalizePath(String path) {
        if (path == null) return "";
        String p = path.trim().replace('\\', '/');
        while (p.length() > 1 && p.endsWith("/")) {
            p = p.substring(0, p.length() - 1);
        }
        return p;
    }

    private static String baseName(String normalizedPath) {
        if (normalizedPath == null || normalizedPath.isEmpty()) return "";
        int idx = normalizedPath.lastIndexOf('/');
        return idx >= 0 ? normalizedPath.substring(idx + 1) : normalizedPath;
    }

    /** 目录名：字母/数字/中日文保留，其它字符压成单个下划线，最多 24 个字符。 */
    private static String slug(String name) {
        StringBuilder sb = new StringBuilder();
        for (int i = 0; i < name.length() && sb.length() < SLUG_MAX; i++) {
            char ch = name.charAt(i);
            if (Character.isLetterOrDigit(ch) || ch == '-' || ch == '_') sb.append(ch);
            else sb.append('_');
        }
        String s = sb.toString().replaceAll("_+", "_");
        while (s.startsWith("_")) s = s.substring(1);
        while (s.endsWith("_")) s = s.substring(0, s.length() - 1);
        return s.isEmpty() ? "game" : s;
    }

    private static String safeSegment(String engineId) {
        if (engineId == null || engineId.trim().isEmpty()) return "unknown";
        String s = engineId.trim().toLowerCase(Locale.US).replaceAll("[^a-z0-9_]", "");
        return s.isEmpty() ? "unknown" : s;
    }

    private static String pathHash(String normalizedPath) {
        CRC32 crc = new CRC32();
        crc.update(normalizedPath.getBytes(StandardCharsets.UTF_8));
        return String.format(Locale.US, "%08x", crc.getValue());
    }
}
