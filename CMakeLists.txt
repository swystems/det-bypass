cmake_minimum_required(VERSION 3.10)
project(det-bypass)

include({CMAKE_CURRENT_SOURCE_DIR}/config.cmake)

add_subdirectory(no-bypass)
add_subdirectory(rdma)
add_subdirectory(xdp)