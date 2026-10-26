add_library(lfs_test_support STATIC test_support.c)
target_link_libraries(lfs_test_support PUBLIC lfs)
target_include_directories(lfs_test_support
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}
)
lfs_enable_warnings(lfs_test_support)

set(LFS_TEST_NAMES format_mount file_io dir_ops metadata)
foreach(name IN LISTS LFS_TEST_NAMES)
    add_executable(test_${name} test_${name}.c)
    target_link_libraries(test_${name} PRIVATE lfs_test_support)
    lfs_enable_warnings(test_${name})
    add_test(NAME lfs.${name} COMMAND test_${name})
endforeach()
