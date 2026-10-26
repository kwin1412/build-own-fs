#include "test_support.h"

#include <inttypes.h>
#include <string.h>

#define FILE_COUNT (140u)

static int setup(lfs_t *fs, lfs_config_t *cfg)
{
    if (test_dev_init(TEST_BLOCK_SIZE, TEST_BLOCK_COUNT_B) != 0) {
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

static void file_name(char *buffer, size_t size, uint32_t index)
{
    snprintf(buffer, size, "f%03" PRIu32, index);
}

static int create_value_file(lfs_t *fs, const char *name, uint32_t value)
{
    lfs_file_t file = {0};

    if (lfs_file_open(fs, &file, name) != 0) {
        return -1;
    }
    if (lfs_file_write(fs, &file, &value, sizeof(value)) !=
        (int)sizeof(value)) {
        return -1;
    }
    if (lfs_file_close(fs, &file) != 0) {
        return -1;
    }
    return 0;
}

static int check_value_file(lfs_t *fs, const char *name, uint32_t expected)
{
    lfs_file_t file = {0};
    uint32_t value = 0;

    if (lfs_file_open(fs, &file, name) != 0) {
        return -1;
    }
    if (file.length != sizeof(value)) {
        return -1;
    }
    if (lfs_file_read(fs, &file, &value, sizeof(value)) !=
        (int)sizeof(value)) {
        return -1;
    }
    if (value != expected) {
        return -1;
    }
    if (lfs_file_close(fs, &file) != 0) {
        return -1;
    }
    return 0;
}

static int count_valid_entries(lfs_t *fs, uint32_t *out_count)
{
    uint32_t dir_entry_id = fs->sb.valid_dir_entry_id;
    uint32_t visited = 0;

    while (dir_entry_id != TEST_INVALID_ID) {
        if (dir_entry_id >= fs->sb.total_block_count ||
            visited >= fs->sb.total_block_count) {
            return -1;
        }
        dir_entry_id = fs->dir_table[dir_entry_id].next_entry_id;
        visited++;
    }
    *out_count = visited;
    return 0;
}

static int test_layout(void)
{
    lfs_t fs = {0};
    lfs_config_t cfg;
    CHECK(setup(&fs, &cfg) == 0);

    CHECK(fs.sb.block_bmp_block_id == 1);
    CHECK(fs.sb.inode_bmp_block_id == 2);
    CHECK(fs.sb.inode_table_block_id == 3);
    CHECK(fs.sb.dir_table_block_id == 7);
    CHECK(fs.sb.free_block_count ==
          TEST_BLOCK_COUNT_B - TEST_DATA_START_B);
    CHECK(fs.sb.free_inode_count == TEST_BLOCK_COUNT_B);
    CHECK(lfs_unmount(&fs) == 0);
    return 0;
}

static int test_multi_block_roundtrip(void)
{
    lfs_t fs = {0};
    lfs_config_t cfg;
    CHECK(setup(&fs, &cfg) == 0);

    uint32_t max_entry_id = 0;
    uint32_t max_inode_id = 0;
    for (uint32_t i = 0; i < FILE_COUNT; i++) {
        char name[16];
        file_name(name, sizeof(name), i);

        lfs_file_t file = {0};
        uint32_t value = i;
        CHECK(lfs_file_open(&fs, &file, name) == 0);
        CHECK(lfs_file_write(&fs, &file, &value, sizeof(value)) == 4);
        CHECK(lfs_file_close(&fs, &file) == 0);

        if (file.dir_entry_id > max_entry_id) {
            max_entry_id = file.dir_entry_id;
        }
        uint32_t inode_id = fs.dir_table[file.dir_entry_id].inode_id;
        if (inode_id > max_inode_id) {
            max_inode_id = inode_id;
        }
    }

    CHECK(max_entry_id >= 12);
    CHECK(max_inode_id >= 128);
    CHECK(fs.sb.free_block_count ==
          TEST_BLOCK_COUNT_B - TEST_DATA_START_B - FILE_COUNT);
    CHECK(fs.sb.free_inode_count == TEST_BLOCK_COUNT_B - FILE_COUNT);

    uint32_t count = 0;
    CHECK(count_valid_entries(&fs, &count) == 0);
    CHECK(count == FILE_COUNT);

    CHECK(lfs_unmount(&fs) == 0);
    CHECK(lfs_mount(&fs, &cfg) == 0);

    const uint32_t samples[] = {0, 12, 13, 127, 128, FILE_COUNT - 1};
    for (size_t i = 0; i < sizeof(samples) / sizeof(samples[0]); i++) {
        char name[16];
        file_name(name, sizeof(name), samples[i]);
        CHECK(check_value_file(&fs, name, samples[i]) == 0);
    }

    count = 0;
    CHECK(count_valid_entries(&fs, &count) == 0);
    CHECK(count == FILE_COUNT);
    CHECK(lfs_dump(&fs) == 0);

    const char *removed[] = {"f000", "f070", "f139"};
    for (size_t i = 0; i < sizeof(removed) / sizeof(removed[0]); i++) {
        CHECK(lfs_file_remove(&fs, removed[i]) == 0);
    }

    CHECK(lfs_unmount(&fs) == 0);
    CHECK(lfs_mount(&fs, &cfg) == 0);

    for (size_t i = 0; i < sizeof(removed) / sizeof(removed[0]); i++) {
        CHECK(lfs_file_rename(&fs, removed[i], "gone") == -1);
    }
    CHECK(check_value_file(&fs, "f001", 1) == 0);
    CHECK(check_value_file(&fs, "f138", 138) == 0);
    CHECK(fs.sb.free_block_count ==
          TEST_BLOCK_COUNT_B - TEST_DATA_START_B - (FILE_COUNT - 3));
    CHECK(fs.sb.free_inode_count ==
          TEST_BLOCK_COUNT_B - (FILE_COUNT - 3));

    count = 0;
    CHECK(count_valid_entries(&fs, &count) == 0);
    CHECK(count == FILE_COUNT - 3);
    CHECK(lfs_dump(&fs) == 0);

    uint32_t head = fs.sb.valid_dir_entry_id;
    uint32_t tail = head;
    uint32_t visited = 0;
    while (fs.dir_table[tail].next_entry_id != TEST_INVALID_ID) {
        CHECK(visited < fs.sb.total_block_count);
        tail = fs.dir_table[tail].next_entry_id;
        visited++;
    }
    fs.dir_table[tail].next_entry_id = head;
    CHECK(lfs_dump(&fs) == -1);
    fs.dir_table[tail].next_entry_id = TEST_INVALID_ID;
    CHECK(lfs_dump(&fs) == 0);

    CHECK(lfs_unmount(&fs) == 0);
    return 0;
}

static int test_crc_after_mutation(void)
{
    lfs_t fs = {0};
    lfs_config_t cfg;
    CHECK(setup(&fs, &cfg) == 0);

    CHECK(create_value_file(&fs, "a", 1) == 0);
    CHECK(lfs_file_remove(&fs, "a") == 0);
    CHECK(create_value_file(&fs, "b", 2) == 0);
    CHECK(lfs_file_rename(&fs, "b", "c") == 0);
    CHECK(lfs_unmount(&fs) == 0);

    CHECK(lfs_mount(&fs, &cfg) == 0);
    CHECK(check_value_file(&fs, "c", 2) == 0);
    CHECK(lfs_unmount(&fs) == 0);
    return 0;
}

static int test_default_layout(void)
{
    lfs_t fs = {0};
    CHECK(test_dev_init(4096, 1024) == 0);
    lfs_config_t cfg = test_dev_config();

    CHECK(lfs_format(&fs, &cfg) == 0);
    CHECK(lfs_mount(&fs, &cfg) == 0);
    CHECK(fs.sb.block_bmp_block_id == 1);
    CHECK(fs.sb.inode_bmp_block_id == 2);
    CHECK(fs.sb.inode_table_block_id == 3);
    CHECK(fs.sb.dir_table_block_id == 5);
    CHECK(fs.sb.free_block_count == 999);
    CHECK(fs.sb.free_inode_count == 1024);

    uint32_t value = 1;
    CHECK(create_value_file(&fs, "boot_count", value) == 0);
    CHECK(lfs_unmount(&fs) == 0);

    CHECK(lfs_mount(&fs, &cfg) == 0);
    CHECK(check_value_file(&fs, "boot_count", value) == 0);
    CHECK(lfs_unmount(&fs) == 0);
    return 0;
}

int main(void)
{
    int failures = 0;

    failures += test_layout();
    test_dev_fini();
    failures += test_multi_block_roundtrip();
    test_dev_fini();
    failures += test_crc_after_mutation();
    test_dev_fini();
    failures += test_default_layout();
    test_dev_fini();

    if (failures != 0) {
        fprintf(stderr, "metadata: %d test(s) failed\n", failures);
        return 1;
    }
    printf("metadata: all tests passed\n");
    return 0;
}
