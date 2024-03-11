cmake_minimum_required(VERSION 3.10)
project(det-bypass)

if (NOT DEFINED __SERVER__)
    message(FATAL_ERROR "Please specify the __SERVER__ compilation variable")
endif ()
add_definitions(-D__SERVER__=${__SERVER__})

if (__SERVER__)
    message(STATUS "Compiling for the server machine")
else ()
    message(STATUS "Compiling for the client machine")
endif ()

if (NOT DEFINED DEBUG)
    set(DEBUG 0)
endif ()
add_definitions(-DDEBUG=${DEBUG})

if (DEBUG)
    message(STATUS "Compiling with debug information")
endif ()

add_subdirectory(no-bypass)
add_subdirectory(rdma)
add_subdirectory(xdp)