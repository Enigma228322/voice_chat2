cmake_minimum_required(VERSION 3.20)
project(skufy LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(MSVC)
    add_compile_options(/W4)
else()
    add_compile_options(-Wall -Wextra -Wpedantic)
endif()

if(APPLE)
    list(APPEND CMAKE_PREFIX_PATH
        "/opt/homebrew/opt/libdatachannel"
        "/usr/local/opt/libdatachannel"
    )
endif()

option(SKUFY_USE_BUNDLED_LIBDATACHANNEL "Build libdatachannel from lib/libdatachannel" ON)
option(SKUFY_BUILD_SERVER "Build skufy_server target" ON)
option(SKUFY_BUILD_CLIENT "Build skufy_client target" ON)

if(SKUFY_USE_BUNDLED_LIBDATACHANNEL AND EXISTS "${CMAKE_SOURCE_DIR}/lib/libdatachannel/CMakeLists.txt")
    # Keep bundled dependency minimal and fast for local prototype builds.
    set(NO_WEBSOCKET OFF CACHE BOOL "" FORCE)
    set(NO_MEDIA ON CACHE BOOL "" FORCE)
    set(NO_EXAMPLES ON CACHE BOOL "" FORCE)
    set(NO_TESTS ON CACHE BOOL "" FORCE)
    add_subdirectory(lib/libdatachannel EXCLUDE_FROM_ALL)
else()
    find_package(LibDataChannel REQUIRED CONFIG)
endif()

if(SKUFY_BUILD_SERVER)
    add_subdirectory(skufy_server)
endif()

if(SKUFY_BUILD_CLIENT)
    add_subdirectory(skufy_client)
endif()
