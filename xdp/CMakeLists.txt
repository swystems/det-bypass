cmake_minimum_required(VERSION 3.10)

project("xdp")

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

# create a target pingpong, which compiles pingpong.c as a bpf program (-target bpf) using clang
add_custom_target(pingpong
        COMMAND clang -Wall -O2 -g -target bpf -c ${CMAKE_CURRENT_SOURCE_DIR}/pingpong.c -o ${CMAKE_CURRENT_BINARY_DIR}/pingpong.o
)

link_libraries(bpf xdp)
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -O2 -g -Wall -Werror -Wextra -Wpedantic")
# glob all files in src/*.c
file(GLOB SOURCES src/*.c)
add_executable(loader ${SOURCES} loader.c)

# add xdp-loading.c xdp-loading.h as dependency of loader
add_dependencies(loader pingpong)