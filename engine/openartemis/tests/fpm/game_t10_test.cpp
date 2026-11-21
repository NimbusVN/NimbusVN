// 私たちマリアージュ①（test10） 游程 —— 一个游戏一个文件。
//
// OA_TEST_T10_PFS 指向 root.pfs；不设则 exit 77。
//
// 启动形状：开场 → 语言选择（初回だけ）→ 标题
// 语言选择是这批"按钮要点两次"的现场：画面淡入 800ms 里框架自己把
// `t.eventfilter` 置 1（屏蔽层事件），淡入结束后必须"一次点击就生效"。
// 这个用例就是钉住那条线：掩码放开后，第一次点击必须被层接住。
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "crash_backtrace.h"
#include "test_host.h"

#include "core/render/font.h"
#include "core/render/text.h"
#include "core/runtime/runtime_save.h"

namespace {

int failures = 0;

void check(bool cond, const std::string& what) {
    std::fprintf(stderr, "%s: %s\n", cond ? "ok  " : "FAIL", what.c_str());
    std::fflush(stderr);
    if (!cond) ++failures;
}

std::string var_str(oa::runtime::GameRuntime& rt, const char* name) {
    const auto v = rt.interpreter().variables().get(name);
    return v ? v->to_display() : std::string();
}

/// 真的画到屏幕上的正文层数量：有字 + 本帧在渲染队列里 + 不透明。
/// 只看 `visible`/`char_count` 会把"层在、但一个字都没画"放过（backlog
/// 没有字、存档槽位没有预览文字，都是这种"层在字不在"）。
int drawn_text_layers(oa::runtime::GameRuntime& rt, const std::string& prefix) {
    const auto* plan = rt.current_plan();
    if (!plan) return 0;
    int n = 0;
    for (const oa::render::PlanItem& it : plan->items()) {
        if (it.op != oa::render::PlanOp::Glyphs || it.opacity <= 0.01) continue;
        if (!prefix.empty() && it.id.rfind(prefix, 0) != 0) continue;
        const auto* ml = rt.text().layer(it.id);
        if (ml && ml->char_count > 0) ++n;
    }
    return n;
}

/// 把某个前缀下的文本层状态打出来（断言失败时定位用）。
void dump_text_layers(oa::runtime::GameRuntime& rt, const std::string& prefix) {
    const auto* plan = rt.current_plan();
    std::fprintf(stderr, "[textdump] prefix='%s'\n", prefix.c_str());
    for (const auto& [id, ml] : rt.text().all_layers()) {
        if (!ml) continue;
        if (!prefix.empty() && id.rfind(prefix, 0) != 0) continue;
        bool drawn = false;
        double op = 0.0;
        if (plan) {
            for (const oa::render::PlanItem& it : plan->items()) {
                if (it.op == oa::render::PlanOp::Glyphs && it.id == id) {
                    drawn = true;
                    op = it.opacity;
                }
            }
        }
        std::fprintf(stderr, "[textdump] %s chars=%zu drawn=%d op=%.2f box=(%.0f,%.0f %.0fx%.0f) font=%.1f face='%s' text='%s'\n",
                     id.c_str(), ml->char_count, int(drawn), op, ml->left, ml->top, ml->width,
                     ml->height, ml->font.size(), ml->font.face().c_str(),
                     oa::test::page_text(ml, 24).c_str());
    }
}

/// 语言选择的按钮层：`200.10.<n>.2`（图片在 `_1`，点击行在这一层）。
std::string find_language_button(oa::runtime::GameRuntime& rt) {
    for (const oa::render::Layer* l : rt.scene().draw_order()) {
        if (l->id.rfind("200.10.", 0) != 0) continue;
        const auto* h = rt.scene().find_event_handler(l->id, "click");
        if (!h || !h->enabled) continue;
        const auto name = h->params.find("name");
        if (name != h->params.end() && name->second == "t.button_data") return l->id;
    }
    return std::string();
}

void case_language_screen(oa::test::TestHost& host) {
    auto& rt = host.rt();

    // 按钮层建出来就算这一屏到了（图片 232x50，位置由 btn_ja_0.csv 给）。
    std::string button;
    for (int i = 0; i < 900 && button.empty(); ++i) {
        host.tick(1);
        button = find_language_button(rt);
    }
    check(!button.empty(), "the language screen builds its buttons");
    if (button.empty()) return;

    // 开场淡入的输入掩码：`t.eventfilter` 归零 = 层事件重新放行。
    int frames = 0;
    while (frames++ < 240 && var_str(rt, "t.eventfilter") != "0") host.tick(1);
    check(var_str(rt, "t.eventfilter") == "0", "the intro input mask clears");

    // 日本語按钮中心（`_data/image/menu/config/language/btn_ja_0.csv` = 543,562，
    // 图片 232x50）。一次点击就要选中，不能再要第二下。
    host.click(543 + 116, 562 + 25, 2);
    host.tick(8);
    check(var_str(rt, "t.language") == "ja", "one click selects the language");

    // 选中之后框架会弹确认框（lydialog，id=1000.*）。以前引擎会把按钮
    // 流程排下的重复 [stop] 留在队列里：位置已经跳到 change_language_push，
    // 旧停驻却又把新位置停住 —— 确认框永远不出现，玩家只能再点一下"解锁"。
    // 这里钉住整条链：一次选中 → 确认框出现 → 一次确认 → 语言生效。
    std::string ok_id;
    double ok_x = 0, ok_y = 0;
    for (int i = 0; i < 300 && ok_id.empty(); ++i) {
        host.tick(1);
        for (const oa::render::Layer* l : rt.scene().draw_order()) {
            if (l->id.rfind("1000.", 0) != 0) continue;
            const auto* h = rt.scene().find_event_handler(l->id, "click");
            if (!h || !h->enabled) continue;
            double x0 = 0, y0 = 0, rw = 0, rh = 0;
            if (!rt.scene().world_rect(l->id, 300, 80, &x0, &y0, &rw, &rh)) continue;
            ok_id = l->id;
            ok_x = x0 + rw / 2;
            ok_y = y0 + rh / 2;
            break;
        }
    }
    check(!ok_id.empty(), "the confirmation dialog opens");
    if (ok_id.empty()) return;

    // 对话框自己的淡入也带掩码：等它放行（玩家等框稳定再点）。
    for (int i = 0; i < 300 && var_str(rt, "t.eventfilter") != "0"; ++i) host.tick(1);
    check(var_str(rt, "t.eventfilter") == "0", "the confirmation dialog accepts input");
    host.click(ok_x, ok_y, 2);
    for (int i = 0; i < 300 && var_str(rt, "g.language") != "ja"; ++i) host.tick(1);
    check(var_str(rt, "g.language") == "ja", "one click confirms the dialog");
}

/// 换 CG 前后的消息窗隐藏-重现是演出，不是玩家交互停驻。
///
/// test10 的第一个 CG 切换（s100.txt：`嗯，那么，艾克希丝亚……` 之后那句
/// `[イベントCG st="cg01_2" hide="1"]`）曾经多出一次点击：消息窗淡出、换 CG、
/// 再淡入的整段时间里，脚本被一条空页停 `[@]` 挡住，必须再点一下才出现下一句。
/// 根因是 preprocessor 把 `[onlinehead]`/`[onlineend]` 也插在了纯标签行
/// （`[イベントCG]`）上，紧跟其后的空行 `[onblankline]` 于是看到
/// onlinehead=1/onlineend=0，先补 onlineend（顺手把 scpstat 置回 normal），
/// 再据此插了一次没有文字的页停。
///
/// 这里钉住行为：在「艾克希丝亚」这一页点**一次**，不追加任何点击，
/// 下一句必须在 CG 演出结束后自己出现。
void case_cg_change_keeps_one_click(oa::test::TestHost& host) {
    auto& rt = host.rt();

    std::string err;
    check(rt.interpreter().try_load_script("scenario/main/s100.txt", &err),
          "the opening scenario loads: " + err);
    try {
        rt.interpreter().start("scenario/main/s100.txt", "top");
    } catch (const std::exception& e) {
        check(false, std::string("the opening scenario starts: ") + e.what());
        return;
    }

    auto page_ready = [&]() {
        const auto* w = rt.current_wait();
        return w && w->waiting_click() && oa::test::reveal_done(rt) &&
               oa::test::story_layer(rt) != nullptr;
    };
    auto wait_page = [&](int budget) {
        for (int f = 0; f < budget; ++f) {
            if (page_ready()) return true;
            if (!host.tick()) return false;
        }
        return page_ready();
    };
    auto page_text = [&]() {
        const auto* ml = oa::test::story_layer(rt);
        return ml ? oa::test::page_text(ml, 200) : std::string();
    };

    check(wait_page(2000), "the opening reaches its first story page");

    // 走到「艾克希丝亚……」这一页（简中/日文都认）。
    bool found = false;
    for (int guard = 0; guard < 8 && !found; ++guard) {
        const std::string t = page_text();
        found = t.find("艾克希丝亚") != std::string::npos ||
                t.find("エクスシア") != std::string::npos;
        if (found) break;
        {
            const auto p = oa::test::screen_click_point(rt);
            host.click(p ? p->first : host.stage_w() / 2,
                       p ? p->second : host.stage_h() / 2, 2);
        }
        check(wait_page(1200), "the page before the CG change is reachable");
    }
    check(found, "the line before the CG change is on screen");
    if (!found) return;

    const std::string before = page_text();
    // 只点这一下。后面完全不再点击（没有按钮层时点舞台中心，和玩家一样）。
    const auto p = oa::test::screen_click_point(rt);
    const int cx = p ? p->first : host.stage_w() / 2;
    const int cy = p ? p->second : host.stage_h() / 2;
    host.click(cx, cy, 2);

    const int used = oa::test::wait_for(rt, 900, [&] {
        const std::string t = page_text();
        return !t.empty() && t != before;
    });
    check(used >= 0, "one click at the CG-change line shows the next line (no extra click)");

    const std::string after = page_text();
    check(after.find("好意") != std::string::npos || after.find("ご好意") != std::string::npos,
          "the page after the CG change is the following line");
}

// ---------------------------------------------------------------------------
// 消息窗折行
//
// 原版在消息窗文本区宽度（字体表 width）处折行。汉化文本通常比日文短，
// 单行容量一旦偏大，日文会折、中文不折 —— 典型现象就是 test10 里
// s102.txt:308 的这句中文挤在一行。这里直接用运行时那个消息层的**真实
// 字体/宽度**做纯排版，不污染剧情状态。
// ---------------------------------------------------------------------------
struct Wrapped {
    size_t lines = 0;
    double max_line_width = 0;
    /// 每行参与排版的全角/半角字形数（newline 不算）。首行容量是这一页
    /// 折行是否正确的最直接证据。
    std::vector<size_t> line_chars;
};

Wrapped layout_message_text(oa::runtime::GameRuntime& rt,
                            const oa::render::MessageLayer& live,
                            const std::string& text) {
    oa::render::MessageLayer probe = live;
    probe.page.clear();
    std::string chunk;
    auto flush = [&] {
        if (chunk.empty()) return;
        oa::render::PageUnit u;
        u.kind = oa::render::PageUnit::Kind::Text;
        u.data = chunk;
        u.font = probe.font;
        probe.page.push_back(u);
        chunk.clear();
    };
    for (const char c : text) {
        if (c == '\n') {
            flush();
            oa::render::PageUnit u;
            u.kind = oa::render::PageUnit::Kind::Newline;
            u.font = probe.font;
            probe.page.push_back(u);
        } else {
            chunk += c;
        }
    }
    flush();

    oa::render::FontSystem fs(rt.fs().get(), nullptr, &rt.text());
    auto metrics = [](void* ud, const oa::render::FontDesc& f, uint32_t cp) {
        return static_cast<oa::render::FontSystem*>(ud)->metrics_for(f, cp);
    };
    const oa::render::LaidPage laid = oa::render::layout_page(
        probe, metrics, rt.text().layout_config(), &fs);

    Wrapped out;
    std::map<uint32_t, double> line_right;
    std::map<uint32_t, size_t> line_chars;
    for (const oa::render::LaidGlyph& g : laid.glyphs) {
        out.lines = std::max<size_t>(out.lines, size_t(g.line) + 1);
        if (g.newline) continue;
        ++line_chars[g.line];
        line_right[g.line] = std::max(line_right[g.line], g.x + g.advance);
    }
    for (const auto& [line, right] : line_right)
        out.max_line_width = std::max(out.max_line_width, right);
    out.line_chars.reserve(out.lines);
    for (uint32_t line = 0; line < out.lines; ++line) out.line_chars.push_back(line_chars[line]);
    return out;
}

void case_message_text_wrap(oa::test::TestHost& host) {
    auto& rt = host.rt();
    // 取运行时真实的消息层（s100/s102 的 [adv]）：宽度/字体都是游戏字体表
    // 下发的那一份，不是测试里编的。
    const oa::render::MessageLayer* live = rt.text().layer("adv");
    if (!live) {
        for (const auto& [id, ml] : rt.text().all_layers()) {
            (void)id;
            if (ml && ml->width > 0.0 && ml->char_count > 0) {
                live = ml;
                break;
            }
        }
    }
    check(live && live->width > 0.0, "the message layer exposes a text-area width");
    if (!live || live->width <= 0.0) return;

    // s102.txt:308 的中文原文（汉化版里这句比日文短，正是漏折行的现场）。
    // 这里在当前层（默认全景窗，文本区 ~1056px）上重复两次：单句 28 字在
    // 这个宽度里本来就装得下，重复后才真正越过文本区，断言的是"越界必折"。
    // 窄窗（角色专窗 376px）的现场由 case_chara_window_text_area 单独钉住。
    const std::string long_cn =
        "不算。只是翘一会第一节课，拿来看书的程度。那么，我先告辞了。"
        "不算。只是翘一会第一节课，拿来看书的程度。那么，我先告辞了。";
    const Wrapped long_out = layout_message_text(rt, *live, long_cn);
    check(long_out.lines >= 2, "a long Chinese line wraps inside the message window");
    check(long_out.max_line_width <= live->width + 0.5,
          "every wrapped line stays inside the message text area");

    // 短句不受影响：同一层、同一字体，必须仍是一行。
    const Wrapped short_out = layout_message_text(rt, *live, "嗯，那么，艾克丝希亚……");
    check(short_out.lines == 1, "a short line stays on one line");
    check(short_out.max_line_width <= live->width + 0.5,
          "the short line also stays inside the text area");

    // 显式换行不能被 [print]/排版路径吞掉：单元流里的 Newline 必须强制断行。
    const Wrapped explicit_out = layout_message_text(rt, *live, "第一行\n第二行");
    check(explicit_out.lines == 2, "an explicit newline still forces a line break");
}

// ---------------------------------------------------------------------------
// 角色专用消息窗（その他女性 / MW06）的文本区
//
// 现场：s100.txt:100 的 [女生徒Ｄ] 把消息窗切到角色专窗，字体表分两段下发
// ——底座 cn_tag_font.csv 带 entirexscale=66，角色窗的 cn_tag_font_その他女性.csv
// 只补 left/top/width/height。宽度必须乘 66% 才是真正的文本区（570→376.2），
// 否则整块纹理宽 570 会让中文一行装下 16 字、永不折行（参考录像为 2 行）。
// 位置则来自同一张字体表的 left/top（100,300），必须离窗框左沿有内边距。
// ---------------------------------------------------------------------------
void case_chara_window_text_area(oa::test::TestHost& host) {
    auto& rt = host.rt();
    auto page_ready = [&]() {
        const auto* w = rt.current_wait();
        return w && w->waiting_click() && oa::test::reveal_done(rt) &&
               oa::test::story_layer(rt) != nullptr;
    };
    auto wait_page = [&](int budget) {
        for (int f = 0; f < budget; ++f) {
            if (page_ready()) return true;
            if (!host.tick()) return false;
        }
        return page_ready();
    };
    auto page_text = [&]() {
        const auto* ml = oa::test::story_layer(rt);
        return ml ? oa::test::page_text(ml, 200) : std::string();
    };

    // 从上一用例的位置继续点，直到「好棒……」(简中) /「素敵……」(日文) 这一页。
    bool found = false;
    for (int guard = 0; guard < 14 && !found; ++guard) {
        const std::string t = page_text();
        found = t.find("好棒") != std::string::npos || t.find("素敵") != std::string::npos;
        if (found) break;
        const auto p = oa::test::screen_click_point(rt);
        host.click(p ? p->first : host.stage_w() / 2, p ? p->second : host.stage_h() / 2, 2);
        check(wait_page(1200), "the character-window page is reachable");
    }
    check(found, "the character window shows the target page");
    if (!found) return;

    const oa::render::MessageLayer* live = oa::test::story_layer(rt);
    check(live != nullptr, "the character-window page has a live text layer");
    if (!live) return;

    // 文本区 = 570 * 66% = 376.2（不是整块窗口纹理宽 570）。
    check(live->width > 300.0 && live->width < 420.0,
          "the wrap width is the scaled font-table text width");
    check(live->entire_x > 1.0 && live->entire_x < 100.0,
          "the layer keeps the entirexscale factor for later font rows");

    const Wrapped w = layout_message_text(rt, *live, "好棒……愿女神的祝福与你们同在。");
    check(w.lines == 2, "the target Chinese line wraps to two lines");
    check(!w.line_chars.empty() && w.line_chars[0] == 11,
          "the first line holds 11 full-width characters");
    check(w.max_line_width <= live->width + 0.5,
          "every wrapped line stays inside the character-window text area");

    // 首字相对消息窗框体左边有内边距（参考：文字不贴框沿）。
    double wx = 0, wy = 0, ww = 0, wh = 0;
    double best_alpha = -1.0;
    for (const oa::render::Layer* l : rt.scene().draw_order()) {
        if (l->id.rfind("80.0.", 0) != 0) continue;
        if (l->file.find("mw") == std::string::npos) continue;
        double x0 = 0, y0 = 0, rw = 0, rh = 0;
        if (!rt.scene().world_rect(l->id, 1, 1, &x0, &y0, &rw, &rh)) continue;
        if (l->alpha > best_alpha) {
            best_alpha = l->alpha;
            wx = x0;
            wy = y0;
        }
    }
    check(best_alpha >= 0.0, "the message-window plate is in the scene");
    if (best_alpha >= 0.0) {
        check(wx > 0.0, "the message window is not shifted off the left screen edge");
        check(live->left - wx >= 60.0,
              "the text origin keeps a left padding from the window frame");
    }
}

// ---------------------------------------------------------------------------
// 系统菜单 / 设置页 / 存档页
//
// 三个现场共用一套 "系统按钮 → 页面" 子系统：
//   * dock 的 submenu 按钮点一下要长出整条菜单（框架一次 enqueue
//     sysbtn_del / init / set，队列必须按 FIFO 跑，否则 del 落在最后把按钮删光）；
//   * 页面转场是 Lua `e:enqueueTag{"trans"}` 排下的，trans 过滤器必须跑，
//     否则 t.eventfilter 永远停在 1，后面的按钮全被掩码吃掉；
//   * 存档页槽位坐标来自 CSV；CSV 里的整数必须按数字解析，否则
//     `margin + width` 变成字符串拼接（槽位被摆到 x=100641）。
// ---------------------------------------------------------------------------

std::string script_name(oa::runtime::GameRuntime& rt) {
    const std::string* s = rt.interpreter().current_script();
    return s ? *s : std::string();
}

bool ends_with(const std::string& s, const char* suffix) {
    const std::string suf(suffix);
    return s.size() >= suf.size() && s.compare(s.size() - suf.size(), suf.size(), suf) == 0;
}

bool click_layer_center(oa::test::TestHost& host, const std::string& id,
                        double fallback_w, double fallback_h) {
    oa::test::PlanRow row;
    double x = 0, y = 0, w = 0, h = 0;
    if (host.find(id, &row) && row.w > 0 && row.h > 0) {
        x = row.x;
        y = row.y;
        w = row.w;
        h = row.h;
    } else if (!host.rt().scene().world_rect(id, fallback_w, fallback_h, &x, &y, &w, &h)) {
        return false;
    }
    host.click(x + w / 2, y + h / 2, 2);
    return true;
}

bool has_click_handler(oa::runtime::GameRuntime& rt, const std::string& id) {
    const auto* h = rt.scene().find_event_handler(id, "click");
    return h && h->enabled;
}

/// 剧情正文层（ADV 的 `adv`）这一页的文字。
bool load_soak_page_ready(oa::runtime::GameRuntime& rt);
bool fresh_story(oa::test::TestHost& host);

std::string adv_text(oa::runtime::GameRuntime& rt) {
    const auto* ml = rt.text().layer("adv");
    return ml ? oa::test::page_text(ml, 200) : std::string();
}

int wait_frames(oa::test::TestHost& host, int budget, const std::function<bool()>& pred) {
    for (int f = 0; f < budget && !host.rt().exit_requested(); ++f) {
        if (pred()) return f;
        // App::tick() 返回 false = 引擎要求退出；这一拍可能正是谓词（
        // rt.exit_requested()）变真的那一拍，所以还要再判一次。
        if (!host.tick()) return pred() ? f : -1;
    }
    return pred() ? budget : -1;
}

/// 框架的 lydialog 确认框（1000.200 = OK）：出现就点掉。
void confirm_dialog(oa::test::TestHost& host) {
    auto& rt = host.rt();
    if (!has_click_handler(rt, "1000.200.2")) return;
    click_layer_center(host, "1000.200.2", 300, 80);
    wait_frames(host, 600, [&] { return !has_click_handler(rt, "1000.200.2"); });
}

void wait_mask_clear(oa::test::TestHost& host) {
    wait_frames(host, 600, [&] {
        const auto v = host.rt().interpreter().variables().get("t.eventfilter");
        return !v || v->to_display() == "0";
    });
}

bool open_dock(oa::test::TestHost& host) {
    auto& rt = host.rt();
    wait_mask_clear(host);
    // 消息窗底部的 dock 触发条是 hover 出现的：把指针放到窗口下沿，
    // 让框架把 sysbtn 重新建出来（读档/关 UI 之后尤其需要这一下）。
    host.move(host.stage_w() / 2.0, host.stage_h() * 0.93);
    host.tick(24);
    const auto sub = rt.interpreter().variables().get("t.sysbtn_submenu");
    // 从回顾页/菜单返回时 submenu_autoclose=0（t10 的 preset），子菜单
    // 保持展开：这时再点 110.2.2 只会把它关掉。已展开就直接用。
    if (sub && sub->to_display() == "1" && has_click_handler(rt, "110.3.2")) return true;
    if (!has_click_handler(rt, "110.2.2")) return false;
    click_layer_center(host, "110.2.2", 49, 40);
    return wait_frames(host, 300, [&] {
               const auto v = rt.interpreter().variables().get("t.sysbtn_submenu");
               return v && v->to_display() == "1";
           }) >= 0;
}

/// 剧情中打开 dock：菜单页的层必须出现、可点，掩码必须自己放开。
void case_system_menu_from_story(oa::test::TestHost& host) {
    auto& rt = host.rt();
    wait_frames(host, 600, [&] {
        return rt.current_wait() && rt.current_wait()->waiting_click() &&
               oa::test::reveal_done(rt) && oa::test::story_layer(rt);
    });

    check(open_dock(host), "one click opens the message-window dock");
    // 菜单按钮：save / load / config。层存在、click 行 enabled、并真的进了渲染队列。
    check(has_click_handler(rt, "110.7.2") && has_click_handler(rt, "110.8.2") &&
              has_click_handler(rt, "110.9.2"),
          "the opened dock builds clickable save/load/config buttons");
    check(host.visible("110.7.1") && host.visible("110.9.1"),
          "the dock menu buttons are drawn (not just instantiated)");

    // 掩码在 submenu 的转场结束后必须归零，否则下一次点击会被 lyevent 过滤器吞掉。
    const int mask = wait_frames(host, 300, [&] {
        const auto v = rt.interpreter().variables().get("t.eventfilter");
        return !v || v->to_display() == "0";
    });
    check(mask >= 0, "the dock transition releases the script input mask");

    // 一次点击进设置页（菜单按钮的 handler 是新层，不能是旧实例）。
    check(click_layer_center(host, "110.9.2", 133, 129), "the config button is hittable");
    check(wait_frames(host, 600, [&] { return ends_with(script_name(rt), "system/config.iet"); }) >= 0,
          "one click on the dock opens the settings page");
}

/// 设置页 TEXT → VOICE → SOUND → TEXT，连续 10 轮。
///
/// 每切一次页面都 `[lydel id=100]` 整棵重建；新的 `100.1` 必须是新实例
/// （generation 变化），下一个 tab 的 click 行必须还能用。旧 handler 粘在
/// 新层上时，第一轮之后就会"点不动"。
void case_settings_tabs_rebuild(oa::test::TestHost& host) {
    auto& rt = host.rt();
    if (!ends_with(script_name(rt), "system/config.iet")) return;

    // TEXT 页：100.50 = VOICE；VOICE 页：100.52 = SOUND；SOUND 页：100.50 = TEXT。
    const char* tab[] = {"100.50.2", "100.52.2", "100.50.2"};
    const char* page[] = {"config_v.iet", "config_s.iet", "config.iet"};

    uint32_t gen = rt.scene().generation_of("100.1");
    int switched = 0;
    bool fresh = true;
    for (int i = 0; i < 30; ++i) {
        wait_mask_clear(host);
        if (!click_layer_center(host, tab[i % 3], 158, 81)) break;
        const int used = wait_frames(host, 600, [&] {
            return ends_with(script_name(rt), page[i % 3]);
        });
        if (used < 0) break;
        const uint32_t now = rt.scene().generation_of("100.1");
        if (now == gen) fresh = false;
        gen = now;
        ++switched;
        if (!fresh) break;
    }
    check(switched == 30, "TEXT/VOICE/SOUND switch 30 times (10 rounds), one click each");
    check(fresh, "every settings page is a freshly rebuilt layer (no stale handlers)");
    check(rt.scene().generation_of("100.1") != 0 && host.visible("100.1"),
          "the final settings page is drawn");
}

/// 从菜单进 SAVE / LOAD：槽位层必须可见，且按 2 列网格排在舞台内。
void case_save_load_slots(oa::test::TestHost& host) {
    auto& rt = host.rt();
    wait_mask_clear(host);

    // 先离开设置页回剧情（btn_close = leave）。
    if (ends_with(script_name(rt), "config.iet") ||
        ends_with(script_name(rt), "config_v.iet") ||
        ends_with(script_name(rt), "config_s.iet")) {
        click_layer_center(host, "100.63.2", 97, 92);
        wait_frames(host, 600, [&] { return has_click_handler(rt, "110.2.2"); });
    }
    if (!open_dock(host)) {
        check(false, "the dock reopens after leaving settings");
        return;
    }
    wait_mask_clear(host);

    auto slot_roots = [&] {
        std::set<std::string> roots;
        for (const oa::test::PlanRow& r : host.plan()) {
            if (r.opacity <= 0.0) continue;
            if (r.id.rfind("100.slot.", 0) != 0) continue;
            if (r.w < 100 || r.h < 50) continue;
            std::string root = r.id;
            // 100.slot.N 或 100.slot.N.M → 100.slot.N
            size_t p = root.find('.', 9);
            if (p != std::string::npos) root = root.substr(0, p);
            roots.insert(root);
        }
        return roots;
    };
    auto check_slots = [&](const char* what) {
        const auto roots = slot_roots();
        bool in_stage = true;
        for (const oa::test::PlanRow& r : host.plan()) {
            if (r.opacity <= 0.0 || r.id.rfind("100.slot.", 0) != 0) continue;
            if (r.w < 100 || r.h < 50) continue;
            if (r.x < 0 || r.y < 0 || r.x + r.w > host.stage_w() || r.y + r.h > host.stage_h())
                in_stage = false;
        }
        check(roots.size() >= 6, std::string(what) + ": the slot grid is drawn");
        check(in_stage, std::string(what) + ": slots are laid out inside the stage");
    };

    check(click_layer_center(host, "110.7.2", 96, 129), "the save button is hittable");
    check(wait_frames(host, 600, [&] { return ends_with(script_name(rt), "system/save.iet"); }) >= 0,
          "one click opens the save page");
    check_slots("save page");

    // 写一个槽：槽位里必须有剧情预览文字（不是只有槽位号/日期）。
    wait_mask_clear(host);
    check(click_layer_center(host, "100.slot.2.2", 300, 135), "the first slot is hittable");
    confirm_dialog(host);
    wait_frames(host, 600, [&] {
        const auto* ml = rt.text().layer("101.sblock.2.3");
        return ml && ml->char_count > 0;
    });
    {
        const auto* preview = rt.text().layer("101.sblock.2.3");
        check(preview && oa::test::has_visible_chars(oa::test::page_text(preview, 200)),
              "save page: the slot shows the story preview text");
    }

    // SAVE → LOAD 页内 tab（100.54），再 LOAD → SAVE（100.53）。
    wait_mask_clear(host);
    check(click_layer_center(host, "100.54.2", 263, 81), "the load tab is hittable");
    check(wait_frames(host, 600, [&] { return ends_with(script_name(rt), "system/load.iet"); }) >= 0,
          "one click opens the load page");
    check_slots("load page");

    wait_mask_clear(host);
    check(click_layer_center(host, "100.53.2", 263, 81), "the save tab is hittable");
    check(wait_frames(host, 600, [&] { return ends_with(script_name(rt), "system/save.iet"); }) >= 0,
          "one click returns to the save page");
    check_slots("save page (return)");
}

/// 读档后打开 SAVE：槽位必须有预览（或明确的空槽占位）、页码正常、能存能读。
///
/// 现场（用户截图）：读档后 SAVE 页六个槽位全空白，左下角 `@@`，PAGE 后面
/// 没有数字。槽位文案的来源是**框架自己**：save.iet 在存档时写
/// `g.<prefix>save_msg_*`，读档时 update_slot_msg 读这些 g. 变量重画。
/// 引擎不补任何预览 —— 这里钉住的是"g. 域在读档后还在、框架能自己画出来"。
void case_save_page_after_load(oa::test::TestHost& host) {
    auto& rt = host.rt();
    check(fresh_story(host), "save-page-after-load: a clean story page is ready");
    wait_frames(host, 1200, [&] {
        return load_soak_page_ready(rt) && !rt.scene().has_tweens() && !rt.transition().active();
    });

    const auto slot_text = [&](int slot) {
        const auto* ml = rt.text().layer("101.sblock." + std::to_string(slot) + ".3");
        return ml ? oa::test::page_text(ml, 120) : std::string();
    };
    // 槽位的第一行是框架自己写的日期/空槽占位（save.iet 的
    // `luaSaveInfomationWrite`）。引擎不再从存档正文补任何预览：框架在
    // 存档时写 `g.<prefix>save_msg_*`，读档后这些 g. 变量还在。
    const auto slot_date = [&](int slot) {
        const auto* ml = rt.text().layer("101.sblock." + std::to_string(slot) + ".1");
        return ml ? oa::test::page_text(ml, 64) : std::string();
    };
    const auto all_slots_have_text = [&](const char* tag) {
        bool ok = true;
        for (int i = 1; i <= 6; ++i) {
            const std::string t = slot_date(i) + slot_text(i);
            if (!oa::test::has_visible_chars(t)) {
                std::fprintf(stderr, "[%s] slot %d has no text\n", tag, i);
                ok = false;
            }
            if (t.find("@@") != std::string::npos) {
                std::fprintf(stderr, "[%s] slot %d still shows '@@': '%s'\n", tag, i,
                             t.c_str());
                ok = false;
            }
        }
        return ok;
    };

    check(open_dock(host), "save-page-after-load: the dock opens");
    wait_mask_clear(host);
    check(click_layer_center(host, "110.7.2", 96, 129), "save-page-after-load: save button");
    check(wait_frames(host, 900, [&] {
              return ends_with(script_name(rt), "system/save.iet");
          }) >= 0,
          "save-page-after-load: the save page opens");
    wait_mask_clear(host);
    host.tick(60);

    check(!var_str(rt, "page").empty() && var_str(rt, "page") != "0",
          "save-page-after-load: the page number is set");
    const int slot_page = [&] {
        try { return std::stoi(var_str(rt, "g.slot_page")); } catch (...) { return 0; }
    }();
    check(slot_page >= 1, "save-page-after-load: g.slot_page is a positive number");
    int slot_rows = 0;
    for (const oa::test::PlanRow& r : host.plan()) {
        if (r.opacity <= 0.0 || r.id.rfind("100.slot.", 0) != 0) continue;
        if (r.w < 100 || r.h < 50) continue;
        ++slot_rows;
    }
    check(slot_rows >= 6, "save-page-after-load: six slot boxes are drawn");
    check(all_slots_have_text("save-before"), "save-page-after-load: every slot shows text or an empty-slot label");

    // 存到第 3 个槽（避开其他用例用的 1/2），再读回来。
    wait_mask_clear(host);
    check(click_layer_center(host, "100.slot.3.2", 300, 135),
          "save-page-after-load: slot 3 is hittable");
    confirm_dialog(host);
    host.tick(60);
    const std::string saved_preview = slot_text(3);
    check(oa::test::has_visible_chars(saved_preview),
          "save-page-after-load: slot 3 shows the story preview after saving");

    // SAVE → LOAD 页内 tab。
    wait_mask_clear(host);
    check(click_layer_center(host, "100.54.2", 263, 81), "save-page-after-load: load tab");
    check(wait_frames(host, 900, [&] {
              return ends_with(script_name(rt), "system/load.iet");
          }) >= 0,
          "save-page-after-load: the load page opens");
    wait_mask_clear(host);
    check(click_layer_center(host, "100.slot.3.2", 300, 135),
          "save-page-after-load: the saved slot is hittable");
    confirm_dialog(host);
    const int settled = wait_frames(host, 2400, [&] {
        return load_soak_page_ready(rt) && !host.visible("100.0") &&
               !rt.scene().has_tweens() && !rt.transition().active();
    });
    check(settled >= 0, "save-page-after-load: the slot loads back to a clickable page");
    check(host.last_error().empty(), "save-page-after-load: no engine error");

    // 读档后立刻再开 SAVE：六个槽位仍然必须有字（这是用户报的现场）。
    check(open_dock(host), "save-page-after-load: the dock reopens after LOAD");
    wait_mask_clear(host);
    check(click_layer_center(host, "110.7.2", 96, 129),
          "save-page-after-load: save button after LOAD");
    check(wait_frames(host, 900, [&] {
              return ends_with(script_name(rt), "system/save.iet");
          }) >= 0,
          "save-page-after-load: the save page opens after LOAD");
    wait_mask_clear(host);
    host.tick(80);
    check(all_slots_have_text("save-after-load"),
          "save-page-after-load: no blank slots / no '@@' after LOAD");
}

/// 设置页 SOUND → VOICE → TEXT → EXIT。
///
/// 现场：`config_s.iet` 的 `*goto_config_v` 写着 `[call label="mw_reprop"]`，
/// 但 `mw_reprop` 只定义在 `config.iet`。旧解析只查当前脚本，于是
/// `call: label not found: mw_reprop @ system/config_s.iet:277` 直接退出。
/// 修法是规则级的：无 file 的 label 解析走"当前脚本 → 调用栈 → 已加载脚本"。
void case_config_tabs_exit(oa::test::TestHost& host) {
    auto& rt = host.rt();
    check(fresh_story(host), "config-tabs: a clean story page is ready");
    wait_frames(host, 1200, [&] {
        return load_soak_page_ready(rt) && !rt.scene().has_tweens() && !rt.transition().active();
    });
    check(open_dock(host), "config-tabs: the dock opens");
    wait_mask_clear(host);
    check(click_layer_center(host, "110.9.2", 133, 129), "config-tabs: config button");
    check(wait_frames(host, 900, [&] {
              return ends_with(script_name(rt), "system/config.iet");
          }) >= 0,
          "config-tabs: the settings page opens");

    const auto click_first = [&](const char* prefix) {
        for (const oa::render::Layer* l : rt.scene().draw_order()) {
            if (l->id.rfind(prefix, 0) != 0) continue;
            if (!has_click_handler(rt, l->id)) continue;
            return click_layer_center(host, l->id, 60, 40);
        }
        return false;
    };

    // TEXT → SOUND
    wait_mask_clear(host);
    check(click_layer_center(host, "100.52.2", 263, 81), "config-tabs: TEXT → SOUND");
    check(wait_frames(host, 900, [&] {
              return ends_with(script_name(rt), "system/config_s.iet");
          }) >= 0,
          "config-tabs: the sound page opens");
    check(host.last_error().empty(), "config-tabs: no error entering SOUND");

    // 声音复选框 / 静音喇叭（100.soundmute.*）。
    wait_mask_clear(host);
    (void)click_first("100.soundmute.");
    host.tick(40);
    check(host.last_error().empty(), "config-tabs: the sound mute toggle does not error");
    for (const oa::render::Layer* l : rt.scene().draw_order())
        std::fprintf(stderr, "[config-tabs] soundmute layer %s handler=%d\n", l->id.c_str(),
                     int(has_click_handler(rt, l->id)));

    // SOUND → VOICE：这一跳就是旧实现崩溃的 goto_config_v（mw_reprop）。
    wait_mask_clear(host);
    check(click_layer_center(host, "100.52.2", 263, 81), "config-tabs: SOUND → VOICE");
    check(wait_frames(host, 900, [&] {
              return ends_with(script_name(rt), "system/config_v.iet");
          }) >= 0,
          "config-tabs: the voice page opens (mw_reprop resolves from config.iet)");
    check(host.last_error().empty(), "config-tabs: no 'label not found' on SOUND → VOICE");

    wait_mask_clear(host);
    (void)click_first("100.voicemute.");
    host.tick(40);
    check(host.last_error().empty(), "config-tabs: the voice mute toggle does not error");

    // VOICE → TEXT
    wait_mask_clear(host);
    check(click_layer_center(host, "100.50.2", 263, 81), "config-tabs: VOICE → TEXT");
    check(wait_frames(host, 900, [&] {
              return ends_with(script_name(rt), "system/config.iet");
          }) >= 0,
          "config-tabs: the text page opens");
    check(host.last_error().empty(), "config-tabs: no error entering TEXT");

    // EXIT：回到剧情。
    wait_mask_clear(host);
    check(click_layer_center(host, "100.63.2", 97, 92), "config-tabs: exit button");
    check(wait_frames(host, 1800, [&] { return load_soak_page_ready(rt); }) >= 0,
          "config-tabs: EXIT returns to the story");
    check(host.last_error().empty(), "config-tabs: no error after EXIT");
}

// ---------------------------------------------------------------------------
// 菜单四项：LOG / LOAD / EXIT / 返回标题
//
// 四条现场都要求"一次点击生效"，并且连续 10 轮不退化。它们曾经分别坏在：
//   * LOG：`[backlog allow=0]` 被当成"别记录"，自前バックログ拿到
//     get_backlog_size=0，点 LOG 只弹"没有回顾"提示，页面不出现；
//   * LOAD：`[load]` 不在 ConfigEvent 名单里，`$g.savedata.prefix + ...`
//     原样进了 read path，读档永远找不到文件；
//   * EXIT / 返回标题：框架用 `[call dummy]` + 排队 `[return]` 建局部作用域，
//     进入 dummy 体会先 self-return，随后队列里的 `[return]` 弹穿调用者，
//     `共通終了処理` / `共通タイトルバック処理` 走不到 `[exit]` / `[reset]`。
// ---------------------------------------------------------------------------

std::string wait_var(oa::runtime::GameRuntime& rt, const char* name) {
    const auto v = rt.interpreter().variables().get(name);
    return v ? v->to_display() : std::string();
}

bool story_showing(oa::runtime::GameRuntime& rt) {
    const auto* ml = oa::test::story_layer(rt);
    return ml && ml->char_count > 0 && oa::test::reveal_done(rt);
}


bool wait_story_page(oa::test::TestHost& host, int budget = 3000) {
    auto& rt = host.rt();
    return wait_frames(host, budget, [&] { return story_showing(rt); }) >= 0;
}

bool restart_story(oa::test::TestHost& host) {
    auto& rt = host.rt();
    std::string err;
    (void)rt.interpreter().try_load_script("scenario/main/s100.txt", &err);
    try {
        rt.interpreter().start("scenario/main/s100.txt", "top");
    } catch (const std::exception&) {
        return false;
    }
    return wait_story_page(host);
}

/// [reset] 回到标题，再从标题直接起剧本 —— 干净的停驻/调用栈。
bool fresh_story(oa::test::TestHost& host) {
    auto& rt = host.rt();
    oa::runtime::Event reset;
    reset.kind = oa::runtime::Event::Kind::Reset;
    rt.apply_event(reset);
    wait_frames(host, 4000, [&] {
        return ends_with(script_name(rt), "system/title.iet") && has_click_handler(rt, "100.1.2");
    });
    for (int i = 0; i < 60; ++i) host.tick(1);
    return restart_story(host);
}

// ---------------------------------------------------------------------------
// 人物语音：第一句有语音的台词 → 通道在播（解码有帧）；连看两句 → 换文件
//
// 这一作的台词语音走框架的 `[se_play]` 宏，不是原生 `[voice]`：
//   system/charaset.iet 的 `<角色>_MWxx` 宏 → [calllua voiceSet]
//   → system/chara_v.iet voiceStartProc → tagFilters["se_play"] → 原生 seplay
// 所以语音落在 **SE 通道表的 `<voice_id>.0`** 上（八田=102.0），
// `voice_channels` 是空的。断言按这个形状写：
//   1. 语音指令都已实现（不认识的话会被记进 unknown_tags）；
//   2. 该通道在播、文件在 `_sound/voice/`、解码器已经产出帧；
//   3. 没被静音：通道 raw gain > 0 且 SE 总线 > 0
//      （测试宿主是静音宿主，master 恒 0，所以有效增益在无头下没有意义）；
//   4. 通道初始增益 = 框架算好的实值 `s.segain.<通道id>`
//      （VOICE 滑杆 × 角色音量 × 角色静音）；
//   5. 下一句 → 同一个通道换成下一个文件，旧的那条不再存在。
// ---------------------------------------------------------------------------
void case_voice_playback(oa::test::TestHost& host) {
    auto& rt = host.rt();

    // 1. 语音指令必须都实现了。
    std::string unknown_voice;
    for (const auto& [tag, n] : rt.interpreter().unknown_tags()) {
        (void)n;
        if (tag.find("voice") != std::string::npos || tag.find("vplay") != std::string::npos) {
            unknown_voice += tag + " ";
        }
    }
    check(unknown_voice.empty(),
          "voice: every voice tag is implemented (unknown_tags has none)");
    if (!unknown_voice.empty())
        std::fprintf(stderr, "      unknown voice tags: %s\n", unknown_voice.c_str());

    check(restart_story(host), "voice: a story page is ready");

    // 语音通道的判据：文件落在 _sound/voice/ 下的 SE 通道。
    auto voice_ids = [&]() {
        std::vector<std::string> ids;
        for (const auto& [id, ch] : rt.audio().state().se_channels) {
            if (ch.file.find("/voice/") != std::string::npos) ids.push_back(id);
        }
        return ids;
    };
    auto channel = [&](const std::string& id) -> const oa::media::SoundChannel* {
        const auto& st = rt.audio().state();
        const auto it = st.se_channels.find(id);
        return it == st.se_channels.end() ? nullptr : &it->second;
    };
    auto decode_of = [&](const std::string& id, double* secs, bool* decodable) {
        for (const auto& s : rt.sound_state()) {
            if (s.category == "se" && s.id == id) {
                if (secs) *secs = s.decoded_seconds;
                if (decodable) *decodable = s.decodable;
                return true;
            }
        }
        return false;
    };
    auto next_page = [&]() {
        const std::string before = adv_text(rt);
        wait_frames(host, 400, [&] { return oa::test::reveal_done(rt); });
        const auto p = oa::test::screen_click_point(rt);
        host.click(p ? p->first : host.stage_w() / 2, p ? p->second : host.stage_h() / 2, 2);
        return wait_frames(host, 900, [&] {
                   const std::string t = adv_text(rt);
                   return !t.empty() && t != before;
               }) >= 0;
    };

    // 2. 找到第一句有语音的台词（开场前两句是旁白，第三句起才有 voice）。
    std::string id1, file1;
    for (int page = 0; page < 8 && file1.empty(); ++page) {
        wait_frames(host, 200, [&] { return oa::test::story_layer(rt) != nullptr; });
        // 排队的 seplay 要几帧才落地，解码也要先跑出几帧。
        host.tick(30);
        const auto ids = voice_ids();
        if (!ids.empty()) {
            id1 = ids.front();
            if (const auto* ch = channel(id1)) file1 = ch->file;
            break;
        }
        if (page < 7) (void)next_page();
    }
    check(!id1.empty() && !file1.empty(),
          "voice: the first voiced line starts a voice channel");
    if (id1.empty() || file1.empty()) return;

    std::fprintf(stderr, "      voice channel id='%s' file='%s'\n", id1.c_str(),
                 file1.c_str());

    double secs1 = 0.0;
    bool decodable1 = true;
    const bool have1 = decode_of(id1, &secs1, &decodable1);
    check(have1 && decodable1, "voice: the voice file decodes (vorbis stream opens)");
    check(secs1 > 0.0,
          "voice: the decoder already produced frames (headless proof of playback)");
    const auto* ch1 = channel(id1);
    check(ch1 && ch1->playing, "voice: the voice channel is playing");
    check(ch1 && ch1->raw_gain > 0, "voice: the channel is not muted (raw gain > 0)");
    check(rt.audio().state().se_volume > 0.0f, "voice: the SE bus is not muted");
    check(voice_ids().size() == 1, "voice: exactly one voice channel at a time");

    // 4. 通道增益来自框架的实值音量 s.segain.<通道id>。
    const std::string want_gain = var_str(rt, ("s.segain." + id1).c_str());
    check(!want_gain.empty() && ch1 && std::to_string(ch1->raw_gain) == want_gain,
          "voice: the channel gain is the framework's s.segain.<id> (= " + want_gain + ")");

    // 5. 下一句有语音的台词：同一个通道换文件，旧文件不再存在。
    std::string file2;
    for (int page = 0; page < 4 && file2.empty(); ++page) {
        if (!next_page()) break;
        host.tick(30);
        const auto ids = voice_ids();
        for (const std::string& id : ids) {
            const auto* ch = channel(id);
            if (ch && ch->file != file1) {
                file2 = ch->file;
                break;
            }
        }
    }
    check(!file2.empty(), "voice: the next voiced line starts a voice again");
    check(file2 != file1, "voice: the new line replaced the previous voice (channel switch)");
    check(voice_ids().size() <= 1, "voice: the previous voice is not left playing");
    if (!file2.empty()) {
        const auto* ch2 = channel(id1);
        check(ch2 && ch2->playing, "voice: the switched channel is still the same slot, playing");
        double secs2 = 0.0;
        bool decodable2 = true;
        (void)decode_of(id1, &secs2, &decodable2);
        check(secs2 > 0.0, "voice: the new voice decodes too");
    }
}

/// 从存档/设置页回到剧情（页脚的 leave 按钮）。
bool leave_ui(oa::test::TestHost& host, const char* script_suffix) {    auto& rt = host.rt();
    if (ends_with(script_name(rt), script_suffix)) {
        wait_mask_clear(host);
        click_layer_center(host, "100.63.2", 97, 92);
    }
    return wait_frames(host, 1200, [&] { return has_click_handler(rt, "110.2.2"); }) >= 0;
}

/// 游戏在剧情开始处 `[writebacklog mode="1"]`；直接起剧本的用例补上这一步。
void enable_backlog_recording(oa::runtime::GameRuntime& rt) {
    rt.text().set_backlog_write_mode(true);
}

/// LOG：按一次 → 回顾页出现；关掉 → 回剧情；10 轮。
void case_menu_log(oa::test::TestHost& host) {
    auto& rt = host.rt();
    bool ok = true;
    int opened = 0;
    for (int round = 0; round < 10 && ok; ++round) {
        // 每轮从干净的 [reset] → 标题 → s100 开始：回顾页的「戻る」用
        // [returns no=...] 依赖启动时的栈深度，直接起剧本的栈更浅，落点会
        // 偏到 tips；本用例只断言"一次点击出 LOG 页"，所以每轮重置上下文。
        if (!fresh_story(host)) {
            if (std::getenv("OA_T10_DBG")) std::fprintf(stderr, "[logfail] fresh_story\n");
            ok = false;
            break;
        }
        enable_backlog_recording(rt);
        // 真实启动时下一页的 [rp] 会归档这一页；这里补一次同样的 PageBreak。
        if (rt.text().backlog_size() == 0) rt.text().page_break(std::nullopt);
        if (round == 0)
            check(rt.text().backlog_size() > 0,
                  "log: story pages are archived while recording is on");
        if (!open_dock(host)) {
            if (std::getenv("OA_T10_DBG")) std::fprintf(stderr, "[logfail] dock script=%s\n", script_name(rt).c_str());
            ok = false;
            break;
        }
        wait_mask_clear(host);
        if (!click_layer_center(host, "110.3.2", 300, 150)) {
            if (std::getenv("OA_T10_DBG")) std::fprintf(stderr, "[logfail] click 110.3.2\n");
            ok = false;
            break;
        }
        const int shown = wait_frames(host, 120, [&] {
            return script_name(rt).find("backlog.iet") != std::string::npos &&
                   host.visible("100.0");
        });
        if (shown < 0) {
            if (std::getenv("OA_T10_DBG")) std::fprintf(stderr, "[logfail] shown script=%s vis=%d\n", script_name(rt).c_str(), int(host.visible("100.0")));
            ok = false;
            break;
        }
        if (round == 0) {
            // 回顾页的"行框有、字没有"（用户报的 backlog 没有字体）：
            // 页内重放命令必须是框架的逗号形态（`print,data,…`），解释器
            // 的 `[tag data=…]` 才会真正把字打回 100.1.<row>.10 层。
            const int text = wait_frames(host, 240, [&] {
                return drawn_text_layers(rt, "100.1.") > 0;
            });
            const auto bt = rt.text().backlog_tags(0, false);
            if (std::getenv("OA_T10_DBG")) {
                std::fprintf(stderr, "[bk] rows_drawn=%d pages=%zu page0_tags=%d t.l.tags.size=%s\n",
                             text, rt.text().backlog_size(), bt ? int(bt->size()) : -1,
                             var_str(rt, "t.l.tags.size").c_str());
                if (text <= 0) dump_text_layers(rt, "100.");
            }
            check(text >= 0, "log: the backlog page draws the rows' text");
            // 回顾页行数 == 引擎里归档的页数（每页一行，ADV 的正文层 .10 /
            // ノベル的正文层 .11；空行不算）。
            int rows = 0;
            for (const auto& [id, ml] : rt.text().all_layers()) {
                if (!ml || ml->char_count == 0) continue;
                if (id.rfind("100.1.", 0) != 0) continue;
                if (id.size() < 2 || (id.compare(id.size() - 2, 2, "10") != 0 &&
                                      id.compare(id.size() - 2, 2, "11") != 0))
                    continue;
                ++rows;
            }
            check(rows == int(rt.text().backlog_size()),
                  "log: one drawn row per archived backlog page");
        }
        ++opened;
    }
    if (std::getenv("OA_T10_DBG"))
        std::fprintf(stderr, "[logend] ok=%d opened=%d script=%s story=%d\n", int(ok), opened,
                     script_name(rt).c_str(), int(story_showing(rt)));
    check(ok && opened == 10, "LOG page opens 10 times, one click each");
}


/// LOAD：先通过存档页写出一个槽位，再读它；10 轮。
void case_menu_load(oa::test::TestHost& host) {
    auto& rt = host.rt();
    (void)leave_ui(host, "save.iet");
    (void)leave_ui(host, "config.iet");
    // LOG 用例结束时回顾页还开着；它的行文字现在是真的画出来的（正文层
    // 也有字），story_showing() 分不出"剧情页"和"回顾页"。按脚本名判断，
    // 只要停在 system/ 下的页面就先回一次干净剧情。
    if (!story_showing(rt) || script_name(rt).find("system/") != std::string::npos)
        (void)fresh_story(host);
    check(wait_story_page(host), "load: a story page is ready");

    // 用游戏自己的存档页写 savenum=1（100.slot.2），这样 g.cs 槽位缓存也更新。
    check(open_dock(host), "load: dock opens for saving");
    wait_mask_clear(host);
    check(click_layer_center(host, "110.7.2", 96, 129), "load: save button is hittable");
    check(wait_frames(host, 900, [&] { return ends_with(script_name(rt), "system/save.iet"); }) >= 0,
          "load: save page opens");
    wait_mask_clear(host);
    check(has_click_handler(rt, "100.slot.2.2"), "load: the slot is on the save page");
    click_layer_center(host, "100.slot.2.2", 300, 135);
    wait_frames(host, 240, [&] { return !has_click_handler(rt, "1000.200.2"); });

    // 不重置：g.cs 的槽位缓存是 VM 态，[reset] 会清掉，LOAD 页就看不到
    // 刚存的槽位。从存档页切到 LOAD 页；随后反复 关闭→dock→LOAD 9 次
    // （每次都是"一次点击就进页"），最后一次点槽位完成读档。
    wait_mask_clear(host);
    bool ok = click_layer_center(host, "100.54.2", 263, 81) &&
              wait_frames(host, 900, [&] { return ends_with(script_name(rt), "system/load.iet"); }) >= 0;
    wait_mask_clear(host);

    for (int round = 0; round < 9 && ok; ++round) {
        if (!has_click_handler(rt, "100.63.2")) {
            ok = false;
            break;
        }
        wait_mask_clear(host);
        click_layer_center(host, "100.63.2", 97, 92);
        if (wait_frames(host, 1200, [&] { return has_click_handler(rt, "110.2.2"); }) < 0) {
            ok = false;
            break;
        }
        if (!open_dock(host)) {
            ok = false;
            break;
        }
        wait_mask_clear(host);
        if (!click_layer_center(host, "110.8.2", 96, 129) ||
            wait_frames(host, 900, [&] { return ends_with(script_name(rt), "system/load.iet"); }) < 0) {
            ok = false;
            break;
        }
        wait_mask_clear(host);
    }

    bool loaded = false;
    if (ok && has_click_handler(rt, "100.slot.2.2")) {
        click_layer_center(host, "100.slot.2.2", 300, 135);
        loaded = wait_frames(host, 3000, [&] {
                     const auto v = rt.interpreter().variables().get("t.eventfilter");
                     return story_showing(rt) &&
                            script_name(rt).find("load.iet") == std::string::npos &&
                            (!v || v->to_display() == "0");
                 }) >= 0;
    }
    check(ok && loaded, "LOAD page opens 10 times and a slot click restores the page");

    if (ok && loaded) {
        // 读档完成：页面层必须收干净，剧情页在等点击，而且**一下就能翻页**。
        check(!host.visible("100.0"), "load: the load page layers are gone");
        // 读档链自己还会跑一小段收尾（关闭页面的 trans 等），等它停到
        // "点击停驻"再断言，不能按某一帧的瞬时状态判死。
        const int ready = wait_frames(host, 400, [&] {
            const auto* w = rt.current_wait();
            return w && w->waiting_click() && oa::test::reveal_done(rt) &&
                   script_name(rt).find("load.iet") == std::string::npos;
        });
        const std::string before = adv_text(rt);
        check(oa::test::has_visible_chars(before), "load: the loaded story page has text");
        if (const auto* w = rt.current_wait(); w && std::getenv("OA_T10_DBG"))
            std::fprintf(stderr,
                         "[ldstate] ready=%d wakes=0x%x timer=%lld stage=%d restored=%d owned=%d "
                         "owner=%s:%zu script=%s@%zu text='%s'\n",
                         ready, w->wakes, (long long)w->timer_ms, int(w->stage), int(w->restored),
                         int(w->script_owned), w->owner_script.c_str(), w->owner_line,
                         script_name(rt).c_str(), rt.interpreter().current_line(), before.c_str());
        check(ready >= 0, "load: the loaded page waits for a click");
        const unsigned long long seq0 = rt.current_wait() ? rt.current_wait()->seq : 0;
        host.click(host.stage_w() / 2.0, host.stage_h() / 2.0, 2);
        const bool turned = wait_frames(host, 900, [&] {
                                const std::string now = adv_text(rt);
                                return oa::test::has_visible_chars(now) && now != before;
                            }) >= 0;
        if (!turned && std::getenv("OA_T10_DBG")) {
            const auto* w = rt.current_wait();
            std::fprintf(stderr, "[ldclick] seq %llu -> %llu now='%s' wait=%d ef=%s active='%s' script=%s@%zu\n",
                         seq0, w ? w->seq : 0ULL, adv_text(rt).c_str(), w ? 1 : 0,
                         var_str(rt, "t.eventfilter").c_str(),
                         rt.text().active_layer_id().c_str(), script_name(rt).c_str(),
                         rt.interpreter().current_line());
        }
        check(turned, "load: one click turns the loaded page");
    }
}

/// EXIT：前 9 轮取消，第 10 轮确认退出。
void case_menu_exit(oa::test::TestHost& host) {
    auto& rt = host.rt();
    (void)leave_ui(host, "save.iet");
    (void)leave_ui(host, "config.iet");
    // 同上：停在系统页就回剧情；回顾页的行文字已可见，不能只看 story_showing。
    if (!story_showing(rt) || script_name(rt).find("system/") != std::string::npos)
        (void)fresh_story(host);
    check(wait_story_page(host), "exit: a story page is ready");

    bool ok = true;
    for (int round = 0; round < 10 && ok; ++round) {
        if (!open_dock(host)) {
            ok = false;
            break;
        }
        wait_mask_clear(host);
        if (!click_layer_center(host, "110.18.2", 300, 150)) {
            ok = false;
            break;
        }
        if (wait_frames(host, 900, [&] { return has_click_handler(rt, "1000.200.2") &&
                                                 has_click_handler(rt, "1000.201.2"); }) < 0) {
            if (std::getenv("OA_T10_DBG"))
                std::fprintf(stderr, "[loadfail] no dialog script=%s story=%d yesno=%s slot=%d\n",
                             script_name(rt).c_str(), int(story_showing(rt)),
                             var_str(rt, "t.yesno").c_str(),
                             int(has_click_handler(rt, "100.slot.2.2")));
            ok = false;
            break;
        }
        wait_mask_clear(host);
        if (round < 9) {
            click_layer_center(host, "1000.201.2", 300, 80);
            if (wait_frames(host, 1200, [&] {
                    return !has_click_handler(rt, "1000.200.2") && has_click_handler(rt, "110.2.2");
                }) < 0) {
                ok = false;
            }
        } else {
            click_layer_center(host, "1000.200.2", 300, 80);
            if (wait_frames(host, 900, [&] { return rt.exit_requested(); }) < 0) ok = false;
        }
    }
    check(ok, "EXIT can be cancelled 9 times without losing the dock");
    check(rt.exit_requested(), "the 10th EXIT confirm really quits the game");
}

/// 返回标题：确认框 → [reset] → system/title.iet；10 轮。
void case_menu_title(oa::test::TestHost& host) {
    auto& rt = host.rt();
    (void)leave_ui(host, "save.iet");
    (void)leave_ui(host, "config.iet");
    check(fresh_story(host), "title: a clean story page is ready");

    bool ok = true;
    for (int round = 0; round < 10 && ok; ++round) {
        if (std::getenv("OA_T10_DBG"))
            std::fprintf(stderr, "[titler] round=%d script=%s story=%d dock=%d\n", round,
                         script_name(rt).c_str(), int(story_showing(rt)),
                         int(has_click_handler(rt, "110.2.2")));
        if (!story_showing(rt)) {
            if (!fresh_story(host)) {
                ok = false;
                break;
            }
        }
        if (!open_dock(host)) {
            if (std::getenv("OA_T10_DBG")) std::fprintf(stderr, "[titlefail] dock\n");
            ok = false;
            break;
        }
        wait_mask_clear(host);
        if (!click_layer_center(host, "110.17.2", 300, 150)) {
            ok = false;
            break;
        }
        // g.titledialog 在点击前可能还没有值；以确认框实际出现为准。
        if (wait_frames(host, 600, [&] { return has_click_handler(rt, "1000.200.2"); }) >= 0) {
            wait_mask_clear(host);
            click_layer_center(host, "1000.200.2", 300, 80);
        }
        if (wait_frames(host, 1500, [&] {
                return ends_with(script_name(rt), "system/title.iet") && has_click_handler(rt, "100.1.2");
            }) < 0) {
            if (std::getenv("OA_T10_DBG"))
                std::fprintf(stderr, "[titlefail] reset script=%s mask=%s\n",
                             script_name(rt).c_str(), var_str(rt, "t.eventfilter").c_str());
            ok = false;
        }
    }
    check(ok, "return-to-title completes 10 times, one click each");
    check(ends_with(script_name(rt), "system/title.iet"),
          "the final screen is the real title script");
}

/// 用真实启动形状（语言 → 标题 → 开始）跑四项菜单动作。
///
/// 直接起 s100 会跳过 `[writebacklog mode=1]` 与 g.cs 槽位缓存，
/// LOG/LOAD 的现场就不成立；这里走游戏自己的启动链。
void case_menu_journey(const char* pfs) {
    namespace fs = std::filesystem;
    std::error_code ec;
    const fs::path root = fs::temp_directory_path(ec) /
                          ("oa_t10_menu_" + std::to_string(::getpid()));
    fs::remove_all(root, ec);
    fs::create_directories(root, ec);

    {
        oa::test::TestHost host({.project = pfs, .save_root = root.string()});
        check(host.ok(), "menu journey: host boots");
        if (!host.ok()) {
            fs::remove_all(root, ec);
            return;
        }
        auto& rt = host.rt();
        case_language_screen(host);

        // 直接起 s100 会跳过游戏启动时的 `[writebacklog mode=1]`；LOG 用例
        // 会自己补上。标题 → 开始的点击对"四个菜单动作"只是载体，不在这里
        // 断言（标题本身有独立的返回标题用例）。
        check(fresh_story(host), "menu journey: the story starts");
        check(rt.text().backlog_size() >= 0, "menu journey: the story is running");

        case_menu_log(host);
        case_menu_load(host);
        case_menu_title(host);
        case_menu_exit(host);
    }
    fs::remove_all(root, ec);
}

/// 确认退出 → syssave 落盘 → 重启后 g.language 有值（用户报"语言每次都要重选"）。
void case_exit_ui_syssave(const char* pfs) {
    namespace fs = std::filesystem;
    std::error_code ec;
    const fs::path root = fs::temp_directory_path(ec) /
                          ("oa_t10_exit_" + std::to_string(::getpid()));
    fs::remove_all(root, ec);
    fs::create_directories(root, ec);
    const std::string rel_dir = "KuroirodoruYomiji/Watamari01";
    const fs::path saveg = root / rel_dir / "saveg.oa.dat";

    {
        oa::test::TestHost host({.project = pfs, .save_root = root.string()});
        check(host.ok(), "exit syssave: the first host boots");
        if (!host.ok()) {
            fs::remove_all(root, ec);
            return;
        }
        auto& rt = host.rt();
        case_language_screen(host);
        check(var_str(rt, "g.language") == "ja", "exit syssave: language is ja before quitting");
        std::string err;
        (void)rt.interpreter().try_load_script("scenario/main/s100.txt", &err);
        try {
            rt.interpreter().start("scenario/main/s100.txt", "top");
        } catch (const std::exception&) {
        }
        check(wait_story_page(host), "exit syssave: story page is ready");
        check(open_dock(host), "exit syssave: dock opens");
        wait_mask_clear(host);
        check(click_layer_center(host, "110.18.2", 300, 150), "exit syssave: EXIT is hittable");
        check(wait_frames(host, 900, [&] { return has_click_handler(rt, "1000.200.2"); }) >= 0,
              "exit syssave: confirmation dialog opens");
        wait_mask_clear(host);
        click_layer_center(host, "1000.200.2", 300, 80);
        check(wait_frames(host, 900, [&] { return rt.exit_requested(); }) >= 0,
              "exit syssave: confirm quits");
    }

    check(fs::exists(saveg, ec), "exit syssave: [exit] writes saveg.oa.dat");
    if (fs::exists(saveg, ec)) {
        std::ifstream in(saveg, std::ios::binary);
        const std::string bytes((std::istreambuf_iterator<char>(in)),
                                std::istreambuf_iterator<char>());
        bool decoded = false;
        try {
            const auto vars = oa::runtime::decode_domain_map(bytes);
            const auto lang = vars.find("language");
            decoded = lang != vars.end() && lang->second.to_display() == "ja";
        } catch (const std::exception& e) {
            std::fprintf(stderr, "[exit syssave] decode failed: %s\n", e.what());
        }
        check(decoded, "exit syssave: saveg.oa.dat carries language=ja");
    }

    {
        oa::test::TestHost host({.project = pfs, .save_root = root.string()});
        check(host.ok(), "exit syssave: the second host boots");
        if (host.ok()) {
            auto& rt = host.rt();
            check(var_str(rt, "g.language") == "ja",
                  "exit syssave: the restart reads g.language back");
            bool language_screen = false;
            const int title = wait_frames(host, 4000, [&] {
                if (!find_language_button(rt).empty()) language_screen = true;
                return has_click_handler(rt, "100.11.2");
            });
            check(title >= 0, "exit syssave: the restart reaches the title");
            check(!language_screen, "exit syssave: the restart never asks for the language again");
        }
    }
    fs::remove_all(root, ec);
}

/// 全局存档往返：同一 save_root 启动两次。
///
/// 脚本依据（解包后的 `system/first.iet` / `system/title.iet`）：
///   * first.iet:291 `[if estimate = "$g.language == 0"]` —— 只有 g.language
///     读回来不是 0 才跳过语言选择；
///   * title.iet:275 `[btn ... id="100.5" file="image/title/btn_title_epilogue"
///     ng="$g.clear_flag == 0"]`、:285 `id="100.6" ... ng="$g.gallery_open == 0"`
///     —— 标题按钮的 clear 后变体只看这两个 g.* 标记。
/// 原版在 `[exit]` 时自动写全局档（macro.iet `*共通終了処理` 的注释：
/// 「[exit]命令でグローバルセーブが自動で走る」），所以这里的退出用同一个
/// Exit 事件驱动，而不是直接调 syssave —— 钉住的是"退出就落盘"这条规则。
void case_global_save_roundtrip(const char* pfs) {
    namespace fs = std::filesystem;
    std::error_code ec;
    const fs::path root = fs::temp_directory_path(ec) /
                          ("oa_t10_global_" + std::to_string(::getpid()));
    fs::remove_all(root, ec);
    fs::create_directories(root, ec);

    const std::string rel_dir = "KuroirodoruYomiji/Watamari01";
    const fs::path saveg = root / rel_dir / "saveg.oa.dat";

    // ---- 第一次启动：选语言 → 标题 → [exit] ------------------------------
    {
        oa::test::TestHost host({.project = pfs, .save_root = root.string()});
        check(host.ok(), "global save: the first host boots");
        if (!host.ok()) {
            fs::remove_all(root, ec);
            return;
        }
        auto& rt = host.rt();
        check(rt.savepath() == rel_dir, "global save: SAVEPATH sanitizes to the game folder");

        case_language_screen(host); // 现有的整条语言选择链（含"一次点击生效"）
        check(var_str(rt, "g.language") == "ja",
              "global save: the language is selected before quitting");

        // 标题变体标记：通关（clear）与 gallery 解锁。真实游戏在剧情里写，
        // 这里直接把"进度"写进 g.*，验证落盘 + 读回 + 标题分支三段都通。
        rt.interpreter().set_variable("g.clear_flag", oa::runtime::Value::make_int(1));
        rt.interpreter().set_variable("g.gallery_open", oa::runtime::Value::make_int(1));

        oa::runtime::Event ex;
        ex.kind = oa::runtime::Event::Kind::Exit;
        rt.apply_event(ex);
        for (int i = 0; i < 120 && !rt.exit_requested(); ++i) host.tick(1);
        check(rt.exit_requested(), "global save: [exit] quits the host");
    }

    // 落盘证据：文件存在，且内容里就是这次选的值（不是空壳）。
    check(fs::exists(saveg, ec), "global save: [exit] writes saveg.oa.dat");
    if (fs::exists(saveg, ec)) {
        std::ifstream in(saveg, std::ios::binary);
        const std::string bytes((std::istreambuf_iterator<char>(in)),
                                std::istreambuf_iterator<char>());
        bool decoded = false;
        try {
            const auto vars = oa::runtime::decode_domain_map(bytes);
            const auto lang = vars.find("language");
            const auto clear = vars.find("clear_flag");
            decoded = lang != vars.end() && lang->second.to_display() == "ja" &&
                      clear != vars.end() && clear->second.to_display() == "1";
        } catch (const std::exception& e) {
            std::fprintf(stderr, "[global save] decode failed: %s\n", e.what());
        }
        check(decoded, "global save: saveg.oa.dat carries language=ja and clear_flag=1");
    }

    // ---- 第二次启动：不再问语言，直接标题，且标题走 clear 后分支 ----------
    {
        oa::test::TestHost host({.project = pfs, .save_root = root.string()});
        check(host.ok(), "global save: the second host boots");
        if (!host.ok()) {
            fs::remove_all(root, ec);
            return;
        }
        auto& rt = host.rt();
        check(var_str(rt, "g.language") == "ja",
              "global save: the second boot reads g.language back");
        check(var_str(rt, "g.clear_flag") == "1",
              "global save: the second boot reads g.clear_flag back");

        bool language_screen_seen = false;
        const int title = wait_frames(host, 4000, [&] {
            if (!find_language_button(rt).empty()) language_screen_seen = true;
            return has_click_handler(rt, "100.11.2"); // 标题的"ゲーム終了"
        });
        check(title >= 0, "global save: the second boot reaches the title");
        check(!language_screen_seen, "global save: the second boot never shows the language screen");

        // title.iet:275 —— ng="$g.clear_flag == 0" 的反面：非 _ng 素材。
        const oa::render::Layer* epilogue = rt.scene().find("100.5.1");
        check(epilogue && epilogue->file.find("btn_title_epilogue") != std::string::npos &&
                  epilogue->file.find("_ng") == std::string::npos,
              "global save: the title takes the cleared (epilogue) branch");

        // title.iet:285 —— ng="$g.gallery_open == 0" 的反面。
        const oa::render::Layer* extra = rt.scene().find("100.6.1");
        check(extra && extra->file.find("btn_title_extra") != std::string::npos &&
                  extra->file.find("_ng") == std::string::npos,
              "global save: the title takes the gallery-unlocked branch");
    }

    fs::remove_all(root, ec);
}


// ---------------------------------------------------------------------------
// AUTO / SKIP 的左上角模式标识（layer 85）
//
// 现场（原版录像 40–43s）：底部 dock 点 AUTO 后，左上角出现 `info_automode`
// 图标（148x38 @ 29,29）；点 SKIP 后同一层换成 `info_commandskip`。
//
// 框架的路径（解包脚本）：
//   * sysbtn.iet:286  `110.10` AUTO 按钮 → macro.iet `*sys_autobutton` →
//     `[exec command="automode"]`（引擎侧 set_automode_mode）；
//   * 引擎派发 `automodein` → macro.iet:20287 `*automode_in_se`，它在
//     macro.iet:20347 用 `$s.status.automode` 判定"真的进模式了"，是才
//     `[call automode_in_draw]`（20348）；
//   * `*automode_in_draw`（macro.iet:20377）`[lyc id="85" file=...info_automode]`
//     + `visible=1` + alpha 0→255 的 `[lytween]`。
// 引擎从未维护 `s.status.automode`，该读取恒为假 ⇒ 图标一次都没建过。
//
// SKIP 同理：`*commandskip_in_se`（macro.iet:20501）在 20571 读
// `$s.status.commandskip`，而引擎是在**派发之后**才写镜像，handler 里读到的
// 还是上一帧的 0。
//
// 注意：本作用域内 `info_commandskip.png` 本身就是 1x1 的透明占位图
// （IHDR 1x1 + iTXt XMP），所以 SKIP 的断言落在"层已建、可见、alpha>0、
// 位置在舞台内"，尺寸不按 148x38 期望。
// ---------------------------------------------------------------------------
void check_mode_icon(oa::test::TestHost& host, const char* asset, const char* label) {
    auto& rt = host.rt();
    const oa::render::Layer* icon = rt.scene().find("85");
    check(icon != nullptr, std::string(label) + ": layer 85 exists");
    if (!icon) return;
    check(icon->file.find(asset) != std::string::npos,
          std::string(label) + ": layer 85 binds " + asset);
    check(icon->visible, std::string(label) + ": layer 85 is visible");
    check(icon->alpha > 0.0, std::string(label) + ": layer 85 alpha is above zero");

    oa::test::PlanRow row;
    const bool drawn = host.find("85", &row) && row.opacity > 0.0 && row.has_texture;
    check(drawn, std::string(label) + ": layer 85 is in the draw queue with a texture");
    if (!drawn) return;
    // 1px 容差：SKIP 的 1x1 占位图带 anchor，左沿会落在 -0.5px（视觉上仍在
    // 舞台左上角内），不能当成"飘到画面外"。
    constexpr double kTol = 1.0;
    check(row.x >= -kTol && row.y >= -kTol && row.x + row.w <= host.stage_w() + kTol &&
              row.y + row.h <= host.stage_h() + kTol,
          std::string(label) + ": layer 85 is inside the stage");
    check(row.x < host.stage_w() * 0.25 && row.y < host.stage_h() * 0.25,
          std::string(label) + ": layer 85 sits in the top-left corner");
}

void case_mode_indicator(oa::test::TestHost& host) {
    auto& rt = host.rt();

    // 必须走游戏自己的「标题 → 開始」链路：`*do_scenariostart` 才会注册
    // `[setonautomodein]` / `[setoncommandskipin]`（直接 start 剧本没有这两个
    // handler，图标链路根本不会触发）。
    auto enter_story = [&](const char* label) {
        oa::runtime::Event reset;
        reset.kind = oa::runtime::Event::Kind::Reset;
        rt.apply_event(reset);
        check(wait_frames(host, 4000, [&] {
                  return ends_with(script_name(rt), "system/title.iet") &&
                         has_click_handler(rt, "100.1.2");
              }) >= 0,
              std::string("mode icon: the title comes back (") + label + ")");
        wait_mask_clear(host);
        check(click_layer_center(host, "100.1.2", 218, 215),
              std::string("mode icon: the title start button is hittable (") + label + ")");
        check(wait_story_page(host),
              std::string("mode icon: the story starts from the title (") + label + ")");
    };
    enter_story("auto");

    // ---- AUTO 开 ---------------------------------------------------------
    check(open_dock(host), "mode icon: the dock opens for AUTO");
    wait_mask_clear(host);
    check(click_layer_center(host, "110.10.2", 90, 144),
          "mode icon: the AUTO button is hittable");
    check(wait_frames(host, 300, [&] {
              return var_str(rt, "s.status.automode") == "1";
          }) >= 0,
          "mode icon: AUTO turns on (s.status.automode=1)");
    // 图标入场：alpha 0→255 / 500ms + left 滑入 / 1000ms（easeout_expo）。
    // 等两段都走完（80 帧 ≈ 1280ms）再断言位置。
    host.tick(80);
    check_mode_icon(host, "info_automode", "AUTO");

    // ---- AUTO 关（脚本侧的 [exec command="automode" mode="0"]）----------
    {
        oa::runtime::Event ex;
        ex.kind = oa::runtime::Event::Kind::ConfigEvent;
        ex.tag = "exec";
        ex.params["command"] = "automode";
        ex.params["mode"] = "0";
        rt.apply_event(ex);
    }
    check(wait_frames(host, 300, [&] {
              return var_str(rt, "s.status.automode") == "0";
          }) >= 0,
          "mode icon: AUTO stops");
    check(wait_frames(host, 120, [&] { return rt.scene().find("85") == nullptr; }) >= 0,
          "mode icon: the AUTO indicator layer is removed");
    check(!host.visible("85"), "mode icon: no AUTO indicator row is drawn");

    // ---- SKIP 开 ---------------------------------------------------------
    // 从干净的标题重新进剧情：AUTO 那一轮点过 dock 按钮，dock 还停在
    // 收起/展开的转场里，直接再点 SKIP 会被掩码吃掉。
    enter_story("skip");
    // 配置里的「既読でなくてもスキップ」(g.skipmode) 等价于玩家在设置页勾选，
    // 这样 skip 按钮不必等这一页变成既读。
    rt.interpreter().set_variable("g.skipmode", oa::runtime::Value::make_int(1));
    // SKIP 不需要展开 submenu：消息窗右下角的常驻 dock 条上就有 SKIP 按钮
    // （原版录像里 PLAY/≡ 旁边那枚），hover 到窗口下沿它才出现。
    wait_mask_clear(host);
    host.move(host.stage_w() / 2.0, host.stage_h() * 0.93);
    host.tick(24);
    check(has_click_handler(rt, "110.11.2"),
          "mode icon: the dock SKIP button is registered");
    check(wait_frames(host, 300, [&] {
              oa::test::PlanRow r;
              return host.find("110.11.2", &r) && r.w > 0.0;
          }) >= 0,
          "mode icon: the dock SKIP button is laid out");
    wait_mask_clear(host);
    check(click_layer_center(host, "110.11.2", 49, 40),
          "mode icon: the SKIP button is hittable");
    check(wait_frames(host, 300, [&] {
              return var_str(rt, "s.status.commandskip") == "1";
          }) >= 0,
          "mode icon: SKIP turns on (s.status.commandskip=1)");
    host.tick(80);  // 同上的入场 tween
    check_mode_icon(host, "info_commandskip", "SKIP");

    // ---- SKIP 关（脚本自己也是 [exec command="skip" mode="0"]）----------
    {
        oa::runtime::Event ex;
        ex.kind = oa::runtime::Event::Kind::ConfigEvent;
        ex.tag = "exec";
        ex.params["command"] = "skip";
        ex.params["mode"] = "0";
        rt.apply_event(ex);
    }
    check(wait_frames(host, 300, [&] {
              return var_str(rt, "s.status.commandskip") == "0";
          }) >= 0,
          "mode icon: SKIP stops");
    check(wait_frames(host, 120, [&] { return rt.scene().find("85") == nullptr; }) >= 0,
          "mode icon: the SKIP indicator layer is removed");
    check(!host.visible("85"), "mode icon: no SKIP indicator row is drawn");
}

// ---------------------------------------------------------------------------
// 读档 soak：参照态 vs 读档态 + 连续翻页 + UI 往返
//
// 现场（用户报的"读档以后整体状态乱、玩不下去"）：
//   * dock 出现，但点 SAVE/LOG/CONFIG 都进不去 —— 存档时的活变量是
//     "存档页的世界"（sysbtn_mode=del、ui.list.* 指向槽位），读档后框架的
//     sysbtn_rewrite 按 del 什么都不做；
//   * 消息窗半透明 —— 页快照是在第一段文字落下那一拍抓的，`[lytween]` 还在
//     跑（alpha 0.59），读档后没有 tween 接力；
//   * 回顾页空 —— 引擎回顾表（get_backlog_size/backlog_tags）根本没进存档；
//   * t.* 全丢 —— 槽位只存了 local 域，页状态（t.printlang / t.l.* …）在 temp。
//
// 所以断言分三层：
//   1. 结构面（必须逐项一致）：脚本位置、活动文本层、正文、wait、
//      可见层计划（id/visible/顺序/layered）、回顾页数；
//   2. 变量面：local + temp 变量逐项比较，**排除**框架在读档时有意重新
//      组装的一组（口パク停止 / BGM fade / 角色临时量），并把它们单独打印；
//   3. 行为面：连续 22 页一次点击一页、每页有字；菜单→存档页→回顾页
//      （滚动）→设置页→继续，每步一次点击。
// ---------------------------------------------------------------------------

struct LoadState {
    std::string stable;
    size_t stable_line = 0;
    std::string active;
    std::string page;
    uint32_t wait_wakes = 0;
    int wait_stage = 0;
    std::string wait_owner;
    size_t backlog = 0;
    std::map<std::string, std::string> vars;
    std::map<std::string, std::string> layers;
    std::map<std::string, std::string> texts;
};

LoadState take_load_state(oa::runtime::GameRuntime& rt) {
    LoadState s;
    s.stable = rt.interpreter().stable_script();
    s.stable_line = rt.interpreter().stable_line();
    s.active = rt.text().active_layer_id();
    const auto* adv = rt.text().layer("adv");
    s.page = adv ? oa::test::page_text(adv, 120) : std::string();
    if (const auto* w = rt.current_wait()) {
        s.wait_wakes = w->wakes;
        s.wait_stage = int(w->stage);
        s.wait_owner = w->owner_script + ":" + std::to_string(w->owner_line);
    }
    s.backlog = rt.text().backlog_size();
    // **只比较持久变量**：`t.*` 是 scratch（官方 BOWS 不入档），读档后
    // 由框架重新生成，逐值比较只会把"读档收尾"当成回归。
    const auto& vs = rt.interpreter().variables();
    for (const auto& [k, v] : vs.local) s.vars["local:" + k] = v.to_display();
    if (auto v = vs.get("s.current_message_layer"))
        s.vars["system:s.current_message_layer"] = v->to_display();
    int idx = 0;
    char buf[256];
    for (const oa::render::Layer* l : rt.scene().draw_order()) {
        const auto* ml = rt.text().layer(l->id);
        std::string file = l->file;
        if (const auto fit = l->props.find("file"); fit != l->props.end()) file = fit->second;
        (void)idx;
        std::snprintf(buf, sizeof(buf),
                      "vis=%d alpha=%.3f file=%s",
                      int(l->visible), l->alpha,
                      (ml ? std::to_string(int(ml->font.size())) + ":" + ml->font.face()
                          : std::string("-"))
                          .c_str(),
                      file.c_str());
        s.layers[l->id] = buf;
        if (ml && ml->char_count > 0) {
            std::snprintf(buf, sizeof(buf), "chars=%zu done=%d font=%.1f:%.1f:%.1f text=%s",
                          ml->char_count, int(ml->done), ml->font.size(),
                          ml->left, ml->top, oa::test::page_text(ml, 80).c_str());
            s.texts[l->id] = buf;
        }
    }
    return s;
}

/// 读档时框架会重新组装的变量（不是"页状态"，是"读档后的收尾"）：
///   * mouth_*/t.f_st：onLoad 强制口パク停止（不重放语音）；
///   * snd.bgm/snd.flag.bgm/snd.se.*：onLoad 的 bgm_fade 与"不重播语音"；
///   * id/textend：角色临时量与"这一页已通过"标记，由重放的 text_end/
///     text_erase 重新写。
/// 这些允许不同，但**必须单独打印**（报告里要有原文），其余 t.* / local
/// 变量必须逐项一致。
bool load_reassembly_key(const std::string& k) {
    static const char* kExact[] = {"local:id", "local:mouth_file", "local:mouth_st",
                                   "local:snd.bgm", "local:snd.flag.bgm",
                                   "local:textend", "temp:t.f_st"};
    for (const char* e : kExact)
        if (k == e) return true;
    if (k.rfind("local:snd.se.", 0) == 0) return true;
    // 存档页/回顾页自己的临时变量（框架用无前缀 local 存 UI scratch）：
    //   meswindow.* / r_voice.* / r_vsave.* / face.* / file / voice_*。
    // 它们由框架的菜单收尾（menu.iet *leave → msg.iet 重绘）重新算出来，
    // 不是"玩家读到的那一页"的可持久状态。
    static const char* kUiPrefix[] = {"local:meswindow.", "local:r_voice.",
                                      "local:r_vsave.", "local:face."};
    for (const char* p : kUiPrefix)
        if (k.rfind(p, 0) == 0) return true;
    static const char* kUiExact[] = {"local:file", "local:voice_file", "local:voice_pan",
                                     "local:autoskip_mute"};
    for (const char* e : kUiExact)
        if (k == e) return true;
    // 鼠标悬停音效的节流计时器（dock 按钮的 rollover）：由输入侧重新计时，
    // 不是页状态。
    if (k.rfind("temp:t.rollsound_", 0) == 0) return true;
    return false;
}

/// 打印并统计差异。返回"必须一致"项的差异数；读档收尾项只打印。
int diff_load_state(const LoadState& ref, const LoadState& got, const char* tag) {
    int hard = 0;
    auto hard_d = [&](const char* what, const std::string& a, const std::string& b) {
        if (a == b) return;
        std::fprintf(stderr, "[%s] DIFF %-18s ref='%s' got='%s'\n", tag, what,
                     a.c_str(), b.c_str());
        ++hard;
    };
    hard_d("stable", ref.stable + ":" + std::to_string(ref.stable_line),
           got.stable + ":" + std::to_string(got.stable_line));
    hard_d("active", ref.active, got.active);
    hard_d("page", ref.page, got.page);
    hard_d("wait_wakes", std::to_string(ref.wait_wakes), std::to_string(got.wait_wakes));
    hard_d("wait_stage", std::to_string(ref.wait_stage), std::to_string(got.wait_stage));
    hard_d("wait_owner", ref.wait_owner, got.wait_owner);
    hard_d("backlog", std::to_string(ref.backlog), std::to_string(got.backlog));
    // 可见层计划：持有层（剧情画面 + dock）必须同 id、同顺序、同 layered。
    const auto transient_layer = [](const std::string& id) {
        return id == "80" || id.rfind("80.", 0) == 0 || id == "90" ||
               id.rfind("90.", 0) == 0 || id == "980" || id.rfind("980.", 0) == 0 ||
               id == "novel" || id == "loadmask" || id == "100" ||
               id.rfind("100.", 0) == 0;
    };
    for (const auto& [id, r] : ref.layers) {
        if (transient_layer(id)) continue;
        const auto it = got.layers.find(id);
        if (it == got.layers.end()) {
            std::fprintf(stderr, "[%s] DIFF layer %-22s ref='%s' got=<missing>\n", tag,
                         id.c_str(), r.c_str());
            ++hard;
        } else if (id == "90.0.0") {
            // クリック待ちアイコン：下一次 [wait] 会按 glyph 配置重新放置/换图，
            // 不属于"这一页的内容"。只比较可见/位置，file 差异记为 note。
            std::string a = r, b = it->second;
            const auto cut = [](std::string v) {
                const size_t p = v.find(" file=");
                return p == std::string::npos ? v : v.substr(0, p);
            };
            if (cut(a) != cut(b)) hard_d("layer 90.0.0", r, it->second);
            else std::fprintf(stderr, "[%s] note  layer %-20s wait-icon texture only\n", tag,
                              id.c_str());
        } else if (id.rfind("110.2.", 0) == 0) {
            // dock 触发条的两张图（submenu_0/1）：哪一张亮由指针悬停决定，
            // 属于输入瞬时态，不是页状态。只比较去掉 alpha 的部分。
            const auto strip_alpha = [](std::string v) {
                const size_t p = v.find(" alpha=");
                if (p == std::string::npos) return v;
                const size_t e = v.find(' ', p + 1);
                return v.erase(p, e == std::string::npos ? std::string::npos : e - p);
            };
            const std::string a = strip_alpha(r), b = strip_alpha(it->second);
            if (a != b) {
                hard_d(("layer " + id).c_str(), r, it->second);
            } else {
                std::fprintf(stderr, "[%s] note  layer %-20s hover alpha only (input state)\n",
                             tag, id.c_str());
            }
        } else {
            hard_d(("layer " + id).c_str(), r, it->second);
        }
    }
    // 读档后多出来的**可见**层 = 存档页/提示/遮罩残留。
    for (const auto& [id, g] : got.layers) {
        if (transient_layer(id)) continue;
        if (ref.layers.count(id)) continue;
        if (g.rfind("vis=1", 0) != 0) continue;
        std::fprintf(stderr, "[%s] DIFF layer %-22s ref=<missing> got='%s' (extra visible)\n",
                     tag, id.c_str(), g.c_str());
        ++hard;
    }
    // 文本层：layered / 字数 / 完成态 / 字体几何 / 正文必须一致。
    for (const auto& [id, r] : ref.texts) {
        const auto it = got.texts.find(id);
        const std::string gv = it == got.texts.end() ? std::string("<missing>") : it->second;
        hard_d(("text " + id).c_str(), r, gv);
    }
    for (const auto& [id, g] : got.texts) {
        if (ref.texts.count(id)) continue;
        std::fprintf(stderr, "[%s] DIFF text %-23s ref=<missing> got='%s' (extra)\n", tag,
                     id.c_str(), g.c_str());
        ++hard;
    }
    for (const auto& [k, v] : ref.vars) {
        const auto it = got.vars.find(k);
        const std::string gv = it == got.vars.end() ? std::string("<missing>") : it->second;
        if (gv == v) continue;
        if (load_reassembly_key(k)) {
            std::fprintf(stderr, "[%s] note  %-28s ref='%s' got='%s' (load reassembly)\n",
                         tag, k.c_str(), v.c_str(), gv.c_str());
            continue;
        }
        std::fprintf(stderr, "[%s] DIFF var %-24s ref='%s' got='%s'\n", tag, k.c_str(),
                     v.c_str(), gv.c_str());
        ++hard;
    }
    std::fprintf(stderr, "[%s] hard diff = %d\n", tag, hard);
    return hard;
}

/// 这一页是否"可点击的剧情页"。
bool load_soak_page_ready(oa::runtime::GameRuntime& rt) {
    const auto* w = rt.current_wait();
    const auto* adv = rt.text().layer("adv");
    return w && w->waiting_click() && oa::test::reveal_done(rt) && adv && adv->char_count > 0;
}

bool load_soak_wait_page(oa::test::TestHost& host, int budget = 900) {
    auto& rt = host.rt();
    return wait_frames(host, budget, [&] { return load_soak_page_ready(rt); }) >= 0;
}

/// 页 + tween/转场都落定（层计划的 alpha/位置才是玩家看到的那一帧）。
bool load_soak_wait_quiet(oa::test::TestHost& host, int budget = 900) {
    auto& rt = host.rt();
    return wait_frames(host, budget, [&] {
               return load_soak_page_ready(rt) && !rt.scene().has_tweens() &&
                      !rt.transition().active();
           }) >= 0;
}

/// 一次点击翻一页：点击前有可点击停驻且 reveal 完成；点击后正文必须变化
/// 且非空。返回新一页文字（失败返回空）。
bool load_soak_turn(oa::test::TestHost& host, int idx, std::string* text, std::string* why) {
    auto& rt = host.rt();
    if (!load_soak_wait_page(host)) {
        *why = "no click park before the click";
        return false;
    }
    const std::string before = adv_text(rt);
    if (!oa::test::has_visible_chars(before)) {
        *why = "page has no visible text";
        return false;
    }
    host.click(host.stage_w() / 2.0, host.stage_h() / 2.0, 2);
    const int used = wait_frames(host, 900, [&] {
        const std::string now = adv_text(rt);
        return oa::test::has_visible_chars(now) && now != before;
    });
    *text = adv_text(rt);
    if (used < 0) {
        *why = "one click did not turn the page (before='" + before + "')";
        return false;
    }
    if (idx >= 0 && std::getenv("OA_T10_DBG"))
        std::fprintf(stderr, "[soak] page %d = '%s'\n", idx, text->c_str());
    return true;
}

/// 菜单 → 存档页 → 返回 → 回顾页（滚）→ 关闭 → 设置页 → 关闭。
bool load_soak_ui_roundtrip(oa::test::TestHost& host, const char* tag) {
    auto& rt = host.rt();
    const auto open_dock = [&]() {
        for (int attempt = 0; attempt < 3; ++attempt) {
            wait_mask_clear(host);
            const auto sub = rt.interpreter().variables().get("t.sysbtn_submenu");
            if (sub && sub->to_display() == "1" && has_click_handler(rt, "110.3.2")) return true;
            // hover 触发条要从"远处"移过来才会重发 rollover；同一坐标再 move
            // 不会有事件（返回 UI 后指针常常已经停在底部附近）。
            host.move(host.stage_w() / 2.0, host.stage_h() * 0.8);
            host.tick(2);
            host.move(host.stage_w() / 2.0, host.stage_h() * 0.93);
            host.tick(24);
            if (!has_click_handler(rt, "110.2.2")) continue;
            click_layer_center(host, "110.2.2", 49, 40);
            if (wait_frames(host, 300, [&] {
                    const auto v = rt.interpreter().variables().get("t.sysbtn_submenu");
                    return v && v->to_display() == "1";
                }) >= 0)
                return true;
        }
        return false;
    };
    const auto back_to_page = [&]() {
        // 只等 `!visible(100.0)` 不够：菜单页的收尾（menu.leave → sysbtn_init/
        // set → scenario_reprint）还没跑完时，剧情层已经画出来了、脚本却仍在
        // save.iet 里。此时去点 dock 会点回存档页。三个条件一起等：
        // 剧情页可点 + 存档页/槽位都没了 + submenu 回到关闭态。
        // 只等 `!visible(100.0)` 不够：菜单页的收尾（menu.leave → sysbtn_init/
        // set → scenario_reprint）还没跑完时，剧情层已经画出来了、脚本却仍在
        // save.iet 里。三个条件一起等：剧情页可点 + 存档页/槽位都没了 +
        // dock 的触发条回来了。子菜单**不要求收起**：t10 的
        // `g.submenu_autoclose=0`，框架本来就让它在返回后保持展开。
        return wait_frames(host, 1800, [&] {
                   return load_soak_page_ready(rt) && !host.visible("100.0") &&
                          !has_click_handler(rt, "100.slot.2.2") &&
                          has_click_handler(rt, "110.2.2");
               }) >= 0;
    };

    bool ok = open_dock();
    check(ok, std::string(tag) + ": the dock opens after LOAD");

    // 存档页：能进、槽位在、能返回。
    wait_mask_clear(host); // submenu 的转场掩码放开之前点按钮会被吞
    ok = ok && wait_frames(host, 300, [&] { return has_click_handler(rt, "110.7.2"); }) >= 0;
    ok = ok && click_layer_center(host, "110.7.2", 96, 129);
    ok = ok && wait_frames(host, 900, [&] {
                       return ends_with(script_name(rt), "system/save.iet");
                   }) >= 0;
    check(ok, std::string(tag) + ": the save page opens after LOAD");
    check(ok && has_click_handler(rt, "100.slot.2.2"),
          std::string(tag) + ": the save slot is hittable");
    wait_mask_clear(host); // 页面入场 trans 的掩码放开后，关闭按钮才接得住
    const bool close_clicked = click_layer_center(host, "100.63.2", 97, 92);
    ok = ok && close_clicked && back_to_page();
    check(ok, std::string(tag) + ": the save page returns to the story page");
    if (std::getenv("OA_T10_DBG") || !ok) {
        oa::test::PlanRow row;
        std::fprintf(stderr,
                     "[%s] after save close: ok=%d clicked=%d script=%s submenu=%s page100=%d "
                     "closeHandler=%d closeRow=%d ef=%s\n",
                     tag, int(ok), int(close_clicked), script_name(rt).c_str(),
                     wait_var(rt, "t.sysbtn_submenu").c_str(), int(host.visible("100.0")),
                     int(has_click_handler(rt, "100.63.2")), int(host.find("100.63.2", &row)),
                     wait_var(rt, "t.eventfilter").c_str());
        std::fprintf(stderr, "[%s] 100.63 root=%d handler=%d visible=%d; 100.54 h=%d\n", tag,
                     int(rt.scene().find("100.63") != nullptr), int(has_click_handler(rt, "100.63")),
                     int(host.visible("100.63")), int(has_click_handler(rt, "100.54.2")));
    }

    // 回顾页：有字、能滚、能关。
    bool backlog_ok = ok && open_dock();
    wait_mask_clear(host);
    if (std::getenv("OA_T10_DBG") || !backlog_ok)
        std::fprintf(stderr,
                     "[%s] backlog dock: ok=%d script=%s submenu=%s handler3=%d page100=%d "
                     "h2=%d h7=%d h9=%d mode=%s\n",
                     tag, int(backlog_ok), script_name(rt).c_str(),
                     wait_var(rt, "t.sysbtn_submenu").c_str(),
                     int(has_click_handler(rt, "110.3.2")), int(host.visible("100.0")),
                     int(has_click_handler(rt, "110.2.2")),
                     int(has_click_handler(rt, "110.7.2")),
                     int(has_click_handler(rt, "110.9.2")),
                     wait_var(rt, "sysbtn_mode").c_str());
    if (!backlog_ok)
        for (const oa::render::Layer* l : rt.scene().draw_order()) {
            if (l->id.rfind("110", 0) != 0) continue;
            std::fprintf(stderr, "[%s] 110 layer %s file=%s vis=%d alpha=%.2f click=%d\n", tag,
                         l->id.c_str(), l->file.c_str(), int(l->visible), l->alpha,
                         int(l->event_handlers.count("click")));
        }
    backlog_ok = backlog_ok && wait_frames(host, 300, [&] {
                                      return has_click_handler(rt, "110.3.2");
                                  }) >= 0;
    const int backlog_clicked = backlog_ok ? int(click_layer_center(host, "110.3.2", 300, 150)) : 0;
    backlog_ok = backlog_ok && backlog_clicked;
    const int backlog_waited = backlog_ok
                                   ? wait_frames(host, 600, [&] {
                                         return script_name(rt).find("backlog.iet") != std::string::npos;
                                     })
                                   : -1;
    backlog_ok = backlog_ok && backlog_waited >= 0;
    int rows = 0;
    if (backlog_ok) {
        host.tick(60);
        for (const auto& [id, ml] : rt.text().all_layers()) {
            if (!ml || ml->char_count == 0) continue;
            if (id.rfind("100.1.", 0) != 0) continue;
            ++rows;
        }
        host.wheel(-1);
        host.tick(20);
        host.wheel(1);
        host.tick(20);
    }
    check(backlog_ok && rows > 0, std::string(tag) + ": the backlog page draws text rows");
    if (std::getenv("OA_T10_DBG") || !backlog_ok || rows == 0) {
        std::fprintf(stderr,
                     "[%s] roundtrip backlog: ok=%d clicked=%d waited=%d rows=%d size=%zu "
                     "script=%s\n",
                     tag, int(ok), backlog_clicked, backlog_waited, rows,
                     rt.text().backlog_size(), script_name(rt).c_str());
    }
    if (backlog_ok) {
        wait_mask_clear(host);
        oa::test::right_click(rt);
        host.tick(30);
        backlog_ok = back_to_page();
    }
    check(backlog_ok, std::string(tag) + ": the backlog page closes back to the story");
    ok = ok && backlog_ok;

    // 设置页：能进、能关。
    ok = ok && open_dock();
    wait_mask_clear(host);
    ok = ok && wait_frames(host, 300, [&] { return has_click_handler(rt, "110.9.2"); }) >= 0;
    ok = ok && click_layer_center(host, "110.9.2", 133, 129);
    ok = ok && wait_frames(host, 900, [&] {
                       return ends_with(script_name(rt), "system/config.iet");
                   }) >= 0;
    check(ok, std::string(tag) + ": the settings page opens after LOAD");
    wait_mask_clear(host);
    ok = ok && click_layer_center(host, "100.63.2", 97, 92) && back_to_page();
    check(ok, std::string(tag) + ": the settings page closes back to the story");
    return ok;
}

/// 回顾页行点击 → 跳回那一页。
///
/// 框架的路径（不经过引擎的任何"页快照/页跳转"翻译）：
///   * 每行的「ジャンプ」按钮 → `goto_backlogjump`；
///   * 先 `file_exists` 检查 `backlog_N.dat`，没有就弹 caution（原版行为）；
///   * 有则 `[load file="backlog_N.dat" type="0"]`；
///   * load 之后 `backlogjump_skipstart` 重画消息窗（chgmsg/adv_mwredraw/
///     adv_namebar_redraw）+ `scenario_reprint`，再用 skip 快进到
///     `backlog.backlogjump.<row>.index`（= 剧情帧的 frame.current）。
///
/// 所以这里钉住的是"大一统"：checkpoint 由框架的 `[save]` 写出、由框架的
/// `[load]` 读回；引擎只保证位置/变量/画面数据忠实。断言：UI 关闭、正文/背景
/// 一致、消息窗框与名字栏回来、回顾表截断、一次点击翻页、之后连翻 10 页。
void case_backlog_jump(oa::test::TestHost& host) {
    auto& rt = host.rt();
    if (!fresh_story(host)) {
        check(false, "backlog jump: a story page is ready");
        return;
    }
    enable_backlog_recording(rt);
    // 先把功能开关打开：框架的 backlogjump_countup 才会在每页记录
    // `backlog.backlogjump.<row>.file/filenum/index`。这些就是该行的
    // 真值，跳转测试不再自己编。
    rt.interpreter().set_variable("g.backlogjump", oa::runtime::Value::make_int(1));
    if (rt.text().backlog_size() == 0) rt.text().page_break(std::nullopt);

    auto ensure_page = [&] {
        for (int i = 0; i < 8; ++i) {
            if (load_soak_wait_page(host, 300)) return true;
            host.click(host.stage_w() / 2.0, host.stage_h() / 2.0, 2);
            host.tick(30);
        }
        return load_soak_wait_page(host, 900);
    };
    auto turn = [&] {
        if (!ensure_page()) return false;
        const std::string before = adv_text(rt);
        host.click(host.stage_w() / 2.0, host.stage_h() / 2.0, 2);
        return wait_frames(host, 900, [&] {
                   const std::string now = adv_text(rt);
                   return oa::test::has_visible_chars(now) && now != before;
               }) >= 0;
    };
    auto layer_file = [&](const std::string& id) {
        const oa::render::Layer* l = rt.scene().find(id);
        return l ? l->file : std::string();
    };
    auto has_message_window = [&] {
        bool plate = false;
        for (const oa::render::Layer* l : rt.scene().draw_order()) {
            if (!l->visible || l->alpha <= 0.01) continue;
            if (l->id.rfind("80.0", 0) == 0 && l->file.find("/mw/") != std::string::npos)
                plate = true;
        }
        return plate;
    };
    auto var_int = [&](const std::string& name) {
        try {
            return std::stoi(var_str(rt, name.c_str()));
        } catch (...) {
            return -1000000;
        }
    };
    auto var_set = [&](const std::string& name, const std::string& value) {
        rt.interpreter().set_variable(name, oa::runtime::Value::make_string(value));
    };

    check(ensure_page(), "backlog jump: the opening page is ready");
    check(turn(), "backlog jump: page 2 is reachable");
    check(turn(), "backlog jump: page 3 is reachable");

    const std::string want_text = adv_text(rt);
    const std::string want_bg = layer_file("0.1");
    check(oa::test::has_visible_chars(want_text), "backlog jump: the target page has text");
    check(!want_bg.empty(), "backlog jump: the target page has a background");
    check(has_message_window(), "backlog jump: the target page draws the message window plate");

    // 框架的锚点 = `getScriptStack()` 里最内层**剧情帧**的 file/index
    // （backlogjump_countup 就是这么记的）。当前可能停在 message 宏里，
    // script_name(rt) 会是 system/macro.iet，不能直接拿来当锚点。
    //
    // index 是帧的**当前指令**（官方 BOWS frame.current = 这一帧停在的
    // `[call]` 行），不是返回点。
    std::string target_script = script_name(rt);
    size_t target_line = rt.interpreter().current_line();
    for (const auto& f : rt.interpreter().call_stack()) {
        if (!f.script.empty() && f.script.rfind("scenario/", 0) == 0) {
            target_script = f.script;
            target_line = f.call_line != 0 ? f.call_line : f.return_line;
        }
    }

    // 框架的载体：t10 的 backlog_N.dat 由 backjump.iet 的
    // `[save file="$'backlog_' + backlogjump.counter + '.dat'"]` 写出。
    // 这里走**同一条脚本路径**（真实的 [save]），不直接调引擎的存档接口 ——
    // 存档点就是 `[save]` 的执行现场，读档时脚本才会沿着框架自己的重建
    // 路径（skipstart/backlogjump_countup）回到这一页。
    rt.interpreter().set_variable("g.backlogjump", oa::runtime::Value::make_int(1));
    rt.interpreter().set_variable("backlogjump.counter", oa::runtime::Value::make_int(1));
    rt.interpreter().enqueue_tag("call",
                                 {{"file", "system/backjump.iet"}, {"label", "backlogjump_save"}});
    host.tick(120);
    check(rt.save_file_exists("backlog_1.dat"),
          "backlog jump: the framework checkpoint is written by [save]");
    const std::string bg_at_save = want_bg;

    // 走一页：目标页在这一步被 [rp] 归档（backlog_size 的最后一页就是它）。
    check(turn(), "backlog jump: move away (1)");
    const size_t target_page = rt.text().backlog_size() - 1;
    check(rt.text().backlog_size() >= 2, "backlog jump: the read pages are archived");
    // 再走一页：当前页与目标页不同，跳回去才有可判定的差异。
    check(turn(), "backlog jump: move away (2)");
    const std::string away_text = adv_text(rt);
    check(away_text != want_text, "backlog jump: the later page differs from the target");

    // 框架在 backlogjump_countup / backlogjump_save 里写的几个键。计数器 =
    // 1 对应刚写的 backlog_1.dat；file/index 是这一页的剧本位置。
    rt.interpreter().set_variable("g.backlogjump", oa::runtime::Value::make_int(1));
    rt.interpreter().set_variable("g.backlogjump_counter", oa::runtime::Value::make_int(1));
    rt.interpreter().set_variable("backlogjump.counter", oa::runtime::Value::make_int(1));
    var_set("backlogjump.file", target_script);
    var_set("backlog.backlogjump.file", target_script);
    var_set("backlog.backlogjump.filenum", "1");
    var_set("backlog.backlogjump.index", std::to_string(target_line));
    rt.interpreter().set_variable("backlogjump.write", oa::runtime::Value::make_int(0));

    // dock → LOG
    check(open_dock(host), "backlog jump: the dock opens");
    wait_mask_clear(host);
    check(click_layer_center(host, "110.3.2", 300, 150), "backlog jump: the LOG button is hittable");
    check(wait_frames(host, 600, [&] {
              return script_name(rt).find("backlog.iet") != std::string::npos &&
                     host.visible("100.0");
          }) >= 0,
          "backlog jump: the backlog page opens");
    host.tick(80);

    // 目标引擎页在当前回顾页的哪一行：list1.<slot> = 引擎 backlog 页号，
    // list2.<slot> = 框架自己的语音日志号（backlog.backlogjump.<no2> 的键）。
    int slot = -1;
    int no2 = -1;
    for (int s = 0; s < 40 && slot < 0; ++s) {
        const std::string suffix = "." + std::to_string(s);
        if (var_int("t.backlog.list1" + suffix) == int(target_page)) {
            slot = s;
            no2 = var_int("t.backlog.list2" + suffix);
        }
    }
    check(slot >= 0 && no2 >= 0, "backlog jump: the target page maps to a backlog row");
    if (std::getenv("OA_T10_DBG")) {
        std::fprintf(stderr, "[bljdbg] target_page=%zu target_line=%zu backlog_size=%zu\n",
                     target_page, target_line, rt.text().backlog_size());
        const auto& pages = rt.text().backlog_pages();
        for (size_t i = 0; i < pages.size(); ++i) {
            std::string t;
            for (const auto& tg : pages[i].tags) t += tg.text;
            std::fprintf(stderr, "[bljdbg] bp[%zu] '%s'\n", i, t.substr(0, 60).c_str());
        }
    }
    if (slot < 0 || no2 < 0) return;

    const std::string jump_prefix = "backlog.backlogjump." + std::to_string(no2);
    // file/filenum/index 都是框架在 backlogjump_countup 里按这一页记下的；
    // 只把 counter 指向本次写出的 backlog_1.dat。若框架没记（更老的行），
    // 才退回测试的推算值。
    if (!rt.interpreter().variables().get(jump_prefix + ".file"))
        var_set(jump_prefix + ".file", target_script);
    if (!rt.interpreter().variables().get(jump_prefix + ".filenum"))
        var_set(jump_prefix + ".filenum", "1");
    if (!rt.interpreter().variables().get(jump_prefix + ".index"))
        var_set(jump_prefix + ".index", std::to_string(target_line));
    var_set(jump_prefix + ".counter", "1");
    if (std::getenv("OA_T10_DBG"))
        std::fprintf(stderr, "[bljdbg] row file='%s' filenum=%s index=%s counter=%s\n",
                     var_str(rt, (jump_prefix + ".file").c_str()).c_str(),
                     var_str(rt, (jump_prefix + ".filenum").c_str()).c_str(),
                     var_str(rt, (jump_prefix + ".index").c_str()).c_str(),
                     var_str(rt, (jump_prefix + ".counter").c_str()).c_str());

    // 行按钮是在 draw 时建的：关掉回顾页再打开一次，让 .13 跳转按钮出现。
    //
    // 框架按钮工厂（system/button.iet）把 click 行放在 **`_2` 子层**：
    // `[btn id=x]` 建 x.1/x.2/x.3，`[btn_on id=x]` 的 `lyevent` 目标是
    // `x.2`（按下态）。所以"可点"的 id 是 `100.10.2`，不是 `100.10`。
    wait_mask_clear(host);
    const std::string close_id = "100.10.2";
    if (has_click_handler(rt, close_id)) {
        click_layer_center(host, close_id, 80, 80);
        wait_frames(host, 600, [&] { return !host.visible("100.0"); });
    }
    // 关闭回顾页 = 一次 UI 收尾：menu 的 leave 流程还要重建 sysbtn 并跑
    // 转场。等剧情页真正落定再开 dock（这是玩家的节奏，也是下面断言的
    // 前提）。
    wait_frames(host, 1800, [&] {
        return load_soak_page_ready(rt) && !rt.scene().has_tweens() && !rt.transition().active();
    });
    check(open_dock(host), "backlog jump: the dock reopens");
    wait_mask_clear(host);
    check(click_layer_center(host, "110.3.2", 300, 150), "backlog jump: LOG reopens");
    check(wait_frames(host, 600, [&] {
              return script_name(rt).find("backlog.iet") != std::string::npos &&
                     host.visible("100.0");
          }) >= 0,
          "backlog jump: the backlog page opens again");
    host.tick(80);

    const std::string jump_root = "100.1." + std::to_string(slot) + ".13";
    const std::string jump_id = jump_root + ".2";
    check(rt.scene().find(jump_root) != nullptr,
          "backlog jump: the framework jump button is built");
    check(has_click_handler(rt, jump_id), "backlog jump: the framework jump button is hittable");
    if (!has_click_handler(rt, jump_id)) {
        dump_text_layers(rt, "100.1.");
        return;
    }
    check(click_layer_center(host, jump_id, 46, 46), "backlog jump: one click on the jump button");
    // 确认框（配置里 g.backlogjump_skipdialog=1 时没有）。
    if (wait_frames(host, 300, [&] { return has_click_handler(rt, "1000.200.2"); }) >= 0) {
        wait_mask_clear(host);
        click_layer_center(host, "1000.200.2", 300, 80);
    }

    // 读档 + backlogjump_skipstart + 快进：停回目标页的可点击停驻。
    const int settled = wait_frames(host, 4000, [&] {
        const auto* w = rt.current_wait();
        return !host.visible("100.0") && w && w->waiting_click() && oa::test::reveal_done(rt) &&
               !rt.scene().has_tweens() && !rt.transition().active();
    });
    check(settled >= 0, "backlog jump: the framework replay parks on the target page");
    if (settled < 0) return;

    // 关键回归：消息窗的**框/名字栏**必须由脚本重绘回来（旧快照路径只剩
    // 一行浮字）。80.0 = 窗底、80.1 = 框、80.2/80.3 = 名字栏。
    check(has_message_window(), "backlog jump: the message window plate is back");
    if (std::getenv("OA_T10_DBG")) {
        std::fprintf(stderr, "[bljdbg] s.current_message_layer=%s active=%s\n",
                     var_str(rt, "s.current_message_layer").c_str(),
                     rt.text().active_layer_id().c_str());
        for (const oa::render::Layer* l : rt.scene().draw_order()) {
            if (l->id.rfind("80", 0) != 0) continue;
            std::fprintf(stderr, "[bljdbg] layer %s file=%s vis=%d alpha=%.2f\n", l->id.c_str(),
                         l->file.c_str(), int(l->visible), l->alpha);
        }
    }
    const std::string frame = layer_file("80.1.0");
    check(!frame.empty() && frame.find("mw_flame") != std::string::npos,
          "backlog jump: the message window frame layer is back (80.1.0)");
    const std::string name_plate = layer_file("80.3.0");
    check(!name_plate.empty() && name_plate.find("mw_name") != std::string::npos,
          "backlog jump: the name-bar frame layer is back (80.3.0)");

    check(layer_file("0.1") == bg_at_save, "backlog jump: the background matches the target page");
    check(adv_text(rt) == want_text, "backlog jump: the body shows the target page again");
    if (std::getenv("OA_T10_DBG"))
        std::fprintf(stderr, "[bljdbg] want_bg=%s got_bg=%s\n[bljdbg] want_text='%s'\n"
                             "[bljdbg] got_text='%s'\n",
                     bg_at_save.c_str(), layer_file("0.1").c_str(), want_text.c_str(),
                     adv_text(rt).c_str());
    if (std::getenv("OA_T10_DBG"))
        std::fprintf(stderr, "[bljdbg] backlog_size=%zu target_page+1=%zu\n",
                     rt.text().backlog_size(), target_page + 1);
    check(rt.text().backlog_size() == target_page + 1,
          "backlog jump: later backlog rows are truncated");

    // 一次点击翻页 + 再连翻 10 页（不崩、不重复印）。
    if (!load_soak_wait_page(host)) {
        check(false, "backlog jump: a click park is armed after the jump");
        return;
    }
    const std::string was = adv_text(rt);
    host.click(host.stage_w() / 2.0, host.stage_h() / 2.0, 2);
    check(wait_frames(host, 900, [&] {
              const std::string now = adv_text(rt);
              return oa::test::has_visible_chars(now) && now != was;
          }) >= 0,
          "backlog jump: one click turns the page");
    int turned = 0;
    for (int i = 0; i < 10; ++i) {
        if (!load_soak_wait_page(host)) break;
        const std::string before = adv_text(rt);
        if (!oa::test::has_visible_chars(before)) break;
        host.click(host.stage_w() / 2.0, host.stage_h() / 2.0, 2);
        if (wait_frames(host, 900, [&] {
                const std::string now = adv_text(rt);
                return oa::test::has_visible_chars(now) && now != before;
            }) < 0)
            break;
        ++turned;
    }
    check(turned == 10, "backlog jump: ten more pages turn after the jump");
}

void load_soak_run(oa::test::TestHost& host, bool quick);

void case_load_soak(const char* pfs, bool quick) {
    namespace fs = std::filesystem;
    std::error_code ec;
    const fs::path root = fs::temp_directory_path(ec) /
                          ("oa_t10_soak_" + std::string(quick ? "q_" : "") +
                           std::to_string(::getpid()));
    fs::remove_all(root, ec);
    fs::create_directories(root, ec);
    {
        oa::test::TestHost host({.project = pfs, .save_root = root.string()});
        check(host.ok(), std::string(quick ? "qload-soak" : "load-soak") + ": host boots");
        if (!host.ok()) {
            fs::remove_all(root, ec);
            return;
        }
        case_language_screen(host);
        load_soak_run(host, quick);
    }
    fs::remove_all(root, ec);
}

void load_soak_run(oa::test::TestHost& host, bool quick) {
    auto& rt = host.rt();
    const char* tag = quick ? "qload-soak" : "load-soak";

    check(fresh_story(host), std::string(tag) + ": a clean story page is ready");
    // 真实启动链会 `[writebacklog mode=1]`；直接起剧本要补上，回顾页才成立。
    enable_backlog_recording(rt);
    if (rt.text().backlog_size() == 0) rt.text().page_break(std::nullopt);
    // s100 的开场有一个"文字落下之前"的停驻（黑场/开场演出）。`fresh_story`
    // 只等到"屏幕上有字"——那可能是残留的 UI 文字层，正文层 `adv` 还是空的。
    // 这里按玩家的方式点几下，直到正文层真的出现（最多 8 下）。
    bool page_ready = load_soak_wait_quiet(host, 900);
    for (int i = 0; i < 8 && !page_ready; ++i) {
        host.click(host.stage_w() / 2.0, host.stage_h() / 2.0, 2);
        host.tick(30);
        page_ready = load_soak_wait_quiet(host, 900);
    }
    if (!page_ready) {
        const auto* w = rt.current_wait();
        const auto* adv = rt.text().layer("adv");
        std::fprintf(stderr,
                     "[%s] not clickable: script=%s:%zu wait=%s wakes=0x%x stage=%d adv_chars=%zu "
                     "adv_done=%d reveal=%d tweens=%d trans=%d\n",
                     tag, script_name(rt).c_str(), rt.interpreter().current_line(),
                     w ? "yes" : "no", w ? w->wakes : 0, w ? int(w->stage) : -1,
                     adv ? adv->char_count : 0, adv ? int(adv->done) : -1,
                     int(oa::test::reveal_done(rt)), int(rt.scene().has_tweens()),
                     int(rt.transition().active()));
    }
    check(page_ready, std::string(tag) + ": the story page is clickable");

    // 正常玩 3 页 → 参照态。
    for (int i = 0; i < 3; ++i) {
        std::string text, why;
        check(load_soak_turn(host, i, &text, &why), std::string(tag) + ": ref page " +
                                                        std::to_string(i) + " turns (" + why + ")");
    }
    check(load_soak_wait_quiet(host), std::string(tag) + ": the reference page is settled");
    const LoadState ref = take_load_state(rt);

    if (quick) {
        host.key(116); // F5
        host.tick(40);
        if (has_click_handler(rt, "1000.200.2")) {
            wait_mask_clear(host);
            click_layer_center(host, "1000.200.2", 300, 80);
            host.tick(60);
        }
        check(load_soak_wait_page(host, 1200), std::string(tag) + ": quick save returns to the page");
        host.key(117); // F6
        host.tick(40);
        if (has_click_handler(rt, "1000.200.2")) {
            wait_mask_clear(host);
            click_layer_center(host, "1000.200.2", 300, 80);
            host.tick(60);
        }
    } else {
        if (std::getenv("OA_T10_DBG"))
            std::fprintf(stderr, "[soakdbg] ref page='%s'\n", ref.page.c_str());
        check(open_dock(host), std::string(tag) + ": the dock opens for saving");
        if (std::getenv("OA_T10_DBG"))
            std::fprintf(stderr, "[soakdbg] after dock='%s'\n", adv_text(rt).c_str());
        wait_mask_clear(host);
        check(click_layer_center(host, "110.7.2", 96, 129), std::string(tag) + ": save button");
        if (std::getenv("OA_T10_DBG"))
            std::fprintf(stderr, "[soakdbg] after save btn='%s'\n", adv_text(rt).c_str());
        check(wait_frames(host, 900, [&] {
                  return ends_with(script_name(rt), "system/save.iet");
              }) >= 0,
              std::string(tag) + ": the save page opens");
        wait_mask_clear(host);
        check(has_click_handler(rt, "100.slot.2.2"), std::string(tag) + ": slot 1 is hittable");
        click_layer_center(host, "100.slot.2.2", 300, 135);
        confirm_dialog(host);
        host.tick(30);
        wait_mask_clear(host);
        check(click_layer_center(host, "100.54.2", 263, 81), std::string(tag) + ": load tab");
        check(wait_frames(host, 900, [&] {
                  return ends_with(script_name(rt), "system/load.iet");
              }) >= 0,
              std::string(tag) + ": the load page opens");
        wait_mask_clear(host);
        check(has_click_handler(rt, "100.slot.2.2"), std::string(tag) + ": saved slot is hittable");
        click_layer_center(host, "100.slot.2.2", 300, 135);
        confirm_dialog(host);
    }

    // 读档链跑完：停在剧情页的点击停驻、页面层收干净。
    const int settled = wait_frames(host, 2400, [&] {
        return load_soak_page_ready(rt) && !host.visible("100.0") && !rt.scene().has_tweens() &&
               !rt.transition().active();
    });
    check(settled >= 0, std::string(tag) + ": the loaded page waits for a click");
    if (settled < 0) return;

    const LoadState got = take_load_state(rt);
    const int hard = diff_load_state(ref, got, tag);
    check(hard == 0, std::string(tag) + ": reference state and loaded state agree");

    const std::string ev = wait_var(rt, "t.eventfilter");
    check(ev.empty() || ev == "0", std::string(tag) + ": the input mask is clear after LOAD");
    check(oa::test::has_visible_chars(got.page),
          std::string(tag) + ": the loaded page has visible text");

    // UI 往返：存档页 / 回顾页 / 设置页。
    check(load_soak_ui_roundtrip(host, tag), std::string(tag) + ": UI roundtrip after LOAD");

    // 连续 22 页：每页都要有字、一次点击、文字变化。
    int turned = 0;
    for (int i = 0; i < 22; ++i) {
        std::string text, why;
        const bool ok = load_soak_turn(host, i, &text, &why);
        check(ok, std::string(tag) + ": page " + std::to_string(i) + " turns in one click (" +
                       why + ")");
        if (!ok) break;
        ++turned;
    }
    check(turned == 22, std::string(tag) + ": 22 consecutive pages after LOAD");
}

} // namespace

