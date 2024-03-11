cmake_minimum_required(VERSION 3.10)

project("rdma")

set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -O3 -g -Wall -Werror -Wextra -Wpedantic")

link_libraries(ibverbs)
file(GLOB SOURCES src/*.c ../common/*.c)

add_executable (rc_pingpong ${SOURCES} rc_pingpong.c)
add_executable (ud_pingpong ${SOURCES} ud_pingpong.c)