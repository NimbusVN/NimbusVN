#pragma once
// 核心测试包装类（TestHost）—— 模拟 main.cpp 的**无头静音**运行环境。
//
// 包装类**只做 main.cpp 会做的事**，一件不多：
//   1. SDL_Init(VIDEO)，建隐藏窗口（尺寸 = stage），attach 到 App；
//   2. 音频静音；
//   3. 事件走 App 的 mouse_*/key_event/wheel —— 和窗口模式**同一条路径**，
//      测试里点的这一下，和玩家点的那一下进的是同一个输入状态机；
//   4. tick 用固定 16ms（AppConfig::headless），结果可复现。
//
// 断言的两个面：
//   * **数值**：`plan()` 返回上一帧的渲染队列（每一项都有 id / 世界矩形 /
//     不透明度 / 是否纯色）。"该不该画、画在哪"全在这里，不需要看像素。
//   * **画面**：`shot()` 真实读回 RGBA，`save_png()` 落盘。
//   状态跟进用 `visible(id)` / `wait_for(id)`：某个关键 layer 在队列里出现
//   或消失，就是流程走到位的判据。
//
// 用法：
//   oa::test::TestHost host("/path/to/root.pfs");
//   if (!host.ok()) return 77;
//   host.click(960, 540);
//   host.wait_for("1.80.mw.adv", 300);
//   auto items = host.plan();
//   host.save_png("/tmp/frame.png");
#include <SDL3/SDL.h>

#include <cstdio>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "core/media/image.h"
#include "core/render/renderer.h"
#include "core/render/text.h"
#include "core/runtime/runtime.h"
#include "core/runtime/runtime_game.h"

