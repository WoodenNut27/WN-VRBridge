cmake_minimum_required(VERSION 3.25)
project(WNVRBridge LANGUAGES CXX)

# 配布先に VC++ の再頒布パッケージが無くても動くように、実行時ライブラリを静的にリンクする。
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(MSVC)
    add_compile_options(/W4 /permissive- /utf-8)
endif()

option(WNVR_BUILD_TESTS "単体テストをビルドする" ON)
enable_testing()

# 共通の部品（依存の少ない順）。
add_subdirectory(shared/version)
if(WNVR_BUILD_TESTS)
    add_subdirectory(shared/testing)
endif()
add_subdirectory(shared/protocol)
add_subdirectory(shared/win32)
add_subdirectory(shared/host_ipc)

add_subdirectory(pc/driver)
add_subdirectory(pc/host)
add_subdirectory(pc/tools/pose_sender)

# Quest のアプリ本体は Gradle でビルドする（quest/）。ここでは、PC で確かめられる部分のテストだけを加える。
if(WNVR_BUILD_TESTS)
    add_subdirectory(quest/tests)
endif()
