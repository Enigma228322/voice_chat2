add_executable(skufy_client
    src/main_webrtc.cpp
)

find_path(PORTAUDIO_INCLUDE_DIR
    NAMES portaudio.h
    PATHS /opt/homebrew/include /usr/local/include
)

find_library(PORTAUDIO_LIBRARY
    NAMES portaudio
    PATHS /opt/homebrew/lib /usr/local/lib
)

if (NOT PORTAUDIO_INCLUDE_DIR OR NOT PORTAUDIO_LIBRARY)
    message(FATAL_ERROR "PortAudio not found. Install it first (macOS: brew install portaudio).")
endif()

target_include_directories(skufy_client PRIVATE ${PORTAUDIO_INCLUDE_DIR})
target_link_libraries(skufy_client PRIVATE
    ${PORTAUDIO_LIBRARY}
    LibDataChannel::LibDataChannel
)
