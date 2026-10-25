#include "lfs.h"
#include <stddef.h>
#include <stdint.h>
#include "lfs_struct.h"
#include <string.h>
#include <stdlib.h>


#define DIV_ROUND_UP(n, d) (((n) + (d) - 1) / (d))

#define INVALID_ID 0xFFFFFFFFu

static uint32_t _crc32(const void *data, size_t size) {
    // Implement the CRC32 calculation here
    // This is a placeholder implementation
    const unsigned char *ptr = (const unsigned char *)data;
    int crc = 0xFFFFFFFF;
    for (size_t i = 0; i < size; i++) {
        crc ^= ptr[i];
        for (int j = 0; j < 8; j++) {
            if (crc & 1) {
                crc = (crc >> 1) ^ 0xEDB88320;
            } else {
                crc >>= 1;
            }
        }
    }
    return crc ^ 0xFFFFFFFF;
}

int lfs_mount(lfs_t *lfs, const lfs_config_t *config) {

    lfs->read = config->read;
    lfs->write = config->write;

    uint32_t block_size = config->block_size;
    uint32_t block_id = 0;

    char *buf = malloc(block_size);

    int err = lfs->read(buf, 0); // Read the super block from the first block
    if (err != 0) {
        return err; // Return the error code if reading the super block fails
    }

    memcpy(&lfs->sb, buf, sizeof(lfs->sb)); // Copy the super block from the buffer to the in-memory structure
    free(buf);

    // block_offset  += 1; // The super block is stored in the first block
    if (lfs->sb.magic != LFS_MAGIC) {
        return -1; // Return an error if the magic number is incorrect
    }
    if (lfs->sb.crc32 != _crc32(&lfs->sb, sizeof(lfs->sb) - sizeof(lfs->sb.crc32))) {
        return -1; // Return an error if the CRC32 check fails
    }

    if (lfs->sb.version != LFS_VERSION) {
        return -1; // Return an error if the version is incorrect
    }

    if (config->block_size != lfs->sb.block_size || config->block_count != lfs->sb.total_block_count) {
        return -1; // Return an error if the block size or block count does not match the super block
    }


    memcpy(&lfs->sb, &lfs->sb, sizeof(lfs->sb)); // Copy the super block to the in-memory structure

    block_size = lfs->sb.block_size;
    uint32_t total_block_count = lfs->sb.total_block_count;

    // every block can contain block_size * 8 bits of bitmap
    uint32_t block_bmp_block_count = DIV_ROUND_UP(total_block_count, block_size * 8);

    lfs->block_bmp = malloc(block_bmp_block_count * block_size);
    if (!lfs->block_bmp) {
        goto cleanup; // Return an error if memory allocation fails
    }

    block_id = lfs->sb.block_bmp_block_id;
    err = lfs->read(lfs->block_bmp, block_id); // Read the block bitmap from the second block
    if (err != 0) {
        goto cleanup; // Return the error code if reading the block bitmap fails
    }

    uint32_t inode_bmp_block_count = DIV_ROUND_UP(total_block_count, block_size * 8);
    lfs->inode_bmp = malloc(inode_bmp_block_count * block_size);
    if (!lfs->inode_bmp) {
        goto cleanup; // Return an error if memory allocation fails
    }

    block_id = lfs->sb.inode_bmp_block_id; // The inode bitmap is stored in the third block
    err = lfs->read(lfs->inode_bmp, block_id); // Read the inode bitmap from the third block
    if (err != 0) {
        goto cleanup; // Return the error code if reading the inode bitmap fails
    }

    uint32_t inode_table_block_count = DIV_ROUND_UP(total_block_count * sizeof(inode_t), block_size);
    lfs->inode_table = malloc(inode_table_block_count * block_size);
    if (!lfs->inode_table) {
        goto cleanup; // Return an error if memory allocation fails
    }

    block_id = lfs->sb.inode_table_block_id; // The inode table is stored after the inode bitmap
    err = lfs->read(lfs->inode_table, block_id); // Read the inode table from the appropriate block
    if (err != 0) {
        goto cleanup; // Return the error code if reading the inode table fails
    }
    // Implement the mount logic here

    uint32_t dir_table_block_count = DIV_ROUND_UP(total_block_count * sizeof(dir_entry_t), block_size);
    lfs->dir_table = malloc(dir_table_block_count * block_size);
    if (!lfs->dir_table) {
        goto cleanup; // Return an error if memory allocation fails
    }

    block_id = lfs->sb.dir_table_block_id; // Update the block offset after reading the inode table
    err = lfs->read(lfs->dir_table, block_id); // Read the directory table from the appropriate block
    if (err != 0) {
        goto cleanup; // Return the error code if reading the directory table fails
    }

    return 0; // Return 0 on success

cleanup:
    if (lfs->block_bmp) {
        free(lfs->block_bmp);
        lfs->block_bmp = NULL;
    }
    if (lfs->inode_bmp) {
        free(lfs->inode_bmp);
        lfs->inode_bmp = NULL;
    }
    if (lfs->inode_table) {
        free(lfs->inode_table);
        lfs->inode_table = NULL;
    }
    if (lfs->dir_table) {
        free(lfs->dir_table);
        lfs->dir_table = NULL;
    }
    return -1;
}


