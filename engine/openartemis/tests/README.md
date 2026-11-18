# openartemis 测试与运行指南 (TESTING)

本文件是 openartemis 的**测试布局**与**仍然生效的 `OA_*` 环境变量清单**。
仓库内唯一权威清单：改动源码里的 `getenv("OA_...")` 时同步本表；删掉的开关
不再保留历史说明。

> 路径约定：本文件位于 `engine/openartemis/tests/`。引擎源码路径相对
> `engine/openartemis/src/`；下文测试源码路径中的 `tests/` 指本目录
> （如 `tests/fpm/x.cpp` = `engine/openartemis/tests/fpm/x.cpp`），
> 构建目录统一在仓库根的 `out/` 下。

---

## 1. app 程序

只有一个：`openartemis`（`environ/main.cpp`）。ctest 不跑 app；app 层验证
= 跑真包冒烟并 `--dump` 一帧。

### CLI

```
openartemis [options] [project.pfs]     # pfs 或解包目录（含 system.ini）
  --frames N          跑 N 帧后退出（结束时写 --dump）
  --dump PATH         把最后一帧写成 PPM
  --headless          无窗口（固定 16ms tick）
  --fixed-delta-ms N  确定性步长（窗口模式也可用）
  --fps N             目标帧率（缺省按真实时钟）
  --platform NAME     windows / android / wasm（缺省=构建平台）
  --renderer NAME     sdl（默认）/ gles
  --mute              静音
  -h, --help
```

窗口模式 Esc / 关窗 / 游戏自身 exit 退出；存档根默认 = 游戏数据所在目录
（可用 `OA_SAVE_ROOT` 覆盖）。

离屏/无人值守跑一律加 `--mute`：**不初始化 SDL 音频、不打开设备**。没有
sink，通道混音和影片自带音轨都无处可去，扬声器上不可能出声（以前只是把
主音量置 0，而视频音频走自己的设备流、不经过通道混音器，会漏出来）。解码
与完成事件照常跑，状态机不受影响。

---

## 2. 测试布局

- `tests/` — **通用引擎单测**（自含 fixture，不需要真实游戏包）：
  compositor / layer_semantics / transform / render2 / text_domain / text_reveal /
  media_state / media_runtime / video_decode / video_mask / decode_pool /
  save_domain / save_compat / save_thumb_readback / p1c2_control /
  backlog_var_bridge / misc_b / emote_track_sample / emote_warp 等。
- `tests/fpm/` — **真实游戏游程**（缺包时自跳 exit 77）。**一个游戏一个文件**：
  `game_<game>_test.cpp`。没有“通用游程”文件 —— 启动形状（标题 → 语言选择
  → 开场 → 角色选择 → 正文）每个游戏都不一样，属于它自己的文件；同一个游戏
  要加流程就在该文件里加一个 `case_*`，共用同一个 `boot()`。
  公共设施只有两个：
  - `test_host.h` — **核心测试包装类**：把 `main.cpp` 的初始化全包掉
    （SDL/隐藏窗口/真后端/静音/固定步长/临时存档目录；静音 = **不打开音频
    设备**，连视频自带音轨也无处输出），事件走
    `App::mouse_*` / `App::key_event`（和窗口模式同一条路径），并提供
    `plan()`（渲染队列：id/世界矩形/透明度/是否文字项）、`shot()`/`save_png()`、
    `click_key()`/`has_button()`/`click_text()`/`wait_for()` 等判定能力。
  - `crash_backtrace.{h,cpp}` — 崩溃时打印 `[crash] #NN +0x偏移`，配
    `addr2line` 直接定位到源码行。

  8 个包（变量名里的缩写是发行包标识，不是游戏名）：

  | 变量 | 游戏 |
  |---|---|
  | `OA_TEST_NEKOMIKO_PFS` | NekoMiko（qureate） |
  | `OA_TEST_FPM_PFS` | ハミダシクリエイティブ（中文版） |
  | `OA_TEST_TG3_PFS` | 甜蜜女友3 |
  | `OA_BT91_PFS` | ヘンタイ・プリズン |
  | `OA_TEST_T5_PFS` | 乙女理論とその周辺 -Ecole de Paris- |
  | `OA_TEST_T13_PFS` | 穢翼のユースティア |
  | `OA_TEST_HCT_PFS` | ハミダシクリエイティブ凸 |
  | `OA_TEST_RR_PFS` | 常轨脱离ReReCall |

包路径在 configure 期用 `-D` 或环境变量给（cache 优先于 env）；两边都没给时
相关测试自跳（exit 77）。例：