namespace oa::test {

// ===========================================================================
// 无窗口驱动原语（并入自 host_input.h）
//
// 这一组直接驱动 GameRuntime：给不需要窗口的纯数据测试用（不建 SDL 窗口、
// 不读像素）。要"和窗口模式同一条路径 + 截图/渲染队列"用下面的 TestHost。
// ===========================================================================

/// One host frame without input.
inline void frame(oa::runtime::GameRuntime& rt) {
    rt.tick(16);
}

/// One host frame with the cursor at (mx,my).
inline void frame_at(oa::runtime::GameRuntime& rt, int mx, int my) {
    rt.mouse_move(float(mx), float(my));
    rt.tick(16);
}

/// Hover, press, release, settle.
inline void click_at(oa::runtime::GameRuntime& rt, int mx, int my) {
    rt.mouse_move(float(mx), float(my));
    rt.tick(16);
    rt.mouse_button(1, true);
    rt.tick(16);
    rt.mouse_button(1, false);
    rt.tick(16);
}

/// Replay one test-shaped input frame through the host path.
///
/// Frame state is applied before the tick (a release therefore lands in the
/// released frame, which drag-out dispatch expects). Held keys are tracked so a
/// key that disappears from keys_down is released by the host.
inline void replay(oa::runtime::GameRuntime& rt, const oa::runtime::FrameInput& in) {
    static std::map<const void*, std::set<int>> held;
    rt.mouse_move(float(in.mouse_x), float(in.mouse_y));

    std::set<int> want(in.keys_down.begin(), in.keys_down.end());
    want.insert(in.key_down_edges.begin(), in.key_down_edges.end());
    for (const int vk : in.key_up_edges) want.erase(vk);
    auto& h = held[&rt];
    for (auto it = h.begin(); it != h.end();) {
        if (want.count(*it)) {
            ++it;
            continue;
        }
        rt.key_event(*it, false);
        it = h.erase(it);
    }
    for (const int vk : want) {
        rt.key_event(vk, true);
        h.insert(vk);
    }

    rt.mouse_button(1, in.left_down || in.left_click_edge);
    if (in.right_click_edge) {
        rt.mouse_button(2, true);
        rt.mouse_button(2, false);
    }
    rt.tick(16);
}

/// Right click (back / menu).
inline void right_click(oa::runtime::GameRuntime& rt) {
    rt.mouse_button(2, true);
    rt.tick(16);
    rt.mouse_button(2, false);
    rt.tick(16);
}

/// Press/release a key.
inline void key(oa::runtime::GameRuntime& rt, int vk, bool down) {
    rt.key_event(vk, down);
    rt.tick(16);
}

/// Centre of the biggest layer handling `event` (the ADV click screen).
inline std::optional<std::pair<int, int>> screen_click_point(
    oa::runtime::GameRuntime& rt, const char* event = "click") {
    const oa::render::Layer* best = nullptr;
    double best_area = 0;
    for (const oa::render::Layer* l : rt.scene().draw_order()) {
        const auto* h = rt.scene().find_event_handler(l->id, event);
        if (!h || !l->visible) continue;
        const double w = l->has_clip ? l->clip_w : (l->width > 0 ? l->width : 0);
        const double hh = l->has_clip ? l->clip_h : (l->height > 0 ? l->height : 0);
        if (w <= 0 || hh <= 0) continue;
        if (w * hh > best_area) {
            best = l;
            best_area = w * hh;
        }
    }
    if (!best) return std::nullopt;
    const double w = best->has_clip ? best->clip_w : best->width;
    const double hh = best->has_clip ? best->clip_h : best->height;
    double x0 = 0, y0 = 0, rw = 0, rh = 0;
    if (!rt.scene().world_rect(best->id, w, hh, &x0, &y0, &rw, &rh)) return std::nullopt;
    return std::make_pair(int(x0 + rw / 2), int(y0 + rh / 2));
}

/// Centre of a click row by key (bt_start, bt_conf, …).
inline std::optional<std::pair<int, int>> row_click_point(oa::runtime::GameRuntime& rt,
                                                          const std::string& key,
                                                          const char* event = "click") {
    for (const oa::render::Layer* l : rt.scene().draw_order()) {
        const auto* h = rt.scene().find_event_handler(l->id, event);
        if (!h || !l->visible) continue;
        bool hit = false;
        // `com` 是脚本 [lyevent] 的按钮命令（bt_start / bt_config / bt_load…）；
        // 没有键盘快捷键的按钮只有它。少这一个字段，标题菜单就"点不动"。
        // `key` 只是快捷键（ESC 之类），`name`/`function` 是选择行与回调名。
        for (const char* field : {"com", "def", "key", "name", "function"}) {
            const auto it = h->params.find(field);
            if (it != h->params.end() && it->second == key) hit = true;
        }
        if (!hit) continue;
        const double w = l->has_clip ? l->clip_w : (l->width > 0 ? l->width : 180.0);
        const double hh = l->has_clip ? l->clip_h : (l->height > 0 ? l->height : 80.0);
        double x0 = 0, y0 = 0, rw = 0, rh = 0;
        if (!rt.scene().world_rect(l->id, w, hh, &x0, &y0, &rw, &rh)) continue;
        return std::make_pair(int(x0 + rw / 2), int(y0 + rh / 2));
    }
    return std::nullopt;
}

// 下面是**原子能力**，不假设任何启动顺序：标题/语言选择/开场都是某个游戏
// 的启动形状，属于它自己的游程文件（journey_test.cpp 的 enter_story），
// 不是引擎的通用路径。

/// 一段文字里有非空白字形吗（全角空格/换行不算内容）。
inline bool has_visible_chars(const std::string& s) {
    for (size_t i = 0; i < s.size();) {
        const unsigned char c = (unsigned char)s[i];
        if (c < 0x80) {
            if (c > 0x20) return true; // ASCII 可见
            ++i;
            continue;
        }
        // UTF-8 多字节：U+3000（全角空格）= E3 80 80
        if (c == 0xE3 && i + 2 < s.size() && (unsigned char)s[i + 1] == 0x80 &&
            (unsigned char)s[i + 2] == 0x80) {
            i += 3;
            continue;
        }
        return true;
    }
    return false;
}

/// Story message layer (largest text layer, name layers excluded).
///
/// 排除 `basefont.*`：那是 [font] 的**排版测量层**，内容常是空格，永远不该
/// 被当成"剧情已经出现"的判据（用它判会让整条流程停在开场黑场）。
///
/// 判据取自**渲染队列**：这一层此刻真的有 Glyphs 项且不透明。节点上的
/// visible/alpha 不够 —— 父组淡出时节点仍是 alpha=1、仍然"有效可见"，可
/// 画面上什么都没有（语言页选完就是这样）。
inline const oa::render::MessageLayer* story_layer(oa::runtime::GameRuntime& rt) {
    const oa::render::RenderPlan* plan = rt.current_plan();
    auto drawn = [&](const std::string& id) {
        if (!plan) return false;
        for (const oa::render::PlanItem& it : plan->items()) {
            if (it.op == oa::render::PlanOp::Glyphs && it.id == id && it.opacity > 0.01)
                return true;
        }
        return false;
    };
    // 正文层优先：消息窗（`<mwid>.mw.*`）里的那一层才是剧情文字。存档/设置
    // 页的百分比、槽位号这类 UI 文字层也满足"可见 + 有字"，而且 UI 关闭过程中
    // 会短暂留在 visible 列表里 —— 只按字数控量会挑到它（`500.z.p01` = "100%"），
    // 断言就变成拿 UI 文字跟正文比。
    const oa::render::MessageLayer* found = nullptr;
    const oa::render::MessageLayer* fallback = nullptr;
    for (const std::string& id : rt.text().visible_content_layers()) {
        const oa::render::MessageLayer* ml = rt.text().layer(id);
        if (!ml || ml->char_count == 0) continue;
        if (id.rfind("basefont.", 0) == 0) continue;
        if (ml->id.find("name") != std::string::npos) continue;
        if (!drawn(id)) continue;
        const bool in_message_window = id.find(".mw.") != std::string::npos;
        if (!in_message_window) {
            if (!fallback || ml->char_count > fallback->char_count) fallback = ml;
            continue;
        }
        if (!found || ml->char_count > found->char_count) found = ml;
    }
    return found ? found : fallback;
}

/// Page text sample.
inline std::string page_text(const oa::render::MessageLayer* ml, size_t cap = 24) {
    if (!ml) return {};
    std::string s;
    for (const auto& u : ml->page) {
        if (u.data.empty()) continue;
        s += u.data;
        if (s.size() > cap) break;
    }
    return s;
}

/// Tick until `pred` holds; returns frames used or -1 on timeout.
inline int wait_for(oa::runtime::GameRuntime& rt, int max_frames,
                    const std::function<bool()>& pred) {
    for (int f = 0; f < max_frames && !rt.exit_requested(); ++f) {
        if (pred()) return f;
        rt.tick(16);
    }
    return pred() ? max_frames : -1;
}

/// Title -> click start -> first story page.
inline bool start_story_from_title(oa::runtime::GameRuntime& rt, int title_budget = 4000,
                                   int story_budget = 3000) {
    if (story_layer(rt)) return true;

    for (int f = 0; f < title_budget && !rt.exit_requested(); ++f) {
        if (const auto p = row_click_point(rt, "bt_start")) {
            click_at(rt, p->first, p->second);
            break;
        }
        rt.tick(16);
    }
    for (int f = 0; f < story_budget && !rt.exit_requested(); ++f) {
        rt.tick(16);
        const auto* ml = story_layer(rt);
        if (ml && ml->char_count > 0 && has_visible_chars(page_text(ml, 64)))
            return true;
        // park without text (dialogs, transitions): keep clicking through
        if (f % 40 == 0) {
            if (const auto p = screen_click_point(rt)) click_at(rt, p->first, p->second);
        }
    }
    return false;
}


/// Hold a key for `frames` then release (what a player does; a single-frame
/// edge is what the data-side push handlers treat as a glitch).
inline void hold_key(oa::runtime::GameRuntime& rt, int vk, int frames = 12, int settle = 4) {
    rt.key_event(vk, true);
    for (int f = 0; f < frames && !rt.exit_requested(); ++f) rt.tick(16);
    rt.key_event(vk, false);
    for (int f = 0; f < settle && !rt.exit_requested(); ++f) rt.tick(16);
}

/// Click the centre of a specific layer (by id). False when it is absent or
/// has no world rect.
inline bool click_layer(oa::runtime::GameRuntime& rt, const std::string& id) {
    const oa::render::Layer* l = rt.scene().find(id);
    if (!l) return false;
    double w = l->has_clip ? l->clip_w : (l->width > 0 ? l->width : 60.0);
    double h = l->has_clip ? l->clip_h : (l->height > 0 ? l->height : 30.0);
    double x0 = 0, y0 = 0, rw = 0, rh = 0;
    if (!rt.scene().world_rect(id, w, h, &x0, &y0, &rw, &rh)) return false;
    click_at(rt, int(x0 + rw / 2), int(y0 + rh / 2));
    return true;
}

/// A story page park: a click-advanceable wait.
inline bool is_page_wait(const oa::runtime::Wait* w) {
    return w && (w->waiting_click() ||
                 (w->timed() && w->can(oa::runtime::Wait::OnClick)));
}

/// True when the visible story page has finished its per-glyph reveal.
inline bool reveal_done(oa::runtime::GameRuntime& rt) {
    for (const std::string& id : rt.text().visible_content_layers()) {
        if (!rt.text().page_has_visible_text(id)) continue;
        const oa::render::MessageLayer* ml = rt.text().layer(id);
        // 揭示是否还在进行以 done 为准（与引擎 is_reveal_complete 一致）。
        if (ml && !ml->done && ml->char_count > 0) return false;
    }
    return true;
}

/// One click on the ADV screen that actually turns the page: waits for the
/// reveal first, then waits until the text changes. Returns false when the page
/// did not change (a [stop] park, a choice, a UI screen).
inline bool advance_page(oa::runtime::GameRuntime& rt, int budget = 1200) {
    const auto* before = story_layer(rt);
    const std::string was = page_text(before);
    if (!wait_for(rt, 400, [&] { return reveal_done(rt); }) && !reveal_done(rt))
        return false;
    if (const auto p = screen_click_point(rt)) click_at(rt, p->first, p->second);
    const int used = wait_for(rt, budget, [&] {
        const auto* now = story_layer(rt);
        return now && page_text(now) != was;
    });
    return used >= 0;
}

/// Get back to a clickable story page: leave any UI screen (right click) and
/// turn pages until text is on screen and settled.
inline bool ensure_story(oa::runtime::GameRuntime& rt, int budget = 4000) {
    for (int f = 0; f < budget && !rt.exit_requested(); ++f) {
        const auto* ml = story_layer(rt);
        const auto* w = rt.current_wait();
        if (ml && ml->char_count > 0 && has_visible_chars(page_text(ml, 64)) && w &&
            w->waiting_click() && reveal_done(rt))
            return true;
        if (f % 60 == 0) {
            if (!ml) right_click(rt);
            else if (const auto p = screen_click_point(rt)) click_at(rt, p->first, p->second);
            else if (const auto p = story_layer(rt))
                (void)p; // 没有可点层：下面是"点屏幕任意处"
            else
                right_click(rt);
        }
        // 没有可点层时点舞台中心 —— 和玩家点一下屏幕同一条路径
        // （通知框、过场都是这种"点哪都行"的形状）。
        if (f % 60 == 30 && !screen_click_point(rt))
            click_at(rt, rt.project().config.stage_width / 2,
                     rt.project().config.stage_height / 2);
        rt.tick(16);
    }
    return false;
}

// ===========================================================================
// TestHost：模拟 main.cpp 的无头静音宿主
// ===========================================================================

/// 渲染队列里的一项（`plan()` 的返回值）。
struct PlanRow {
    std::string id;    // 层 id
    int op = 0;        // 0=Content 1=Glyphs 2=GroupBegin 3=GroupEnd
    double x = 0, y = 0, w = 0, h = 0; // 世界矩形（stage 坐标）
    double opacity = 1;
    bool solid = false;        // 纯色块（无纹理）
    bool has_texture = false;
    bool glyphs = false;       // 文本绘制项
};

class TestHost {
public:
    struct Options {
        std::string project;              // .pfs 或解包目录（必填）
        std::string save_root;            // 空 = 临时目录
        std::string platform = "windows";
        std::string renderer = "sdl";
        bool mute = true;
        /// false = 不建窗口/不接渲染器（纯数据二分用）。
        bool render = true;
        /// 帧步长（毫秒）。固定值保证可复现。
        int delta_ms = 16;
    };

