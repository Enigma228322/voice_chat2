add_executable(skufy_server
    src/main_webrtc.cpp
)

target_link_libraries(skufy_server PRIVATE LibDataChannel::LibDataChannel)
