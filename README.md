# NimbusVN — 多内核视觉小说引擎

NimbusVN 是 **krkrsdl3** 与 **openartemis** 两个视觉小说引擎内核的统一构建与发行项目。

两个内核共享同一套平台壳、依赖与发行物：

| 内核 | 游戏格式 | 仓库 | 原生入口 |
|---|---|---|---|
| **krkrsdl3** | XP3 + TJS（KAG） | `engine/krkrsdl3`（子模块） | `libkrkrsdl3.so` / `krkrsdl3` |
| **openartemis** | PFS / 解包工程（Artemis） | `engine/openartemis`（子模块） | `libopenartemis.so` / `openartemis` |

桌面（Windows/Linux/macOS）与 WebAssembly 在一次构建里同时产出两个内核；
Android APK（`org.nimbusvn`，应用名 **NimbusVN**）同时打包两个原生库，由启动器**按游戏目录内容自动识别内核**；
OHOS 完成工程与启动器的多内核接入，当前原生只构建 krkrsdl3
（openartemis 的 OHOS 后端尚未实现，PFS 游戏会得到明确提示）。

## 目录结构

```
📁engine/                      # 每个引擎自成一层
├── 📁krkrsdl3/
│   ├── 📁src/                 # XP3/TJS 内核源码（git 子模块）
│   ├── 📁cmake/               # 源码清单与目标接线（main/console/core/…/fuzz）
│   └── 📁tests/               # 离屏 AutoTest 宿主、TJS harness、bpg、fuzz
└── 📁openartemis/
    ├── 📁src/                 # PFS/Artemis 内核源码（git 子模块）
    ├── 📁cmake/               # 库/宿主/测试/工具目标接线
    ├── 📁tests/               # 单元测试 + 真包回归 + fuzz
    └── 📁tools/               # PFS/PSB/ASB/存档命令行工具与 Python 助手
📁platform/                    # 平台壳
├── 📁android/                 # Gradle 工程：统一启动器 + 两个 Activity + 两个 .so
├── 📁ohos/                    # HarmonyOS 工程（krkrsdl3 原生 + 内核识别）
└── 📁emscripten/              # 浏览器页面壳（krkr wasm_shell + openartemis shell）
📁Res/                         # krkrsdl3 程序资源（Android assets / wasm 预载）
📁script/                      # 各平台构建脚本
📁vcpkg/                       # 自定义 vcpkg ports/triplets（多内核共用依赖）
📄CMakeLists.txt/CMakePresets.json   # 根 CMake：统合两个引擎与平台
📄vcpkg.json/vcpkg-configuration.json
```

> FFmpeg 固定为 **6.1.1**（`vcpkg/ports/ffmpeg` overlay port）：两个内核都只做解码，
> 因此 configure 里额外关闭了网络、全部编码器、封装器和输入/输出设备
> （`--disable-network --disable-encoders --disable-muxers --disable-indevs --disable-outdevs`），
> 显著降低编译内存。
>
> Android 上该端口固定为共享构建（`libav*.so` / `libsw*.so`），两个内核共用一份；
> CMake 以 imported target 提供给 AGP，APK 会一并打包。其它平台仍静态链接。

## 子模块

首次克隆后初始化两个内核：

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

## 桌面构建（两个内核一起出）

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

只构建其中一个内核：

```bash
# 只构建 krkrsdl3（以 Windows 为例）
cmake --preset "Windows Config" -DENGINE_OPENARTEMIS=OFF
cmake --build --preset "Windows Release Build"

# 只构建 openartemis（预设已带 OA_ENABLE_TESTS=ON）
cmake --preset "Windows Config OA"
cmake --build --preset "Windows Release Build OA"
```

openartemis 单元测试（桌面）：

```bash
cmake --preset "Linux Config OA"
cmake --build --preset "Linux Release Build OA"
ctest --test-dir out/linux-oa -C Release
```

## Android（一个 APK，两个内核）

```bat
script\build-android.bat
```

- 脚本固定构建 release；需要 debug 时在 `platform/android` 下执行
  `gradlew assembleDebug`。
- 产物：`out/android/app/outputs/apk/release/NimbusVN_v<版本>.apk`。
- Gradle `externalNativeBuild` 一次编译 `libkrkrsdl3.so` + `libopenartemis.so`，
  以及共享的 `libSDL3.so`、`libav*.so`、`libsw*.so`，一起进入 APK。
- 启动器 `MainActivity` 扫描游戏目录：
  - 发现 `*.xp3` / `startup.tjs` → 启动 **KRKRActivity**（krkrsdl3）
  - 发现 `*.pfs` / `system.ini` → 启动 **GameActivity**（openartemis）
  - 详情页可手动覆盖内核（自动 / XP3 / PFS）。
- 需要 `VCPKG_ROOT`、`ANDROID_NDK_HOME`（r28）、`ANDROID_HOME`、JDK 17。

## HarmonyOS（结构多内核，krkrsdl3 原生）

```bat
script\build-ohos.bat <sdk-root> full arm64-v8a debug
```

- 工程位于 `platform/ohos`，原生构建直接使用仓库根 `CMakeLists.txt`。
- 启动器（`Index.ets`）按扩展名识别内核：`data.xp3` → krkrsdl3；
  `*.pfs` → openartemis（当前构建未包含该内核时提示）。
- openartemis 的 OHOS 原生后端（EGL/音频/输入）尚未实现，因此
  `ENGINE_OPENARTEMIS` 在 OHOS 上默认关闭。

## WebAssembly（两个页面壳）

详细启动参数、数据准备与服务器用法见
`platform/emscripten/使用方法.md`。

```bat
call C:\path\to\emsdk\emsdk_env.bat
script\build-emscripten.bat
```

产物分目录输出，互不覆盖：

- `out/emscripten/krkrsdl3/<Config>/index.html`（krkrsdl3；预载 `Res/`）
- `out/emscripten/openartemis/<Config>/index.html`（openartemis；页面内选包/目录）

openartemis 页面壳需要 COOP/COEP 才能使用多线程：

```bash
python platform/emscripten/serve.py out/emscripten/openartemis/Release 8080
```

## 测试

- krkrsdl3：`engine/krkrsdl3/tests/autotest.md`（离屏 AutoTest / `-offscreen` /
  `-frames` / `-dump`）+ `engine/krkrsdl3/tests/fuzz`（TJS2 fuzzer）。
- openartemis：`engine/openartemis/tests/README.md` 的 `OA_*` 环境变量；
  桌面配置 `-DOA_ENABLE_TESTS=ON` 后 `ctest`。

## 许可

- `engine/krkrsdl3`：遵循其原始 Kirikiri/Z 许可（见根 `LICENSE` 前半部分）。
- `engine/openartemis`：**MIT 或 GPL-3.0-or-later 二选一**，见
  `engine/openartemis/src/LICENSE`。
- 仓库根 `LICENSE` 末尾补充了多内核分发的说明；`third_party/` 与 vcpkg
  依赖分别遵循各自原始许可。

系统越复杂问题越难以排除，所以非必要时请保持当前的构建系统。
