#include "lfs.h"

#include <stdio.h>

#define BLOCK_SIZE      (4096)
#define BLOCK_COUNT     (1024)


static const char image_path[] = "./fs.img";

static int block_dev_read_impl(void *buffer, uint32_t block_id) {
    // Implement the actual block device read logic here
    FILE *fp = fopen(image_path, "rb");
    if (!fp) {
        return -1;
    }

    fseek(fp, block_id * BLOCK_SIZE, SEEK_SET);
    fread(buffer, 1, BLOCK_SIZE, fp);
    fclose(fp);

    return 0;
}

static int block_dev_write_impl(const void *buffer, uint32_t block_id) {
    // Implement the actual block device write logic here
    FILE *fp = fopen(image_path, "rb+");
    if (!fp) {
        return -1;
    }

    fseek(fp, block_id * BLOCK_SIZE, SEEK_SET);
    fwrite(buffer, 1, BLOCK_SIZE, fp);
    fclose(fp);

    return 0;
}




int main(void) {

    const lfs_config_t cfg = {
        .read = block_dev_read_impl,
        .write = block_dev_write_impl,

        .block_size = BLOCK_SIZE,
        .block_count = BLOCK_COUNT,
    };

    lfs_t lfs;
    int err = lfs_mount(&lfs, &cfg);
    if (err) {
        // Handle mount error
        lfs_format(&lfs, &cfg);
        lfs_mount(&lfs, &cfg);
    }


    // read current count
    uint32_t boot_count = 0;
    lfs_file_t file;
    lfs_file_open(&lfs, &file, "boot_count");
    lfs_file_read(&lfs, &file, &boot_count, sizeof(boot_count));

    // update boot count
    boot_count += 1;
    lfs_file_rewind(&lfs, &file);
    lfs_file_write(&lfs, &file, &boot_count, sizeof(boot_count));

    // remember the storage is not updated until the file is closed successfully
    lfs_file_close(&lfs, &file);

    // release any resources we were using
    lfs_unmount(&lfs);

    // print the boot count
    printf("boot_count: %d\n", boot_count);


    return 0;
}