int lfs_format(lfs_t *fs, const lfs_config_t *config) 
{
    fs->read = config->read;
    fs->write = config->write;

    // Initialize the super block
    fs->sb.magic = LFS_MAGIC;
    fs->sb.version = LFS_VERSION;
    fs->sb.block_size = config->block_size;
    fs->sb.total_block_count = config->block_count;
    fs->sb.free_block_count = config->block_count;
    fs->sb.total_inode_count = config->block_count; // Assuming one inode per block for simplicity
    fs->sb.free_inode_count = config->block_count;
    fs->sb.valid_dir_entry_id = INVALID_ID;
    fs->sb.free_dir_entry_id = 0;

    uint32_t block_offset = 1; // The block bitmap is stored in the second block

    // block bmp
    fs->sb.block_bmp_block_id = block_offset; // The block bitmap is stored in the second block
    uint32_t block_bmp_block_count = DIV_ROUND_UP(fs->sb.total_block_count, fs->sb.block_size * 8);
    block_offset += block_bmp_block_count; // Move to the next block for the inode bitmap

    // inode bmp
    fs->sb.inode_bmp_block_id = block_offset; // The inode bitmap is stored in the third block
    uint32_t inode_bmp_block_count = DIV_ROUND_UP(fs->sb.total_inode_count, fs->sb.block_size * 8);
    block_offset += inode_bmp_block_count; // Move to the next block for the inode table

    // inode table
    fs->sb.inode_table_block_id = block_offset; // The inode table is stored after the inode bitmap
    uint32_t inode_table_block_count = DIV_ROUND_UP(fs->sb.total_inode_count * sizeof(inode_t), fs->sb.block_size);
    block_offset += inode_table_block_count; // Move to the next block for the directory table

    // directory table
    fs->sb.dir_table_block_id = block_offset; // The directory table is stored after the inode table
    uint32_t dir_table_block_count = DIV_ROUND_UP(fs->sb.total_block_count * sizeof(dir_entry_t), fs->sb.block_size);
    block_offset += dir_table_block_count; // Move to the next block for the next section

    fs->sb.crc32 = _crc32(&fs->sb, sizeof(fs->sb) - sizeof(fs->sb.crc32)); // Initialize CRC32 to 0

    // init directory table
    dir_entry_t *dir_entry = fs->dir_table;
    for (uint32_t i = 0; i < fs->sb.total_block_count; i++) {
        dir_entry->inode_id = 0;
        dir_entry->length = 0;
        dir_entry->next_entry_id = i + 1;
        if (dir_entry->next_entry_id == fs->sb.total_block_count) {
            dir_entry->next_entry_id = INVALID_ID;
        }
        dir_entry->name[0] = '\0';
        dir_entry++;
    }

    // write all block to dev
    uint32_t total_block_count = block_offset; // The total number of blocks used by the file system
    for (uint32_t i = 0; i < total_block_count; i++) {
        fs->write(fs, i); // Write each block to the device
    }

    return 0; // Return 0 on success
}


static dir_entry_t *_find_dir_entry(lfs_t *fs, const char *path) 
{
    uint32_t valid_dir_entry_id = fs->sb.valid_dir_entry_id;
    if (valid_dir_entry_id == INVALID_ID) {
        return NULL; // No valid directory entry available
    }
    dir_entry_t *dir_entry = &fs->dir_table[valid_dir_entry_id]; // Start from the beginning of the directory table
    while (dir_entry->next_entry_id != INVALID_ID) { // Assuming 0 indicates an empty entry
        if (strcmp(dir_entry->name, path) == 0) {
            return dir_entry; // Found the directory entry
        }
        dir_entry = &fs->dir_table[dir_entry->next_entry_id];
    
    }
    return NULL; // Directory entry not found
}

static inline int _find_first_zero_bit(bmp_t *bmp, uint32_t len)
{
    for (uint32_t i = 0; i < len; i++) {
        if (bmp[i] == 0) {
            return i; // Found the first zero bit
        }
    }
    return -1; // No zero bit found
}

static inline int _bmp_set(bmp_t *bmp, uint32_t bit)
{
    uint32_t offset = bit / 8;
    uint32_t index = bit % 8;
    if (bit < 0) {
        return -1; // Invalid offset
    }

    bmp[offset] |= (1 << index); // Set the bit to 1
    return 0; // Return 0 on success
}

static inline int _bmp_clear(bmp_t *bmp, uint32_t bit)
{
    uint32_t offset = bit / 8;
    uint32_t index = bit % 8;
    if (bit < 0) {
        return -1; // Invalid offset
    }

    bmp[offset] &= ~(1 << index); // Clear the bit to 0
    return 0; // Return 0 on success
}

