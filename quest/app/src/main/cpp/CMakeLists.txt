cmake_minimum_required(VERSION 3.25)
project(wnvrbridge_client LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(native_app_glue STATIC
    ${ANDROID_NDK}/sources/android/native_app_glue/android_native_app_glue.c)
target_include_directories(native_app_glue PUBLIC
    ${ANDROID_NDK}/sources/android/native_app_glue)

# PC と共通の部品。単体テストは PC 側のビルドで実行するので、ここではビルドしない。
set(WNVR_SHARED_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../../../../shared")
set(WNVR_BUILD_TESTS OFF CACHE BOOL "" FORCE)
add_subdirectory("${WNVR_SHARED_DIR}/version" "${CMAKE_BINARY_DIR}/wnvr_version")
add_subdirectory("${WNVR_SHARED_DIR}/protocol" "${CMAKE_BINARY_DIR}/wnvr_protocol")
target_compile_options(wnvr_protocol PRIVATE -Wall -Wextra)

find_package(OpenXR REQUIRED CONFIG)

add_library(wnvrbridge_client SHARED
    # 入口と本体（OpenXR のセッション、描画ループ）
    main.cpp
    xr_app.cpp
    xr_input.cpp
    egl_context.cpp
    config_file.cpp
    device_identity.cpp
    # 通信と映像・音声
    network_client.cpp
    frame_assembler.cpp
    video_decoder.cpp
    video_renderer.cpp
    audio_player.cpp
    mic_packetizer.cpp
    mic_capture.cpp
    # PC につながるまでの状態の表示
    status_overlay.cpp
    restart_notice.cpp
)
target_compile_options(wnvrbridge_client PRIVATE -Wall -Wextra)
target_link_libraries(wnvrbridge_client PRIVATE
    native_app_glue wnvr_protocol wnvr_version OpenXR::openxr_loader android log EGL GLESv3 mediandk nativewindow aaudio jnigraphics)
target_link_options(wnvrbridge_client PRIVATE "-u" "ANativeActivity_onCreate")