    explicit TestHost(Options opt) : opt_(std::move(opt)) {
        if (!SDL_Init(SDL_INIT_VIDEO)) {
            std::fprintf(stderr, "[host] SDL_Init failed: %s\n", SDL_GetError());
            return;
        }
        sdl_ = true;
        // 存档目录默认落临时目录：测试绝不能写到游戏自己的存档里。
        if (opt_.save_root.empty()) {
            std::error_code ec;
            tmp_root_ = std::filesystem::temp_directory_path(ec) /
                        ("oa_host_" + std::to_string(::getpid()));
            std::filesystem::remove_all(tmp_root_, ec);
            opt_.save_root = tmp_root_.string();
        }
        oa::runtime::AppConfig cfg;
        cfg.project = opt_.project;
        cfg.save_root = opt_.save_root;
        cfg.platform = opt_.platform;
        cfg.renderer = opt_.renderer;
        cfg.mute = opt_.mute;
        // 逻辑无头：固定步长、不按 fps 节流。渲染器仍然开着（见下）。
        cfg.headless = true;
        cfg.fixed_delta_ms = opt_.delta_ms;
        app_ = std::make_unique<oa::runtime::App>(std::move(cfg));
        if (!app_->boot()) {
            std::fprintf(stderr, "[host] boot failed: %s\n", opt_.project.c_str());
            return;
        }
        // 测试要确定性：解码走调用线程。异步解码池会让"第几帧崩"随机器
        // 而变，也只有它能让 headless 复现出竞态。
        app_->runtime().enable_decode_pool(0);
        if (!opt_.render) {
            ok_ = true;
            return;
        }
        win_ = SDL_CreateWindow("oa-test", app_->stage_width(),
                                app_->stage_height(), SDL_WINDOW_HIDDEN);
        if (!win_) {
            std::fprintf(stderr, "[host] window failed: %s\n", SDL_GetError());
            return;
        }
        if (!app_->attach_window(win_)) {
            std::fprintf(stderr, "[host] renderer failed: %s\n", SDL_GetError());
            return;
        }
        ok_ = true;
    }

