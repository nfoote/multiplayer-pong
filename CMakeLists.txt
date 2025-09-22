cmake_minimum_required(VERSION 3.16)
project(multiplayer_pong LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Header-only "common" (protocol.hpp)
add_library(common INTERFACE common/protocol.hpp)
target_include_directories(common INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})

# --- Server ---
add_executable(pong_server server/server.cpp)
target_link_libraries(pong_server PRIVATE common)

# --- Console client (no SDL) ---
add_executable(pong_client client/client.cpp)
target_link_libraries(pong_client PRIVATE common)

# --- SDL client ---
# Homebrew provides a CMake config package on macOS.
find_package(SDL2 REQUIRED CONFIG)

add_executable(pong_sdl_client client/client_sdl.cpp)
# SDL2main provides the SDL_main wrapper needed on macOS/Windows.
target_link_libraries(pong_sdl_client PRIVATE common SDL2::SDL2 SDL2::SDL2main)
