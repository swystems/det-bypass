cmake_minimum_required(VERSION 3.10)

project("xdp")

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

# create a target pingpong, which compiles pingpong.c as a bpf program (-target bpf) using clang
add_custom_target(pingpong
        COMMAND clang -Wall -O2 -g -target bpf -c ${CMAKE_CURRENT_SOURCE_DIR}/pingpong.c -o ${CMAKE_CURRENT_BINARY_DIR}/pingpong.o
)

link_libraries(bpf xdp)
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wall -O2 -g")
add_executable(loader loader.c)