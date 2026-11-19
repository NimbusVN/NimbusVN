# ONScripter 内核（NimbusVN）

NScripter / ONScripter 游戏内核：`nscript.dat` / `nscript.___` 脚本 +
`*.nsa` / `arc.nsa` / `*.sar` 封包，外加散装的 PNG/BMP/JPG、`default.ttf` 等资源。
SDL3 窗口与纹理送显；字体、图像、音频、影片由 `media/` 直接实现。

源基线 ONScripter，GPLv2，原始版权头保留在 `src/ons/`；许可正文见 `COPYING`。

## 构建

根 CMake 已把本内核接成第三个目标，桌面默认一起构建：

```bat
:: 三个内核一起（krkrsdl3 + openartemis + onscripter）
script\build-windows.bat

:: 只构建 onscripter
cmake --preset "Windows Config ONS"
cmake --build --preset "Windows Release Build ONS"
```

Linux / macOS 对应 `Linux Config ONS` / `macOS Config ONS`（以及
`Linux Release Build ONS` / `macOS Release Build ONS`）。
产物：`out/windows-ons/Release/onscripter.exe`、`out/linux-ons/Release/onscripter`、
`out/macos-ons/Release/onscripter`。

依赖（vcpkg）：SDL3（仅核心）、FreeType、libpng、libjpeg-turbo、BZip2，
FFmpeg 可选（找不到时音频/视频后端关闭，configure 打 warning）。

Android：`ENGINE_ONSCRIPTER=ON` 时生成 `libonscripter.so`，由
`platform/android/app/java/org/nimbusvn/ONSActivity.java` 启动（`getLibraries()`
返回 `SDL3` + `onscripter`）。原生入口在 `__ANDROID__` 下包含
`<SDL3/SDL_main.h>`，保证 SDLActivity 能找到 `SDL_main`。

ONSActivity 固定 `sensorLandscape`（默认横屏，支持左右横屏）；启动器不强制方向。
触摸手势由 `core/input.cpp` 统一翻译，不依赖 SDL 的 touch→mouse 模拟：
单指轻点=左键，**双指轻点=右键**，双指上下滑=滚轮，三指轻点=右键，
长按后拖动=按住左键拖动。

版本号：根 `CMakeLists.txt` 的 `NIMBUSVN_VERSION` 通过
`NIMBUSVN_VERSION` 编译定义传给宿主，`onscripter --version` 显示
`NimbusVN <版本> - ONScripter <上游版本>`，与 APK `versionName` 一致。

WebAssembly：目标输出 `out/emscripten/onscripter/<Config>/index.html`，
页面壳为 `platform/emscripten/onscripter_shell.html`。Emscripten 下
CMake 条件跳过 Freetype/PNG/JPEG/FFmpeg。

## 运行

```bat
set ONS_GAME=D:/Games/SomeGame
out\windows-ons\Release\onscripter.exe "%ONS_GAME%"
out\windows-ons\Release\onscripter.exe "%ONS_GAME%" --save-dir out\ons_save
out\windows-ons\Release\onscripter.exe "%ONS_GAME%" --fullscreen
```

`<game-dir>` 是游戏目录，作为第一参数传入（与 krkrsdl3 / openartemis 一致）：
宿主会 `chdir` 进去，并把 `.` 作为 archive path。
非 Windows 平台如果 `chdir` 失败（只读挂载 / 受限存储），会自动退回
“绝对路径 + 结尾斜杠”作为 archive path；Windows 盘符路径含 `:`（DirPaths
的分隔符），chdir 失败仍然直接报错退出。

脚本、封包与字体都由引擎在游戏目录里自行探测，所以启动参数里**没有**
“启动文件名”这一项（Android 壳也按此拼 `<目录>`）。

## 测试参数

```
--ons-frames N            渲染 N 帧后退出
--ons-offscreen           不创建窗口/渲染器，只做 CPU 合成
--ons-dump-frame PATH     保存当前画面 PNG
--ons-dump-at N           指定 dump 帧号（默认最后一帧）
--ons-auto-click MS       定时注入左键
--ons-auto-click-until N  到第 N 帧停止自动点击
--ons-click X,Y           注入点击的逻辑坐标
--ons-click-at N X,Y      第 N 帧后注入一次左键
--ons-right-click-at N    第 N 帧后注入右键（打开 rmenu）
--ons-key-at N KEY        第 N 帧后注入按键（enter/space/esc/up/down/a/s/l/f…）
--ons-start-label NAME    执行完 *define 后从 *NAME 开始
--ons-encoding MODE       auto|gbk|sjis|utf8
--ons-audio-dump PATH     混音输出 WAV（诊断）
--ons-movie-test FILE     单独解码影片后退出
--ons-text-test PATH      用 FreeType 渲染 GBK 样张并退出
<game-dir>                游戏目录（第一参数）
--font FILE               覆盖字体
--save-dir PATH           覆盖存档目录（不存在会自动创建）
```

300 帧冒烟：

```bat
set ONS_GAME=D:/Games/SomeGame
out\windows-ons\Release\onscripter.exe "%ONS_GAME%" --ons-frames 300 --ons-offscreen
```

长跑/存档回归脚本在 `tests/`（`run_stability.py`、`run_save_test.py`）。

## 架构简述

```
src/
├── core/    Bitmap（CPU RGBA8）、Renderer（SDL3 窗口/流纹理）、InputSystem、Clock
├── media/   FreeType 字体、libpng/libjpeg 图像、FFmpeg 音频混音、FFmpeg 影片
├── ons/     ONScripter 引擎逻辑（脚本解析、命令、图层/特效）
└── host/    ons_main.cpp（参数解析 + 测试宿主）、ons_snapshot.cpp（PNG dump）
```

送显路径：`Layer/AnimationInfo` 软件合成到 `Bitmap screen` → `Renderer::present`
上传 streaming texture → `SDL_RenderTexture` + `SDL_RenderPresent`。
音频：FFmpeg 解码 → 环形缓冲 → 自研混音（BGM/SE/Voice/Movie）→ SDL3 音频回调。
视频：FFmpeg 软解 → RGBA 帧贴到 screen，音轨走 AudioEngine 的 Movie 总线。

存档格式与 ONScripter 兼容，不额外加密。