```sh
# Windows 为例；Linux/macOS 换成对应预设
cmake --preset "Windows Config" -DOA_ENABLE_TESTS=ON \
  -DOA_TEST_FPM_PFS=/games/fpm/root.pfs \
  -DOA_TEST_NEKOMIKO_PFS=/games/nm/root.pfs \
  -DOA_TEST_TG3_PFS=/games/tg3/root.pfs \
  -DOA_BT91_PFS=/games/bt91/root.pfs \
  -DOA_TEST_T5_PFS=/games/t5/root.pfs \
  -DOA_TEST_T13_PFS=/games/t13/root.pfs \
  -DOA_TEST_HCT_PFS=/games/hct/root.pfs \
  -DOA_TEST_RR_PFS=/games/rr/root.pfs
cmake --build --preset "Windows Release Build"
```

---

## 3. 环境变量

### A. 宿主 / app

| 变量 | 作用 | 示例 |
|---|---|---|
| `OA_PFS` | 无位置参数时的默认项目（没有本机硬编码默认值） | `OA_PFS=/x/root.pfs ./openartemis --frames 900` |
| `OA_SAVE_ROOT` | 覆盖可写存档根目录（默认=平台默认：desktop 用游戏数据目录，android 用 app 私有目录，wasm 用 IDBFS `/save`） | `OA_SAVE_ROOT=/tmp/s1 ./openartemis a.pfs` |
| `OA_RENDERER` | 默认渲染后端 `sdl`（默认）/ `gles`，等价于 `--renderer` | `OA_RENDERER=gles ./openartemis a.pfs` |

### B. 回归门控（configure 期）

| 变量 | 作用 |
|---|---|
| `OA_TEST_FPM_PFS` | ハミダシクリエイティブ（中文版）包路径；`game_fpm_test` 与少量通用测试（asb/ini/script/video_decode 等）靠它工作 |
| `OA_TEST_NEKOMIKO_PFS` | NekoMiko：`game_nekomiko_test` |
| `OA_TEST_TG3_PFS` | 甜蜜女友3：`game_tg3_test` |
| `OA_BT91_PFS` | ヘンタイ・プリズン：`game_bt91_test` |
| `OA_TEST_T5_PFS` | 乙女理論とその周辺 -Ecole de Paris-：`game_t5_test` |
| `OA_TEST_T13_PFS` | 穢翼のユースティア：`game_t13_test` |
| `OA_TEST_HCT_PFS` | ハミダシクリエイティブ凸：`game_hct_test` |
| `OA_TEST_RR_PFS` | 常轨脱离ReReCall：`game_rr_test` |

### C. 引擎诊断（core 内 env 门控，不设即零开销零输出）

| 变量 | 作用 | 模块 |
|---|---|---|
| `OA_SCENEDUMP` | 按 id 前缀转储场景节点（`id left top w h vis alpha eff file`），`""` = 全部。核对"某层到底在不在/位置对不对" | `core/runtime/runtime.cpp` |
| `OA_VARDUMP` | 结束前按前缀导出引擎变量表（local/temp/global/system），`""` = 全部 | 同上 |
| `OA_TXTDUMP` | 转储每个消息层（`chars/shown/done/hidden/positioned/page/alpha/vis/box/size/align/sample`）。区分"文本没派发进来"（全层 `chars=0`）与"进了错的层/没定位" | 同上 |
| `OA_PLAN_REUSE` | 帧计划复用开关（默认开）。计划按"层容器版本 + 文本版本 + 全局淡出 + stage 尺寸"缓存；宿主供帧不失效计划。`=0` 强制每帧重建，用于定位"复用门漏失效信号"（A/B：同帧开/关比像素与 `[scene]`） | `core/render/renderer.cpp` |
| `OA_STACKDBG` | 每次 `e:getScriptStack()` 打一行 `[stack] file@line …`（事件处理帧显示为空文件名）。核对"框架看到的栈"与"引擎真实的栈"是否一致——UI 会话/backlog 关闭路径的落点全靠这一层 | `core/runtime/runtime_iet.cpp` |
| `OA_PARKDBG` | 每次武装停驻打一行 `[park] arm <owner>:<line> (now …) stage/click/timed/owned/wakes`。定位"这一页到底停在谁身上、被哪个 wake 放行" | `core/runtime/runtime.cpp` |

### D. 游程排查开关（非 ctest，report-only）

只在排查某个游戏的启动/推进时用，正常跑不设：

