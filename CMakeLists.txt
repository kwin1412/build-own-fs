cmake_minimum_required(VERSION 3.16)
project(build_own_fs VERSION 0.3.0 LANGUAGES C)

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

function(lfs_enable_warnings target)
    if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(${target} PRIVATE
            -Wall
            -Wextra
            -Wpedantic
            -Wshadow
        )
    endif()
endfunction()

option(LFS_SANITIZE
    "Enable AddressSanitizer and UndefinedBehaviorSanitizer" OFF
)
if(LFS_SANITIZE)
    if(NOT CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
        message(FATAL_ERROR "LFS_SANITIZE requires GCC or Clang")
    endif()
    add_compile_options(
        -fsanitize=address,undefined
        -fno-omit-frame-pointer
        -g
    )
    add_link_options(-fsanitize=address,undefined)
endif()

# 核心库只包含文件系统实现，示例程序单独编译。
add_library(lfs STATIC
    src/lfs.c
    include/lfs.h
    include/lfs_struct.h
)
target_include_directories(lfs
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
)
lfs_enable_warnings(lfs)

add_executable(build_own_fs src/main.c)
target_link_libraries(build_own_fs PRIVATE lfs)
lfs_enable_warnings(build_own_fs)

include(CTest)
if(BUILD_TESTING)
    add_subdirectory(tests)
endif()
