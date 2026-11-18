set(WNVR_IMGUI_DIR "${PROJECT_SOURCE_DIR}/third_party/imgui")
add_library(wnvr_imgui STATIC
    "${WNVR_IMGUI_DIR}/imgui.cpp"
    "${WNVR_IMGUI_DIR}/imgui_draw.cpp"
    "${WNVR_IMGUI_DIR}/imgui_tables.cpp"
    "${WNVR_IMGUI_DIR}/imgui_widgets.cpp"
    "${WNVR_IMGUI_DIR}/imgui_demo.cpp"
    "${WNVR_IMGUI_DIR}/backends/imgui_impl_win32.cpp"
    "${WNVR_IMGUI_DIR}/backends/imgui_impl_dx11.cpp"
)
target_include_directories(wnvr_imgui SYSTEM PUBLIC "${WNVR_IMGUI_DIR}")
if(MSVC)
    # 外部コードの警告をホスト自身の警告と分ける。
    target_compile_options(wnvr_imgui PRIVATE /W0)
endif()

add_executable(wnvrbridge_host WIN32
    resources/app.rc
    # 入口と本体
    src/main.cpp
    src/host_app.h src/host_app.cpp
    src/host_status.h
    src/host_log.h src/host_log.cpp
    src/host_settings.h src/host_settings.cpp
    # 接続（Quest の待ち受け、ドライバとのパイプ、承認）
    src/host_listener.h src/host_listener.cpp
    src/driver_pipe_client.h src/driver_pipe_client.cpp
    src/pairing_manager.h src/pairing_manager.cpp
    src/network_adapters.h src/network_adapters.cpp
    # セットアップ（ドライバの登録、ファイアウォール、スタートアップ）
    src/setup_tasks.h src/setup_tasks.cpp
    src/driver_registration.h src/driver_registration.cpp
    src/firewall_rules.h src/firewall_rules.cpp
    src/startup_registration.h src/startup_registration.cpp
    # 画面（タスクトレイ、設定画面）
    src/status_summary.h src/status_summary.cpp
    src/app_icon.h src/app_icon.cpp
    src/tray_icon.h src/tray_icon.cpp
    src/settings_window.h src/settings_window.cpp
    src/settings_view.h src/settings_view.cpp
    src/ui_widgets.h src/ui_widgets.cpp
)
target_include_directories(wnvrbridge_host SYSTEM PRIVATE "${PROJECT_SOURCE_DIR}/third_party/nlohmann_json/include")
target_compile_definitions(wnvrbridge_host PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(wnvrbridge_host PRIVATE
    wnvr_protocol wnvr_host_ipc wnvr_win32 wnvr_version wnvr_imgui
    d3d11 dxgi d3dcompiler shell32 ole32 oleaut32 uuid dwmapi iphlpapi ws2_32 advapi32
)
set_target_properties(wnvrbridge_host PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/host")
foreach(config DEBUG RELEASE RELWITHDEBINFO MINSIZEREL)
    set_target_properties(wnvrbridge_host PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY_${config} "${CMAKE_BINARY_DIR}/host"
    )
endforeach()

if(WNVR_BUILD_TESTS)
    # 画面に出す文言を決める部分は、ウィンドウなしで確かめられる。
    wnvr_add_test(wnvr_host_tests
        SOURCES tests/status_summary_tests.cpp src/status_summary.cpp
        LIBRARIES wnvr_host_ipc
    )
    target_include_directories(wnvr_host_tests PRIVATE src)
endif()
