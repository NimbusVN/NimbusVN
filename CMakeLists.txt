cmake_minimum_required(VERSION 3.28)

#
# 0. 内核选择
#
# NimbusVN：多内核集成项目（当前为 krkrsdl3 + openartemis，后续可继续接入其它内核）。
#   engine/krkrsdl3   — Kirikiri SDL3（XP3/TJS）
#   engine/openartemis — Artemis 系视觉小说引擎（PFS/Artemis）
#
# 两个内核都是 git 子模块；桌面/Android/WASM 默认全部构建。
# OHOS 目前只构建 krkrsdl3（openartemis 的 OHOS 原生后端尚未实现），
# 启动器会做游戏类型自动识别，PFS 游戏会提示当前构建不含该内核。
#
option(ENGINE_KRKRSDL3 "Build the krkrsdl3 (XP3/TJS) engine" ON)
if(OHOS)
    option(ENGINE_OPENARTEMIS "Build the openartemis (PFS/Artemis) engine" OFF)
else()
    option(ENGINE_OPENARTEMIS "Build the openartemis (PFS/Artemis) engine" ON)
endif()

# 每个引擎是一层目录：engine/<name>/{src(子模块), docs, cmake, tests}。
# 本文件只负责统合与平台选择；各引擎的源码清单/目标接线在 engine/<name>/cmake。
set(KRKRSDL3_DIR ${CMAKE_CURRENT_SOURCE_DIR}/engine/krkrsdl3)
set(KRKRSDL3_ROOT ${KRKRSDL3_DIR}/src)
set(OPENARTEMIS_DIR ${CMAKE_CURRENT_SOURCE_DIR}/engine/openartemis)
set(OPENARTEMIS_ROOT ${OPENARTEMIS_DIR}/src)

#
# 1. 准备阶段
#

# 使用vcpkg进行包管理
if(ANDROID)
    if (ANDROID_ABI MATCHES "arm64-v8a")
        set(VCPKG_TARGET_TRIPLET "arm64-android" CACHE STRING "" FORCE)
    elseif(ANDROID_ABI MATCHES "armeabi-v7a")
        set(VCPKG_TARGET_TRIPLET "arm-android" CACHE STRING "" FORCE)
    elseif(ANDROID_ABI MATCHES "x86_64")
        set(VCPKG_TARGET_TRIPLET "x64-android" CACHE STRING "" FORCE)
    else()
        message(FATAL_ERROR "安卓当前仅支持arm64-v8a、armeabi-v7a和x86_64架构")
    endif()
	if(ONLYCONSOLE)
		message(FATAL_ERROR "安卓不支持仅编译tjs2")
	endif()
    set(CMAKE_ANDROID_NDK $ENV{ANDROID_NDK_HOME})
    set(VCPKG_CHAINLOAD_TOOLCHAIN_FILE $ENV{ANDROID_NDK_HOME}/build/cmake/android.toolchain.cmake)
elseif(LINUX)
    set(VCPKG_TARGET_TRIPLET "x64-linux-release")
elseif(EMSCRIPTEN)
    set(VCPKG_TARGET_TRIPLET "wasm32-emscripten")
elseif(WINDOWS OR WIN32)
    if(NOT DEFINED VCPKG_TARGET_TRIPLET)
        if(MSVC OR CMAKE_GENERATOR MATCHES "Visual Studio")
            add_compile_options("$<$<CXX_COMPILER_ID:MSVC>:/utf-8>")
            if(CMAKE_GENERATOR_PLATFORM STREQUAL "x64")
                set(VCPKG_TARGET_TRIPLET "x64-windows")
            elseif(CMAKE_GENERATOR_PLATFORM STREQUAL "Win32")
                set(VCPKG_TARGET_TRIPLET "x86-windows")
            endif()
        else()
            set(VCPKG_TARGET_TRIPLET "x64-mingw-dynamic")
        endif()
    else()
        if(MSVC OR CMAKE_GENERATOR MATCHES "Visual Studio")
            add_compile_options("$<$<CXX_COMPILER_ID:MSVC>:/utf-8>")
        endif()
    endif()
elseif(MACOS)
    if(NOT DEFINED VCPKG_TARGET_TRIPLET)
        if(CMAKE_OSX_ARCHITECTURES MATCHES "x86_64" AND NOT CMAKE_OSX_ARCHITECTURES MATCHES "arm64")
            set(VCPKG_TARGET_TRIPLET "x64-osx" CACHE STRING "" FORCE)
        else()
            set(VCPKG_TARGET_TRIPLET "arm64-osx" CACHE STRING "" FORCE)
        endif()
    endif()