static int _alloc_inode(lfs_t *fs)
{
    bmp_t *bmp = fs->inode_bmp;
    uint32_t len = fs->sb.total_inode_count;
    int free_inode_id = _find_first_zero_bit(bmp, len);
    if (free_inode_id == -1) {
        return -1; // No free inode available
    }
    _bmp_set(bmp, free_inode_id); // Mark the inode as allocated
    return free_inode_id; // Return the allocated inode ID
}

static int _free_inode(lfs_t *fs, uint32_t inode_id)
{
    bmp_t *bmp = fs->inode_bmp;
    int ret = _bmp_clear(bmp, inode_id);
    return ret; // Return 0 on success, -1 on failure
}

static int _alloc_block(lfs_t *fs)
{
    bmp_t *bmp = fs->block_bmp;
    uint32_t len = fs->sb.total_block_count;
    int free_block_id = _find_first_zero_bit(bmp, len);
    if (free_block_id == -1) {
        return -1; // No free block available
    }
    _bmp_set(bmp, free_block_id); // Mark the block as allocated
    return free_block_id; // Return the allocated block ID
}

static int _free_block(lfs_t *fs, uint32_t block_id)
{
    bmp_t *bmp = fs->block_bmp;
    int ret = _bmp_clear(bmp, block_id);
    return ret; // Return 0 on success, -1 on failure
}

static int _create_file(lfs_t *fs, const char *path) 
{
    // Find an empty directory entry
    uint32_t free_dir_entry_id = fs->sb.free_dir_entry_id;
    if (free_dir_entry_id == INVALID_ID) {
        return -1; // No free directory entry available
    }
    dir_entry_t *dir_entry = &fs->dir_table[free_dir_entry_id];

    // Find an empty inode
    int free_inode_id = _alloc_inode(fs);
    if (free_inode_id == -1) {
        return -1; // No free inode available
    }
    inode_t *inode = &fs->inode_table[free_inode_id];

    int block_id = _alloc_block(fs);
    if (block_id == -1) {
        _free_inode(fs, free_inode_id); // Free the allocated inode before returning
        return -1; // No free block available
    }

    // Initialize the directory entry
    dir_entry->inode_id = free_inode_id;
    dir_entry->length = 0;
    strncpy(dir_entry->name, path, sizeof(dir_entry->name) - 1);
    dir_entry->name[sizeof(dir_entry->name) - 1] = '\0';

    // Initialize the inode
    inode->block_id = block_id;
    inode->next_inode_id = INVALID_ID;

    // Update the free lists
    fs->sb.free_dir_entry_id = dir_entry->next_entry_id;

    return 0; // Return 0 on success
}


static int _remove_file(lfs_t *fs, const char *path)
{
    // Find the directory entry for the given path
    dir_entry_t *dir_entry = _find_dir_entry(fs, path);
    if (!dir_entry) {
        return -1; // File not found
    }

    // Get the inode for the file
    inode_t *inode = &fs->inode_table[dir_entry->inode_id];

    // Free the block
    int ret = _free_block(fs, inode->block_id);
    if (ret != 0) {
        return -1; // Failed to free the block
    }
    // Free the inode
    ret = _free_inode(fs, dir_entry->inode_id);
    if (ret != 0) {
        return -1; // Failed to free the inode
    }

    // Free the directory entry
    dir_entry->next_entry_id = fs->sb.free_dir_entry_id;
    fs->sb.free_dir_entry_id = dir_entry - fs->dir_table;

    return 0; // Return 0 on success
}

int lfs_file_open(lfs_t *fs, lfs_file_t *file, const char *path) 
{
    // Find the directory entry for the given path
    dir_entry_t *dir_entry = _find_dir_entry(fs, path);
    if (!dir_entry) {
        // create file
        int ret = _create_file(fs, path);
        if (ret != 0) {
            return -1; // Failed to create the file
        }
        dir_entry = _find_dir_entry(fs, path);
        if (!dir_entry) {
            return -1; // File not found after creation
        }
        // File found after creation, proceed to open it
    }

    // Get the inode for the file
    inode_t *inode = &fs->inode_table[dir_entry->inode_id];

    // Initialize the file structure
    file->block_id = inode->block_id;
    file->length = dir_entry->length;

    return 0; // Return 0 on success
}

int lfs_unmount(lfs_t *lfs)
{
    return 0;
}

int lfs_file_close(lfs_t *lfs, lfs_file_t *file)
{
    return 0;
}

int lfs_file_read(lfs_t *lfs, lfs_file_t *file, void *buffer, uint32_t size)
{
    return 0;
}


int lfs_file_write(lfs_t *lfs, lfs_file_t *file, const void *buffer, uint32_t size)
{
    return 0;
}


int lfs_file_rewind(lfs_t *lfs, lfs_file_t *file)
{
    return 0;
}
int lfs_file_seek(lfs_t *lfs, lfs_file_t *file, int32_t offset, uint32_t whence)
{
    return 0;
}
int lfs_file_dump(lfs_t *lfs, lfs_file_t *file)
{
    return 0;
}   
int lfs_remove(lfs_t *lfs, const char *path)
{
    return 0;
}
int lfs_rename(lfs_t *lfs, const char *oldpath, const char *newpath)
{
    return 0;
}