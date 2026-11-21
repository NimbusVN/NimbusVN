// ハミダシクリエイティブ（中文版，FPM） 游程 —— 一个游戏一个文件。
//
// OA_TEST_FPM_PFS 指向 root.pfs；不设则 exit 77。
//
// 这个游戏的启动形状：标题 → 正文（**没有语言选择**），然后按顺序跑动作。
#include <cstdio>
#include <cstdlib>
#include <string>

#include "crash_backtrace.h"
#include "test_host.h"


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

/// 等到正文真的画在消息窗里再取样。
///
/// 读档/关界面的收尾阶段消息窗是藏着的：正文层还在，但没进渲染队列。
/// 这时 `story_layer()` 会退到别的可见文字层（存档页残留的 "100%"），
/// 断言就成了拿 UI 文字跟正文比。按一下会重新显示消息窗。
std::string sample_story(oa::test::TestHost& host) {
    for (int attempt = 0; attempt < 3; ++attempt) {
        host.tick(20);
        const oa::render::MessageLayer* ml = oa::test::story_layer(host.rt());
        if (ml) {
            const std::string t = oa::test::page_text(ml, 40);
            if (!t.empty() && ml->id.find(".mw.") != std::string::npos) return t;
        }
        host.key(13);
    }
    return sample(host);
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

/// 标题 → 正文。没有语言页，也没有名字输入。
bool boot(oa::test::TestHost& host) {
    auto& rt = host.rt();
    bool started = false;
    for (int t = 0; t < 40000 && !rt.exit_requested(); ++t) {
        const auto* w = rt.current_wait();
        const auto* ml = oa::test::story_layer(rt);
        if (started && ml && oa::test::is_page_wait(w) && oa::test::reveal_done(rt) &&
            oa::test::has_visible_chars(oa::test::page_text(ml, 64)))
            return true;

        if (std::getenv("OA_JOURNEY_TRACE") && t % 300 == 0) {
            std::fprintf(stderr, "[fpmtrace] t=%d page='%s'\n", t,
                         oa::test::page_text(oa::test::story_layer(rt), 16).c_str());
            host.dump(stderr);
        }
        if (!started && w && w->is_park() && w->stage_id.empty() &&
            has_key_handler(rt, "bt_start")) {
            // 标题按钮是滑入的：层和 click 行先出现，矩形还在屏幕外。点
            // "半截在外面"的按钮时命中的是别的层（玩家也不会这么点）。
            // 等按钮真正落进舞台再点，时序变化（转场过滤器多一拍 wait）
            // 才不会把启动变成随机成败。
            const oa::render::Layer* btn = nullptr;
            for (const oa::render::Layer* l : rt.scene().draw_order()) {
                const auto* h = rt.scene().find_event_handler(l->id, "click");
                if (!h || !h->enabled) continue;
                const auto it = h->params.find("key");
                if (it != h->params.end() && it->second == "bt_start") btn = l;
            }
            oa::test::PlanRow row;
            const bool on_screen =
                btn && host.find(btn->id, &row) && row.x >= 0 && row.y >= 0 &&
                row.x + row.w <= host.stage_w() && row.y + row.h <= host.stage_h();
            if (btn && on_screen) {
                host.move(row.x + row.w / 2, row.y + row.h / 2);
                host.tick(20);
                host.click(row.x + row.w / 2, row.y + row.h / 2, 3);
                started = true;
                host.tick(60);
                continue;
            }
        }
        if (started && oa::test::is_page_wait(w) && oa::test::reveal_done(rt)) {
            host.key(13);
            host.tick(4);
            continue;
        }
        // logo/开场阶段：没有可点层就点屏幕中心，把它推过去。
        if (!started && t > 180 && t % 60 == 0) {
            if (const auto p = oa::test::screen_click_point(rt))
                host.click(p->first, p->second, 2);
            else
                host.click(host.stage_w() / 2.0, host.stage_h() * 0.6, 2);
        }
        host.tick(1);
    }
    return false;
}

/// 开一个界面再右键关掉：界面反复重建之后，正文必须还在、还能翻。
void case_ui_toggle(oa::test::TestHost& host) {
    const std::string before = sample(host);
    check(host.click_key("bt_conf"), "open settings (bt_conf)");
    host.tick(40);
    check(host.plan().size() > 0, "settings screen draws");
    host.rt().mouse_button(2, true);
    host.tick(2);
    host.rt().mouse_button(2, false);
    host.tick(60);
    check(sample(host) == before, "story page survives a ui round-trip");

    bool moved = false;
    for (int i = 0; i < 10 && !moved; ++i) {
        host.key(13);
        host.tick(40);
        moved = sample(host) != before;
    }
    check(moved, "pages keep turning after a ui round-trip");
}

/// 存档 → 再翻 → 读档：回到存档那一页，并且文字还能继续换。
void case_save_load(oa::test::TestHost& host) {
    for (int i = 0; i < 3; ++i) {
        host.key(13);
        host.tick(40);
    }
    const std::string saved = sample(host);
    check(!saved.empty(), "page before save carries text");
    check(host.rt().save_game_to("fpm.dat"), "save writes to the store");
    host.key(13);
    host.tick(60);
    check(host.rt().load_game_from("fpm.dat", 0), "load succeeds");
    host.tick(120);
    check(sample(host) == saved, "restored to the saved page");

    bool moved = false;
    for (int i = 0; i < 14 && !moved; ++i) {
        host.key(13);
        host.tick(60);
        moved = sample(host) != saved;
    }
    check(moved, "text keeps changing after load");
}

/// 回顾（backlog）开 → 关：回来还在原来那一页，且能继续翻。
void case_backlog(oa::test::TestHost& host) {
    // 采样前先让场景落定：上一段是读档，读档收尾会把消息窗藏起来，正文层
    // 没进渲染队列（取样会落到残留的存档页数字上）。点一下让消息窗回来。
    const std::string before = sample_story(host);
    check(!before.empty(), "page before backlog carries text");
    check(host.click_key("bt_back"), "open backlog (bt_back)");
    host.tick(60);
    host.rt().mouse_button(2, true);
    host.tick(2);
    host.rt().mouse_button(2, false);
    host.tick(60);
    {
        const std::string after = sample(host);
        if (after != before)
            std::fprintf(stderr, "[bl] before='%s' after='%s'\n", before.c_str(), after.c_str());
        check(after == before, "backlog returns to the same page");
    }
    bool moved = false;
    for (int i = 0; i < 10 && !moved; ++i) {
        host.key(13);
        host.tick(40);
        moved = sample(host) != before;
    }
    check(moved, "pages keep turning after backlog");
}

void case_pages(oa::test::TestHost& host) {
    check(oa::test::ensure_story(host.rt(), 6000), "stay on a story page");
    check(!sample(host).empty(), "page carries text");
    std::string prev = sample(host);
    int turned = 0;
    for (int i = 0; i < 36 && turned < 3; ++i) {
        host.key(13);
        host.tick(24);
        const std::string s = sample(host);
        if (!s.empty() && s != prev) {
            prev = s;
            ++turned;
        }
    }
    check(turned >= 3, "advances across 3 pages");
}

// ---------------------------------------------------------------------------
// 用例：解锁后的消息窗菜单栏，悬停到按钮上不能"一闪一闪地躲开"
//
// dock 热区 `1.80.mw.-1` 是一整条透明条，展开后的按钮正好盖在它上面。
// 指针从热区移到按钮上时，按钮成了最顶层 —— 热区如果按"不再最顶层"判
// 就会立刻收 rollout → `mwarea_out` 关 dock → 指针又落回热区 → 再开，
// 每帧一次。正确的判据是"指针有没有离开这一层的矩形"。
// ---------------------------------------------------------------------------
void case_dock_hover_stable(oa::test::TestHost& host) {
    auto& rt = host.rt();
    check(oa::test::ensure_story(rt), "back to a story page for the dock check");
    // 解锁（conf.dock=0）+ 收起，等价于玩家点 dock 上的锁按钮
    rt.interpreter().lua_bridge().run_code(
        "conf.dock = 0\nmwarea_close()\n", "fpm_dock_unlock");
    host.tick(20);

    const auto* bar = rt.scene().find("1.80.mw.bt.dc");
    check(bar && !bar->visible, "dock bar starts retracted after unlock");

    int leave_count = 0;
    rt.set_pointer_observer([&leave_count](const oa::runtime::GameRuntime::PointerDispatch& d) {
        if (d.kind == oa::runtime::GameRuntime::PointerDispatch::Kind::HoverOut &&
            d.layer == "1.80.mw.-1")
            ++leave_count;
    });

    host.move(700, 690); // 热区里，同时落在一个 dock 按钮上
    host.tick(6);
    for (int i = 0; i < 30; ++i) {
        host.move(700 + (i % 2), 690); // 真人手抖：指针一直在微动
        host.tick(1);
    }
    bar = rt.scene().find("1.80.mw.bt.dc");
    check(bar && bar->visible, "dock stays open while hovering its buttons");
    check(leave_count == 0, "hotspot never reports rollout while still under the pointer");

    // 真移出热区才该收起来
    host.move(640, 300);
    host.tick(30);
    bar = rt.scene().find("1.80.mw.bt.dc");
    check(!bar || !bar->visible, "dock retracts once the pointer leaves the strip");
    rt.set_pointer_observer(nullptr);
}


// ---------------------------------------------------------------------------
// 用例：读档后正文只出现一次
//
// 存档里带了当前页的文字，框架的 `restoreText() → mw_redraw()` 又把同一页
// 原样重打一遍；`push_text` 是追加语义 ⇒ 同一句在页上出现两次
// （test2：读档以后剧情文字重复显示一句）。
// ---------------------------------------------------------------------------
void case_load_text_once(oa::test::TestHost& host) {
    auto& rt = host.rt();
    check(oa::test::ensure_story(rt), "back to a story page for the load check");
    const std::string before = sample_story(host);
    check(!before.empty(), "page before save carries text");
    check(rt.save_game_to("dup.dat"), "save for duplicate check");
    check(rt.load_game_from("dup.dat", 0), "load it back");
    host.tick(150);
    const std::string after = sample_story(host);
    if (after != before)
        std::fprintf(stderr, "[dup] before='%s' after='%s'\n", before.c_str(), after.c_str());
    check(after == before, "loaded page is not printed twice");
}

// ---------------------------------------------------------------------------
// 用例：开场动画期间点"是"，弹窗必须照样关掉
//
// 演出期间点击只推进演出（KAG 的 process loop 约束），但**不能吞掉**：
// 玩家在弹窗刚出来时就点，期望的是"这一下既跳过动画又确认"。
// ---------------------------------------------------------------------------
void case_click_during_open_animation(oa::test::TestHost& host) {
    auto& rt = host.rt();
    auto lua = [&](const std::string& code) {
        try {
            rt.interpreter().lua_bridge().run_code(code, "fpm_anim");
        } catch (const std::exception& e) {
            std::fprintf(stderr, "[anim] lua error: %s\n", e.what());
        }
    };
    lua("dialog('title')");
    host.tick(3); // 开场动画刚开始
    // 点"否"：同样走 yesno_exit 的关闭链，但不会像"是"那样真的回标题
    //（那会把后续用例的剧情页抽走）。
    const bool hit = host.click_key("bt_no");
    host.tick(200);
    const bool closed = rt.scene().find("600.1") == nullptr;
    std::fprintf(stderr, "[anim] click=%d closed=%d\n", hit ? 1 : 0, closed ? 1 : 0);
    check(hit, "yes button is hittable during the open animation");
    check(closed, "a click during the open animation still closes the dialog");
}

// ---------------------------------------------------------------------------
// 用例：调速度必须在同一个 Lua 调用里落地
//
// `set_message_speed()` 是
//     [chgmsg layered=1] → [scetween init] → [scetween add] → [/chgmsg]
// 四步**必须在同一拍**按序执行：chgmsg 打开的图层上下文要罩住 scetween。
// `e:tag` 一旦被排队，就变成 chgmsg → /chgmsg → scetween，补间落到关闭
// 之后，玩家看到的是"每次调速，剧情速度还是上一次的"。
// ---------------------------------------------------------------------------
void case_speed_applies_now(oa::test::TestHost& host) {
    auto& rt = host.rt();
    auto lua = [&](const std::string& code) {
        try {
            rt.interpreter().lua_bridge().run_code(code, "fpm_speed");
        } catch (const std::exception& e) {
            std::fprintf(stderr, "[speed] lua error: %s\n", e.what());
        }
    };
    lua("conf.mspeed = 25\nset_message_speed()");
    check(rt.interpreter().queued_tag_count() == 0,
          "speed change applies within the same call (no deferred tags)");
    lua("conf.mspeed = 75\nset_message_speed()");
    check(rt.interpreter().queued_tag_count() == 0,
          "second speed change applies the same way");
    host.tick(30);
}

// ---------------------------------------------------------------------------
// 用例：确认框点一次"是"就关掉，且 `dialog_return` 只跑一次
//
// 两次 `dialog_return` 就是 test2 的 `dlgp` nil 崩溃的直接成因
// （第二次进来时 flg.dlg 已经被第一次清掉）。
// ---------------------------------------------------------------------------
void case_dialog_confirm_once(oa::test::TestHost& host) {
    auto& rt = host.rt();
    auto lua = [&](const std::string& code) {
        try {
            rt.interpreter().lua_bridge().run_code(code, "fpm_dialog");
        } catch (const std::exception& e) {
            std::fprintf(stderr, "[fpm] lua error: %s\n", e.what());
        }
    };
    lua("__dr = 0\n"
        "local _dr = dialog_return\n"
        "function dialog_return() __dr = __dr + 1\n"
        "  return _dr()\n"
        "end\n"
        "dialog('title')");
    host.tick(90);
    check(host.rt().scene().find("600.1") != nullptr, "confirm dialog draws");

    check(host.click_key("bt_yes"), "click yes");
    host.tick(240);
    const bool closed = rt.scene().find("600.1") == nullptr;
    check(closed, "one click closes the confirm dialog");

    // 再点一下（对话框已经没了）：绝不能让 `dialog_return` 再跑一遍。
    host.click(640, 360, 2);
    host.tick(120);
    bool once = true;
    try {
        rt.interpreter().lua_bridge().run_code(
            "if __dr ~= 1 then error('dialog_return ran '..tostring(__dr)..' times') end",
            "fpm_dr_probe");
    } catch (const std::exception& e) {
        once = false;
        std::fprintf(stderr, "[fpm] %s\n", e.what());
    }
    check(once, "dialog_return runs exactly once per confirm");
}

} // namespace

