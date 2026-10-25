#pragma once

#include <stdint.h>
#include "lfs_struct.h"

typedef int block_dev_read(void *buffer, uint32_t block_id);
typedef int block_dev_write(const void *buffer, uint32_t block_id);

typedef uint8_t bmp_t;

typedef struct {
    // for mount and format
    block_dev_read *read;
    block_dev_write *write;

    // for format
    uint32_t block_size;
    uint32_t block_count;
} lfs_config_t;

typedef struct {
    block_dev_read *read;
    block_dev_write *write;

    super_block_t sb;

    bmp_t *block_bmp;
    bmp_t *inode_bmp;

    inode_t *inode_table;
    dir_entry_t *dir_table;
} lfs_t;

typedef struct {
    uint32_t block_id;
    uint32_t length;
} lfs_file_t;



// file system API declarations
int lfs_mount(lfs_t *lfs, const lfs_config_t *config);
int lfs_unmount(lfs_t *lfs);
int lfs_format(lfs_t *fs, const lfs_config_t *config);
int lfs_dump(lfs_t *fs);

// file API declarations
int lfs_file_open(lfs_t *lfs, lfs_file_t *file, const char *path);
int lfs_file_close(lfs_t *lfs, lfs_file_t *file);
int lfs_file_read(lfs_t *lfs, lfs_file_t *file, void *buffer, uint32_t size);
int lfs_file_write(lfs_t *lfs, lfs_file_t *file, const void *buffer, uint32_t size);
int lfs_file_rewind(lfs_t *lfs, lfs_file_t *file);
int lfs_file_seek(lfs_t *lfs, lfs_file_t *file, int32_t offset, uint32_t whence);
int lfs_file_dump(lfs_t *lfs, lfs_file_t *file);
int lfs_remove(lfs_t *lfs, const char *path);
int lfs_rename(lfs_t *lfs, const char *oldpath, const char *newpath);