    ~TestHost() {
        app_.reset(); // 先释放引擎（渲染器用到窗口）
        if (win_) SDL_DestroyWindow(win_);
        if (sdl_) SDL_Quit();
        if (!tmp_root_.empty()) {
            std::error_code ec;
            std::filesystem::remove_all(tmp_root_, ec);
        }
    }

    TestHost(const TestHost&) = delete;
    TestHost& operator=(const TestHost&) = delete;

    bool ok() const { return ok_; }
    int stage_w() const { return app_ ? app_->stage_width() : 0; }
    int stage_h() const { return app_ ? app_->stage_height() : 0; }
    oa::runtime::GameRuntime& rt() { return app_->runtime(); }
    const oa::runtime::GameRuntime& rt() const { return app_->runtime(); }

    // ---- 帧推进 ----------------------------------------------------------
    /// 推进 n 帧（每帧固定 delta_ms）。返回 false = 引擎要求退出。
    bool tick(int n = 1) {
        for (int i = 0; i < n && app_; ++i) {
            if (app_->tick() == oa::runtime::AppResult::Exit) return false;
        }
        return true;
    }

    // ---- 事件（与 main.cpp 同一条路径）----------------------------------
    void move(double x, double y) {
        if (app_) app_->mouse_move(float(x), float(y));
    }
    void button(bool down) {
        if (app_) app_->mouse_button(1, down);
    }
    /// 一次完整的左键点击：移动 → 按下 → 抬起，各跨一帧（真人节奏）。
    void click(double x, double y, int settle = 1) {
        move(x, y);
        tick(settle);
        button(true);
        tick(settle);
        button(false);
        tick(settle);
    }
    /// 按住拖动（drag 类交互）。
    void drag(double x0, double y0, double x1, double y1, int steps = 8) {
        move(x0, y0);
        tick();
        button(true);
        tick();
        for (int i = 1; i <= steps; ++i) {
            const double t = double(i) / steps;
            move(x0 + (x1 - x0) * t, y0 + (y1 - y0) * t);
            tick();
        }
        button(false);
        tick();
    }
    void key(int vk) {
        if (!app_) return;
        app_->key_event(vk, true);
        tick();
        app_->key_event(vk, false);
        tick();
    }
    void wheel(int dir) {
        if (app_) app_->mouse_wheel(dir);
    }