| 变量 | 作用 |
|---|---|
| `OA_JOURNEY_TRACE` | 每若干帧打印当前页文本、停驻/可点状态、可点层（`com=/def=/key=` + 世界矩形）、渲染队列项。启动流程对不上时先看这里，不要猜按钮 |
| `OA_JOURNEY_UI_KEY` | 指定 `case_ui_toggle` 点哪个按钮（默认按游戏自己的 `def`/`com`，如 `load`） |
| `OA_JOURNEY_LOAD_KEY` | NekoMiko 存读档用例里"读档"按钮名 |
| `OA_JOURNEY_LIST` | NekoMiko：只列出当前屏的可点按钮后退出 |
| `OA_JOURNEY_SHOT` | NekoMiko：把该帧落成 PNG 的目录（`test_host::save_png`） |
| `OA_JOURNEY_NO_RENDER` | NekoMiko：不接渲染器（纯数据二分，用来判断问题在数据层还是绘制层） |

> `OA_FRAMES` / `OA_SAMPLE_ALL` / `OA_SAMPLE_LEGACY` 仍属 `tools/psb.cpp`；
> `OA_TEST_OFFICIAL_SAVE_DIR` 属 `tests/official_save_test.cpp`。

---

## 4. 快速验证

```sh
cmake --preset "Windows Config" -DOA_ENABLE_TESTS=ON
cmake --build --preset "Windows Release Build"
ctest --test-dir out/windows -C Release          # 41 项

# app 冒烟（离屏 + 一帧 PPM）
OA_SAVE_ROOT=/tmp/s ./out/windows/Release/openartemis.exe \
  --frames 300 --mute --fps 60 --dump /tmp/frame.ppm /path/root.pfs

# 视觉核对某层/某段文本（结束前转储）
OA_SCENEDUMP="500" OA_TXTDUMP=1 OA_SAVE_ROOT=/tmp/s \
  ./out/windows/Release/openartemis.exe --headless --frames 600 /path/root.pfs
```

---

## 5. sanitizer / fuzz（llvm-mingw）

Windows 上的内存错误检查走 **llvm-mingw（clang 17）**：它有 `libclang_rt.asan` 和
`libclang_rt.fuzzer`，同一个编译器既能编 ASan 也能编 libFuzzer。MSVC 的
`/fsanitize=address` 也能跑 ASan 构建，但没有 libFuzzer，只能走暴力变异模式。

前置：把 llvm-mingw 的 `bin` 放进 PATH（vcpkg 的 mingw 端口是按
`x86_64-w64-mingw32-gcc` 找编译器的，llvm-mingw 提供了同名别名，指向 clang）：

```sh
export PATH="/path/llvm-mingw1706_64/bin:$PATH"
```

### 构建

```sh
cmake --preset mingw-asan-fuzz      # x64-mingw-static + ASan + fuzz 目标
cmake --build --preset mingw-asan-fuzz -j
ctest --test-dir out/mingw-asan --output-on-failure   # 全量单测跑在 ASan 下
```

另有 `"Windows Config" + -DOA_ENABLE_TESTS=ON`（Release、只带单测）用来验证正常产物。
依赖（vcpkg 的 SDL3/freetype/ffmpeg…）**不插桩**——ASan 只能看见本工程自己的
堆/栈/悬垂访问，这是有意的取舍（给 ffmpeg 编 ASan 不值当）。

### 解析面 fuzz

四个目标共用 `tests/fuzz/oa_fuzz.cpp`，编译期宏选输入面：

| 目标 | 输入面 | 典型问题 |
| --- | --- | --- |
| `oa_fuzz_psb` | E-mote PSB 容器（表/对象/资源区） | 越界读、偏移未校验 |
| `oa_fuzz_iet` | 剧本文本 + ASB 二进制（先 decode_asb 再 parse） | 解析器越界/死循环 |
| `oa_fuzz_image` | PNG/JPEG 解码 + PNG tEXt | 解码器尺寸/行宽计算 |
| `oa_fuzz_save` | 官方存档 BOWX/BOWG/BOWS（zlib + 尾部目录） | 目录/字段越界 |

Clang 构建下它们就是 libFuzzer（覆盖率引导、自动最小化、corpus 可累积）。
llvm-mingw 的驱动**拒绝** `-fsanitize=fuzzer`，但 clang 自带的运行时库还在，CMake
会自己找到并手动链接（`libclang_rt.fuzzer-x86_64.a`，来自 `clang -print-resource-dir`），
覆盖率用 `-fsanitize-coverage=inline-8bit-counters,pc-table,trace-cmp` 显式打开。

```sh
mkdir -p /tmp/corpus_psb
./out/mingw-asan/openartemis-fuzz/oa_fuzz_psb /tmp/corpus_psb -max_total_time=300
```

