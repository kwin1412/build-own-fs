cmake_minimum_required(VERSION 3.16)
project(build_own_fs VERSION 0.3.0 LANGUAGES C)

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(LFS_SANITIZE "Enable AddressSanitizer and UndefinedBehaviorSanitizer" OFF)
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    add_compile_options(-Wall -Wextra -Wpedantic)
    if(LFS_SANITIZE)
        add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
        add_link_options(-fsanitize=address,undefined)
    endif()
endif()

add_library(lfs STATIC
    src/lfs.c
    src/lfs_device.c
    src/lfs_storage.c
    src/lfs_inode.c
    src/lfs_dir.c
    src/lfs_dump.c
)
target_include_directories(lfs PUBLIC include PRIVATE src)

add_executable(build_own_fs src/main.c)
target_link_libraries(build_own_fs PRIVATE lfs)

include(CTest)
if(BUILD_TESTING)
    foreach(name mount block_alloc file name fault dump)
        add_executable(lfs_${name}_test tests/lfs_${name}_test.c)
        target_include_directories(lfs_${name}_test PRIVATE src tests)
        target_link_libraries(lfs_${name}_test PRIVATE lfs)
        add_test(NAME lfs_${name} COMMAND lfs_${name}_test)
    endforeach()
    add_test(NAME lfs_image COMMAND ${CMAKE_COMMAND}
        -DEXAMPLE=$<TARGET_FILE:build_own_fs>
        -DTEST_DIR=${CMAKE_CURRENT_BINARY_DIR}/image-test
        -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/image_test.cmake)
endif()