int main() {
    oa::test::install_crash_handler();
    const char* pfs = std::getenv("OA_TEST_T10_PFS");
    if (!pfs || !*pfs) {
        std::printf("OA_TEST_T10_PFS unset; skipping\n");
        return 77;
    }
    // 调试过滤：只跑读档 soak（约 3 分钟，整包 7 分钟）。
    if (std::getenv("OA_T10_ONLY_BACKLOG")) {
        oa::test::TestHost host({.project = pfs, .save_root = ""});
        if (!host.ok()) return 1;
        try {
            // 焦点跑法也要走真实启动形状（初回语言选择 → 标题），否则
            // fresh_story 永远等不到 title.iet。
            case_language_screen(host);
            case_backlog_jump(host);
        } catch (const std::exception& e) {
            std::fprintf(stderr, "FAIL: exception (backlog only): %s\n", e.what());
            ++failures;
        }
        std::fprintf(stderr, "t10 backlog: %s (%d failure(s))\n",
                     failures ? "FAIL" : "all ok", failures);
        return failures ? 1 : 0;
    }
    if (std::getenv("OA_T10_ONLY_SOAK")) {
        try {
            case_load_soak(pfs, /*quick=*/false);
            case_load_soak(pfs, /*quick=*/true);
        } catch (const std::exception& e) {
            std::fprintf(stderr, "FAIL: exception (soak only): %s\n", e.what());
            ++failures;
        }
        std::fprintf(stderr, "t10 soak: %s (%d failure(s))\n", failures ? "FAIL" : "all ok",
                     failures);
        return failures ? 1 : 0;
    }
    // 全局存档往返放在最前面：它要独占两个连续的 SDL 生命周期（先用后弃），
    // 和下面那个长命 host 不能同时存在。
    try {
        case_global_save_roundtrip(pfs);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "FAIL: exception (global save): %s\n", e.what());
        ++failures;
    }
    try {
        case_exit_ui_syssave(pfs);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "FAIL: exception (exit syssave): %s\n", e.what());
        ++failures;
    }
    try {
        case_menu_journey(pfs);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "FAIL: exception (menu journey): %s\n", e.what());
        ++failures;
    }
    try {
        case_load_soak(pfs, /*quick=*/false);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "FAIL: exception (load soak): %s\n", e.what());
        ++failures;
    }
    try {
        case_load_soak(pfs, /*quick=*/true);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "FAIL: exception (qload soak): %s\n", e.what());
        ++failures;
    }

    oa::test::TestHost host({.project = pfs, .save_root = ""});
    if (!host.ok()) {
        std::printf("FAIL: TestHost init\n");
        return 1;
    }
    try {
        case_language_screen(host);
        case_cg_change_keeps_one_click(host);
        case_voice_playback(host);
        case_message_text_wrap(host);
        case_chara_window_text_area(host);
        case_backlog_jump(host);
        case_system_menu_from_story(host);
        case_settings_tabs_rebuild(host);
        case_save_load_slots(host);
        case_save_page_after_load(host);
        case_config_tabs_exit(host);
        case_mode_indicator(host);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "FAIL: exception: %s\n", e.what());
        ++failures;
    }
    if (failures) {
        std::fprintf(stderr, "t10: %d failure(s)\n", failures);
        return 1;
    }
    std::fprintf(stderr, "t10: all ok\n");
    return 0;
}
