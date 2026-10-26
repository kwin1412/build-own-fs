#include "test_support.h"

#include <stdlib.h>
#include <string.h>

static int setup(lfs_t *fs, lfs_config_t *cfg)
{
    if (test_dev_init(TEST_BLOCK_SIZE, TEST_BLOCK_COUNT_A) != 0) {
        return -1;
    }
    *cfg = test_dev_config();
    memset(fs, 0, sizeof(*fs));
    if (lfs_format(fs, cfg) != 0) {
        return -1;
    }
    if (lfs_mount(fs, cfg) != 0) {
        return -1;
    }
    return 0;
}

static int test_zero_length_and_persistence(void)
{
    lfs_t fs = {0};
    lfs_config_t cfg;
    CHECK(setup(&fs, &cfg) == 0);

    lfs_file_t file = {0};
    CHECK(lfs_file_open(&fs, &file, "counter") == 0);
    CHECK(file.length == 0);
    CHECK(file.offset == 0);

    uint32_t value = 0;
    CHECK(lfs_file_read(&fs, &file, &value, sizeof(value)) == 0);
    CHECK(file.offset == 0);

    value = 42;
    CHECK(lfs_file_write(&fs, &file, &value, sizeof(value)) == 4);
    CHECK(file.length == 4);
    CHECK(file.offset == 4);
    CHECK(fs.dir_table[file.dir_entry_id].length == 4);

    CHECK(lfs_file_rewind(&fs, &file) == 0);
    value = 0;
    CHECK(lfs_file_read(&fs, &file, &value, sizeof(value)) == 4);
    CHECK(value == 42);
    CHECK(file.offset == 4);
    CHECK(lfs_file_read(&fs, &file, &value, sizeof(value)) == 0);

    CHECK(lfs_file_close(&fs, &file) == 0);
    CHECK(lfs_unmount(&fs) == 0);

    CHECK(lfs_mount(&fs, &cfg) == 0);
    memset(&file, 0, sizeof(file));
    CHECK(lfs_file_open(&fs, &file, "counter") == 0);
    CHECK(file.length == 4);
    value = 0;
    CHECK(lfs_file_read(&fs, &file, &value, sizeof(value)) == 4);
    CHECK(value == 42);
    CHECK(lfs_file_close(&fs, &file) == 0);
    CHECK(lfs_unmount(&fs) == 0);
    return 0;
}

static int test_read_modify_write(void)
{
    lfs_t fs = {0};
    lfs_config_t cfg;
    CHECK(setup(&fs, &cfg) == 0);

    lfs_file_t file = {0};
    CHECK(lfs_file_open(&fs, &file, "f") == 0);
    CHECK(lfs_file_write(&fs, &file, "abcdef", 6) == 6);
    CHECK(lfs_file_seek(&fs, &file, 3, 0) == 0);
    CHECK(lfs_file_write(&fs, &file, "XY", 2) == 2);
    CHECK(file.length == 6);
    CHECK(file.offset == 5);

    char buf[8] = {0};
    CHECK(lfs_file_rewind(&fs, &file) == 0);
    CHECK(lfs_file_read(&fs, &file, buf, 6) == 6);
    CHECK(memcmp(buf, "abcXYf", 6) == 0);

    CHECK(lfs_file_seek(&fs, &file, 0, 2) == 0);
    CHECK(file.offset == 6);
    CHECK(lfs_file_write(&fs, &file, "ZZ", 2) == 2);
    CHECK(file.length == 8);

    memset(buf, 0, sizeof(buf));
    CHECK(lfs_file_rewind(&fs, &file) == 0);
    CHECK(lfs_file_read(&fs, &file, buf, 8) == 8);
    CHECK(memcmp(buf, "abcXYfZZ", 8) == 0);

    uint8_t *block = test_dev_block(file.block_id);
    CHECK(block != NULL);
    for (uint32_t i = 8; i < TEST_BLOCK_SIZE; i++) {
        CHECK(block[i] == 0);
    }

    CHECK(lfs_file_close(&fs, &file) == 0);
    CHECK(lfs_unmount(&fs) == 0);
    return 0;
}

