project(pingpong)

add_compile_options(-g -O2)

add_library(rc_pingpong_lib SHARED pingpong.c)
add_library(libibverbs SHARED IMPORTED)
set_property(TARGET libibverbs PROPERTY IMPORTED_LOCATION /usr/lib/x86_64-linux-gnu/libibverbs.so)

add_executable(rc_pingpong rc_pingpong.c)
target_link_libraries(rc_pingpong rc_pingpong_lib)
target_link_libraries(rc_pingpong libibverbs)

add_executable(srq_pingpong srq_pingpong.c)
target_link_libraries(srq_pingpong rc_pingpong_lib)
target_link_libraries(srq_pingpong libibverbs)

add_executable(uc_pingpong uc_pingpong.c)
target_link_libraries(uc_pingpong rc_pingpong_lib)
target_link_libraries(uc_pingpong libibverbs)

add_executable(ud_pingpong ud_pingpong.c)
target_link_libraries(ud_pingpong rc_pingpong_lib)
target_link_libraries(ud_pingpong libibverbs)

add_executable(xsrq_pingpong xsrq_pingpong.c)
target_link_libraries(xsrq_pingpong rc_pingpong_lib)
target_link_libraries(xsrq_pingpong libibverbs)