elseif(IOS)
    if(ONLYCONSOLE)
        message(FATAL_ERROR "iOS 不支持仅编译 tjs2")
    endif()
    set(CMAKE_SYSTEM_NAME iOS CACHE STRING "" FORCE)
    if(NOT DEFINED VCPKG_TARGET_TRIPLET)
        if(CMAKE_OSX_SYSROOT MATCHES "iphonesimulator")
            if(CMAKE_OSX_ARCHITECTURES MATCHES "x86_64")
                set(VCPKG_TARGET_TRIPLET "x64-ios-simulator" CACHE STRING "" FORCE)
            else()
                set(VCPKG_TARGET_TRIPLET "arm64-ios-simulator" CACHE STRING "" FORCE)
            endif()
        else()
            set(VCPKG_TARGET_TRIPLET "arm64-ios" CACHE STRING "" FORCE)
        endif()
    endif()
endif()
if(OHOS)
    # OHOS: 预先编译第三方依赖库
    set(OHOS_DEPS_SCRIPT "${CMAKE_SOURCE_DIR}/platform/ohos/build-ohos.ps1")
    set(OHOS_DEPS_STAMP "${CMAKE_SOURCE_DIR}/out/ohos/deps/install/${OHOS_ARCH}/.stamp")
    if(NOT EXISTS "${OHOS_DEPS_STAMP}")
        message(STATUS "OHOS: Building dependencies (one-time)...")
        execute_process(
            COMMAND powershell -ExecutionPolicy Bypass -File "${OHOS_DEPS_SCRIPT}" deps -Arch ${OHOS_ARCH}
            WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
            RESULT_VARIABLE _ohos_deps_rc
        )
        if(NOT _ohos_deps_rc EQUAL 0)
            message(WARNING "OHOS deps build returned ${_ohos_deps_rc}")
        endif()
    endif()
else()
    set(CMAKE_TOOLCHAIN_FILE $ENV{VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake)
endif()

# 离屏自动化测试宿主（AutoTest / -offscreen / -silent / -frames / -dump / -at_harness）
# 关掉后所有入口在头文件里内联成空实现，主循环与送显路径不留任何调用开销
option(ENABLE_AUTOTEST "Build the offscreen AutoTest host (tests/autotest.md)" ON)
# 视频解码后端: ON=FFmpeg, OFF=系统API(MediaFoundation/MediaCodec)
# 感觉没必要禁用，OFF的实现基本没用༼ ༎ຶ ෴ ༎ຶ༽
option(USE_FFMPEG "Use FFmpeg for video/audio decoding" ON)
# 额外的独立视频解码后端：libtheora（不经过 FFmpeg 的解码器接入示范）
option(USE_THEORA_DECODER "Build the standalone libtheora movie backend" OFF)
# 图形后端编译选项
option(USE_RENDER_GL "OpenGL 3.3+/OpenGLES 3.0+ backend" ON)
option(USE_RENDER_VK "Vulkan backend" ON)
# ASan 构建（clang/llvm-mingw 自带运行时）
option(TJS2_ASAN "Build with AddressSanitizer" OFF)
# libFuzzer 目标（源解析与字节码加载）
option(TJS2_FUZZERS "Build libFuzzer targets for tjs2" OFF)
# openartemis 内核的宿主/测试/工具开关（桌面构建）
option(OA_ENABLE_TESTS "Build openartemis unit + real-asset tests" OFF)
option(OA_BUILD_TOOLS "Build openartemis command-line tools" OFF)
option(OA_ENABLE_ASAN "Build openartemis with AddressSanitizer" OFF)
option(OA_ENABLE_FUZZ "Build the openartemis parser fuzzers + monkey" OFF)

if(ONLYCONSOLE)
    set(ENGINE_KRKRSDL3 ON)
    set(ENGINE_OPENARTEMIS OFF)
endif()
if(NOT ENGINE_KRKRSDL3 AND NOT ENGINE_OPENARTEMIS)
    message(FATAL_ERROR "ENGINE_KRKRSDL3 与 ENGINE_OPENARTEMIS 至少开启一个")
endif()
if(ENGINE_OPENARTEMIS AND OHOS)
    message(STATUS "OHOS: openartemis 原生后端尚未实现，本次只构建 krkrsdl3")
    set(ENGINE_OPENARTEMIS OFF)
endif()
if(ENABLE_AUTOTEST)
    add_definitions(-D_KRKRSDL3_AUTOTEST=1)
endif()

# ================================================================
#  版本号唯一定义处（Gradle / OHOS 脚本会解析这一行）
#  正式版: "1.2.3"    预发布: "1.2.3-beta.1" / "-rc.2" / "-alpha.3"
# ================================================================
set(NIMBUSVN_VERSION "0.1.0")
if(NOT NIMBUSVN_VERSION MATCHES "^[0-9]+\\.[0-9]+\\.[0-9]+(-(alpha|beta|rc)(\\.[0-9]+)?)?$")
    message(FATAL_ERROR "NIMBUSVN_VERSION 格式必须为 MAJOR.MINOR.PATCH[-alpha|beta|rc[.N]]，当前值: '${NIMBUSVN_VERSION}'")
endif()
string(REGEX MATCH "^[0-9]+\\.[0-9]+\\.[0-9]+" NIMBUSVN_VERSION_CORE "${NIMBUSVN_VERSION}")

# 建立项目（NimbusVN 是统合两个内核的集成项目名；内核目标名保持
# krkrsdl3 / openartemis，Android 原生库与平台入口依赖这些名字）
if(ONLYCONSOLE)
    project(tjs2 VERSION "${NIMBUSVN_VERSION_CORE}" LANGUAGES C CXX)
else()
    if(IOS OR MACOS)
        project(NimbusVN VERSION "${NIMBUSVN_VERSION_CORE}" LANGUAGES C CXX OBJC OBJCXX)
    else()
        project(NimbusVN VERSION "${NIMBUSVN_VERSION_CORE}" LANGUAGES C CXX)
    endif()
endif()

set(CMAKE_C_STANDARD 17)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)