    /// 点一个"带 click 行、且 key 匹配"的层（标题的 bt_start、Dock 的
    /// bt_save 这类）。找不到返回 false —— 断言里直接用它判"按钮在不在"。
    bool click_key(const std::string& key, int settle = 1) {
        if (!app_) return false;
        for (const oa::render::Layer* l : rt().scene().draw_order()) {
            const auto* h = rt().scene().find_event_handler(l->id, "click");
            if (!h || !h->enabled) continue;
            bool match = false;
            // com=脚本命令（bt_start/btn_06）、def=人读的名字（save/load）、
            // key=快捷键、name=选择行。
            for (const char* field : {"com", "def", "key", "name"}) {
                const auto it = h->params.find(field);
                if (it != h->params.end() && it->second == key) match = true;
            }
            if (!match) continue;
            // 优先用渲染队列里的真实矩形（画在哪就点哪）；没有绘制项时
            // 退回场景几何 —— 命中层不一定自己出图（它可能是按钮的组）。
            PlanRow row;
            if (find(l->id, &row)) {
                click(row.x + row.w / 2, row.y + row.h / 2, settle);
                return true;
            }
            if (click_layer(rt(), l->id)) {
                tick(settle);
                return true;
            }
        }
        return false;
    }

    /// 点一段含 `needle` 的文字（对话框里的"知道了/确定"这一类）。
    /// 用它的字形项矩形，画在哪就点哪。
    bool click_text(const std::string& needle, int settle = 2) {
        if (!app_) return false;
        for (const auto& [id, ml] : rt().text().all_layers()) {
            const std::string t = oa::test::page_text(ml, 400);
            if (t.empty() || t.find(needle) == std::string::npos) continue;
            PlanRow row;
            if (!find(id, &row)) continue;
            click(row.x + row.w / 2, row.y + row.h / 2, settle);
            return true;
        }
        return false;
    }

