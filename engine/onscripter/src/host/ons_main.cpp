/* -*- C++ -*-
 *
 *  ons_main.cpp - host entry point for the NimbusVN ONScripter port
 *
 *  Copyright (c) 2026 NimbusVN contributors.
 *
 *  Derived from ONScripter's onscripter.cpp argument handling
 *  (Copyright (c) 2001-2011 Ogapee, 2007-2011 Mion Sonozaki, 2023 Galladite).
 *  GPLv2 or later; see COPYING.
 */

#include "ONScripterLabel.h"
#include "bitmap.h"
#include "clock.h"
#include "font.h"
#include "movie.h"
#include "renderer.h"
#include "snapshot.h"
#include "version.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#if defined(__ANDROID__)
/* SDLActivity 通过 JNI 调 SDL_main：这里必须让 SDL3 的头把 main 改名，
 * 否则 .so 里只有普通 main 符号，Java 侧找不到入口。 */
#include <SDL3/SDL_main.h>
#endif

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

extern unsigned short convGBK2UTF16(unsigned short gbk);

namespace fs = std::filesystem;

static std::string g_dump_path_storage;

#if defined(_WIN32)
static LONG WINAPI ons_crash_filter(EXCEPTION_POINTERS *info)
{
    const unsigned long code = info && info->ExceptionRecord
                                   ? info->ExceptionRecord->ExceptionCode : 0;
    void *address = info && info->ExceptionRecord
                        ? info->ExceptionRecord->ExceptionAddress : NULL;
    const uintptr_t base = (uintptr_t)GetModuleHandleA(NULL);
    fprintf(stderr,
            "ons: FATAL exception 0x%08lx at %p (module+0x%llx)\n",
            code, address, (unsigned long long)((uintptr_t)address - base));
    void *frames[32];
    const USHORT count = CaptureStackBackTrace(0, 32, frames, NULL);
    for (USHORT i = 0; i < count; ++i) {
        fprintf(stderr, "ons: bt[%u] module+0x%llx\n", (unsigned)i,
                (unsigned long long)((uintptr_t)frames[i] - base));
    }
    fflush(stderr);
    return EXCEPTION_EXECUTE_HANDLER;
}
#endif

#ifdef NIMBUSVN_VERSION
#define ONS_RELEASE_VERSION NIMBUSVN_VERSION
#else
#define ONS_RELEASE_VERSION ONS_VERSION
#endif

static void print_version()
{
    printf("NimbusVN %s - ONScripter %s SDL3\n",
           ONS_RELEASE_VERSION, ONS_VERSION);
}

static void print_help(const char *argv0)
{
    print_version();
    printf("Usage: %s [options] <game-dir>\n", argv0);
    printf("  -f, --fullscreen         start fullscreen\n");
    printf("  -w, --window             force window mode\n");
    printf("      --font FILE          override font file\n");
    printf("      --save-dir PATH      override save directory\n");
    printf("      --gameid ID          override game identifier\n");
    printf("      --debug[=N]          debug verbosity\n");
    printf("      --english/--japanese script language preference\n");
    printf("      --force-png-alpha / --force-png-nscripter\n");
    printf("      --no-layers --disable-rescale --no-movie-upscale\n");
    printf("      --skip-past-newline --ignore-textgosub-newline\n");
    printf("      --enable-edit --enable-wheeldown-advance --force-button-shortcut\n");
    printf("      --preferred-width N --preferred-automode-time MS\n");
    printf("      --nomusic --nosound  mute output\n");
    printf("  -v, --version            show version and exit\n");
    printf("Test host:\n");
    printf("      --ons-frames N       quit after N presented frames\n");
    printf("      --ons-dump-frame P   write the current screen to P (PNG)\n");
    printf("      --ons-dump-at N      dump at frame N (default: last)\n");
    printf("      --ons-offscreen      no window/renderer, CPU composition only\n");
    printf("      --ons-auto-click MS  inject a left click every MS milliseconds\n");
    printf("      --ons-auto-click-until F  stop auto clicking at frame F\n");
    printf("      --ons-click X,Y      logical position of injected clicks\n");
    printf("      --ons-click-at F X,Y inject a left click after frame F\n");
    printf("      --ons-right-click-at F  inject a right click after frame F\n");
    printf("      --ons-key-at F KEY   inject KEY after frame F (enter/space/esc/\n");
    printf("                           up/down/left/right/a/s/l/f/...)\n");
    printf("      --ons-start-label N  start executing at *N instead of *start\n");
    printf("      --ons-encoding MODE  auto|gbk|sjis|utf8\n");
    printf("      --ons-audio-dump P   WAV dump path\n");
    printf("      --ons-movie-test F   decode movie F with MoviePlayer, then exit\n");
    printf("      --ons-text-test P    render a GBK sample with the FreeType\n");
    printf("                           font to P (PNG) and exit\n");
}