`tests/fuzz/asan_defaults.cpp` 把 ASan 默认选项导出（必须 dllexport：ASan 运行时
是按导出表找 `__asan_default_options` 的），其中 `detect_container_overflow=0`
是因为 libFuzzer 自己枚举语料时会被 libc++ 容器注解误报，跟被测代码无关。

非 Clang 构建下同一个可执行文件退化成自带暴力变异循环（可选给一个语料目录当种子）：

```sh
OA_FUZZ_ITERS=200000 OA_FUZZ_SEED=1 ./oa_fuzz_psb /path/to/corpus
```

| env | 含义 |
| --- | --- |
| `OA_FUZZ_ITERS` | 暴力模式迭代次数（默认 20000） |
| `OA_FUZZ_SEED` | 变异种子（默认固定，便于复现） |
| `OA_FUZZ_MAX_INPUT` | 单个输入上限字节数（默认 256 KiB） |
| `OA_FUZZ_DUMP` | 最后一个输入的落盘路径（默认 `oa_fuzz_last.bin`），崩了用它复现 |

### 暴力驱动（真包猴子）

`oa_monkey` 把真包塞进 `GameRuntime`，随机点击/右键/按键/移动一直 tick；加
`--render` 还会建一个隐藏窗口每帧真渲染（视频纹理上传、组离屏合成只有在真渲染
时才跑得到）。每帧的输入先写进 trace，ASan 报错时 trace 最后一行就是复现序列。

```sh
./out/mingw-asan/openartemis-fuzz/oa_monkey /path/to/root.pfs \
    --frames 5000 --seed 7 --render --trace /tmp/monkey.trace
```

| 参数 | 含义 |
| --- | --- |
| `--frames N` | 走多少帧（默认 3000） |
| `--seed S` | 随机种子（默认固定） |
| `--render` | 每帧真渲染（隐藏窗口） |
| `--trace FILE` | 输入序列落盘（默认 `oa_monkey.trace`） |

脚本错误（剧本/资源缺失）在暴力输入下属预期，猴子会记账继续跑；只有 ASan 报告、
断言或信号才算发现。

### 并发压力（oa_stress）

`oa_stress` 不靠随机点击碰运气，而是把**会起线程的子系统**同时摇起来，专打
worker 线程与主线程的交错（安卓上那次 video 驱动线程写坏纹理表就是这么炸的）：

| 角色 | 被压的线程面 |
| --- | --- |
| `video` | `VideoEngine` 的 driver 线程解码 + 推帧；主线程高频起停/收尾/落纹理 |
| `audio` | decode pool worker + 每通道 prefetch 线程 + SDL 设备回调线程；主线程起停/淡入淡出/改音量 |
| `emote` | PSB 并行解码线程池 + 画布合成；同 id 反复重建（替换旧 player） |
| `layers` | 主线程建/删/改图层（几何、裁剪、掩码），逼计划重建与栅格化 |

`tick` 永远在跑。加 `--render` 时用 **runtime 自己的渲染器**（和 app/安卓同一条
主线程路径：tick 里直接 `render_frame`，HostTexture flush、组离屏合成、视频音轨
都在生产路径上）；`--silent` 把主音量归零但设备照开（回调线程照跑，夜里不吵）。

```sh
./out/mingw-asan/openartemis-fuzz/oa_stress /path/to/root.pfs \
    --minutes 10 --threads 2 --render --silent --seed 7
```

| 参数 | 含义 |
| --- | --- |
| `--minutes M` | 跑多少分钟（默认 5） |
| `--threads N` | N 个独立 runtime 并行（查跨实例共享状态） |
| `--roles csv` | `video,audio,layers,emote`（默认全开；找不到资产的角色自动跳过） |
| `--render` | 挂 runtime 自己的渲染器（隐藏窗口）+ 音频设备 |
| `--silent` | 主音量 0（设备仍开） |
| `--video/--audio/--emote P` | 手工指定资产（包里没找到时用） |
| `--seed S` | 随机种子 |

### 跑一夜

```sh
bash script/openartemis-nightly-fuzz.sh out/mingw-asan 7 20   # 构建目录 小时数 每目标切片分钟
```

三线并行（各占各的核）：6 个输入面按切片轮转、每个真包猴子跑满 30 分钟、每个
真包并发压力跑满 10 分钟（`--render --silent`）。语料跨轮累积，崩溃样本落在
`<build>/nightly/artifacts/`，进度一行一条写在 `<build>/nightly/summary.txt`。
Windows 上等价入口是 `script/openartemis-nightly-fuzz.ps1`（同样的产物布局，`-StressMinutes`
控制压力额度）。
