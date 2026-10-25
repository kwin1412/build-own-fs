#pragma once

#include <assert.h>
#include <stdint.h>


#define LFS_MAGIC 0x004c4653u
#define LFS_VERSION 1u
#define LFS_MIN_BLOCK_SIZE 1024u
#define LFS_MAX_FILE_NAME_LEN 64u

/*
| super block | block bitmap | inode bitmap | inode table | dir_inode_entry | data block |
*/

typedef struct {
    uint32_t magic;
    uint32_t version;

    uint32_t block_size;
    uint32_t total_block_count;
    uint32_t free_block_count;

    uint32_t total_inode_count;
    uint32_t free_inode_count;

    uint32_t block_bmp_block_id;
    uint32_t inode_bmp_block_id;

    uint32_t inode_table_block_id;

    uint32_t dir_table_block_id;

    uint32_t valid_dir_entry_id;
    uint32_t free_dir_entry_id;

    uint32_t crc32;
} super_block_t;

static_assert(sizeof(super_block_t) < LFS_MIN_BLOCK_SIZE, "super_block_t must be less than LFS_MIN_BLOCK_SIZE");


typedef struct {
    uint32_t block_id;
    uint32_t next_inode_id;
} inode_t;

static_assert(sizeof(inode_t) < LFS_MIN_BLOCK_SIZE, "inode_t must be less than LFS_MIN_BLOCK_SIZE");

typedef struct {
    uint32_t inode_id;
    uint32_t length;
    uint32_t next_entry_id;
    char name[LFS_MAX_FILE_NAME_LEN + 1];
} dir_entry_t;

static_assert(sizeof(dir_entry_t) < LFS_MIN_BLOCK_SIZE, "dir_entry_t must be less than LFS_MIN_BLOCK_SIZE");