    /// 这一帧有这个名字的按钮吗（com/def/key/name 任一匹配）。
    ///
    /// 三套命名各有来源：`com` 是脚本命令（bt_start/btn_06），`def` 是人读
    /// 的名字（save/load），`key` 只是快捷键。只看某一个就会漏按钮。
    bool has_button(const std::string& name) const {
        if (!app_) return false;
        for (const oa::render::Layer* l : rt().scene().draw_order()) {
            const auto* h = rt().scene().find_event_handler(l->id, "click");
            if (!h || !h->enabled) continue;
            for (const char* field : {"com", "def", "key", "name"}) {
                const auto it = h->params.find(field);
                if (it != h->params.end() && it->second == name) return true;
            }
        }
        return false;
    }

    /// 当前屏幕上有文字绘制项吗（Glyphs 项 = 有字要画）。
    bool has_glyphs() const {
        for (const PlanRow& r : plan()) {
            if (r.glyphs && r.opacity > 0.0) return true;
        }
        return false;
    }

    /// 等画面出现文字；返回帧号（-1 = 没等到）。
    int wait_glyphs(int max_frames = 600) {
        for (int f = 0; f < max_frames; ++f) {
            if (has_glyphs()) return f;
            if (!tick()) return -1;
        }
        return has_glyphs() ? max_frames : -1;
    }

    // ---- 渲染队列（数值面）----------------------------------------------
    std::vector<PlanRow> plan() const {
        std::vector<PlanRow> out;
        if (!app_) return out;
        const oa::render::RenderPlan* p = app_->runtime().current_plan();
        if (!p) return out;
        out.reserve(p->items().size());
        for (const oa::render::PlanItem& it : p->items()) {
            PlanRow row;
            row.id = it.id;
            row.op = int(it.op);
            row.opacity = it.opacity;
            row.solid = it.solid;
            row.has_texture = !it.texture.empty();
            row.glyphs = it.op == oa::render::PlanOp::Glyphs;
            double x = 0, y = 0;
            it.world.transform_point(0, 0, &x, &y);
            row.x = x;
            row.y = y;
            // 无旋转/切变时，局部尺寸 × 缩放即屏幕尺寸。
            row.w = it.w * it.world.a;
            row.h = it.h * it.world.d;
            out.push_back(std::move(row));
        }
        return out;
    }

    /// 该层此刻是否会被画（渲染队列里有它，且不透明）。
    bool visible(const std::string& id) const {
        for (const PlanRow& r : plan()) {
            if (r.id == id && r.opacity > 0.0) return true;
        }
        return false;
    }

    /// 该层的绘制项（没有则 nullptr 语义：返回 false）。
    bool find(const std::string& id, PlanRow* out) const {
        for (const PlanRow& r : plan()) {
            if (r.id != id) continue;
            if (out) *out = r;
            return true;
        }
        return false;
    }

    /// 等某个 layer 出现在渲染队列里；返回出现时的帧号（-1 = 没等到）。
    int wait_for(const std::string& id, int max_frames = 300) {
        for (int f = 0; f < max_frames; ++f) {
            if (visible(id)) return f;
            if (!tick()) return -1;
        }
        return visible(id) ? max_frames : -1;
    }