static bool parse_int(const char *text, int *out)
{
    if (!text || !*text)
        return false;
    char *end = NULL;
    long value = strtol(text, &end, 10);
    if (end == text || *end != '\0')
        return false;
    *out = (int)value;
    return true;
}

/* Parses "X,Y" logical screen coordinates. */
static bool parse_point(const char *text, int *x, int *y)
{
    if (!text || !*text)
        return false;
    char *end = NULL;
    long px = strtol(text, &end, 10);
    if (end == text || *end != ',')
        return false;
    const char *second = end + 1;
    long py = strtol(second, &end, 10);
    if (end == second || *end != '\0')
        return false;
    *x = (int)px;
    *y = (int)py;
    return true;
}

/*
 * Rasterises a GBK sample string with the FreeType backend and writes the
 * result to a PNG.  It touches no game data, so it is also a quick font/GBK
 * smoke test.
 */
static int run_text_test(const std::string &font_path, const std::string &out_path)
{
    const int kWidth = 640;
    const int kHeight = 120;
    const int kFontSize = 32;
    const int kBaseline = 80;

    /* "简体中文 GBK 测试 ABC 123" encoded as GBK. */
    static const unsigned char kText[] = {
        0xBC, 0xF2, 0xCC, 0xE5, 0xD6, 0xD0, 0xCE, 0xC4, 0x20,
        0x47, 0x42, 0x4B, 0x20, 0xB2, 0xE2, 0xCA, 0xD4, 0x20,
        0x41, 0x42, 0x43, 0x20, 0x31, 0x32, 0x33
    };
    const size_t kTextLen = sizeof(kText);

    ons::FontEngine engine;
    ons::Font *font = engine.open(font_path, kFontSize);
    if (font == NULL) {
        fprintf(stderr, "ons: --ons-text-test: cannot open font '%s'\n",
                font_path.c_str());
        return 1;
    }

    ons::Bitmap bitmap;
    if (!bitmap.create(kWidth, kHeight)) {
        fprintf(stderr, "ons: --ons-text-test: cannot allocate %dx%d bitmap\n",
                kWidth, kHeight);
        return 1;
    }
    bitmap.fill(ons::Color{0, 0, 0, 255});

    int pen_x = 12;
    int drawn = 0;
    int missing = 0;
    uint32_t previous = 0;

    for (size_t i = 0; i < kTextLen; ) {
        uint32_t unicode;
        const unsigned char c0 = kText[i];
        if (c0 >= 0x81 && c0 <= 0xfe && i + 1 < kTextLen) {
            unicode = convGBK2UTF16((unsigned short)((c0 << 8) | kText[i + 1]));
            i += 2;
        } else {
            unicode = c0;
            ++i;
        }

        if (unicode == 0) {
            unicode = '?';
            ++missing;
        }

        if (previous != 0)
            pen_x += font->kerning(previous, unicode);
        previous = unicode;

        const ons::Glyph *glyph = font->glyph(unicode);
        if (glyph == NULL || glyph->w <= 0 || glyph->h <= 0) {
            pen_x += kFontSize / 2;
            continue;
        }

        const int dx = pen_x + glyph->minx;
        const int dy = kBaseline - glyph->maxy;

        for (int y = 0; y < glyph->h; ++y) {
            const int py = dy + y;
            if (py < 0 || py >= kHeight)
                continue;
            const uint8_t *src = &glyph->pixels[(size_t)y * glyph->pitch];
            for (int x = 0; x < glyph->w; ++x) {
                const int px = dx + x;
                if (px < 0 || px >= kWidth)
                    continue;
                const unsigned int a = src[x];
                if (a == 0)
                    continue;

                /* White text over the opaque black background. */
                const uint32_t dst = bitmap.get_pixel(px, py);
                const unsigned int dr = (dst >> 16) & 0xff;
                const unsigned int dg = (dst >> 8) & 0xff;
                const unsigned int db = dst & 0xff;
                const unsigned int r = (255u * a + dr * (255u - a)) / 255u;
                const unsigned int g = (255u * a + dg * (255u - a)) / 255u;
                const unsigned int b = (255u * a + db * (255u - a)) / 255u;
                bitmap.set_pixel(px, py, ons::pack_rgba((uint8_t)r, (uint8_t)g,
                                                        (uint8_t)b, 255));
            }
        }

        pen_x += glyph->advance;
        ++drawn;
    }

    long lit = 0;
    long opaque = 0;
    for (int y = 0; y < kHeight; ++y) {
        for (int x = 0; x < kWidth; ++x) {
            const uint32_t pixel = bitmap.get_pixel(x, y);
            if (((pixel >> 24) & 0xff) != 0)
                ++opaque;
            if ((pixel & 0x00ffffffu) != 0)
                ++lit;
        }
    }

    const bool saved = ons::save_png(bitmap, out_path.c_str());
    const double ratio = 100.0 * (double)lit / (double)(kWidth * kHeight);

    fprintf(stderr,
            "ons: --ons-text-test font='%s' size=%d glyphs=%d missing=%d "
            "pen_x=%d\n",
            font_path.c_str(), kFontSize, drawn, missing, pen_x);
    fprintf(stderr,
            "ons: --ons-text-test png='%s' %dx%d non_black=%ld (%.3f%%) "
            "opaque=%ld save=%s\n",
            out_path.c_str(), kWidth, kHeight, lit, ratio, opaque,
            saved ? "ok" : "FAILED");

    return saved ? 0 : 1;
}

