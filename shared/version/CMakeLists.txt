# 版はリポジトリのルートの VERSION ファイルだけに書く。
# PC（ルートの CMake）と Quest（Gradle から呼ばれる CMake）の両方がここを取り込み、
# 同じ wnvr/version.h を作る。Gradle の versionName と配布スクリプトも同じファイルを読む。
set(WNVR_VERSION_FILE "${CMAKE_CURRENT_LIST_DIR}/../../VERSION")
file(STRINGS "${WNVR_VERSION_FILE}" WNVR_VERSION LIMIT_COUNT 1)
string(STRIP "${WNVR_VERSION}" WNVR_VERSION)
if(NOT WNVR_VERSION MATCHES "^[0-9A-Za-z][0-9A-Za-z._+-]*$")
    message(FATAL_ERROR "VERSION ファイルの内容が不正です: '${WNVR_VERSION}'")
endif()
# VERSION を書き換えたら、次のビルドで構成し直してヘッダを作り直す。
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${WNVR_VERSION_FILE}")

configure_file(version.h.in "${CMAKE_CURRENT_BINARY_DIR}/include/wnvr/version.h" @ONLY)
add_library(wnvr_version INTERFACE)
target_include_directories(wnvr_version INTERFACE "${CMAKE_CURRENT_BINARY_DIR}/include")
