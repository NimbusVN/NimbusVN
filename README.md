# NimbusVN — 多内核视觉小说引擎

NimbusVN 是 **krkrsdl3**、**openartemis** 与 **ONScripter** 三个视觉小说引擎内核的统一构建与发行项目。

三个内核共享同一套平台壳、依赖与发行物：

| 内核 | 游戏格式 | 仓库 | 原生入口 |
|---|---|---|---|
| **krkrsdl3** | XP3 + TJS（KAG） | `engine/krkrsdl3`（子模块） | `libkrkrsdl3.so` / `krkrsdl3` |
| **openartemis** | PFS / 解包工程（Artemis） | `engine/openartemis`（子模块） | `libopenartemis.so` / `openartemis` |
| **onscripter** | `nscript.dat` + `*.nsa` / `arc.nsa`（NScripter） | `engine/onscripter` | `libonscripter.so` / `onscripter` |

桌面（Windows/Linux/macOS）与 WebAssembly 在一次构建里同时产出三个内核；
Android APK（`org.nimbusvn`，应用名 **NimbusVN**）同时打包三个原生库，由启动器**按游戏目录内容自动识别内核**；
OHOS 完成工程与启动器的多内核接入，当前原生只构建 krkrsdl3
（openartemis / onscripter 的 OHOS 后端尚未实现，PFS/ONS 游戏会得到明确提示）。

## 目录结构

```
📁engine/                      # 每个引擎自成一层
├── 📁krkrsdl3/
│   ├── 📁src/                 # XP3/TJS 内核源码（git 子模块）
│   ├── 📁cmake/               # 源码清单与目标接线（main/console/core/…/fuzz）
│   └── 📁tests/               # 离屏 AutoTest 宿主、TJS harness、bpg、fuzz
├── 📁openartemis/
│   ├── 📁src/                 # PFS/Artemis 内核源码（git 子模块）
│   ├── 📁cmake/               # 库/宿主/测试/工具目标接线
│   ├── 📁tests/               # 单元测试 + 真包回归 + fuzz
│   └── 📁tools/               # PFS/PSB/ASB/存档命令行工具与 Python 助手
└── 📁onscripter/
    ├── 📁src/                 # ONScripter 内核（ons / core / media / host）
    ├── 📁cmake/               # libonscripter + onscripter 目标接线
    └── 📁tests/               # 长跑/存档回归脚本
📁platform/                    # 平台壳
├── 📁android/                 # Gradle 工程：统一启动器 + 三个 Activity + 三个 .so
├── 📁ohos/                    # HarmonyOS 工程（krkrsdl3 原生 + 内核识别）
└── 📁emscripten/              # 浏览器页面壳（krkr wasm_shell + openartemis + onscripter）
📁Res/                         # krkrsdl3 程序资源（Android assets / wasm 预载）
📁script/                      # 各平台构建脚本
📁vcpkg/                       # 自定义 vcpkg ports/triplets（多内核共用依赖）
📄CMakeLists.txt/CMakePresets.json   # 根 CMake：统合三个引擎与平台
📄vcpkg.json/vcpkg-configuration.json
```

> FFmpeg 固定为 **6.1.1**（`vcpkg/ports/ffmpeg` overlay port）：三个内核都只做解码，
> 因此 configure 里额外关闭了网络、全部编码器、封装器和输入/输出设备
> （`--disable-network --disable-encoders --disable-muxers --disable-indevs --disable-outdevs`），
> 显著降低编译内存。
>
> Android 上该端口固定为共享构建（`libav*.so` / `libsw*.so`），三个内核共用一份；
> CMake 以 imported target 提供给 AGP，APK 会一并打包。其它平台仍静态链接。
> Emscripten 下 FFmpeg 不参与（`vcpkg.json` 里 `platform: "!emscripten"`），
> CMake 对 onscripter 也做了条件跳过。

## 子模块

