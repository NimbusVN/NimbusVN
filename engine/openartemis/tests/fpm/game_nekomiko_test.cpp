// NekoMiko（qureate） 游程 —— 一个游戏一个文件。
//
// OA_TEST_NEKOMIKO_PFS 指向它的 root.pfs；不设则 exit 77（跳过）。
//
// 这个文件的启动形状就是**这个游戏自己的**：标题 → 语言选择 → 开场 → 正文。
// 别的游戏（没有语言页、没有开场视频）在自己的文件里写自己的形状 —— 所以
// 没有"通用游程"。要加流程，就往下加一个 case_*，共用这里的 boot()。
//
// 判据只看**渲染队列**（哪些 layer 在画、文字项在不在），要留证时设
// OA_JOURNEY_SHOT=<dir> 落 PNG。
#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>
#include <map>
#include <process.h>
#include <unistd.h>
#include <chrono>
#include <set>
#include <cstring>
#include <cmath>
#include <array>
#include <algorithm>
#include <functional>

extern "C" {
#include "lua.h"
}

#include "crash_backtrace.h"
#include "test_host.h"
#include "core/fs/physfs_fs.h"
#include "core/runtime/runtime_game.h"
#include "core/runtime/runtime_save.h"
#include "core/media/video.h"
#include "core/emote/emote_file.h"
#include "core/media/image.h"
#include "core/emote/emote_player.h"

