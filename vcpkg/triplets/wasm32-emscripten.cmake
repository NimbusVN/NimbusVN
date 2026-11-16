# wasm32-emscripten, THREAD-ENABLED (krkrsdl3 + openartemis 共用).
#
# 两个内核的浏览器构建都链接 pthreads：
#   - krkrsdl3: ASYNCIFY + USE_PTHREADS=1 + PTHREAD_POOL_SIZE
#   - openartemis: 引擎解码/音频 worker 是 std::thread
# vcpkg 的库存 wasm32-emscripten triplet 不带 -pthread，非 atomics 对象会让
# wasm-ld 拒绝链接（--shared-memory is disallowed ... atomics/bulk-memory），
# 所以依赖必须用同一组 flag 构建。
set(VCPKG_ENV_PASSTHROUGH_UNTRACKED EMSCRIPTEN_ROOT EMSDK PATH)

if(NOT DEFINED ENV{EMSCRIPTEN_ROOT})
   find_path(EMSCRIPTEN_ROOT "emcc")
else()
   set(EMSCRIPTEN_ROOT "$ENV{EMSCRIPTEN_ROOT}")
endif()

if(NOT EMSCRIPTEN_ROOT)
   if(NOT DEFINED ENV{EMSDK})
      message(FATAL_ERROR "The emcc compiler not found in PATH")
   endif()
   set(EMSCRIPTEN_ROOT "$ENV{EMSDK}/upstream/emscripten")
endif()

if(NOT EXISTS "${EMSCRIPTEN_ROOT}/cmake/Modules/Platform/Emscripten.cmake")
   message(FATAL_ERROR "Emscripten.cmake toolchain file not found")
endif()

set(VCPKG_TARGET_ARCHITECTURE wasm32)
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE static)
set(VCPKG_CMAKE_SYSTEM_NAME Emscripten)
# Chainload vcpkg's wrapper toolchain (it includes Emscripten.cmake and then
# applies VCPKG_{C,CXX,LINKER}_FLAGS, which a direct chainload would drop).
set(VCPKG_CHAINLOAD_TOOLCHAIN_FILE "${VCPKG_ROOT_DIR}/scripts/toolchains/emscripten.cmake")

set(VCPKG_C_FLAGS "-pthread")
set(VCPKG_CXX_FLAGS "-pthread")
set(VCPKG_LINKER_FLAGS "-pthread")

# Ports that do their own configure (autotools/make) read the environment.
set(ENV{CFLAGS} "-pthread -matomics -mbulk-memory")
set(ENV{CXXFLAGS} "-pthread -matomics -mbulk-memory")
set(ENV{LDFLAGS} "-pthread -sSHARED_MEMORY=1")