/*
 * Decodes a movie file with MoviePlayer exactly like the engine does
 * (renderer + screen bitmap + AudioEngine movie bus) and writes the last
 * presented frame plus an optional WAV of the mix.
 */
static bool wav_stats(const std::string &path, double *rms, int *peak)
{
    FILE *fp = fopen(path.c_str(), "rb");
    if (!fp)
        return false;

    unsigned char riff[12];
    if (fread(riff, 1, 12, fp) != 12 || memcmp(riff, "RIFF", 4) != 0 ||
        memcmp(riff + 8, "WAVE", 4) != 0) {
        fclose(fp);
        return false;
    }

    uint64_t total = 0;
    double sumsq = 0.0;
    int pk = 0;

    for (;;) {
        unsigned char ch[8];
        if (fread(ch, 1, 8, fp) != 8)
            break;
        const uint32_t sz = (uint32_t)ch[4] | ((uint32_t)ch[5] << 8) |
                            ((uint32_t)ch[6] << 16) | ((uint32_t)ch[7] << 24);
        if (memcmp(ch, "data", 4) == 0) {
            unsigned char buf[8192];
            uint32_t left = sz;
            while (left > 0) {
                const size_t want = left > sizeof(buf) ? sizeof(buf) : left;
                const size_t got = fread(buf, 1, want, fp);
                if (got == 0)
                    break;
                for (size_t i = 0; i + 1 < got; i += 2) {
                    const int16_t v =
                        (int16_t)((uint16_t)buf[i] | ((uint16_t)buf[i + 1] << 8));
                    const int a = v < 0 ? -v : v;
                    if (a > pk)
                        pk = a;
                    sumsq += (double)v * (double)v;
                    ++total;
                }
                left -= (uint32_t)got;
            }
            break;
        }
        if (fseek(fp, (long)sz + (long)(sz & 1u), SEEK_CUR) != 0)
            break;
    }
    fclose(fp);

    if (total == 0)
        return false;
    *rms = sqrt(sumsq / (double)total) / 32768.0;
    *peak = pk;
    return true;
}

static int run_movie_test(const std::string &path, const std::string &png_path,
                          const std::string &wav_path, int max_frames,
                          bool offscreen)
{
    const int kWidth = 640;
    const int kHeight = 480;

    ons::Renderer renderer;
    if (!renderer.init(kWidth, kHeight, "ONScripter movie test", false,
                       offscreen)) {
        fprintf(stderr, "ons: --ons-movie-test: renderer init failed: %s\n",
                renderer.error());
        return 1;
    }

    ons::Bitmap screen;
    if (!screen.create(kWidth, kHeight)) {
        fprintf(stderr, "ons: --ons-movie-test: cannot allocate screen\n");
        renderer.shutdown();
        return 1;
    }
    screen.fill(ons::Color{0, 0, 0, 255});

    ons::AudioEngine audio;
    if (!audio.open())
        fprintf(stderr, "ons: --ons-movie-test: audio device unavailable, "
                        "offline mixing only\n");

    ons::MoviePlayer movie;
    movie.set_renderer(&renderer);
    movie.set_audio_engine(&audio);
    if (!movie.open(path)) {
        fprintf(stderr, "ons: --ons-movie-test: cannot open '%s'\n",
                path.c_str());
        audio.close();
        renderer.shutdown();
        return 1;
    }

    fprintf(stderr,
            "ons: movie test: '%s' %dx%d video=%s pixel=%s %.3fs audio=%s\n",
            path.c_str(), movie.width(), movie.height(),
            movie.video_codec_name(), movie.pixel_format_name(),
            movie.duration(),
            movie.has_audio() ? movie.audio_codec_name() : "none");

    Rect full = {0, 0, kWidth, kHeight};
    movie.set_output(&screen, full);
    movie.set_volume(1.0f);

    if (!wav_path.empty())
        audio.dump_wav(wav_path);

    if (!movie.play(false, true)) {
        fprintf(stderr, "ons: --ons-movie-test: play() failed\n");
        audio.close();
        renderer.shutdown();
        return 1;
    }

    const uint32_t start_ms = ons::ticks_ms();
    while (max_frames <= 0 || movie.presented_frames() < max_frames) {
        if (!movie.update(true))
            break;
    }
    const uint32_t elapsed_ms = ons::ticks_ms() - start_ms;
    movie.stop();

    long non_black = 0;
    long opaque = 0;
    for (int y = 0; y < screen.h; ++y) {
        const uint32_t *row = screen.pixels + (size_t)y * (size_t)(screen.pitch / 4);
        for (int x = 0; x < screen.w; ++x) {
            const uint32_t p = row[x];
            if ((p & 0x00ffffffu) != 0)
                ++non_black;
            if (((p >> 24) & 0xffu) != 0)
                ++opaque;
        }
    }

    bool saved = true;
    if (!png_path.empty())
        saved = ons::save_png(screen, png_path.c_str());

    audio.close();

    double rms = 0.0;
    int peak = 0;
    const bool have_wav = !wav_path.empty() && wav_stats(wav_path, &rms, &peak);

    fprintf(stderr,
            "ons: movie test: decoded=%lld presented=%lld dropped=%lld "
            "audio_frames=%lld elapsed=%ums\n",
            (long long)movie.decoded_frames(),
            (long long)movie.presented_frames(),
            (long long)movie.dropped_frames(),
            (long long)movie.played_audio_frames(), elapsed_ms);

    if (!png_path.empty()) {
        const double ratio =
            100.0 * (double)non_black / (double)(kWidth * kHeight);
        fprintf(stderr,
                "ons: movie test: png='%s' %dx%d non_black=%ld (%.3f%%) "
                "opaque=%ld save=%s\n",
                png_path.c_str(), kWidth, kHeight, non_black, ratio, opaque,
                saved ? "ok" : "FAILED");
    }
    if (have_wav)
        fprintf(stderr,
                "ons: movie test: wav='%s' rms=%.5f peak=%d\n",
                wav_path.c_str(), rms, peak);
    else if (!wav_path.empty())
        fprintf(stderr, "ons: movie test: wav='%s' EMPTY/UNREADABLE\n",
                wav_path.c_str());

    renderer.shutdown();
    return saved ? 0 : 1;
}

