add_library(wnvr_host_ipc STATIC
    include/wnvr/host_ipc/host_ipc.h
    src/host_ipc.cpp
)
if(WIN32)
    target_sources(wnvr_host_ipc PRIVATE include/wnvr/host_ipc/pipe_io.h src/pipe_io.cpp)
    target_compile_definitions(wnvr_host_ipc PUBLIC WIN32_LEAN_AND_MEAN NOMINMAX)
endif()
target_include_directories(wnvr_host_ipc PUBLIC include)
target_include_directories(wnvr_host_ipc SYSTEM PRIVATE "${PROJECT_SOURCE_DIR}/third_party/nlohmann_json/include")
target_compile_features(wnvr_host_ipc PUBLIC cxx_std_20)
# 端末の ID の決まりと名前の長さの上限を、Quest との通信と共有する。
target_link_libraries(wnvr_host_ipc PUBLIC wnvr_protocol)

if(WNVR_BUILD_TESTS)
    wnvr_add_test(wnvr_host_ipc_tests SOURCES tests/host_ipc_tests.cpp LIBRARIES wnvr_host_ipc)
endif()
