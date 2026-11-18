# PC と Quest の両方でビルドする。Windows や Android の API は使わない。
add_library(wnvr_protocol STATIC
    include/wnvr/protocol/byte_buffer.h
    include/wnvr/protocol/messages.h
    src/messages.cpp
)
target_include_directories(wnvr_protocol PUBLIC include)
target_compile_features(wnvr_protocol PUBLIC cxx_std_20)

if(WNVR_BUILD_TESTS)
    wnvr_add_test(wnvr_protocol_tests SOURCES tests/protocol_tests.cpp LIBRARIES wnvr_protocol)
endif()
