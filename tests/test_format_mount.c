#include "test_support.h"

#include <stddef.h>
#include <string.h>

static int test_format_mount_roundtrip(void)
{
    lfs_t fs = {0};
    CHECK(test_dev_init(TEST_BLOCK_SIZE, TEST_BLOCK_COUNT_A) == 0);
    lfs_config_t cfg = test_dev_config();

    CHECK(lfs_format(&fs, &cfg) == 0);
    CHECK(fs.block_bmp == NULL);
    CHECK(fs.inode_bmp == NULL);
    CHECK(fs.inode_table == NULL);
    CHECK(fs.dir_table == NULL);

    CHECK(lfs_mount(&fs, &cfg) == 0);
    CHECK(fs.sb.magic == LFS_MAGIC);
    CHECK(fs.sb.version == LFS_VERSION);
    CHECK(fs.sb.block_size == TEST_BLOCK_SIZE);
    CHECK(fs.sb.total_block_count == TEST_BLOCK_COUNT_A);
    CHECK(fs.sb.total_inode_count == TEST_BLOCK_COUNT_A);
    CHECK(fs.sb.free_block_count ==
          TEST_BLOCK_COUNT_A - TEST_DATA_START_A);
    CHECK(fs.sb.free_inode_count == TEST_BLOCK_COUNT_A);
    CHECK(fs.sb.block_bmp_block_id == 1);
    CHECK(fs.sb.inode_bmp_block_id == 2);
    CHECK(fs.sb.inode_table_block_id == 3);
    CHECK(fs.sb.dir_table_block_id == 4);
    CHECK(fs.sb.valid_dir_entry_id == TEST_INVALID_ID);
    CHECK(fs.sb.free_dir_entry_id == 0);

    CHECK(lfs_unmount(&fs) == 0);
    CHECK(lfs_unmount(&fs) == -1);
    return 0;
}

static int test_mount_corruption(void)
{
    lfs_t fs = {0};
    CHECK(test_dev_init(TEST_BLOCK_SIZE, TEST_BLOCK_COUNT_A) == 0);
    lfs_config_t cfg = test_dev_config();
    CHECK(lfs_format(&fs, &cfg) == 0);

    super_block_t *sb = (super_block_t *)test_dev_block(0);
    CHECK(sb != NULL);
    uint32_t magic = sb->magic;
    uint32_t version = sb->version;
    uint32_t crc = sb->crc32;
    uint32_t dir_table_id = sb->dir_table_block_id;

    sb->magic = 0;
    CHECK(lfs_mount(&fs, &cfg) == -1);
    CHECK(fs.block_bmp == NULL && fs.inode_bmp == NULL);
    CHECK(fs.inode_table == NULL && fs.dir_table == NULL);
    sb->magic = magic;

    sb->version = LFS_VERSION + 1;
    sb->crc32 = test_crc32(sb, offsetof(super_block_t, crc32));
    CHECK(lfs_mount(&fs, &cfg) == -1);
    sb->version = version;
    sb->crc32 = crc;

    sb->crc32 = crc ^ 1u;
    CHECK(lfs_mount(&fs, &cfg) == -1);
    sb->crc32 = crc;

    sb->dir_table_block_id = TEST_BLOCK_COUNT_A;
    sb->crc32 = test_crc32(sb, offsetof(super_block_t, crc32));
    CHECK(lfs_mount(&fs, &cfg) == -1);
    sb->dir_table_block_id = dir_table_id;
    sb->crc32 = crc;

    lfs_config_t bad = cfg;
    bad.block_size = TEST_BLOCK_SIZE * 2;
    CHECK(lfs_mount(&fs, &bad) == -1);
    bad = cfg;
    bad.block_count = TEST_BLOCK_COUNT_A / 2;
    CHECK(lfs_mount(&fs, &bad) == -1);

    CHECK(lfs_mount(&fs, &cfg) == 0);
    CHECK(lfs_unmount(&fs) == 0);
    return 0;
}

static int test_config_validation(void)
{
    lfs_t fs = {0};
    CHECK(test_dev_init(TEST_BLOCK_SIZE, TEST_BLOCK_COUNT_A) == 0);
    lfs_config_t cfg = test_dev_config();

    CHECK(lfs_format(NULL, &cfg) == -1);
    CHECK(lfs_format(&fs, NULL) == -1);

    lfs_config_t bad = cfg;
    bad.read = NULL;
    CHECK(lfs_format(&fs, &bad) == -1);
    bad = cfg;
    bad.write = NULL;
    CHECK(lfs_format(&fs, &bad) == -1);
    bad = cfg;
    bad.block_size = LFS_MIN_BLOCK_SIZE - 1;
    CHECK(lfs_format(&fs, &bad) == -1);
    bad = cfg;
    bad.block_count = 0;
    CHECK(lfs_format(&fs, &bad) == -1);
    bad = cfg;
    bad.block_count = 4;
    CHECK(lfs_format(&fs, &bad) == -1);

    CHECK(lfs_format(&fs, &cfg) == 0);
    CHECK(fs.block_bmp == NULL && fs.inode_bmp == NULL);
    CHECK(fs.inode_table == NULL && fs.dir_table == NULL);

    CHECK(lfs_mount(&fs, &cfg) == 0);
    CHECK(lfs_format(&fs, &cfg) == -1);
    CHECK(lfs_mount(&fs, &cfg) == -1);
    CHECK(lfs_unmount(&fs) == 0);

    CHECK(lfs_format(&fs, &cfg) == 0);
    return 0;
}

static int test_fault_injection(void)
{
    lfs_t fs = {0};
    CHECK(test_dev_init(TEST_BLOCK_SIZE, TEST_BLOCK_COUNT_A) == 0);
    lfs_config_t cfg = test_dev_config();

    test_dev_fail_write(0);
    CHECK(lfs_format(&fs, &cfg) == -1);
    CHECK(fs.block_bmp == NULL && fs.inode_bmp == NULL);
    CHECK(fs.inode_table == NULL && fs.dir_table == NULL);
    test_dev_clear_faults();
    CHECK(lfs_format(&fs, &cfg) == 0);

    test_dev_fail_read(0);
    CHECK(lfs_mount(&fs, &cfg) == -1);
    CHECK(fs.block_bmp == NULL && fs.inode_bmp == NULL);
    test_dev_clear_faults();
    CHECK(lfs_mount(&fs, &cfg) == 0);
    CHECK(lfs_unmount(&fs) == 0);

    CHECK(test_dev_init(TEST_BLOCK_SIZE, TEST_BLOCK_COUNT_B) == 0);
    cfg = test_dev_config();
    CHECK(lfs_format(&fs, &cfg) == 0);
    test_dev_fail_read(8);
    CHECK(lfs_mount(&fs, &cfg) == -1);
    CHECK(fs.block_bmp == NULL && fs.inode_bmp == NULL);
    test_dev_clear_faults();
    CHECK(lfs_mount(&fs, &cfg) == 0);
    CHECK(lfs_unmount(&fs) == 0);
    return 0;
}

int main(void)
{
    int failures = 0;

    failures += test_format_mount_roundtrip();
    test_dev_fini();
    failures += test_mount_corruption();
    test_dev_fini();
    failures += test_config_validation();
    test_dev_fini();
    failures += test_fault_injection();
    test_dev_fini();

    if (failures != 0) {
        fprintf(stderr, "format_mount: %d test(s) failed\n", failures);
        return 1;
    }
    printf("format_mount: all tests passed\n");
    return 0;
}
