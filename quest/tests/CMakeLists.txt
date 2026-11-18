# Quest のコードのうち Android の API を使わない部分を、PC でビルドして確かめる。
# （Quest の実機が無くても、通信の組み立てや設定ファイルの読み取りの誤りに気づけるようにする）
set(WNVR_QUEST_SOURCE_DIR "${PROJECT_SOURCE_DIR}/quest/app/src/main/cpp")

wnvr_add_test(wnvr_quest_tests
    SOURCES
        config_file_tests.cpp
        restart_notice_tests.cpp
        "${WNVR_QUEST_SOURCE_DIR}/restart_notice.cpp"
        mic_packetizer_tests.cpp
        "${WNVR_QUEST_SOURCE_DIR}/mic_packetizer.cpp"
        frame_assembler_tests.cpp
        "${WNVR_QUEST_SOURCE_DIR}/config_file.cpp"
        "${WNVR_QUEST_SOURCE_DIR}/frame_assembler.cpp"
    LIBRARIES wnvr_protocol
)
# stubs には、Android のヘッダ（<android/log.h>）の代わりを置いてある。
target_include_directories(wnvr_quest_tests PRIVATE "${WNVR_QUEST_SOURCE_DIR}" stubs)
