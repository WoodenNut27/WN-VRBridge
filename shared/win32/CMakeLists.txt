add_library(wnvr_win32 STATIC
    include/wnvr/win32/platform.h
    src/platform.cpp
    include/wnvr/win32/audio_devices.h src/audio_devices.cpp
)
target_include_directories(wnvr_win32 PUBLIC include)
target_compile_features(wnvr_win32 PUBLIC cxx_std_20)
target_compile_definitions(wnvr_win32 PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(wnvr_win32 PUBLIC shell32 ole32 propsys uuid)

if(WNVR_BUILD_TESTS)
    wnvr_add_test(wnvr_win32_tests SOURCES tests/platform_tests.cpp LIBRARIES wnvr_win32)
endif()
