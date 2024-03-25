cmake_minimum_required(VERSION 3.10)

project(xdp)

include(${CMAKE_CURRENT_SOURCE_DIR}/../config.cmake)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

file(GLOB SOURCES src/*.c ../common/*.c)

string(REPLACE " " ";" CMAKE_C_FLAGS_LIST "${CMAKE_C_FLAGS} -g")
function(add_xdp_hook target)
    add_custom_command(
            OUTPUT ${target}.o
            COMMAND clang ${CMAKE_C_FLAGS_LIST} -DSERVER=${SERVER} -target bpf -c ${target}.c -o ${CMAKE_CURRENT_BINARY_DIR}/${target}.o
            DEPENDS ${target}.c ${SOURCES}
            WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
            COMMENT "Compiling XDP program ${target}.c"
    )
    add_custom_target(${target} ALL DEPENDS ${target}.o)
endfunction()

add_xdp_hook(pingpong)
add_xdp_hook(pingpong_xsk)
add_xdp_hook(pingpong_pure)

link_libraries(bpf xdp)

add_executable(pp_poll ${SOURCES} pp_poll.c)
add_dependencies(pp_poll pingpong)

add_executable(pp_sock ${SOURCES} pp_sock.c)
add_dependencies(pp_sock pingpong_xsk)

add_executable(pp_pure ${SOURCES} pp_pure.c)
add_dependencies(pp_pure pingpong_pure)
