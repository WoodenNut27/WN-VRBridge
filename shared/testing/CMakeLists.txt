# 単体テストの共通部分。テストは wnvr_testing をリンクし、<doctest/doctest.h> を読み込むだけでよい。
add_library(wnvr_testing STATIC test_main.cpp)
target_include_directories(wnvr_testing SYSTEM PUBLIC "${PROJECT_SOURCE_DIR}/third_party/doctest")
target_compile_features(wnvr_testing PUBLIC cxx_std_20)
# 文字列などを失敗の説明に出すとき、doctest の前方宣言ではなく標準ヘッダの定義を使わせる。
target_compile_definitions(wnvr_testing PUBLIC DOCTEST_CONFIG_USE_STD_HEADERS)
if(WIN32)
    target_compile_definitions(wnvr_testing PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
endif()

# テストの実行ファイルを作って CTest に登録する。
function(wnvr_add_test name)
    cmake_parse_arguments(ARG "" "" "SOURCES;LIBRARIES" ${ARGN})
    add_executable(${name} ${ARG_SOURCES})
    target_link_libraries(${name} PRIVATE wnvr_testing ${ARG_LIBRARIES})
    add_test(NAME ${name} COMMAND ${name})
endfunction()