namespace {

int failures = 0;

void check(bool cond, const std::string& what) {
    std::fprintf(stderr, "%s: %s\n", cond ? "ok  " : "FAIL", what.c_str());
    std::fflush(stderr);
    if (!cond) ++failures;
}

std::string sample(oa::test::TestHost& host) {
    return oa::test::page_text(oa::test::story_layer(host.rt()), 40);
}

int glyph_rows(oa::test::TestHost& host) {
    int n = 0;
    for (const auto& row : host.plan())
        if (row.glyphs && row.opacity > 0.0) ++n;
    return n;
}

const char* load_key() {
    const char* e = std::getenv("OA_JOURNEY_LOAD_KEY");
    return e && *e ? e : "bt_load";
}

// ---------------------------------------------------------------------------
// 启动：标题 → 语言选择 → 开场 → 正文
//
// 首启会停在语言选择（两行 key=CLICK 的按钮，标题是 简体中文 / English）。
// 它不是正文，不能被 story_layer 当成"已经进游戏"。
// ---------------------------------------------------------------------------
/// 选择行（name=select）：语言选择、章节选择、确认框都是这种行。
struct SelectOpt {
    std::string layer;
    int no = 0;
    double cx = 0, cy = 0;
};

bool find_selects(oa::runtime::GameRuntime& rt, std::vector<SelectOpt>* out) {
    std::map<int, SelectOpt> by_no;
    for (const oa::render::Layer* l : rt.scene().draw_order()) {
        const auto* h = rt.scene().find_event_handler(l->id, "click");
        if (!h || !h->enabled) continue;
        const auto name = h->params.find("name");
        if (name == h->params.end() || name->second != "select") continue;
        SelectOpt o;
        o.layer = l->id;
        const auto no = h->params.find("no");
        if (no != h->params.end()) o.no = std::atoi(no->second.c_str());
        double w = l->has_clip ? l->clip_w : 560.0;
        double hh = l->has_clip ? l->clip_h : 74.0;
        double x0 = 0, y0 = 0, rw = 0, rh = 0;
        if (rt.scene().world_rect(l->id, w, hh, &x0, &y0, &rw, &rh)) {
            o.cx = x0 + rw / 2;
            o.cy = y0 + rh / 2;
        }
        by_no[o.no] = o;
    }
    for (auto& [k, v] : by_no) out->push_back(v);
    return !out->empty();
}

bool has_key_handler(oa::runtime::GameRuntime& rt, const char* key) {
    for (const oa::render::Layer* l : rt.scene().draw_order()) {
        const auto* h = rt.scene().find_event_handler(l->id, "click");
        if (!h || !h->enabled) continue;
        const auto it = h->params.find("key");
        if (it != h->params.end() && it->second == key) return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// 启动：这个游戏自己的形状。
//
// 不是固定三段，而是**按当前屏幕该做的那一下**推进（和玩家一样）：
//   选择行（语言/章节/确认）→ 悬停 40 帧再点（框架是 cursor 模型）
//   标题停驻且有 bt_start  → 点开始
//   正文页停驻             → Enter 翻页
// 逐字显示没走完不点，避免把一页点成两页。
// ---------------------------------------------------------------------------
bool boot(oa::test::TestHost& host) {
    auto& rt = host.rt();
    bool started = false;
    for (int t = 0; t < 40000 && !rt.exit_requested(); ++t) {
        const auto* w = rt.current_wait();
        const auto* ml = oa::test::story_layer(rt);
        const std::string page = oa::test::page_text(ml, 32);
        // 名字输入是"开始游戏"流程里的一步，不是正文：Enter 用默认名过去。
        const bool name_prompt =
            page.find("名字") != std::string::npos || page.find("名前") != std::string::npos;
        // 名字确认框（"，确定吗？"）也不是正文：Enter 就是"确定"。
        const bool confirm_prompt =
            page.find("确定") != std::string::npos || page.find("確定") != std::string::npos;
        if (started && (name_prompt || confirm_prompt)) {
            host.key(13);
            host.tick(40);
            continue;
        }
        if (started && ml && oa::test::is_page_wait(w) && oa::test::reveal_done(rt) &&
            oa::test::has_visible_chars(page))
            return true;

        std::vector<SelectOpt> opts;
        if (find_selects(rt, &opts)) {
            // cursor 模型：先把指针停在行上，让 rollover 生效，再按下去。
            host.move(opts[0].cx, opts[0].cy);
            host.tick(40);
            host.click(opts[0].cx, opts[0].cy, 2);
            host.tick(60); // 选择后的退场/分支跳转，别再补输入
            continue;
        }

        if (!started && w && w->is_park() && w->stage_id.empty() &&
            has_key_handler(rt, "bt_start")) {
            host.move(237, 968); // はじめから（500.d.2）世界中心附近
            host.tick(20);
            host.click(237, 968, 2);
            started = true;
            host.tick(60);
            continue;
        }

        if (started && oa::test::is_page_wait(w) && oa::test::reveal_done(rt)) {
            // 正文翻页走 Enter（和玩家的"决定"同一条链）。
            host.key(13);
            host.tick(4);
            continue;
        }
        host.tick(1);
    }
    return false;
}

// ---------------------------------------------------------------------------
// 用例：存档界面里存档 → 读档：背景必须回来
//
// 回归：场景快照的"CSV 操作页面"判据（根 id 纯数字 + 子树里有 csvbtn 的
// `name=` 点击行）曾把画面根 "1" 也判成操作页面 —— 1.80 消息窗按钮同样由
// csvbtn 生成，而整幅画面（背景/立绘/消息窗）都挂在 "1" 下。于是存档里
// 只剩消息槽节点，读档后背景直接消失（用户报的"存档读档后背景丢失"）。
// 这里走玩家的真实路径：开存档界面（UI 页 500 同时在场）→ 存档 → 关界面
// → 读档，断言背景层在、并且存档页自己的层没有跟着回来。
// ---------------------------------------------------------------------------
std::string story_bg_layer(oa::runtime::GameRuntime& rt) {
    for (const oa::render::Layer* l : rt.scene().draw_order()) {
        const auto it = l->props.find("file");
        if (it == l->props.end()) continue;
        if (it->second.rfind(":bg/", 0) == 0 && it->second != ":bg/black") return l->id;
    }
    return {};
}

void case_save_ui_keeps_scene(oa::test::TestHost& host) {
    auto& rt = host.rt();
    check(oa::test::ensure_story(rt), "back to a story page");
    const std::string bg = story_bg_layer(rt);
    check(!bg.empty(), "story page has a background layer");
    std::fprintf(stderr, "[jt] background layer='%s' layers=%zu\n", bg.c_str(),
                 rt.scene().draw_order().size());

    check(host.click_key("bt_save"), "open save screen (bt_save)");
    host.tick(60);
    check(rt.save_game_to("ui_roundtrip.dat"), "save from inside the save screen");

    rt.mouse_button(2, true);
    host.tick(2);
    rt.mouse_button(2, false);
    host.tick(60);
    check(rt.load_game_from("ui_roundtrip.dat", 0), "load the ui save");
    host.tick(120);

    const oa::render::Layer* back = bg.empty() ? nullptr : rt.scene().find(bg);
    check(back != nullptr, "background layer is in the restored scene");
    check(back && rt.scene().is_effectively_visible(bg),
          "background layer is visible after load");

    // 存档页自己的层（500.*）不属于剧情状态：它们必须留在存档外，否则读档后
    // 整页按钮会盖在剧情上（这条判据当初就是为它加的，不能修背景时丢掉）。
    // 判"有没有回来"看**有内容的层**（file 非空）：框架读档后自己会重新
    // materialize 500 / 500.pageno 这类空容器与文字槽，那是它的正常流程。
    bool ui_page_restored = false;
    for (const oa::render::Layer* l : rt.scene().draw_order())
        if ((l->id == "500" || l->id.rfind("500.", 0) == 0) && !l->file.empty()) {
            ui_page_restored = true;
            std::fprintf(stderr, "[jt] ui layer back: %s file=%s vis=%d\n", l->id.c_str(),
                         l->file.c_str(), l->visible ? 1 : 0);
        }
    check(!ui_page_restored, "the save page's own layers are not restored");
    std::fprintf(stderr, "[jt] after ui-save load layers=%zu text='%s'\n",
                 rt.scene().draw_order().size(), sample(host).c_str());
}

// ---------------------------------------------------------------------------
// 用例：load 界面开 → 右键关 → 再开
//
// csvbtn 每次开界面都把上一棵按钮树 [lydel] 掉再 [lyc] 建新的。连着开关
// 两次，就是在"派发回调里删掉正在派发的节点"这条路径上连踩两次。
// ---------------------------------------------------------------------------
void case_ui_toggle(oa::test::TestHost& host) {
    check(host.click_key(load_key()), std::string("open load screen (") + load_key() + ")");
    host.tick(40);
    std::fprintf(stderr, "[jt] load screen items=%zu\n", host.plan().size());

    host.rt().mouse_button(2, true);
    host.tick(2);
    host.rt().mouse_button(2, false);
    host.tick(40);
    std::fprintf(stderr, "[jt] after right-click items=%zu glyphs=%d\n", host.plan().size(),
                 glyph_rows(host));

    check(host.click_key(load_key()), "open load screen again");
    host.tick(160);
    std::fprintf(stderr, "[jt] survived second open, items=%zu\n", host.plan().size());
    check(host.plan().size() > 0, "screen still draws after reopen");

    // 关掉，回到正文：界面反复开关之后，剧情必须还能翻页。
    host.rt().mouse_button(2, true);
    host.tick(2);
    host.rt().mouse_button(2, false);
    host.tick(60);
    const std::string before = sample(host);
    check(oa::test::advance_page(host.rt(), 900), "story advances after ui round-trip");
    check(sample(host) != before, "page actually changed after ui round-trip");
}

// ---------------------------------------------------------------------------
// 用例：翻页 → 存档 → 再翻 → 读档
// ---------------------------------------------------------------------------
void case_save_load(oa::test::TestHost& host) {
    const double cx = host.stage_w() / 2.0;
    const double cy = host.stage_h() * 0.58; // 消息窗中部，避开底部 Dock

    check(oa::test::ensure_story(host.rt()), "back to a story page");
    const std::string page0 = sample(host);
    check(!page0.empty(), "story page carries text");

    std::string prev = page0;
    int turned = 0;
    for (int i = 0; i < 24 && turned < 3; ++i) {
        host.click(cx, cy);
        host.tick(20);
        const std::string s = sample(host);
        if (!s.empty() && s != prev) {
            prev = s;
            ++turned;
        }
    }
    check(turned >= 3, "advances across pages");

    const std::string saved_page = sample(host);
    const auto* saved_ml = oa::test::story_layer(host.rt());
    const std::string saved_layer = saved_ml ? saved_ml->id : std::string();
    check(host.rt().save_game_to("nekomiko.dat"), "save writes to the store");

    for (int i = 0; i < 12; ++i) {
        host.click(cx, cy);
        host.tick(20);
        if (sample(host) != saved_page) break;
    }
    check(sample(host) != saved_page, "moved past the saved page");

    check(host.rt().load_game_from("nekomiko.dat", 0), "load succeeds");
    host.tick(120);
    std::fprintf(stderr, "[jt] after load sample='%s' saved='%s'\n", sample(host).c_str(),
                 saved_page.c_str());
    check(sample(host) == saved_page, "restored to the saved page");
    std::fprintf(stderr, "[jt] saved layer='%s' after='%s'\n", saved_layer.c_str(),
                 host.rt().text().active_layer_id().c_str());
    for (const auto& r : host.plan())
        if (r.glyphs && r.opacity > 0.0)
            std::fprintf(stderr, "[jt] glyph row %-28s a=%.2f\n", r.id.c_str(), r.opacity);
    const auto* after_ml = oa::test::story_layer(host.rt());
    check(after_ml && after_ml->id == saved_layer,
          "restored onto the same visible text layer");
    check(glyph_rows(host) <= 3, "no stray text layers after load");

    // 读档不是终点：继续翻页，文字必须继续替换（原 txtload 用例的核心）。
    // 这里用"点一下看文字变没变"，而不是 advance_page：读档后的第一个停驻
    // 可能已经是下一段，页号判断会误报。
    const std::string loaded = sample(host);
    bool page_moved = false;
    for (int i = 0; i < 14 && !page_moved; ++i) {
        host.click(cx, cy);
        host.tick(60);
        page_moved = sample(host) != loaded;
    }
    check(page_moved, "text keeps changing after load");
}

// ---------------------------------------------------------------------------
// 用例：回顾（backlog）里点"跳转"——按下必须到得了按钮
//
// 回归现场：旧框架把整页拖拽层 `500.0.drag` 画在 jump 按钮下面。引擎如果
// 按"最前面的 drag 层"选拖拽目标，就会起拖拽、把这一下从按钮手里抢走，
// 表现是 backlog 里点跳转毫无反应（按钮的 click 行一次都没执行）。
// 正确规则：拖拽只认"指针下最前面的可交互层"（owner）；它是按钮，就派发
// 按钮的 click，下面的 drag 层没有资格。
// ---------------------------------------------------------------------------
void dump_state(oa::test::TestHost& host);

struct ClickSpot {
    std::string id;
    double cx = 0, cy = 0;
};

std::optional<ClickSpot> find_click(oa::runtime::GameRuntime& rt,
                                    const std::function<bool(const oa::render::LayerEventHandler&)>& pred) {
    for (const oa::render::Layer* l : rt.scene().draw_order()) {
        const auto* h = rt.scene().find_event_handler(l->id, "click");
        if (!h || !h->enabled || !rt.scene().is_effectively_visible(l->id)) continue;
        if (!pred(*h)) continue;
        double w = l->has_clip ? l->clip_w : (l->width > 0 ? l->width : 60.0);
        double hh = l->has_clip ? l->clip_h : (l->height > 0 ? l->height : 30.0);
        double x0 = 0, y0 = 0, rw = 0, rh = 0;
        if (!rt.scene().world_rect(l->id, w, hh, &x0, &y0, &rw, &rh)) continue;
        return ClickSpot{l->id, x0 + rw / 2, y0 + rh / 2};
    }
    return std::nullopt;
}

std::string lua_field(oa::runtime::GameRuntime& rt, const char* table, const char* field) {
    lua_State* L = rt.interpreter().lua_bridge().state();
    lua_getglobal(L, table);
    if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        return {};
    }
    lua_getfield(L, -1, field);
    std::string out;
    if (lua_isnumber(L, -1)) out = std::to_string((long long)lua_tonumber(L, -1));
    else if (lua_isstring(L, -1)) out = lua_tostring(L, -1);
    lua_pop(L, 2);
    return out;
}

void case_backlog_jump(oa::test::TestHost& host) {
    auto& rt = host.rt();
    check(oa::test::ensure_story(rt), "story page before backlog");

    // 走几页：backlog 里要有可跳的条目（否则 jump 按钮全是隐藏的）。
    for (int i = 0; i < 8; ++i) {
        host.key(13);
        host.tick(30);
    }

    auto back = find_click(rt, [](const oa::render::LayerEventHandler& h) {
        const auto it = h.params.find("key");
        return it != h.params.end() && it->second == "bt_back";
    });
    check(back.has_value(), "message window has the backlog button");
    if (!back) return;
    host.move(back->cx, back->cy);
    host.tick(20);
    host.click(back->cx, back->cy, 2);
    host.tick(90);

    auto jump = find_click(rt, [](const oa::render::LayerEventHandler& h) {
        const auto it = h.params.find("key");
        return it != h.params.end() && it->second.rfind("jump", 0) == 0;
    });
    check(jump.has_value(), "backlog shows a jump button");
    if (!jump) {
        dump_state(host);
        return;
    }

    std::fprintf(stderr, "[bl] jump button %s at %.0f,%.0f\n", jump->id.c_str(), jump->cx, jump->cy);
    host.move(jump->cx, jump->cy);
    host.tick(20);
    host.click(jump->cx, jump->cy, 2);
    host.tick(60);

    // 点中 = btn_click -> log_click("jump") -> goBacklogJump 记下目标并弹确认框。
    const std::string no = lua_field(rt, "flg", "blogno");
    check(!no.empty(), "one click on the jump button reaches the backlog handler");
}

void dump_state(oa::test::TestHost& host) {
    for (const auto& [id, mlp] : host.rt().text().all_layers()) {
        const auto& ml = *mlp;
        if (ml.char_count == 0) continue;
        const oa::render::Layer* n = host.rt().scene().find(id);
        std::fprintf(stderr, "[txt] %-26s chars=%zu a=%.2f vis=%d eff=%d sample='%s'\n",
                     id.c_str(), ml.char_count, n ? n->alpha : -1.0,
                     n ? (n->visible ? 1 : 0) : -1,
                     n ? (host.rt().scene().is_effectively_visible(id) ? 1 : 0) : -1,
                     oa::test::page_text(&ml, 20).c_str());
    }
    for (const oa::render::Layer* l : host.rt().scene().draw_order()) {
        const auto* h = host.rt().scene().find_event_handler(l->id, "click");
        if (!h || !h->enabled) continue;
        const auto it = h->params.find("key");
        if (it != h->params.end() && !it->second.empty()) {
            double w = l->has_clip ? l->clip_w : (l->width > 0 ? l->width : 60.0);
            double hh = l->has_clip ? l->clip_h : (l->height > 0 ? l->height : 30.0);
            double x0 = 0, y0 = 0, rw = 0, rh = 0;
            host.rt().scene().world_rect(l->id, w, hh, &x0, &y0, &rw, &rh);
            std::fprintf(stderr, "[key] %-24s key=%-8s rect=(%.0f,%.0f %.0fx%.0f) vis=%d\n",
                         l->id.c_str(), it->second.c_str(), x0, y0, rw, rh,
                         l->visible ? 1 : 0);
        }
    }
    for (const oa::test::PlanRow& r : host.plan())
        std::fprintf(stderr, "[plan] %-34s op=%d a=%.2f %s\n", r.id.c_str(), r.op, r.opacity,
                     r.glyphs ? "GLYPHS" : "");
}

} // namespace

int main() {
    oa::test::install_crash_handler();
    const char* pfs = std::getenv("OA_TEST_NEKOMIKO_PFS");
    if (!pfs || !*pfs) {
        std::printf("OA_TEST_NEKOMIKO_PFS unset; skipping\n");
        return 77;
    }

    const bool no_render = std::getenv("OA_JOURNEY_NO_RENDER") != nullptr;
    oa::test::TestHost host({.project = pfs, .render = !no_render});
    if (!host.ok()) {
        std::printf("FAIL: TestHost init\n");
        return 1;
    }

    check(boot(host), "boot: title -> language -> intro -> story");
    std::fprintf(stderr, "[jt] first page='%s'\n", sample(host).c_str());
    // 诊断：OA_JOURNEY_STORY_SHOT=<dir> 时把第一张剧情页落盘（对话框回归取证）。
    if (const char* dir = std::getenv("OA_JOURNEY_STORY_SHOT")) {
        const std::string path = std::string(dir) + "/nekomiko_story.png";
        if (host.save_png(path)) std::printf("[shot] %s\n", path.c_str());
    }
    case_ui_toggle(host);
    case_save_ui_keeps_scene(host);
    case_save_load(host);
    case_backlog_jump(host);

    if (const char* dir = std::getenv("OA_JOURNEY_SHOT")) {
        const std::string path = std::string(dir) + "/nekomiko_after_load.png";
        if (host.save_png(path)) std::printf("[shot] %s\n", path.c_str());
    }

    if (failures) {
        std::printf("nekomiko: %d failure(s)\n", failures);
        return 1;
    }
    std::printf("nekomiko: all ok\n");
    return 0;
}
