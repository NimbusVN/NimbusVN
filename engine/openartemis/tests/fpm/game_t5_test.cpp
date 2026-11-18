// 乙女理論とその周辺 -Ecole de Paris-（T5/B42） 游程 —— 一个游戏一个文件。
//
// OA_TEST_T5_PFS 指向 root.pfs；不设则 exit 77。
//
// 启动形状：标题 → 正文
// 要加流程就在下面加 case_*，共用同一个 boot()。
#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>
#include <vector>

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

bool has_key(oa::runtime::GameRuntime& rt, const char* key) {
    for (const oa::render::Layer* l : rt.scene().draw_order()) {
        const auto* h = rt.scene().find_event_handler(l->id, "click");
        if (!h || !h->enabled) continue;
        const auto it = h->params.find("key");
        if (it != h->params.end() && it->second == key) return true;
    }
    return false;
}

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

/// 标题/语言/开场 → 第一页正文。每一步"有就做，没有就跳过"。
bool boot(oa::test::TestHost& host) {
    auto& rt = host.rt();
    bool title_seen = false; // 见过标题菜单
    bool started = false;    // 标题菜单已离开（说明那一下真被脚本吃下了）
    int last_click = -1000;
    for (int t = 0; t < 40000 && !rt.exit_requested(); ++t) {
        const auto* w = rt.current_wait();
        const auto* ml = oa::test::story_layer(rt);
        const std::string page = oa::test::page_text(ml, 64);
        if (started && ml && oa::test::is_page_wait(w) && oa::test::reveal_done(rt) &&
            oa::test::has_visible_chars(page))
            return true;

        if (std::getenv("OA_JOURNEY_TRACE") && t % 240 == 0) {
            std::fprintf(stderr, "[trace] t=%d started=%d page='%s'\n", t, started ? 1 : 0,
                         oa::test::page_text(ml, 16).c_str());
            host.dump(stderr);
        }

        // 语言选择（bt_cn/bt_en/tw 这一类具名按钮）优先。
        if (!started && (host.has_button("bt_cn") || host.has_button("bt_en"))) {
            // 先点语言，再点它的确认键（bt_*_next）—— 只点语言会一直停在这屏。
            const char* want[] = {"bt_cn_next", "bt_en_next", "bt_tw_next",
                                  "bt_cn", "bt_en", "bt_tw"};
            for (const char* k : want) {
                if (!host.has_button(k)) continue;
                host.move(host.stage_w() / 2.0, host.stage_h() * 0.5);
                host.tick(12);
                if (host.click_key(k, 2)) {
                    host.tick(60);
                    break;
                }
            }
            continue;
        }

        std::vector<SelectOpt> opts;
        if (find_selects(rt, &opts)) {
            host.move(opts[0].cx, opts[0].cy);
            host.tick(40);
            host.click(opts[0].cx, opts[0].cy, 2);
            host.tick(60);
            continue;
        }

        // 标题按钮：命令常在 `com`（bt_start），`key` 只是快捷键 —— 用
        // has_button 统一认。
        const bool title_up = host.has_button("bt_start") ||
                              host.has_button("bt_cont") || host.has_button("bt_new");
        if (title_up) title_seen = true;
        // 菜单消失 = 那一下真的被脚本吃下了。点空/被 vsync 吞掉时菜单还在，
        // 所以"点过"不等于"点中"，必须重试到屏幕变化为止。
        if (title_seen && !title_up && !started) {
            started = true;
            host.tick(40);
            continue;
        }

        if (title_up && t - last_click >= 40) {
            last_click = t;
            host.move(host.stage_w() / 2.0, host.stage_h() * 0.5);
            host.tick(10); // 进 rollover（框架是 cursor 模型）
            if (host.click_key("bt_start", 3) || host.click_key("bt_cont", 3) ||
                host.click_key("bt_new", 3)) {
                host.tick(30);
                continue;
            }
        }

        // 标题之前的 logo/开场：点屏幕推过去（标题出现后就不再乱点）。
        if (!title_seen && t > 150 && t % 90 == 0) {
            if (const auto p = oa::test::screen_click_point(rt))
                host.click(p->first, p->second, 2);
            else
                host.click(host.stage_w() / 2.0, host.stage_h() * 0.6, 2);
        }

        // 开始之后的过场：没有正文、也没有可点菜单时才点屏幕。
        if (started && !oa::test::has_visible_chars(page) &&
            !oa::test::screen_click_point(rt).has_value() && t % 90 == 0) {
            host.click(host.stage_w() / 2.0, host.stage_h() * 0.6, 2);
        }
        host.tick(1);
    }
    return false;
}

/// 正文连推三页。
void case_pages(oa::test::TestHost& host) {
    check(oa::test::ensure_story(host.rt(), 6000), "stay on a story page");
    check(!sample(host).empty(), "page carries text");
    std::string prev = sample(host);
    int turned = 0;
    for (int i = 0; i < 48 && turned < 3; ++i) {
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

} // namespace

int main() {
    oa::test::install_crash_handler();
    const char* pfs = std::getenv("OA_TEST_T5_PFS");
    if (!pfs || !*pfs) {
        std::printf("OA_TEST_T5_PFS unset; skipping\n");
        return 77;
    }
    oa::test::TestHost host({.project = pfs});
    if (!host.ok()) {
        std::printf("FAIL: TestHost init\n");
        return 1;
    }
    if (!boot(host)) {
        std::printf("t5: boot did not reach story; skipping\n");
        return 77;
    }
    std::fprintf(stderr, "[t5] first page='%s'\n", sample(host).c_str());
    case_pages(host);

    if (failures) {
        std::printf("t5: %d failure(s)\n", failures);
        return 1;
    }
    std::printf("t5: all ok\n");
    return 0;
}