int main() {
    oa::test::install_crash_handler();
    const char* pfs = std::getenv("OA_TEST_FPM_PFS");
    if (!pfs || !*pfs) {
        std::printf("OA_TEST_FPM_PFS unset; skipping\n");
        return 77;
    }
    const char* save_root = std::getenv("OA_SAVE_ROOT");
    oa::test::TestHost host({.project = pfs, .save_root = save_root ? save_root : ""});
    if (!host.ok()) {
        std::printf("FAIL: TestHost init\n");
        return 1;
    }
    if (!boot(host)) {
        std::printf("fpm: boot did not reach story; skipping\n");
        return 77;
    }
    std::fprintf(stderr, "[fpm] first page='%s'\n", sample(host).c_str());

    case_pages(host);
    case_ui_toggle(host);
    case_save_load(host);
    case_backlog(host);
    case_speed_applies_now(host);
    case_click_during_open_animation(host);
    case_load_text_once(host);
    // dock 用例会改 conf.dock 并让指针停在底栏，放在所有剧情用例之后。
    case_dock_hover_stable(host);
    // 会走到标题（[reset] 重跑启动头），放最后。
    case_dialog_confirm_once(host);

    if (failures) {
        std::printf("fpm: %d failure(s)\n", failures);
        return 1;
    }
    std::printf("fpm: all ok\n");
    return 0;
}
