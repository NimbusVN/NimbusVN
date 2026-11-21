#!/usr/bin/env bash
# ASan 下的 fuzz + 真包猴子，两条线**并行**跑（互不抢时间）。
#
#   bash script/openartemis-nightly-fuzz.sh [build-dir] [hours] [fuzz-slice-min] [monkey-min]
#
#   build-dir        默认 out/mingw-asan
#   hours            总时长（默认 6）
#   fuzz-slice-min   每个 libFuzzer 目标一轮多少分钟（默认 20）
#   monkey-min       每个真包猴子跑多少分钟（默认 30；0 = 只做冒烟轮转）
#   stress-min       每个真包并发压力跑多少分钟（默认 10；0 = 不跑）
#
# 产物（都在 <build>/nightly/）：
#   summary.txt            进度/结果，一行一件事
#   <target>.log           每个 fuzz 目标的完整输出
#   monkey-<pkg>.log       每个真包猴子的完整输出
#   artifacts/             fuzz 崩溃样本（libFuzzer 的 crash-* / oom-*）
#   monkey-<pkg>-<seed>.trace  猴子崩溃时的输入序列（没崩就删掉）
#
# 真包是**扫出来的**（out/packages/*/root.pfs），新加游戏不用改脚本；猴子按包轮转，
# 每个包跑满 monkey-min 分钟而不是几百帧 —— 内存增长、状态机卡死、视频/音频
# 流这类问题只有跑够时间才出得来。
set -u
cd "$(dirname "$0")/.." || exit 1

BUILD="${1:-out/mingw-asan}"
HOURS="${2:-6}"
SLICE_MIN="${3:-20}"
MONKEY_MIN="${4:-30}"
STRESS_MIN="${5:-10}"
OUT="$BUILD/nightly"
ART="$OUT/artifacts"
CORPUS_ROOT="$OUT/corpus"
FUZZ="$BUILD/openartemis-fuzz"
mkdir -p "$ART" "$CORPUS_ROOT"
SUMMARY="$OUT/summary.txt"
CRASH_FILE="$OUT/crashes.count"

# libFuzzer 枚举语料时会被 libc++ 容器注解误报成 container-overflow；长驻进程
# 的泄漏报告也没意义。
# allocator_may_return_null=1：分配失败返回 null 而不是 ReportMmapFailureAndDie。
# 这不是"放过 OOM"——引擎里每个大分配都要接住 bad_alloc 并优雅失败（emote
# 图集已经这么做了），跑批要能看见"哪条路径没接住"，而不是整进程被带走。
# libFuzzer 自己也默认这么配（配合 -rss_limit_mb 用）。
export ASAN_OPTIONS="detect_leaks=0:abort_on_error=1:detect_container_overflow=0:allocator_may_return_null=1"
export UBSAN_OPTIONS="print_stacktrace=1"

# 一行一条写进 summary：两条线并行，整行写才是原子的。
say() { printf '%s  %s\n' "$(date '+%m-%d %H:%M:%S')" "$*" >> "$SUMMARY"; }

# OOM **算崩溃**：谁申请了这么大的内存才是根因（通常是未受限的不可信
# 尺寸）。这里只负责把"死于分配失败"和"死于越界"分开标，方便一眼归类，
# 两者都写进 crashes.count —— 别让一夜的成果看起来是干净的。
is_oom() {
  grep -qE "ReportMmapFailureAndDie|MmapOrDie|out of memory|failed to allocate|VirtualAlloc" "$1" 2>/dev/null
}

[ -d "$FUZZ" ] || { say "FATAL 找不到 $FUZZ"; exit 1; }

# 真包只从 out/ 里找（out/ 是 gitignore 的临时区，工作区根目录不放东西）。
# 换目录用 OA_FUZZ_PACKAGES=<dir>，里面按 <包名>/root.pfs 摆。
PKG_DIR="${OA_FUZZ_PACKAGES:-out/packages}"
PACKAGES=()
for f in "$PKG_DIR"/*/root.pfs; do [ -f "$f" ] && PACKAGES+=("$f"); done

DEADLINE=$(( $(date +%s) + HOURS * 3600 ))
: > "$CRASH_FILE"
say "START build=$BUILD hours=$HOURS slice=${SLICE_MIN}m monkey=${MONKEY_MIN}m stress=${STRESS_MIN}m pkgs=${#PACKAGES[@]}"

monkey_loop() {
  local monkey="$FUZZ/oa_monkey.exe"
  if [ ! -f "$monkey" ]; then say "SKIP monkey（没构建出来）"; return; fi
  if [ "${#PACKAGES[@]}" -eq 0 ]; then say "SKIP monkey（out/packages/*/root.pfs 一个都没有）"; return; fi
  local i=0
  while [ "$(date +%s)" -lt "$DEADLINE" ]; do
    local pkg="${PACKAGES[$((i % ${#PACKAGES[@]}))]}"
    i=$((i + 1))
    [ "$(date +%s)" -lt "$DEADLINE" ] || break
    local seed=$(( (RANDOM << 15 | RANDOM) % 2000000000 ))
    local name
    name=$(basename "$(dirname "$pkg")")
    local trace="$OUT/monkey-$name-$seed.trace"
    say "RUN  monkey $name minutes=$MONKEY_MIN seed=$seed"
    if [ "$MONKEY_MIN" -gt 0 ]; then
      "$monkey" "$pkg" --minutes "$MONKEY_MIN" --frames 2000000 --seed "$seed" \
          --render --trace "$trace" >> "$OUT/monkey-$name.log" 2>&1
    else
      "$monkey" "$pkg" --frames 400 --seed "$seed" --render --trace "$trace" \
          >> "$OUT/monkey-$name.log" 2>&1
    fi
    if [ $? -ne 0 ]; then
      if is_oom "$OUT/monkey-$name.log"; then
        echo x >> "$CRASH_FILE"
        say "CRASH-OOM monkey $name seed=$seed trace=$trace"
      else
        echo x >> "$CRASH_FILE"
        say "CRASH monkey $name seed=$seed trace=$trace"
      fi
    else
      say "OK   monkey $name"
      rm -f "$trace"
    fi
  done
}

