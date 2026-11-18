// 测试用崩溃定位实现（Windows）。
//
// 只用三个 Win32 入口，且都在这个 .cpp 里 —— <windows.h> 的 near/far 宏
// 不能泄漏到任何包含 test_host.h 的测试里。
#include "crash_backtrace.h"

#ifdef _WIN32
#include <windows.h>

#include <cstdio>

namespace oa::test {
namespace {

LONG WINAPI crash_filter(EXCEPTION_POINTERS* ep) {
    if (!ep || !ep->ExceptionRecord) return EXCEPTION_EXECUTE_HANDLER;
    std::fprintf(stderr, "[crash] code=0x%lx addr=%p\n",
                 (unsigned long)ep->ExceptionRecord->ExceptionCode,
                 ep->ExceptionRecord->ExceptionAddress);
    void* frames[48] = {nullptr};
    const USHORT n = CaptureStackBackTrace(0, 48, frames, nullptr);
    const unsigned long long base =
        (unsigned long long)(uintptr_t)GetModuleHandleA(nullptr);
    for (USHORT i = 0; i < n; ++i) {
        const unsigned long long a = (unsigned long long)(uintptr_t)frames[i];
        std::fprintf(stderr, "[crash] #%02u %p +0x%llx\n", (unsigned)i, frames[i],
                     a >= base ? a - base : 0ull);
    }
    std::fflush(stderr);
    return EXCEPTION_EXECUTE_HANDLER;
}

} // namespace

void install_crash_handler() { SetUnhandledExceptionFilter(&crash_filter); }

} // namespace oa::test

#else

namespace oa::test {
void install_crash_handler() {}
}

#endif
