cmake_minimum_required(VERSION 3.16)
project(pong_net LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# shared includes (protocol.hpp)
add_library(common INTERFACE
        common/protocol.hpp)
target_include_directories(common INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})

# executables
add_executable(pong_server server/server.cpp
        server/server.cpp)
target_link_libraries(pong_server PRIVATE common)

add_executable(pong_client client/client.cpp)
target_link_libraries(pong_client PRIVATE common)