stress_loop() {
  local exe="$FUZZ/oa_stress.exe"
  [ "$STRESS_MIN" -gt 0 ] || return
  if [ ! -f "$exe" ]; then say "SKIP stress（没构建出来）"; return; fi
  if [ "${#PACKAGES[@]}" -eq 0 ]; then return; fi
  local i=0
  while [ "$(date +%s)" -lt "$DEADLINE" ]; do
    local pkg="${PACKAGES[$(( (i + 3) % ${#PACKAGES[@]} ))]}"  # 与猴子错开包
    i=$((i + 1))
    [ "$(date +%s)" -lt "$DEADLINE" ] || break
    local seed=$(( (RANDOM << 15 | RANDOM) % 2000000000 ))
    local name
    name=$(basename "$(dirname "$pkg")")
    say "RUN  stress $name minutes=$STRESS_MIN seed=$seed"
    # --render：走 runtime 自己的渲染器（与 app/安卓同一条主线程路径：tick 里
    # 直接 render_frame），音频设备/视频音轨/纹理 flush 都在这条线上；
    # --silent：设备照开（回调线程照跑），只是主音量归零，夜里不吵。
    "$exe" "$pkg" --minutes "$STRESS_MIN" --threads 1 --seed "$seed" --render --silent \
        >> "$OUT/stress-$name.log" 2>&1
    if [ $? -ne 0 ]; then
      if is_oom "$OUT/stress-$name.log"; then
        echo x >> "$CRASH_FILE"
        say "CRASH-OOM stress $name seed=$seed"
      else
        echo x >> "$CRASH_FILE"
        say "CRASH stress $name seed=$seed"
      fi
    else
      say "OK   stress $name"
    fi
  done
}

# 猴子先起：它按墙钟跑满每包额度，和 fuzz 目标用的是不同的核。
monkey_loop &
MONKEY_PID=$!
stress_loop &
STRESS_PID=$!

TARGETS="psb iet image save runtime render"
ROUND=0
while [ "$(date +%s)" -lt "$DEADLINE" ]; do
  ROUND=$((ROUND + 1))
  for t in $TARGETS; do
    [ "$(date +%s)" -lt "$DEADLINE" ] || break
    exe="$FUZZ/oa_fuzz_$t.exe"
    [ -f "$exe" ] || { say "SKIP $t（没构建出来）"; continue; }
    corpus="$CORPUS_ROOT/$t"
    mkdir -p "$corpus"
    seed=$(( (RANDOM << 15 | RANDOM) % 2000000000 ))
    say "RUN  $t round=$ROUND seed=$seed"
    "$exe" "$corpus" "-max_total_time=$((SLICE_MIN * 60))" "-seed=$seed" \
        "-artifact_prefix=$ART/$t-" "-rss_limit_mb=4096" "-timeout=60" "-print_final_stats=1" \
        >> "$OUT/$t.log" 2>&1
    if [ $? -ne 0 ]; then
      if is_oom "$OUT/$t.log"; then
        echo x >> "$CRASH_FILE"
        say "CRASH-OOM $t"
        ls -1t "$ART" 2>/dev/null | grep "^$t-" | head -3 | while read -r f; do say "      $f"; done
      else
        echo x >> "$CRASH_FILE"
        say "CRASH $t exit 非零"
        ls -1t "$ART" 2>/dev/null | grep "^$t-" | head -3 | while read -r f; do say "      $f"; done
      fi
    else
      say "OK   $t 无崩溃"
    fi
  done
done

wait "$MONKEY_PID" 2>/dev/null
wait "$STRESS_PID" 2>/dev/null
CRASHES=$(wc -l < "$CRASH_FILE" 2>/dev/null | tr -d ' ')
say "DONE rounds=$ROUND crashes=${CRASHES:-0}"