static int test_capacity_and_seek(void)
{
    lfs_t fs = {0};
    lfs_config_t cfg;
    CHECK(setup(&fs, &cfg) == 0);

    uint8_t *data = malloc(TEST_BLOCK_SIZE);
    CHECK(data != NULL);
    for (uint32_t i = 0; i < TEST_BLOCK_SIZE; i++) {
        data[i] = (uint8_t)(i & 0xffu);
    }

    lfs_file_t file = {0};
    CHECK(lfs_file_open(&fs, &file, "big") == 0);
    CHECK(lfs_file_write(&fs, &file, data, TEST_BLOCK_SIZE) ==
          (int)TEST_BLOCK_SIZE);
    CHECK(file.length == TEST_BLOCK_SIZE);
    CHECK(file.offset == TEST_BLOCK_SIZE);
    CHECK(lfs_file_write(&fs, &file, data, 1) == -1);

    CHECK(lfs_file_seek(&fs, &file, (int32_t)(TEST_BLOCK_SIZE - 2), 0) == 0);
    CHECK(lfs_file_write(&fs, &file, data, 4) == 2);
    CHECK(file.length == TEST_BLOCK_SIZE);
    CHECK(file.offset == TEST_BLOCK_SIZE);
    data[TEST_BLOCK_SIZE - 2] = data[0];
    data[TEST_BLOCK_SIZE - 1] = data[1];
    CHECK(lfs_file_write(&fs, &file, NULL, 0) == 0);

    CHECK(lfs_file_seek(&fs, &file, -1, 2) == 0);
    CHECK(file.offset == TEST_BLOCK_SIZE - 1);
    CHECK(lfs_file_seek(&fs, &file, 0, 2) == 0);
    CHECK(file.offset == TEST_BLOCK_SIZE);
    CHECK(lfs_file_seek(&fs, &file, 1, 2) == -1);
    CHECK(file.offset == TEST_BLOCK_SIZE);
    CHECK(lfs_file_seek(&fs, &file, (int32_t)TEST_BLOCK_SIZE + 1, 0) == -1);
    CHECK(file.offset == TEST_BLOCK_SIZE);
    CHECK(lfs_file_seek(&fs, &file, 0, 3) == -1);
    CHECK(file.offset == TEST_BLOCK_SIZE);

    CHECK(lfs_file_seek(&fs, &file, -2, 1) == 0);
    CHECK(file.offset == TEST_BLOCK_SIZE - 2);
    CHECK(lfs_file_seek(&fs, &file, -((int32_t)TEST_BLOCK_SIZE + 1), 1) ==
          -1);
    CHECK(file.offset == TEST_BLOCK_SIZE - 2);

    CHECK(lfs_file_rewind(&fs, &file) == 0);
    uint8_t *out = malloc(TEST_BLOCK_SIZE);
    CHECK(out != NULL);
    CHECK(lfs_file_read(&fs, &file, out, TEST_BLOCK_SIZE) ==
          (int)TEST_BLOCK_SIZE);
    CHECK(memcmp(out, data, TEST_BLOCK_SIZE) == 0);

    free(out);
    free(data);
    CHECK(lfs_file_close(&fs, &file) == 0);
    CHECK(lfs_unmount(&fs) == 0);
    return 0;
}

