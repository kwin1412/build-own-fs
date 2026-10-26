#include "test_support.h"

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

static int create_file(lfs_t *fs, const char *name, const char *data)
{
    lfs_file_t file = {0};
    size_t len = strlen(data);

    if (lfs_file_open(fs, &file, name) != 0) {
        return -1;
    }
    if (lfs_file_write(fs, &file, data, (uint32_t)len) != (int)len) {
        return -1;
    }
    if (lfs_file_close(fs, &file) != 0) {
        return -1;
    }
    return 0;
}

static int check_file(lfs_t *fs, const char *name, const char *expected)
{
    lfs_file_t file = {0};
    char buffer[32] = {0};
    size_t len = strlen(expected);

    if (len >= sizeof(buffer)) {
        return -1;
    }
    if (lfs_file_open(fs, &file, name) != 0) {
        return -1;
    }
    if (lfs_file_read(fs, &file, buffer, (uint32_t)len) != (int)len) {
        return -1;
    }
    if (memcmp(buffer, expected, len) != 0) {
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

static int test_duplicate_open(void)
{
    lfs_t fs = {0};
    lfs_config_t cfg;
    CHECK(setup(&fs, &cfg) == 0);

    lfs_file_t first = {0};
    CHECK(lfs_file_open(&fs, &first, "a") == 0);
    uint32_t block_id = first.block_id;
    uint32_t entry_id = first.dir_entry_id;
    CHECK(lfs_file_write(&fs, &first, "x", 1) == 1);
    CHECK(lfs_file_close(&fs, &first) == 0);

    lfs_file_t second = {0};
    CHECK(lfs_file_open(&fs, &second, "a") == 0);
    CHECK(second.block_id == block_id);
    CHECK(second.dir_entry_id == entry_id);
    CHECK(second.length == 1);
    CHECK(lfs_file_close(&fs, &second) == 0);

    uint32_t count = 0;
    CHECK(count_valid_entries(&fs, &count) == 0);
    CHECK(count == 1);
    CHECK(lfs_unmount(&fs) == 0);
    return 0;
}

static int test_remove_unlink(void)
{
    lfs_t fs = {0};
    lfs_config_t cfg;
    CHECK(setup(&fs, &cfg) == 0);

    CHECK(create_file(&fs, "a", "A") == 0);
    CHECK(create_file(&fs, "b", "B") == 0);
    CHECK(create_file(&fs, "c", "C") == 0);

    CHECK(lfs_file_remove(&fs, "b") == 0);
    CHECK(lfs_file_rename(&fs, "b", "x") == -1);
    CHECK(check_file(&fs, "a", "A") == 0);
    CHECK(check_file(&fs, "c", "C") == 0);

    CHECK(lfs_file_remove(&fs, "a") == 0);
    CHECK(lfs_file_rename(&fs, "a", "x") == -1);
    CHECK(check_file(&fs, "c", "C") == 0);

    CHECK(lfs_file_remove(&fs, "c") == 0);
    CHECK(lfs_file_rename(&fs, "c", "x") == -1);
    CHECK(fs.sb.valid_dir_entry_id == TEST_INVALID_ID);
    CHECK(lfs_dump(&fs) == 0);

    CHECK(create_file(&fs, "d", "D") == 0);
    CHECK(fs.sb.valid_dir_entry_id != TEST_INVALID_ID);
    uint32_t count = 0;
    CHECK(count_valid_entries(&fs, &count) == 0);
    CHECK(count == 1);
    CHECK(check_file(&fs, "d", "D") == 0);
    CHECK(lfs_unmount(&fs) == 0);
    return 0;
}

static int test_free_counts(void)
{
    lfs_t fs = {0};
    lfs_config_t cfg;
    CHECK(setup(&fs, &cfg) == 0);

    uint32_t free_blocks = fs.sb.free_block_count;
    uint32_t free_inodes = fs.sb.free_inode_count;

    CHECK(create_file(&fs, "a", "x") == 0);
    CHECK(fs.sb.free_block_count == free_blocks - 1);
    CHECK(fs.sb.free_inode_count == free_inodes - 1);

    CHECK(create_file(&fs, "b", "y") == 0);
    CHECK(fs.sb.free_block_count == free_blocks - 2);
    CHECK(fs.sb.free_inode_count == free_inodes - 2);

    CHECK(lfs_file_remove(&fs, "a") == 0);
    CHECK(fs.sb.free_block_count == free_blocks - 1);
    CHECK(fs.sb.free_inode_count == free_inodes - 1);

    CHECK(lfs_file_remove(&fs, "b") == 0);
    CHECK(fs.sb.free_block_count == free_blocks);
    CHECK(fs.sb.free_inode_count == free_inodes);
    CHECK(lfs_unmount(&fs) == 0);
    return 0;
}

static int test_remove_invalid_metadata(void)
{
    lfs_t fs = {0};
    lfs_config_t cfg;
    CHECK(setup(&fs, &cfg) == 0);

    CHECK(create_file(&fs, "a", "x") == 0);
    uint32_t entry_id = fs.sb.valid_dir_entry_id;
    dir_entry_t *entry = &fs.dir_table[entry_id];
    uint32_t inode_id = entry->inode_id;
    uint32_t block_id = fs.inode_table[inode_id].block_id;

    entry->inode_id = fs.sb.total_inode_count;
    CHECK(lfs_file_remove(&fs, "a") == -1);
    entry->inode_id = inode_id;

    fs.inode_table[inode_id].block_id = 0;
    CHECK(lfs_file_remove(&fs, "a") == -1);
    CHECK(fs.inode_table[inode_id].block_id == 0);
    fs.inode_table[inode_id].block_id = block_id;

    CHECK(lfs_file_remove(&fs, "a") == 0);
    CHECK(fs.sb.valid_dir_entry_id == TEST_INVALID_ID);
    CHECK(lfs_unmount(&fs) == 0);
    return 0;
}

static int test_rename(void)
{
    lfs_t fs = {0};
    lfs_config_t cfg;
    CHECK(setup(&fs, &cfg) == 0);

    CHECK(create_file(&fs, "a", "hello") == 0);
    CHECK(lfs_file_rename(&fs, "a", "b") == 0);
    CHECK(lfs_file_rename(&fs, "a", "x") == -1);
    CHECK(check_file(&fs, "b", "hello") == 0);

    CHECK(create_file(&fs, "c", "c") == 0);
    CHECK(lfs_file_rename(&fs, "b", "c") == -1);
    CHECK(check_file(&fs, "b", "hello") == 0);
    CHECK(lfs_file_rename(&fs, "b", "b") == 0);

    CHECK(lfs_file_rename(&fs, "b", NULL) == -1);
    CHECK(lfs_file_rename(&fs, "b", "") == -1);

    char long_name[LFS_MAX_FILE_NAME_LEN + 2];
    memset(long_name, 'x', sizeof(long_name));
    long_name[sizeof(long_name) - 1] = '\0';
    CHECK(lfs_file_rename(&fs, "b", long_name) == -1);
    CHECK(check_file(&fs, "b", "hello") == 0);

    char max_name[LFS_MAX_FILE_NAME_LEN + 1];
    memset(max_name, 'y', LFS_MAX_FILE_NAME_LEN);
    max_name[LFS_MAX_FILE_NAME_LEN] = '\0';
    CHECK(lfs_file_rename(&fs, "b", max_name) == 0);
    CHECK(check_file(&fs, max_name, "hello") == 0);

    CHECK(lfs_file_rename(&fs, "missing", "z") == -1);
    CHECK(lfs_unmount(&fs) == 0);
    return 0;
}

static int test_broken_chain(void)
{
    lfs_t fs = {0};
    lfs_config_t cfg;
    CHECK(setup(&fs, &cfg) == 0);

    CHECK(create_file(&fs, "a", "a") == 0);
    CHECK(create_file(&fs, "b", "b") == 0);

    uint32_t head = fs.sb.valid_dir_entry_id;
    uint32_t tail = fs.dir_table[head].next_entry_id;
    CHECK(tail != TEST_INVALID_ID);
    fs.dir_table[tail].next_entry_id = fs.sb.total_block_count;

    lfs_file_t file = {0};
    CHECK(lfs_file_open(&fs, &file, "missing") == -1);
    CHECK(lfs_file_remove(&fs, "missing") == -1);
    CHECK(lfs_file_rename(&fs, "missing", "x") == -1);
    CHECK(lfs_dump(&fs) == -1);

    fs.dir_table[tail].next_entry_id = TEST_INVALID_ID;
    CHECK(lfs_file_open(&fs, &file, "missing") == 0);
    CHECK(lfs_file_close(&fs, &file) == 0);
    CHECK(lfs_unmount(&fs) == 0);
    return 0;
}

static int test_bitmap_protection(void)
{
    lfs_t fs = {0};
    lfs_config_t cfg;
    CHECK(setup(&fs, &cfg) == 0);

    fs.block_bmp[0] &= (bmp_t)~1u;

    lfs_file_t file = {0};
    CHECK(lfs_file_open(&fs, &file, "p") == 0);
    CHECK(file.block_id >= TEST_DATA_START_A);
    CHECK(lfs_file_close(&fs, &file) == 0);
    CHECK(lfs_unmount(&fs) == 0);
    return 0;
}

int main(void)
{
    int failures = 0;

    failures += test_duplicate_open();
    test_dev_fini();
    failures += test_remove_unlink();
    test_dev_fini();
    failures += test_free_counts();
    test_dev_fini();
    failures += test_remove_invalid_metadata();
    test_dev_fini();
    failures += test_rename();
    test_dev_fini();
    failures += test_broken_chain();
    test_dev_fini();
    failures += test_bitmap_protection();
    test_dev_fini();

    if (failures != 0) {
        fprintf(stderr, "dir_ops: %d test(s) failed\n", failures);
        return 1;
    }
    printf("dir_ops: all tests passed\n");
    return 0;
}
