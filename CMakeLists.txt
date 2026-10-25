cmake_minimum_required(VERSION 3.16)
project(build_own_fs VERSION 0.3.0 LANGUAGES C)

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# 递归查找 src 下所有 .c 文件
file(GLOB_RECURSE SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.c"
)

# 递归查找 include 下所有 .h 文件
file(GLOB_RECURSE HEADERS CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/include/*.h"
)

add_library(lfs STATIC
    ${SOURCES}
    ${HEADERS}
)
target_include_directories(lfs PUBLIC include PRIVATE src)

add_executable(build_own_fs src/main.c)
target_link_libraries(build_own_fs PRIVATE lfs)
