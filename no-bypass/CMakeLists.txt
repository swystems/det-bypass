cmake_minimum_required(VERSION 3.10)
project(no-bypass)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -O3 -g -Wall -Werror -Wextra -Wpedantic")

file(GLOB SOURCES src/*.c ../common/*.c)

add_executable(no-bypass ${SOURCES} main.c)