static int test_callback_errors(void)
{
    lfs_t fs = {0};
    lfs_config_t cfg;
    CHECK(setup(&fs, &cfg) == 0);

    uint8_t data[4] = {1, 2, 3, 4};
    uint8_t out[4] = {0};
    lfs_file_t file = {0};
    CHECK(lfs_file_open(&fs, &file, "err") == 0);
    CHECK(lfs_file_write(&fs, &file, data, sizeof(data)) == 4);
    CHECK(lfs_file_rewind(&fs, &file) == 0);

    test_dev_fail_read(file.block_id);
    CHECK(lfs_file_read(&fs, &file, out, 1) == -1);
    CHECK(file.offset == 0);
    test_dev_clear_faults();

    test_dev_fail_write(file.block_id);
    CHECK(lfs_file_write(&fs, &file, data, 2) == -1);
    CHECK(file.offset == 0);
    CHECK(file.length == 4);
    test_dev_clear_faults();

    CHECK(lfs_file_read(&fs, &file, out, sizeof(out)) == 4);
    CHECK(memcmp(out, data, sizeof(data)) == 0);

    CHECK(lfs_file_close(&fs, &file) == 0);
    CHECK(lfs_unmount(&fs) == 0);
    return 0;
}

static int test_invalid_handles(void)
{
    lfs_t fs = {0};
    lfs_config_t cfg;
    CHECK(setup(&fs, &cfg) == 0);

    lfs_file_t bad = {0};
    CHECK(lfs_file_read(&fs, &bad, NULL, 0) == -1);
    CHECK(lfs_file_write(&fs, &bad, NULL, 0) == -1);
    CHECK(lfs_file_close(&fs, &bad) == -1);
    CHECK(lfs_file_rewind(&fs, &bad) == -1);
    CHECK(lfs_file_seek(&fs, &bad, 0, 0) == -1);
    CHECK(lfs_file_dump(&fs, &bad) == -1);

    lfs_file_t file = {0};
    CHECK(lfs_file_open(&fs, &file, NULL) == -1);
    CHECK(lfs_file_open(&fs, &file, "") == -1);
    CHECK(lfs_file_open(NULL, &file, "x") == -1);
    CHECK(lfs_file_open(&fs, NULL, "x") == -1);

    CHECK(lfs_file_open(&fs, &file, "ok") == 0);
    CHECK(lfs_file_read(&fs, &file, NULL, 1) == -1);
    CHECK(lfs_file_write(&fs, &file, NULL, 1) == -1);
    CHECK(lfs_file_read(&fs, &file, NULL, 0) == 0);
    CHECK(lfs_file_write(&fs, &file, NULL, 0) == 0);
    CHECK(lfs_file_close(&fs, &file) == 0);
    CHECK(lfs_unmount(&fs) == 0);
    return 0;
}

static int test_unmount_without_close(void)
{
    lfs_t fs = {0};
    lfs_config_t cfg;
    CHECK(setup(&fs, &cfg) == 0);

    lfs_file_t file = {0};
    uint32_t value = 7;
    CHECK(lfs_file_open(&fs, &file, "u") == 0);
    CHECK(lfs_file_write(&fs, &file, &value, sizeof(value)) == 4);
    CHECK(lfs_unmount(&fs) == 0);

    CHECK(lfs_mount(&fs, &cfg) == 0);
    memset(&file, 0, sizeof(file));
    CHECK(lfs_file_open(&fs, &file, "u") == 0);
    CHECK(file.length == 4);
    value = 0;
    CHECK(lfs_file_read(&fs, &file, &value, sizeof(value)) == 4);
    CHECK(value == 7);
    CHECK(lfs_file_close(&fs, &file) == 0);
    CHECK(lfs_unmount(&fs) == 0);
    return 0;
}

int main(void)
{
    int failures = 0;

    failures += test_zero_length_and_persistence();
    test_dev_fini();
    failures += test_read_modify_write();
    test_dev_fini();
    failures += test_capacity_and_seek();
    test_dev_fini();
    failures += test_callback_errors();
    test_dev_fini();
    failures += test_invalid_handles();
    test_dev_fini();
    failures += test_unmount_without_close();
    test_dev_fini();

    if (failures != 0) {
        fprintf(stderr, "file_io: %d test(s) failed\n", failures);
        return 1;
    }
    printf("file_io: all tests passed\n");
    return 0;
}
