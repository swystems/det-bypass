cmake_minimum_required(VERSION 3.10)

project(xdp)

include(${CMAKE_CURRENT_SOURCE_DIR}/../config.cmake)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -O3 -Wall -Werror -Wextra -Wpedantic")

# create a target pingpong, which compiles pingpong.c as a bpf program (-target bpf) using clang
add_custom_target(pingpong
        COMMAND clang ${CMAKE_C_FLAGS} -target bpf -c ${CMAKE_CURRENT_SOURCE_DIR}/pingpong.c -o ${CMAKE_CURRENT_BINARY_DIR}/pingpong.o
)
add_custom_target(pingpong_xsk
        COMMAND clang ${CMAKE_C_FLAGS} -target bpf -c ${CMAKE_CURRENT_SOURCE_DIR}/pingpong_xsk.c -o ${CMAKE_CURRENT_BINARY_DIR}/pingpong_xsk.o
)
link_libraries(bpf xdp)

file(GLOB SOURCES src/*.c ../common/*.c)
add_executable(pp_poll ${SOURCES} pp_poll.c)
add_dependencies(pp_poll pingpong)

add_executable(pp_sock ${SOURCES} pp_sock.c)
add_dependencies(pp_sock pingpong_xsk)