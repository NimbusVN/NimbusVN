#pragma once
// 测试用崩溃定位（只声明，不引任何平台头）。
//
// 实现在 crash_backtrace.cpp：装一个未处理异常过滤器，崩了就打
//   [crash] code=... addr=...
//   [crash] #NN <addr> +0x<模块内偏移>
// 然后用 addr2line 落到行：
//   addr2line -e <test>.exe -f -C 0x140000000+<偏移>
//
// 放在 .cpp 里而不是 test_host.h：Windows 的 <windows.h> 会带 near/far
// 宏，污染每一个包含包装类的测试。
namespace oa::test {
void install_crash_handler();
}