首次克隆后初始化两个子模块（onscripter 直接放在 `engine/onscripter`，不是子模块）：

```bash
git submodule update --init --recursive
```

更新内核：进入对应子模块（`engine/krkrsdl3/src` 或 `engine/openartemis/src`）
`git pull` 后，在仓库根提交子模块指针。

## 外部环境依赖

- cmake ≥ 3.28 / ninja：跨平台构建工具
- vcpkg：包管理（桌面 / Android / WASM）
- LLVM-MinGW 或 Visual Studio 2022：Windows 构建工具链
- Android SDK / Android NDK r28：安卓构建工具链
- Emscripten SDK：WebAssembly 构建工具链
- DevEco Studio / command-line-tools：鸿蒙构建工具
- Xcode / Apple Command Line Tools：macOS 构建可用 CLT；iOS 需要完整 Xcode/iOS SDK

## 桌面构建（三个内核一起出）

```bash
# Windows（llvm-mingw，脚本会调用对应 preset）
script\build-windows.bat

# Linux
script/build-linux.sh

# macOS
script/build-macos.sh
```

产物（以 Windows 为例）：

- `out/windows/<Config>/krkrsdl3.exe` — XP3/TJS 内核
- `out/windows/<Config>/openartemis.exe` — Artemis/PFS 内核
- `out/windows/<Config>/onscripter.exe` — NScripter/ONS 内核

只构建其中一个内核：

```bash
# 只构建 krkrsdl3（以 Windows 为例）
cmake --preset "Windows Config" -DENGINE_OPENARTEMIS=OFF -DENGINE_ONSCRIPTER=OFF
cmake --build --preset "Windows Release Build"

# 只构建 openartemis（预设已带 OA_ENABLE_TESTS=ON）
cmake --preset "Windows Config OA"
cmake --build --preset "Windows Release Build OA"

# 只构建 onscripter（Linux / macOS 同名预设为 "Linux Config ONS" / "macOS Config ONS"）
cmake --preset "Windows Config ONS"
cmake --build --preset "Windows Release Build ONS"
```

onscripter 运行示例：

```bat
set ONS_GAME=D:/Games/SomeGame
out\windows-ons\Release\onscripter.exe "%ONS_GAME%"
out\windows-ons\Release\onscripter.exe "%ONS_GAME%" --save-dir out\ons_save
out\windows-ons\Release\onscripter.exe "%ONS_GAME%" --ons-frames 300 --ons-offscreen
out\windows-ons\Release\onscripter.exe "%ONS_GAME%" --ons-encoding gbk --ons-dump-frame out\ons.png
```

常用测试参数：`--ons-frames N`（渲染 N 帧后退出）、`--ons-offscreen`（无窗口）、
`--ons-dump-frame PATH` / `--ons-dump-at N`（PNG 快照）、`--ons-auto-click MS`、
`--ons-click-at N X,Y`、`--ons-key-at N KEY`、`--ons-start-label NAME`、
`--ons-encoding auto|gbk|sjis|utf8`、`--ons-audio-dump PATH`。

openartemis 单元测试（桌面）：

```bash
cmake --preset "Linux Config OA"
cmake --build --preset "Linux Release Build OA"
ctest --test-dir out/linux-oa -C Release
```

## Android（一个 APK，三个内核）

```bat
script\build-android.bat
```

- 脚本固定构建 release；需要 debug 时在 `platform/android` 下执行
  `gradlew assembleDebug`。
- 产物：`out/android/app/outputs/apk/release/NimbusVN_v<版本>.apk`。
- Gradle `externalNativeBuild` 一次编译 `libkrkrsdl3.so` + `libopenartemis.so` +
  `libonscripter.so`，以及共享的 `libSDL3.so`、`libav*.so`、`libsw*.so`，一起进入 APK。
