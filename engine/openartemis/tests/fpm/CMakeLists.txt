# Real-game tests: **一个游戏一个文件、一个可执行文件**。
#
# 目录里只有两类源文件：
#   test_host.h / crash_backtrace.*   —— 包装类（模拟 main.cpp 的无头静音宿主）
#   game_<name>_test.cpp              —— 一个游戏的全部游程与动作
#
# 每个游戏由自己的环境变量指向 root.pfs，未配置就 exit 77（跳过）。
# 变量名 ↔ 游戏：
#   OA_TEST_FPM_PFS       ハミダシクリエイティブ
#   OA_TEST_HCT_PFS       ハミダシクリエイティブ凸
#   OA_TEST_RR_PFS        常轨脱离ReReCall
#   OA_TEST_NEKOMIKO_PFS  NekoMiko
#   OA_TEST_TG3_PFS       甜蜜女友3
#   OA_BT91_PFS           ヘンタイ・プリズン
#   OA_TEST_T5_PFS        乙女理論とその周辺 -Ecole de Paris-
#   OA_TEST_T13_PFS       穢翼のユースティア
#
# 通用（asset-free）引擎测试在 tests/（../CMakeLists.txt），不在这里。
# 资源分级：重测试默认串行（内存安全）；内存够的机器可
# -DOA_TEST_SERIALIZE_HEAVY=OFF 并行跑。

find_package(SDL3 CONFIG REQUIRED)
find_package(PNG CONFIG REQUIRED)

option(OA_TEST_SERIALIZE_HEAVY "Serialize heavy real-game tests (memory-safe default)" ON)
if(OA_TEST_SERIALIZE_HEAVY)
  set(_OA_RS RUN_SERIAL TRUE)   # 两个参数，不是 "K=V" 字符串
else()
  set(_OA_RS "")
endif()

function(oa_game_test name var)
  add_executable(game_${name}_test game_${name}_test.cpp crash_backtrace.cpp)
  target_link_libraries(game_${name}_test PRIVATE libopenartemis SDL3::SDL3)
  add_test(NAME game_${name} COMMAND game_${name}_test)
  set_tests_properties(game_${name} PROPERTIES
    ENVIRONMENT "${var}=${${var}}"
    SKIP_RETURN_CODE 77
    ${_OA_RS})
endfunction()

oa_game_test(nekomiko OA_TEST_NEKOMIKO_PFS)
oa_game_test(fpm      OA_TEST_FPM_PFS)
oa_game_test(tg3      OA_TEST_TG3_PFS)
oa_game_test(bt91     OA_BT91_PFS)
oa_game_test(t5       OA_TEST_T5_PFS)
oa_game_test(t13      OA_TEST_T13_PFS)
oa_game_test(hct      OA_TEST_HCT_PFS)
oa_game_test(rr       OA_TEST_RR_PFS)