    /// 把当前屏幕摊开：可点的键名 + 渲染队列（id/op/透明度/矩形）。
    /// 启动流程对不上时用它看清"现在停在哪一屏"，不要再猜。
    void dump(std::FILE* out = stderr) const {
        std::fprintf(out, "[dump] plan=%zu\n", plan().size());
        // 所有可点层：名字（com/def/key）+ 场景矩形。启动对不上时先看这里，
        // 不要靠猜"哪个按钮该点"。
        for (const oa::render::Layer* l : rt().scene().draw_order()) {
            const auto* h = rt().scene().find_event_handler(l->id, "click");
            if (!h || !h->enabled) continue;
            std::string name;
            for (const char* field : {"com", "def", "key", "name"}) {
                const auto it = h->params.find(field);
                if (it != h->params.end() && !it->second.empty()) {
                    name += std::string(field) + "=" + it->second + " ";
                }
            }
            double w = l->has_clip ? l->clip_w : (l->width > 0 ? l->width : 0.0);
            double hh = l->has_clip ? l->clip_h : (l->height > 0 ? l->height : 0.0);
            double x0 = 0, y0 = 0, rw = 0, rh = 0;
            rt().scene().world_rect(l->id, w, hh, &x0, &y0, &rw, &rh);
            std::fprintf(out, "[dump] btn %-32s %-46s vis=%d %.0f,%.0f %.0fx%.0f\n",
                         l->id.c_str(), name.c_str(), l->visible ? 1 : 0, x0, y0, rw, rh);
        }
        for (const PlanRow& r : plan())
            std::fprintf(out, "[dump] %-30s op=%d a=%.2f %.0f,%.0f %.0fx%.0f\n",
                         r.id.c_str(), r.op, r.opacity, r.x, r.y, r.w, r.h);
    }

    /// 等某个 layer 从渲染队列里消失；返回消失时的帧号（-1 = 一直没消失）。
    int wait_gone(const std::string& id, int max_frames = 300) {
        for (int f = 0; f < max_frames; ++f) {
            if (!visible(id)) return f;
            if (!tick()) return -1;
        }
        return visible(id) ? -1 : max_frames;
    }

    // ---- 画面（像素面）--------------------------------------------------
    /// 真实读回一帧。空 = 无后端。
    std::vector<uint8_t> shot(uint32_t* w, uint32_t* h) const {
        if (!app_) return {};
        // 复用 --dump 的那条读回路径（RenderEngine → backend->read_target）。
        if (auto px = app_->runtime().capture_frame(w, h)) return std::move(*px);
        return {};
    }

    /// 落盘 PNG（测试留证）。返回是否成功。
    bool save_png(const std::string& path) const {
        uint32_t w = 0, h = 0;
        std::vector<uint8_t> px = shot(&w, &h);
        if (px.empty() || !w || !h) return false;
        const std::vector<uint8_t> png = oa::media::encode_png(w, h, px);
        if (png.empty()) return false;
        FILE* fp = std::fopen(path.c_str(), "wb");
        if (!fp) return false;
        std::fwrite(png.data(), 1, png.size(), fp);
        std::fclose(fp);
        return true;
    }

    /// 落盘 PPM（无第三方依赖，最快的留证方式）。
    bool save_ppm(const std::string& path) const {
        uint32_t w = 0, h = 0;
        std::vector<uint8_t> px = shot(&w, &h);
        if (px.empty() || !w || !h) return false;
        FILE* fp = std::fopen(path.c_str(), "wb");
        if (!fp) return false;
        std::fprintf(fp, "P6\n%u %u\n255\n", w, h);
        const size_t n = size_t(w) * h;
        std::vector<uint8_t> rgb(n * 3);
        for (size_t i = 0; i < n; ++i) {
            rgb[i * 3 + 0] = px[i * 4 + 0];
            rgb[i * 3 + 1] = px[i * 4 + 1];
            rgb[i * 3 + 2] = px[i * 4 + 2];
        }
        std::fwrite(rgb.data(), 1, rgb.size(), fp);
        std::fclose(fp);
        return true;
    }

private:
    Options opt_;
    std::filesystem::path tmp_root_;
    bool ok_ = false;
    bool sdl_ = false;
    SDL_Window* win_ = nullptr;
    std::unique_ptr<oa::runtime::App> app_;
};

} // namespace oa::test