- 启动器 `MainActivity` 扫描游戏目录（识别顺序 XP3 → PFS → ONS，避免误判）：
  - 发现 `*.xp3` / `startup.tjs` → 启动 **KRKRActivity**（krkrsdl3）
  - 发现 `*.pfs` / `system.ini` → 启动 **GameActivity**（openartemis）
  - 发现 `nscript.dat` / `*.nsa` / `arc.nsa` / `*.sar` → 启动 **ONSActivity**（onscripter）
  - 详情页可手动覆盖内核（自动 / XP3 / PFS / ONS），并按内核显示各自的
    单游戏选项：krkrsdl3 可选软件/GL/Vulkan，openartemis 可选 SDL/GL 与
    android/windows 平台段（默认 android），onscripter 无额外选项。
  - 选项按 `EngineOptions` 的能力表生成和拼参数，新增引擎/选项只改这一张表。
- ONS 的启动参数是 `<游戏目录>`（第一参数）；勾选「隔离存档目录」时追加
  `--save-dir <应用私有目录>/save/<游戏名>`。
- 只有 ONS 游戏界面固定为 `sensorLandscape`（默认横屏，支持左右横屏）；
  启动器不强制方向，跟随系统/设备当前方向；
  版本号唯一来源是根 `CMakeLists.txt` 的 `NIMBUSVN_VERSION`，APK 的
  `versionName` 与 `onscripter --version` 输出保持一致。
- ONScripter 触控映射（参考 krkrsdl3）：
  - 单指轻点 = 左键（推进对话/点选项）
  - **双指轻点 = 右键**（打开游戏自己的右键菜单）
  - 双指上下滑 = 滚轮（回想/翻页）
  - 三指轻点 = 右键（备用菜单手势）
  - 长按后拖动 = 按住左键拖动（滑杆/滚动条）
- 虚拟输入层（三个内核共用，代码在
  `platform/android/app/java/org/nimbusvn/input/`）：
  - 挂在 `SDLActivity` 的根布局上，KRKR / OpenArtemis / ONS 以及后续新增的
    SDL 内核自动继承，不需要各自实现；
  - **虚拟键盘和虚拟鼠标是两套独立开关**：右上角工具条分别点「键盘」「鼠标」，
    互不影响，可以同时开启；两个都关就是纯触摸；
  - **虚拟键盘**默认加载一套内置键位（方向盘 + ABXY + L/R + 菜单/跳过），
    每个键都是独立的 `VirtualKey`：设置页里可以逐键改映射、拖动位置、
    用底部滑杆调大小、删除，也可以「＋ 添加按键」从键位目录新增；
    默认键位见 `VirtualKey.defaults()`；
  - **虚拟鼠标**：只做定位和 hover，屏幕上没有 L/R/M 按键。单指拖动移动
    光标并持续发 MouseMove；单指轻点在光标处按左键；长按不动再拖 =
    按住左键拖拽（滑块/滚动条）；双指轻点右键；双指上下滑滚轮。
    普通拖动过程不按任何鼠标键，不会“拖一下就点一下”。鼠标开启时所有触摸
    在 overlay 根部截断，内核收不到任何 `FINGER_*`，触摸手势被完全隔离；
  - 右上角工具条：鼠标/键盘开关、系统输入法、设置、收起；收起只把工具条
    缩成一个圆形「≡」，当前控件保持显示；收起状态会记住；
  - 设置页可调手柄大小、光标大小、透明度、是否显示操作提示；
    配置存在 `SharedPreferences("nimbus_vinput")`，对所有内核生效；
  - 全局设置 → 游戏内控件 → 「启用游戏内虚拟手柄 / 鼠标」是总开关；关掉后
    游戏里不挂任何悬浮控件（包括右上角「≡」），下次启动游戏生效；
  - 注入只走 `SDLActivity.onNativeMouse / onNativeKeyDown / onNativeKeyUp`，
    不新增 `.so`；`krkrsdl3` 的 Android 构建已打开鼠标事件分支并过滤
    `SDL_TOUCH_MOUSEID`，openartemis / onscripter 原本就接受 SDL 鼠标事件。
    光标坐标是 Surface 像素，各内核再自行映射到逻辑分辨率。
