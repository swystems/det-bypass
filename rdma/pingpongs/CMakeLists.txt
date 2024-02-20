cmake_minimum_required(VERSION 3.10)

project("rdma")

if (NOT DEBUG)
    set(DEBUG 0)
endif ()
add_definitions(-DDEBUG=${DEBUG})

set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -O1 -g -Wall -Werror -Wextra -Wpedantic")

add_library(libibverbs SHARED IMPORTED)
set_property(TARGET libibverbs PROPERTY IMPORTED_LOCATION /usr/lib/x86_64-linux-gnu/libibverbs.so)

link_libraries(libibverbs)
file(GLOB SOURCES src/*.c ../../common/*.c)

add_executable (rc_pingpong ${SOURCES} rc_pingpong2.c)
add_executable (ud_pingpong ${SOURCES} ud_pingpong2.c)