/* Maps a symbolic name (or a single printable character) to the engine's
 * KEY_* numbering.  Test host only. */
static int parse_key_name(const char *text)
{
    if (!text || !*text)
        return -1;
    if (text[1] == '\0') {
        const unsigned char c = (unsigned char)text[0];
        if (c >= 'A' && c <= 'Z')
            return (int)(c - 'A' + 'a');
        return (int)c;
    }

    char name[16];
    size_t n = 0;
    while (text[n] && n + 1 < sizeof(name)) {
        char c = text[n];
        if (c >= 'A' && c <= 'Z')
            c = (char)(c - 'A' + 'a');
        name[n] = c;
        ++n;
    }
    name[n] = '\0';

    struct NamedKey { const char *name; int key; };
    static const NamedKey kNamed[] = {
        {"enter", KEY_RETURN}, {"return", KEY_RETURN}, {"kp_enter", KEY_KP_ENTER},
        {"space", KEY_SPACE}, {"esc", KEY_ESCAPE}, {"escape", KEY_ESCAPE},
        {"tab", KEY_TAB}, {"backspace", KEY_BACKSPACE}, {"delete", KEY_DELETE},
        {"up", KEY_UP}, {"down", KEY_DOWN}, {"left", KEY_LEFT}, {"right", KEY_RIGHT},
        {"home", KEY_HOME}, {"end", KEY_END},
        {"pageup", KEY_PAGEUP}, {"pagedown", KEY_PAGEDOWN},
        {"insert", KEY_INSERT}, {"ctrl", KEY_LCTRL}, {"shift", KEY_LSHIFT},
        {"alt", KEY_LALT},
        {"f1", KEY_F1}, {"f2", KEY_F2}, {"f3", KEY_F3}, {"f4", KEY_F4},
        {"f5", KEY_F5}, {"f6", KEY_F6}, {"f7", KEY_F7}, {"f8", KEY_F8},
        {"f9", KEY_F9}, {"f10", KEY_F10}, {"f11", KEY_F11}, {"f12", KEY_F12}
    };
    for (size_t i = 0; i < sizeof(kNamed) / sizeof(kNamed[0]); ++i) {
        if (!strcmp(name, kNamed[i].name))
            return kNamed[i].key;
    }
    return -1;
}