- 需要 `VCPKG_ROOT`、`ANDROID_NDK_HOME`（r28）、`ANDROID_HOME`、JDK 17。

## HarmonyOS（结构多内核，krkrsdl3 原生）

```bat
script\build-ohos.bat <sdk-root> full arm64-v8a debug
```

- 工程位于 `platform/ohos`，原生构建直接使用仓库根 `CMakeLists.txt`。
- 启动器（`Index.ets`）按扩展名识别内核：`data.xp3` → krkrsdl3；
  `*.pfs` → openartemis（当前构建未包含该内核时提示）；
  `nscript.dat` / `*.nsa` → onscripter（当前构建未包含该内核时提示）。
- openartemis 的 OHOS 原生后端（EGL/音频/输入）尚未实现，因此
  `ENGINE_OPENARTEMIS` 在 OHOS 上默认关闭；`ENGINE_ONSCRIPTER` 同理。

## WebAssembly（三个页面壳）

详细启动参数、数据准备与服务器用法见
`platform/emscripten/使用方法.md`。

```bat
call C:\path\to\emsdk\emsdk_env.bat
script\build-emscripten.bat
```

产物分目录输出，互不覆盖：

- `out/emscripten/krkrsdl3/<Config>/index.html`（krkrsdl3；预载 `Res/`）
- `out/emscripten/openartemis/<Config>/index.html`（openartemis；页面内选包/目录）
- `out/emscripten/onscripter/<Config>/index.html`（onscripter；页面内选目录/多选文件）

onscripter 页面壳把选中的目录按相对路径写进 MEMFS `/data`（保留
`nscript.dat`、`*.nsa`、`default.ttf`、图片等），再调用
`Module.callMain(["/data", ...])`；支持 `?dir=/data` 与
`?args=--ons-encoding,gbk`。存档写到 `/save`（IDBFS → IndexedDB）。

openartemis 页面壳需要 COOP/COEP 才能使用多线程：

```bash
python platform/emscripten/serve.py out/emscripten/openartemis/Release 8080
```

## 测试

- krkrsdl3：`engine/krkrsdl3/tests/autotest.md`（离屏 AutoTest / `-offscreen` /
  `-frames` / `-dump`）+ `engine/krkrsdl3/tests/fuzz`（TJS2 fuzzer）。
- openartemis：`engine/openartemis/tests/README.md` 的 `OA_*` 环境变量；
  桌面配置 `-DOA_ENABLE_TESTS=ON` 后 `ctest`。
- onscripter：`engine/onscripter/README.md` 的无头测试参数
  （`--ons-frames` / `--ons-offscreen` / `--ons-dump-frame` / 注入点击与按键）；
  `engine/onscripter/tests/run_stability.py` 为长跑回归，游戏目录通过
  `--game` / `--clannad` / `--ever17` 显式传入，脚本内不写死本机路径。

## 许可

- `engine/krkrsdl3`：遵循其原始 Kirikiri/Z 许可（见根 `LICENSE` 前半部分）。
- `engine/openartemis`：**MIT 或 GPL-3.0-or-later 二选一**，见
  `engine/openartemis/src/LICENSE`。
- `engine/onscripter`：源自 **ONScripter，GPLv2**（`engine/onscripter/COPYING`）。
  这意味着**包含该内核的整体分发（APK / 桌面包）必须按 GPLv2 分发**，
  并随包提供对应源码；单独构建/分发其它内核时不受该内核的 GPL 约束。
- 仓库根 `LICENSE` 末尾补充了多内核分发的说明；`third_party/` 与 vcpkg
  依赖分别遵循各自原始许可。

系统越复杂问题越难以排除，所以非必要时请保持当前的构建系统。