if(TJS2_ASAN)
    add_compile_options(-fsanitize=address -fno-omit-frame-pointer -g)
    add_link_options(-fsanitize=address)
endif()

if(ANDROID)
    add_definitions(-D_KRKRSDL3_ANDROID=1)
elseif(LINUX)
    add_definitions(-D_KRKRSDL3_LINUX=1)
elseif(EMSCRIPTEN)
    add_definitions(-D_KRKRSDL3_EMSCRIPTEN=1)
elseif(WINDOWS)
    add_definitions(-D_KRKRSDL3_WINDOWS=1)
elseif(OHOS)
    add_definitions(-D_KRKRSDL3_OHOS=1)
elseif(MACOS)
    add_definitions(-D_KRKRSDL3_MACOS=1)
elseif(IOS)
    add_definitions(-D_KRKRSDL3_IOS=1)
else()
    message(FATAL_ERROR "unsupported: ${CMAKE_SYSTEM_NAME}")
endif()
if(USE_RENDER_GL)
    if(ANDROID OR EMSCRIPTEN OR OHOS OR IOS)
        add_definitions(-D_KRKRSDL3_GLES=1)
    elseif(LINUX OR WINDOWS OR MACOS)
        add_definitions(-D_KRKRSDL3_GL=1)
    endif()
endif()

# openartemis 的 Emscripten 构建需要全模块 pthread（引擎的解码/音频 worker
# 是 std::thread；模块里每个对象都必须带 atomics/bulk-memory）。vcpkg 依赖
# 由 vcpkg/triplets/wasm32-emscripten.cmake 用同样的 flag 构建。
if(EMSCRIPTEN AND ENGINE_OPENARTEMIS)
    add_compile_options(-pthread)
    add_link_options(-pthread)
endif()

# FFmpeg 共享库以裸文件路径传给链接器时，AGP 的 runtimeFiles 不会收集它们，
# APK 里会缺 libav*.so。Android 上转成 IMPORTED target 再链接。
function(nimbus_link_ffmpeg target)
    if(NOT ANDROID OR NOT FFMPEG_libavcodec_LIBRARY_RELEASE MATCHES "\\.so$")
        target_link_libraries(${target} PRIVATE ${FFMPEG_LIBRARIES})
        return()
    endif()
    set(ffmpeg_targets)
    foreach(component avdevice avfilter avformat avcodec swresample swscale avutil)
        set(release "${FFMPEG_lib${component}_LIBRARY_RELEASE}")
        if(NOT release)
            continue()
        endif()
        set(imported "nimbus_ffmpeg_${component}")
        if(NOT TARGET ${imported})
            add_library(${imported} SHARED IMPORTED)
            set_target_properties(${imported} PROPERTIES IMPORTED_LOCATION "${release}")
            set(debug "${FFMPEG_lib${component}_LIBRARY_DEBUG}")
            if(debug)
                set_target_properties(${imported} PROPERTIES IMPORTED_LOCATION_DEBUG "${debug}")
            endif()
        endif()
        list(APPEND ffmpeg_targets ${imported})
    endforeach()
    set(ffmpeg_extra)
    foreach(item IN LISTS FFMPEG_LIBRARIES)
        if(item STREQUAL "optimized" OR item STREQUAL "debug")
            continue()
        endif()
        if(item MATCHES "lib(avdevice|avfilter|avformat|avcodec|swresample|swscale|avutil)\\.so$")
            continue()
        endif()
        list(APPEND ffmpeg_extra "${item}")
    endforeach()
    target_link_libraries(${target} PRIVATE ${ffmpeg_targets} ${ffmpeg_extra})
endfunction()

#
# 2. 区分编译
#

if(ONLYCONSOLE)
    include(${KRKRSDL3_DIR}/cmake/cmake_console.txt)
else()
    if(ENGINE_KRKRSDL3)
        include(${KRKRSDL3_DIR}/cmake/cmake_main.txt)
    endif()
    if(ENGINE_OPENARTEMIS)
        include(${OPENARTEMIS_DIR}/cmake/cmake_main.txt)
    endif()
endif()

if(TJS2_FUZZERS)
    include(${KRKRSDL3_DIR}/cmake/cmake_fuzz.txt)
endif()
