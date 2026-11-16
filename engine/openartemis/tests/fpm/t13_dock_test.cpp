// test13 (穢翼のユースティア): every button on the message-window bar under the
// dialogue box must be clickable, and the story must go on after the screen it
// opens is closed.
//
// Two engine defects this guards:
//  1. [yesno] was swallowed as a host-widget no-op. The framework's
//     confirmation dialogs are script-side (ui/dialog.lua's global yesno()
//     builds 600.x and parks on [call asbmacro.asb dialog_open]), so quick
//     save / quick load / suspend / emergency did nothing at all: t.yesno stayed
//     0 and the flow fell through to its own [return].
//  2. That [return] then popped the story's frame ("jump <flow> ... [return]"
//     is the framework idiom for a handler-issued escape), which rewound the
//     script a page instead of putting the reader back on it.
//
// Real flow only: title -> story -> click the bar button -> close the screen
// (or answer the dialog) -> assert the page is the same one and that three more
// clicks keep turning pages. No injected story tags.
//
// Env: OA_TEST_T13_PFS (skip 77 when unset).
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

extern "C" {
#include "lua.h"
}

#include "core/fs/physfs_fs.h"
#include "core/runtime/runtime_game.h"
#include "host_input.h"

namespace {
int failures = 0;
void check(bool cond, const char* what, const std::string& who) {
    if (!cond) {
        std::fprintf(stderr, "FAIL [%s]: %s\n", who.c_str(), what);
        ++failures;
    }
}

std::string lua_eval(oa::runtime::GameRuntime& rt, const char* code) {
    rt.interpreter().lua_bridge().run_code(code, "t13_dock");
    lua_State* L = rt.interpreter().lua_bridge().state();
    lua_getglobal(L, "_zz");
    std::string out;
    if (lua_isstring(L, -1)) out = lua_tostring(L, -1);
    lua_pop(L, 1);
    return out;
}

std::string page(oa::runtime::GameRuntime& rt) {
    return oa::test::page_text(oa::test::story_layer(rt), 40);
}

bool on_story(oa::runtime::GameRuntime& rt) {
    const std::string* s = rt.interpreter().current_script();
    return s && s->find("scenario/") != std::string::npos;
}

bool dialogue_open(oa::runtime::GameRuntime& rt) {
    return lua_eval(rt, "_zz=tostring(dlg ~= nil)") == "true";
}

void frames(oa::runtime::GameRuntime& rt, int n) {
    for (int i = 0; i < n && !rt.exit_requested(); ++i) oa::test::frame(rt);
}

// Centre of a layer, using its clip box when it has no texture size (headless).
bool click_layer(oa::runtime::GameRuntime& rt, const std::string& id) {
    const oa::render::Layer* l = rt.scene().find(id);
    if (!l) return false;
    const double w = l->has_clip ? l->clip_w : (l->width > 0 ? l->width : 60.0);
    const double h = l->has_clip ? l->clip_h : (l->height > 0 ? l->height : 30.0);
    double x0 = 0, y0 = 0, rw = 0, rh = 0;
    if (!rt.scene().world_rect(id, w, h, &x0, &y0, &rw, &rh)) return false;
    oa::test::click_at(rt, int(x0 + rw / 2), int(y0 + rh / 2));
    return true;
}

// The screen's own back button is the one the data marks with key=EXIT (it also
// binds ESC / right click through setonpush).
bool click_exit_layer(oa::runtime::GameRuntime& rt) {
    std::vector<const oa::render::Layer*> rows;
    for (const oa::render::Layer* l : rt.scene().draw_order()) {
        if (!l->visible) continue;
        const auto* h = rt.scene().find_event_handler(l->id, "click");
        if (!h || !h->enabled) continue;
        const auto it = h->params.find("key");
        if (it != h->params.end() && it->second == "EXIT") rows.push_back(l);
    }
    if (rows.empty()) return false;
    return click_layer(rt, rows.back()->id);
}

// One page turn with the role-advance key (headless clicks cannot reach the
// full-screen ADV hit layer without textures).
bool turns_page(oa::runtime::GameRuntime& rt) {
    const std::string was = page(rt);
    frames(rt, 60);
    oa::test::hold_key(rt, 0x0D, 4, 4);
    for (int i = 0; i < 150 && !rt.exit_requested(); ++i) {
        oa::test::frame(rt);
        const std::string now = page(rt);
        if (!now.empty() && now != was) return true;
    }
    return false;
}

struct Case {
    const char* id;    // layer suffix on 1.80.100.
    const char* name;  // screen / action, for messages
    const char* func;  // adv.func the click must open ("" = no page)
};

// The bar under the message window: config / save / load / help / route /
// backlog open a page, quick save opens the framework's confirmation dialog.
const Case kCases[] = {
    {"1.6.0", "config", "conf"},
    {"1.7.0", "save", "save"},
    {"1.8.0", "load", "load"},
    {"1.3.0", "help", "help"},
    {"1.5.0", "route", "rout"},
    {"2.3.0", "backlog", "blog"},
    {"2.1.0", "quick save", ""},
};
// The framework's confirmation dialog: its buttons carry no hit box without
// textures, so answer through the same handler the button runs.
void answer_dialog(oa::runtime::GameRuntime& rt, bool yes) {
    lua_eval(rt, yes ? "btn_cursor = 'bt_yes'" : "btn_cursor = 'bt_no'");
    lua_eval(rt, yes ? "e:tag{'var', name='t.yesno', data='1'}"
                     : "e:tag{'var', name='t.yesno', data='0'}");
    lua_eval(rt, "if yesno_exit then yesno_exit() end");
    frames(rt, 400);
}

// Backlog row -> jump confirmation -> the answer must be usable, and the jump
// must land on a readable page that still advances.
//
// Regressions this guards:
//  1. The confirmation answered once and then every press re-ran one line
//     forever: the queued [call ... last] restored a park on a line the jump
//     had already left (a park belongs to the line that armed it).
//  2. The landed page was blank: opening a UI [sceout]s the message layers and
//     the framework's quick-jump path never runs the normal UI-close [msg_show];
//     the redraw only prints a new page, so the stale "hidden" flag has to end
//     with the page it hid.
//  3. The end-of-replay redraw (exskip_end -> advTextRefresh) only ran on the
//     next click, because e:debugSkip's index was ignored and the boundary was
//     only noticed while a park was being resolved.
void backlog_jump_case(oa::runtime::GameRuntime& rt) {
    const char* who = "backlog row jump";
    for (int i = 0; i < 6; ++i) {
        oa::test::ensure_story(rt);
        oa::test::advance_page(rt);
    }
    check(oa::test::ensure_story(rt), "story settled", who);
    check(click_layer(rt, "1.80.100.2.3.0"), "backlog button present", who);
    frames(rt, 240);
    check(lua_eval(rt, "_zz=tostring(adv and adv.func)") == "blog", "backlog opened", who);

    // Row 4 is a real ADV page. Row 1 of this chapter is a
    // RegisterTextToHistory line (drawn on a movie sprite), and jumping to it
    // legitimately replays the cutscene up to the first printable page — that
    // is the script's own continuation, not a landing error.
    const char* kRow = "500.2.4.0";

    // No: the dialog closes and the backlog is still there.
    check(click_layer(rt, kRow), "backlog row present", who);
    frames(rt, 240);
    check(dialogue_open(rt), "row click asks for confirmation", who);
    const std::string row_text =
        lua_eval(rt,
                 "local no=tonumber(e:var('t.no')) local r='' "
                 "if no and log.stack[no] then local ax=log.stack[no] "
                 "r=tostring(ax.text and ax.text[1] and ax.text[1].data) end _zz=r");
    // The line this row was printed on, recorded in the backlog entry: the
    // replay has to stop on that page, not at the top of the script.
    const std::string target_line =
        lua_eval(rt,
                 "local no=tonumber(e:var('t.no')) local r='' "
                 "if no and log.stack[no] then local f=log.stack[no].stack.files "
                 "if f and #f>0 then r=tostring(f[#f].index) end end _zz=r");
    answer_dialog(rt, false);
    check(!dialogue_open(rt), "dialog closed on no", who);
    check(lua_eval(rt, "_zz=tostring(adv and adv.func)") == "blog",
          "backlog is back after no", who);

    // Yes: the story jumps to that row and keeps working.
    const std::string line_before = std::to_string(rt.interpreter().current_line());
    // The rows are hover-driven (btn_cursor is set by rollover): move off the
    // row first, or a click at the same point dispatches no rollover at all.
    rt.mouse_move(20.0f, 20.0f);
    frames(rt, 4);
    check(click_layer(rt, kRow), "backlog row present (second try)", who);
    frames(rt, 240);
    check(dialogue_open(rt), "row click asks again", who);
    // Answer inline instead of answer_dialog() so the frames that follow can be
    // watched: a quick-jump replays the chapter from its entry label, and if
    // the replay costs one frame per page (the "complete the text, advance on
    // the next tick" rule applied to a forced fast-forward) the player sees the
    // pages in between flash by. The message layer may be empty while the
    // window is rebuilt, but it must never show a page other than this row's.
    lua_eval(rt, "btn_cursor = 'bt_yes'");
    lua_eval(rt, "e:tag{'var', name='t.yesno', data='1'}");
    lua_eval(rt, "if yesno_exit then yesno_exit() end");
    bool flashed_other_page = false;
    bool landed = false;
    for (int f = 0; f < 150 && !rt.exit_requested(); ++f) {
        oa::test::frame(rt);
        // Watch only while the story script itself is running: the confirm
        // dialog's own script still has the old page on the layer, and that is
        // not part of the replay.
        const std::string* cur = rt.interpreter().current_script();
        if (!cur || cur->find("scenario/main/") == std::string::npos) continue;
        const std::string now = oa::test::page_text(rt.text().layer("adv"), 200);
        if (!now.empty() && !row_text.empty() && now != row_text) flashed_other_page = true;
        if (!row_text.empty() && now == row_text) {
            landed = true;
            break;
        }
    }
    check(!flashed_other_page, "the replay flashes no page other than the row's", who);
    check(landed, "the row's page arrives without extra input", who);
    check(!dialogue_open(rt), "dialog closed on yes", who);
    // Let the page finish revealing: a press on a still-typing page only
    // completes the text (the framework's one-press-one-command rule).
    for (int f = 0; f < 200 && !rt.text().is_reveal_complete(); ++f) oa::test::frame(rt);
    const std::string* script = rt.interpreter().current_script();
    check(script && script->find("scenario/") != std::string::npos,
          "story script after the jump", who);
    check(lua_eval(rt, "_zz=tostring(adv and adv.func)") == "nil",
          "no UI left open after the jump", who);
    check(std::to_string(rt.interpreter().current_line()) != line_before,
          "the jump moved the story", who);
    if (!target_line.empty()) {
        const size_t want = size_t(std::strtoul(target_line.c_str(), nullptr, 10));
        check(rt.interpreter().current_line() >= want,
              ("the replay stopped on the recorded page, not before it (target " +
               target_line + ")").c_str(), who);
    }
    check(lua_eval(rt, "_zz=tostring(adv and adv.exskip)") == "nil",
          "fast forward is over", who);
    // The page has to come back readable: the row's own text on the message
    // layer, and the layer not left in [sceout] state.
    const oa::render::MessageLayer* adv = rt.text().layer("adv");
    const std::string shown = oa::test::page_text(adv, 200);
    check(!shown.empty(), "jumped page is not blank", who);
    if (!row_text.empty()) {
        check(shown == row_text,
              ("jumped page shows the row's text (got '" + shown + "', want '" +
               row_text + "')").c_str(), who);
    }
    check(adv && !adv->text_hidden, "jumped page is visible again", who);
    check(rt.current_wait() != nullptr, "jumped page is parked, not mid-run", who);
    // A press after the jump must move the script on (the reported "no reaction
    // after the popup").
    const size_t line_after_jump = rt.interpreter().current_line();
    oa::test::hold_key(rt, 0x0D, 4, 4);
    frames(rt, 200);
    check(rt.interpreter().current_line() != line_after_jump,
          "input moves the story after the jump", who);
}
} // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    const char* pfs = std::getenv("OA_TEST_T13_PFS");
    if (!pfs || !*pfs) {
        std::printf("OA_TEST_T13_PFS unset; skipping\n");
        return 77;
    }
    // The framework writes its system/quicksave files through the plain Lua
    // io.open with a path relative to the working directory (savedata/...), so
    // the test has to sit where the game sits — closing the config screen runs
    // syssave() on the way out, and a failing write aborts the close.
    {
        std::error_code ec;
        auto dir = std::filesystem::path(pfs).parent_path();
        if (!dir.empty()) std::filesystem::current_path(dir, ec);
        if (ec) std::printf("[test] cannot enter %s: %s\n", dir.string().c_str(),
                            ec.message().c_str());
    }
    for (const Case& c : kCases) {
        std::printf("[test] ---- %s (%s) ----\n", c.name, c.id);
        const int before_case = failures;
        auto fs = std::make_shared<oa::fs::PhysFileSystem>(pfs, true);
        oa::runtime::GameRuntime rt(fs);
        rt.open_project("windows");
        rt.boot_project();
        frames(rt, 900);
        if (!oa::test::start_story_from_title(rt)) {
            std::printf("story not reached; skipping\n");
            return 77;
        }
        for (int i = 0; i < 6; ++i) {
            oa::test::ensure_story(rt);
            oa::test::advance_page(rt);
        }
        if (!oa::test::ensure_story(rt)) {
            check(false, "story page settled before the click", c.name);
            continue;
        }
        const std::string before = page(rt);
        check(!before.empty(), "story text before the click", c.name);

        const std::string layer = std::string("1.80.100.") + c.id;
        check(click_layer(rt, layer), "bar button present", c.name);
        frames(rt, 200);

        // A click on the bar is a command for the button, never a page turn.
        const bool opened_func =
            !std::string(c.func).empty() && lua_eval(rt, "_zz=tostring(adv and adv.func)") == c.func;
        if (!std::string(c.func).empty()) check(opened_func, "button opened its screen", c.name);

        if (dialogue_open(rt)) {
            // The dialog's buttons carry no hit box without textures; answer it
            // through the framework's own click handler, exactly what the yes
            // button runs.
            lua_eval(rt, "btn_cursor = 'bt_yes'");
            lua_eval(rt, "e:tag{'var', name='t.yesno', data='1'}");
            lua_eval(rt, "if yesno_exit then yesno_exit() end");
            frames(rt, 300);
            check(!dialogue_open(rt), "confirmation dialog closed", c.name);
        } else if (opened_func) {
            for (int attempt = 0; attempt < 5 && !on_story(rt); ++attempt) {
                if (click_exit_layer(rt)) {
                    frames(rt, 240);
                } else {
                    oa::test::hold_key(rt, 27, 6, 6); // ESC: the data's back key
                    frames(rt, 200);
                }
            }
            frames(rt, 120);
        }

        check(page(rt) == before || on_story(rt) == false,
              "the click did not turn the page", c.name);
        check(on_story(rt), "story script is current again", c.name);
        check(rt.current_wait() && rt.current_wait()->waiting_click(),
              "story page waits for a click", c.name);
        check(page(rt) == before, "same story page after the screen closed", c.name);
        check(turns_page(rt), "story advances afterwards", c.name);
        if (failures == before_case) std::printf("[test] %s OK\n", c.name);
    }

    if (failures) {
        std::fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    if (failures) {
        std::fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    // Backlog row -> jump confirmation (its own boot: the jump moves the story).
    std::printf("[test] ---- backlog row jump ----\n");
    {
        const int before_case = failures;
        auto fs = std::make_shared<oa::fs::PhysFileSystem>(pfs, true);
        oa::runtime::GameRuntime rt(fs);
        rt.open_project("windows");
        rt.boot_project();
        frames(rt, 900);
        if (oa::test::start_story_from_title(rt)) {
            backlog_jump_case(rt);
        } else {
            std::printf("story not reached; skipping\n");
            return 77;
        }
        if (failures == before_case) std::printf("[test] backlog row jump OK\n");
    }
    if (failures) {
        std::fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    std::printf("[test] message-window buttons OK\n");
    return 0;
}
