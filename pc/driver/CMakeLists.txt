set(WNVR_DRIVER_OUTPUT_DIR "${CMAKE_BINARY_DIR}/driver/wnvrbridge")

add_library(driver_wnvrbridge SHARED
    # 入口と本体
    src/driver_main.cpp
    src/device_provider.h src/device_provider.cpp
    src/driver_log.h src/driver_log.cpp
    src/driver_settings.h src/driver_settings.cpp
    src/stats_collector.h src/stats_collector.cpp
    # SteamVR に見せる機器
    src/hmd_device.h src/hmd_device.cpp
    src/controller_device.h src/controller_device.cpp
    # 映像（SteamVR の画像の受け取り → エンコード）
    src/virtual_display.h src/virtual_display.cpp
    src/video_encoder.h src/video_encoder.cpp
    src/frame_dump.h src/frame_dump.cpp
    src/ivf_writer.h src/ivf_writer.cpp
    # 音声
    src/audio_capture.h src/audio_capture.cpp
    src/mic_jitter_buffer.h src/mic_jitter_buffer.cpp
    src/mic_playback.h src/mic_playback.cpp
    src/speaker_mute.h src/speaker_mute.cpp
    # 通信（Quest との UDP、ホストとのパイプ）
    src/network_server.h src/network_server.cpp
    src/host_pipe_server.h src/host_pipe_server.cpp
)

target_link_libraries(driver_wnvrbridge PRIVATE
    wnvr_protocol wnvr_host_ipc wnvr_win32 wnvr_version
    ws2_32 winmm d3d11 d3dcompiler dxgi ole32 uuid shell32
)

# SYSTEM 指定で外部ヘッダの警告を自分のコードの警告と分ける。
target_include_directories(driver_wnvrbridge SYSTEM PRIVATE
    "${PROJECT_SOURCE_DIR}/third_party/openvr/headers"
    "${PROJECT_SOURCE_DIR}/third_party/nvenc/include"
)

set_target_properties(driver_wnvrbridge PROPERTIES
    PREFIX ""
    OUTPUT_NAME "driver_wnvrbridge"
    RUNTIME_OUTPUT_DIRECTORY "${WNVR_DRIVER_OUTPUT_DIR}/bin/win64"
)
foreach(config DEBUG RELEASE RELWITHDEBINFO MINSIZEREL)
    set_target_properties(driver_wnvrbridge PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY_${config} "${WNVR_DRIVER_OUTPUT_DIR}/bin/win64"
    )
endforeach()

# SteamVR に登録するフォルダの形（driver.vrdrivermanifest と resources）を、ビルドの出力先に作る。
add_custom_command(TARGET driver_wnvrbridge POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${WNVR_DRIVER_OUTPUT_DIR}/resources/settings"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/resources/driver.vrdrivermanifest"
        "${WNVR_DRIVER_OUTPUT_DIR}/driver.vrdrivermanifest"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/resources/settings/default.vrsettings"
        "${WNVR_DRIVER_OUTPUT_DIR}/resources/settings/default.vrsettings"
    VERBATIM
)

if(WNVR_BUILD_TESTS)
    # SteamVR が無くても確かめられる部分（統計の集計）。ログは tests/log_stub.cpp が受け取る。
    wnvr_add_test(wnvr_driver_tests
        SOURCES tests/stats_collector_tests.cpp tests/log_stub.cpp src/stats_collector.cpp
            tests/mic_jitter_buffer_tests.cpp src/mic_jitter_buffer.cpp
        LIBRARIES wnvr_protocol wnvr_host_ipc
    )
    target_include_directories(wnvr_driver_tests PRIVATE src tests)
endif()
