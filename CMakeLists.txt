cmake_minimum_required(VERSION 3.10)
project(det-bypass)

include(${CMAKE_CURRENT_SOURCE_DIR}/config.cmake)

set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -O3 -Werror -Wall -Wextra")

if (DEBUG)
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -g")
endif()

add_subdirectory(no-bypass)
add_subdirectory(rdma)
add_subdirectory(xdp)