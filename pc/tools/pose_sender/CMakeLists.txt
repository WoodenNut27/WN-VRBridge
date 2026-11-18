add_executable(pose_sender src/main.cpp)
target_link_libraries(pose_sender PRIVATE wnvr_protocol ws2_32 winmm)