int main(int argc, char **argv)
{
#if defined(_WIN32)
    SetUnhandledExceptionFilter(ons_crash_filter);
#endif
    std::string root;
    std::string font;
    std::string save_dir;
    std::string gameid;
    std::string dump_path;
    std::string encoding = "auto";
    std::string audio_dump;
    std::string key_exe;
    std::string text_test_path;
    std::string movie_test_path;
    std::string start_label;

    struct ClickAt { int frame; int x; int y; };
    struct RightClickAt { int frame; };
    struct KeyAt { int frame; int key; };
    std::vector<ClickAt> click_at;
    std::vector<RightClickAt> right_click_at;
    std::vector<KeyAt> key_at;

    bool fullscreen = false;
    bool window_mode = false;
    bool english = false;
    bool japanese = false;
    bool offscreen = false;
    int debug = 0;
    int frames = 0;
    int dump_at = 0;
    int auto_click_ms = 0;
    int auto_click_until = 0;
    int click_x = 0;
    int click_y = 0;
    bool click_set = false;

    for (int i = 1; i < argc; ++i) {
        const char *arg = argv[i];
        const char *next = (i + 1 < argc) ? argv[i + 1] : NULL;

        if (!strcmp(arg, "-v") || !strcmp(arg, "--version")) {
            print_version();
            return 0;
        } else if (!strcmp(arg, "-h") || !strcmp(arg, "--help")) {
            print_help(argv[0]);
            return 0;
        } else if (!strcmp(arg, "-f") || !strcmp(arg, "--fullscreen")) {
            fullscreen = true;
        } else if (!strcmp(arg, "-w") || !strcmp(arg, "--window")) {
            window_mode = true;
        } else if (!strcmp(arg, "--font")) {
            if (!next) { fprintf(stderr, "%s requires a file\n", arg); return 1; }
            font = next; ++i;
        } else if (!strncmp(arg, "--save-dir=", 11)) {
            save_dir = arg + 11;
        } else if (!strcmp(arg, "--save-dir")) {
            if (!next) { fprintf(stderr, "%s requires a path\n", arg); return 1; }
            save_dir = next; ++i;
        } else if (!strcmp(arg, "--gameid")) {
            if (!next) { fprintf(stderr, "%s requires an id\n", arg); return 1; }
            gameid = next; ++i;
        } else if (!strcmp(arg, "--key-exe")) {
            if (!next) { fprintf(stderr, "%s requires a file\n", arg); return 1; }
            key_exe = next; ++i;
        } else if (!strcmp(arg, "--english")) {
            english = true;
        } else if (!strcmp(arg, "--japanese")) {
            japanese = true;
        } else if (!strcmp(arg, "--force-png-alpha")) {
            /* applied after construction */
        } else if (!strcmp(arg, "--force-png-nscripter") ||
                   !strcmp(arg, "--detect-png-nscmask")) {
            /* applied after construction */
        } else if (!strcmp(arg, "--no-layers") ||
                   !strcmp(arg, "--disable-rescale") ||
                   !strcmp(arg, "--no-movie-upscale") ||
                   !strcmp(arg, "--skip-past-newline") ||
                   !strcmp(arg, "--ignore-textgosub-newline") ||
                   !strcmp(arg, "--enable-edit") ||
                   !strcmp(arg, "--enable-wheeldown-advance") ||
                   !strcmp(arg, "--force-button-shortcut") ||
                   !strcmp(arg, "--nomusic") ||
                   !strcmp(arg, "--nosound") ||
                   !strcmp(arg, "--scaled") ||
                   !strcmp(arg, "--use-app-icons") ||
                   !strcmp(arg, "--match-bgm-audio") ||
                   !strcmp(arg, "--cdaudio")) {
            /* applied after construction */
        } else if (!strcmp(arg, "--preferred-width") ||
                   !strcmp(arg, "--preferred-automode-time") ||
                   !strcmp(arg, "--audiobuffer") ||
                   !strcmp(arg, "--cd-number") ||
                   !strcmp(arg, "--fileversion")) {
            if (!next) { fprintf(stderr, "%s requires a value\n", arg); return 1; }
            ++i;
        } else if (!strcmp(arg, "--debug")) {
            debug = 1;
        } else if (!strncmp(arg, "--debug=", 8)) {
            if (!parse_int(arg + 8, &debug))
                debug = 1;
        } else if (!strcmp(arg, "--ons-frames")) {
            if (!next || !parse_int(next, &frames)) {
                fprintf(stderr, "--ons-frames requires an integer\n");
                return 1;
            }
            ++i;
        } else if (!strcmp(arg, "--ons-dump-frame")) {
            if (!next) { fprintf(stderr, "--ons-dump-frame requires a path\n"); return 1; }
            dump_path = next; ++i;
        } else if (!strcmp(arg, "--ons-dump-at")) {
            if (!next || !parse_int(next, &dump_at)) {
                fprintf(stderr, "--ons-dump-at requires an integer\n");
                return 1;
            }
            ++i;
        } else if (!strcmp(arg, "--ons-offscreen")) {
            offscreen = true;
        } else if (!strcmp(arg, "--ons-auto-click")) {
            if (!next || !parse_int(next, &auto_click_ms)) {
                fprintf(stderr, "--ons-auto-click requires milliseconds\n");
                return 1;
            }
            ++i;
        } else if (!strcmp(arg, "--ons-auto-click-until")) {
            if (!next || !parse_int(next, &auto_click_until)) {
                fprintf(stderr, "--ons-auto-click-until requires a frame\n");
                return 1;
            }
            ++i;
        } else if (!strcmp(arg, "--ons-click")) {
            if (!next || !parse_point(next, &click_x, &click_y)) {
                fprintf(stderr, "--ons-click requires X,Y logical coordinates\n");
                return 1;
            }
            click_set = true;
            ++i;
        } else if (!strcmp(arg, "--ons-click-at")) {
            int frame = 0, x = 0, y = 0;
            if (!next || !parse_int(next, &frame) || i + 2 >= argc ||
                !parse_point(argv[i + 2], &x, &y)) {
                fprintf(stderr, "--ons-click-at requires FRAME X,Y\n");
                return 1;
            }
            ClickAt item = {frame, x, y};
            click_at.push_back(item);
            i += 2;
        } else if (!strcmp(arg, "--ons-right-click-at")) {
            int frame = 0;
            if (!next || !parse_int(next, &frame)) {
                fprintf(stderr, "--ons-right-click-at requires FRAME\n");
                return 1;
            }
            RightClickAt item = {frame};
            right_click_at.push_back(item);
            ++i;
        } else if (!strcmp(arg, "--ons-key-at")) {
            int frame = 0;
            if (!next || !parse_int(next, &frame) || i + 2 >= argc) {
                fprintf(stderr, "--ons-key-at requires FRAME KEY\n");
                return 1;
            }
            const int key = parse_key_name(argv[i + 2]);
            if (key < 0) {
                fprintf(stderr, "--ons-key-at: unknown key '%s'\n", argv[i + 2]);
                return 1;
            }
            KeyAt item = {frame, key};
            key_at.push_back(item);
            i += 2;
        } else if (!strcmp(arg, "--ons-start-label")) {
            if (!next) { fprintf(stderr, "--ons-start-label requires a label\n"); return 1; }
            start_label = next; ++i;
        } else if (!strcmp(arg, "--ons-encoding")) {
            if (!next) { fprintf(stderr, "--ons-encoding requires a mode\n"); return 1; }
            encoding = next; ++i;
        } else if (!strcmp(arg, "--ons-audio-dump")) {
            if (!next) { fprintf(stderr, "--ons-audio-dump requires a path\n"); return 1; }
            audio_dump = next; ++i;
        } else if (!strcmp(arg, "--ons-text-test")) {
            if (!next) { fprintf(stderr, "--ons-text-test requires a path\n"); return 1; }
            text_test_path = next; ++i;
        } else if (!strcmp(arg, "--ons-movie-test")) {
            if (!next) { fprintf(stderr, "--ons-movie-test requires a file\n"); return 1; }
            movie_test_path = next; ++i;
        } else if (arg[0] != '-' || !strcmp(arg, "-")) {
            /* Like krkrsdl3/openartemis: the game directory is the positional
             * argument, matching the other kernels. */
            if (!root.empty()) {
                fprintf(stderr, "onscripter: multiple game directories: '%s' and '%s'\n",
                        root.c_str(), arg);
                return 1;
            }
            root = arg;
        } else {
            fprintf(stderr, "onscripter: unknown option '%s'\n", arg);
            print_help(argv[0]);
            return 1;
        }
    }

    /* A game directory is the first positional argument.  The two standalone
     * media tests do not need one. */
    if (root.empty() && text_test_path.empty() && movie_test_path.empty()) {
        fprintf(stderr, "onscripter: missing game directory\n");
        print_help(argv[0]);
        return 1;
    }

    /* Resolve relative paths before changing the working directory. */
    std::error_code ec;
    fs::path original_cwd = fs::current_path(ec);
    if (ec) {
        fprintf(stderr, "onscripter: cannot query working directory: %s\n", ec.message().c_str());
        return 1;
    }

    fs::path dump_abs;
    if (!dump_path.empty()) {
        dump_abs = fs::absolute(fs::path(dump_path), ec);
        if (ec) {
            fprintf(stderr, "onscripter: invalid dump path '%s'\n", dump_path.c_str());
            return 1;
        }
    }

    /* The engine chdir()s into the game directory, so resolve diagnostic
     * outputs too. */
    fs::path audio_abs;
    if (!audio_dump.empty()) {
        audio_abs = fs::absolute(fs::path(audio_dump), ec);
        if (ec) {
            fprintf(stderr, "onscripter: invalid audio dump path '%s'\n",
                    audio_dump.c_str());
            return 1;
        }
    }

    /* Text rendering test: no game data needed, runs before any chdir. */
    if (!text_test_path.empty()) {
        fs::path out_abs = fs::absolute(fs::path(text_test_path), ec);
        if (ec) {
            fprintf(stderr, "onscripter: invalid text-test path '%s'\n",
                    text_test_path.c_str());
            return 1;
        }

        std::string font_path = font;
        if (font_path.empty()) {
            if (!root.empty())
                font_path = (fs::path(root) / "default.ttf").string();
            else
                font_path = "default.ttf";
        }
        return run_text_test(font_path, out_abs.string());
    }

    /* Standalone movie decode test: no game data needed. */
    if (!movie_test_path.empty()) {
        fs::path movie_abs = fs::absolute(fs::path(movie_test_path), ec);
        if (ec || !fs::is_regular_file(movie_abs, ec)) {
            fprintf(stderr, "onscripter: movie file not found: %s\n",
                    movie_test_path.c_str());
            return 1;
        }
        fs::path png_abs = dump_abs;
        if (png_abs.empty() && !dump_path.empty()) {
            png_abs = fs::absolute(fs::path(dump_path), ec);
            if (ec) {
                fprintf(stderr, "onscripter: invalid dump path '%s'\n",
                        dump_path.c_str());
                return 1;
            }
        }
        fs::path wav_abs;
        if (!audio_dump.empty()) {
            wav_abs = fs::absolute(fs::path(audio_dump), ec);
            if (ec) {
                fprintf(stderr, "onscripter: invalid audio dump path '%s'\n",
                        audio_dump.c_str());
                return 1;
            }
        }
        return run_movie_test(movie_abs.string(), png_abs.string(),
                              wav_abs.string(), frames, offscreen);
    }

    /* Resolve CLI paths against the caller's directory, not the game root:
     * the engine chdir()s into the game directory below. */
    if (!font.empty()) {
        fs::path p = fs::absolute(fs::path(font), ec);
        if (!ec)
            font = p.string();
    }
    if (!save_dir.empty()) {
        fs::path p = fs::absolute(fs::path(save_dir), ec);
        if (!ec) {
            save_dir = p.string();
            /* The engine expects the save directory to exist; creating it
             * here also covers --save-dir pointing outside the game root. */
            fs::create_directories(p, ec);
            if (ec) {
                fprintf(stderr, "onscripter: cannot create save directory '%s': %s\n",
                        save_dir.c_str(), ec.message().c_str());
                return 1;
            }
        }
    }
    if (!key_exe.empty()) {
        fs::path p = fs::absolute(fs::path(key_exe), ec);
        if (!ec)
            key_exe = p.string();
    }

    /* 引擎的 archive path：默认是 chdir 之后的当前目录 "."。
     * Android 上游戏目录位于 /storage/...，正常情况下 chdir 可用；如果受限
     * （只读挂载、FUSE 权限、路径被重定向）就退回绝对路径 —— DirPaths 只把
     * ':' 当分隔符，Android 路径里没有 ':'，所以直接把目录交进去是安全的。 */
    std::string archive_path_arg = ".";

    if (!root.empty()) {
        fs::path root_abs = fs::absolute(fs::path(root), ec);
        if (ec || !fs::is_directory(root_abs, ec)) {
            fprintf(stderr, "onscripter: game root not found: %s\n", root.c_str());
            return 1;
        }
        fs::current_path(root_abs, ec);
        if (ec) {
#if defined(_WIN32)
            /* Windows 盘符路径含 ':'，DirPaths 会拆坏，保持原来的硬失败。 */
            fprintf(stderr, "onscripter: cannot enter '%s': %s\n",
                    root_abs.string().c_str(), ec.message().c_str());
            return 1;
#else
            ec.clear();
            std::string abs = root_abs.generic_string();
            if (abs.empty() || abs.back() != '/')
                abs += '/';
            archive_path_arg = abs;
            fprintf(stderr,
                    "ons: chdir to '%s' failed, using absolute archive path '%s'\n",
                    root_abs.string().c_str(), archive_path_arg.c_str());
#endif
        }
    }

    if (!dump_abs.empty())
        g_dump_path_storage = dump_abs.string();

    fprintf(stderr, "ons: version %s (ONScripter %s), root=%s, offscreen=%d, frames=%d\n",
            ONS_RELEASE_VERSION, ONS_VERSION,
            fs::current_path(ec).string().c_str(),
            offscreen ? 1 : 0, frames);
    if (encoding != "auto")
        fprintf(stderr, "ons: --ons-encoding %s\n", encoding.c_str());
    if (!audio_abs.empty())
        fprintf(stderr, "ons: --ons-audio-dump %s\n", audio_abs.string().c_str());
    if (click_set)
        fprintf(stderr, "ons: injected click point %d,%d (logical)\n", click_x, click_y);

    ONScripterLabel ons;

    /* Platform configuration must happen before init() creates the window. */
    ons.renderer().set_snapshot_hook(&ons::save_png);
    ons.renderer().set_test_options(frames,
                                    g_dump_path_storage.empty() ? NULL
                                                                : g_dump_path_storage.c_str(),
                                    dump_at, auto_click_ms);
    ons.renderer().set_auto_click_until(auto_click_until);
    if (click_set)
        ons.renderer().set_click_point(click_x, click_y);
    for (size_t k = 0; k < click_at.size(); ++k)
        ons.renderer().add_click_at(click_at[k].frame, click_at[k].x, click_at[k].y);
    for (size_t k = 0; k < right_click_at.size(); ++k)
        ons.renderer().add_right_click_at(right_click_at[k].frame);
    for (size_t k = 0; k < key_at.size(); ++k)
        ons.renderer().add_key_at(key_at[k].frame, key_at[k].key);
    ons.renderer().set_offscreen(offscreen);
    ons.input().set_offscreen(offscreen);

    /* 已经 chdir 进游戏根目录时 "." 是安全的 archive path；
     * chdir 失败时 archive_path_arg 是绝对目录（见上面的回退）。 */
    ons.setArchivePath(archive_path_arg.c_str());

    if (!font.empty())
        ons.setFontFile(font.c_str());
    ons.setEncodingMode(encoding.c_str());
    if (!save_dir.empty())
        ons.setSavePath(save_dir.c_str());
    if (!gameid.empty())
        ons.setGameIdentifier(gameid.c_str());
    if (!key_exe.empty())
        ons.setKeyEXE(key_exe.c_str());

    ons.setDebugLevel(debug);
    if (!start_label.empty()) {
        fprintf(stderr, "ons: will start at label '%s' after define\n",
                start_label.c_str());
        ons.setStartLabel(start_label.c_str());
    }

    if (fullscreen)
        ons.setFullscreenMode();
    if (window_mode)
        ons.setWindowMode();
    if (english) {
        ons.setEnglishDefault();
        ons.setEnglishPreferred();
        ons.setEnglishMenu();
    }
    if (japanese) {
        ons.setJapaneseDefault();
        ons.setJapanesePreferred();
        ons.setJapaneseMenu();
    }

    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--no-layers")) {
#ifndef NO_LAYER_EFFECTS
            ons.setNoLayers();
#endif
        } else if (!strcmp(argv[i], "--disable-rescale")) {
            ons.disableRescale();
        } else if (!strcmp(argv[i], "--no-movie-upscale")) {
            ons.setNoMovieUpscale();
        } else if (!strcmp(argv[i], "--skip-past-newline")) {
            ons.setSkipPastNewline();
        } else if (!strcmp(argv[i], "--ignore-textgosub-newline")) {
            ons.setIgnoreTextgosubNewline();
        } else if (!strcmp(argv[i], "--enable-edit")) {
            ons.enableEdit();
        } else if (!strcmp(argv[i], "--enable-wheeldown-advance")) {
            ons.enableWheelDownAdvance();
        } else if (!strcmp(argv[i], "--force-button-shortcut")) {
            ons.enableButtonShortCut();
        } else if (!strcmp(argv[i], "--use-app-icons")) {
            ons.setUseAppIcons();
        } else if (!strcmp(argv[i], "--scaled")) {
            ons.setScaled();
        } else if (!strcmp(argv[i], "--nomusic") || !strcmp(argv[i], "--nosound")) {
            ons.setVolumeOn(false);
        } else if (!strcmp(argv[i], "--match-bgm-audio")) {
            ons.setMatchBgmAudio(true);
        } else if (!strcmp(argv[i], "--cdaudio")) {
            ons.enableCDAudio();
        } else if (!strcmp(argv[i], "--force-png-alpha")) {
            ons.setMaskType(ONScripterLabel::PNG_MASK_USE_ALPHA);
        } else if (!strcmp(argv[i], "--force-png-nscripter") ||
                   !strcmp(argv[i], "--detect-png-nscmask")) {
            ons.setMaskType(ONScripterLabel::PNG_MASK_USE_NSCRIPTER);
        } else if (!strcmp(argv[i], "--preferred-width") && i + 1 < argc) {
            ons.setPreferredWidth(argv[++i]);
        } else if (!strcmp(argv[i], "--preferred-automode-time") && i + 1 < argc) {
            ons.setPreferredAutomodeTime(argv[++i]);
        } else if (!strcmp(argv[i], "--audiobuffer") && i + 1 < argc) {
            ons.setAudioBufferSize(atoi(argv[++i]));
        } else if (!strcmp(argv[i], "--cd-number") && i + 1 < argc) {
            ons.setCDNumber(atoi(argv[++i]));
        } else if (!strcmp(argv[i], "--fileversion") && i + 1 < argc) {
            ons.setFileVersion(argv[++i]);
        }
    }

    if (ons.init()) {
        fprintf(stderr, "onscripter: initialization failed\n");
        ons.renderer().shutdown();
        ons::shutdown_timers();
        return 1;
    }

    fprintf(stderr, "ons: init complete, entering script\n");
    if (!audio_abs.empty())
        ons.audio().dump_wav(audio_abs.string());
    ons.executeLabel();
    ons.runEventLoop();

    ons.renderer().dump_if_needed();
    ons.renderer().shutdown();
    ons::shutdown_timers();
    return 0;
}
