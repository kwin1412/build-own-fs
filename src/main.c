#include "lfs.h"

#include <inttypes.h>
#include <stdio.h>

#define BLOCK_SIZE (4096)
#define BLOCK_COUNT (1024)

static const char image_path[] = "./fs.img";

static int block_dev_read_impl(void *buffer, uint32_t block_id)
{
    if (!buffer || block_id >= BLOCK_COUNT) {
        return -1;
    }

    FILE *fp = fopen(image_path, "rb");
    if (!fp) {
        return -1;
    }

    int result = 0;
    if (fseek(fp, (long)block_id * BLOCK_SIZE, SEEK_SET) != 0 ||
        fread(buffer, 1, BLOCK_SIZE, fp) != (size_t)BLOCK_SIZE) {
        result = -1;
    }
    if (fclose(fp) != 0) {
        result = -1;
    }
    return result;
}

static int block_dev_write_impl(const void *buffer, uint32_t block_id)
{
    if (!buffer || block_id >= BLOCK_COUNT) {
        return -1;
    }

    FILE *fp = fopen(image_path, "r+b");
    if (!fp) {
        return -1;
    }

    int result = 0;
    if (fseek(fp, (long)block_id * BLOCK_SIZE, SEEK_SET) != 0 ||
        fwrite(buffer, 1, BLOCK_SIZE, fp) != (size_t)BLOCK_SIZE) {
        result = -1;
    }
    if (fclose(fp) != 0) {
        result = -1;
    }
    return result;
}

static int fail(lfs_t *lfs, const char *message)
{
    fprintf(stderr, "%s\n", message);
    lfs_unmount(lfs);
    return 1;
}

int main(void)
{
    const lfs_config_t cfg = {
        .read = block_dev_read_impl,
        .write = block_dev_write_impl,

        .block_size = BLOCK_SIZE,
        .block_count = BLOCK_COUNT,
    };

    lfs_t lfs = {0};
    lfs_file_t file = {0};
    uint32_t boot_count = 0;

    if (lfs_mount(&lfs, &cfg) != 0) {
        if (lfs_format(&lfs, &cfg) != 0) {
            return fail(&lfs, "format failed");
        }
        if (lfs_mount(&lfs, &cfg) != 0) {
            return fail(&lfs, "mount failed");
        }
    }

    if (lfs_file_open(&lfs, &file, "boot_count") != 0) {
        return fail(&lfs, "open failed");
    }

    int transferred = lfs_file_read(&lfs, &file, &boot_count,
                                    sizeof(boot_count));
    if (transferred < 0 ||
        (transferred != 0 && transferred != (int)sizeof(boot_count))) {
        return fail(&lfs, "read failed");
    }
    printf("stored boot_count: %" PRIu32 "\n", boot_count);

    boot_count += 1;
    if (lfs_file_rewind(&lfs, &file) != 0) {
        return fail(&lfs, "rewind failed");
    }
    transferred = lfs_file_write(&lfs, &file, &boot_count,
                                 sizeof(boot_count));
    if (transferred != (int)sizeof(boot_count)) {
        return fail(&lfs, "write failed");
    }
    if (lfs_file_close(&lfs, &file) != 0) {
        return fail(&lfs, "close failed");
    }

    if (lfs_dump(&lfs) != 0) {
        return fail(&lfs, "dump failed");
    }
    if (lfs_unmount(&lfs) != 0) {
        return fail(&lfs, "unmount failed");
    }

    printf("boot_count: %" PRIu32 "\n", boot_count);
    return 0;
}
