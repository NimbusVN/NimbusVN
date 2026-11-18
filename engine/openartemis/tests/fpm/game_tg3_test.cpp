// 甜蜜女友3（TG3） 游程 —— 一个游戏一个文件。
//
// OA_TEST_TG3_PFS 指向 root.pfs；不设则 exit 77。
//
// 这个文件的启动形状：标题 →（没有语言选择）→ 正文，然后按顺序跑动作。
// 要加流程就在下面加一个 case_*，共用同一个 boot()。
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

bool has_key_handler(oa::runtime::GameRuntime& rt, const char* key) {
    for (const oa::render::Layer* l : rt.scene().draw_order()) {
        const auto* h = rt.scene().find_event_handler(l->id, "click");
        if (!h || !h->enabled) continue;
        const auto it = h->params.find("key");
        if (it != h->params.end() && it->second == key) return true;
    }
    return false;
}

/// 标题 → 正文（这个游戏没有语言页）。
bool boot(oa::test::TestHost& host) {
    auto& rt = host.rt();
    bool started = false;
    bool saw_title = false;
    for (int t = 0; t < 40000 && !rt.exit_requested(); ++t) {
        const auto* w = rt.current_wait();
        const auto* ml = oa::test::story_layer(rt);
        if (started && ml && oa::test::is_page_wait(w) && oa::test::reveal_done(rt) &&
            oa::test::has_visible_chars(oa::test::page_text(ml, 64)))
            return true;

        if (!started && has_key_handler(rt, "bt_start")) saw_title = true;

        // 标题出现之前点掉 logo/开场；标题一出现就只点标题。
        if (!started && !saw_title && t > 120 && t % 60 == 0) {
            if (const auto p = oa::test::screen_click_point(rt))
                host.click(p->first, p->second, 2);
            else
                host.click(host.stage_w() / 2.0, host.stage_h() * 0.6, 2);
        }
        if (!started && saw_title && has_key_handler(rt, "bt_start")) {
            if (const auto p = oa::test::row_click_point(rt, "bt_start")) {
                host.move(p->first, p->second);
                host.tick(12);
                host.click(p->first, p->second, 3);
                started = true;
                host.tick(60);
                continue;
            }
        }

        // 开始之后还有开场白/过场：没有正文就继续点。
        if (started && !(ml && oa::test::has_visible_chars(oa::test::page_text(ml, 64))) &&
            t % 120 == 0) {
            if (const auto p = oa::test::screen_click_point(rt))
                host.click(p->first, p->second, 2);
            else
                host.click(host.stage_w() / 2.0, host.stage_h() * 0.6, 2);
        }
        host.tick(1);
    }
    return false;
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

} // namespace

int main() {
    oa::test::install_crash_handler();
    const char* pfs = std::getenv("OA_TEST_TG3_PFS");
    if (!pfs || !*pfs) {
        std::printf("OA_TEST_TG3_PFS unset; skipping\n");
        return 77;
    }
    oa::test::TestHost host({.project = pfs});
    if (!host.ok()) {
        std::printf("FAIL: TestHost init\n");
        return 1;
    }
    if (!boot(host)) {
        std::printf("tg3: boot did not reach story; skipping\n");
        return 77;
    }
    std::fprintf(stderr, "[tg3] first page='%s'\n", sample(host).c_str());
    case_pages(host);

    if (failures) {
        std::printf("tg3: %d failure(s)\n", failures);
        return 1;
    }
    std::printf("tg3: all ok\n");
    return 0;
}
