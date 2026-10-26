#include "lfs.h"
#include <inttypes.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include "lfs_struct.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>


#define DIV_ROUND_UP(n, d) (((n) + (d) - 1) / (d))

#define INVALID_ID 0xFFFFFFFFu

#define min(a, b) ((a) < (b) ? (a) : (b))

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

static inline int _find_first_zero_bit(bmp_t *bmp, uint32_t len)
{
    uint32_t bit_per_idx = sizeof(bmp_t) * 8;
    for (uint32_t i = 0; i < len; i++) {
        uint32_t idx = i / bit_per_idx;
        uint32_t offset = i % bit_per_idx;
        if ((bmp[idx] & ((bmp_t)1 << offset)) == 0) {
            return (int)i;
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


int lfs_mount(lfs_t *fs, const lfs_config_t *config) {

    fs->read = config->read;
    fs->write = config->write;

    uint32_t block_size = config->block_size;
    uint32_t block_id = 0;

    char *buf = malloc(block_size);

    int err = fs->read(buf, 0); // Read the super block from the first block
    if (err != 0) {
        return err; // Return the error code if reading the super block fails
    }

    memcpy(&fs->sb, buf, sizeof(fs->sb)); // Copy the super block from the buffer to the in-memory structure
    free(buf);

    // block_offset  += 1; // The super block is stored in the first block
    if (fs->sb.magic != LFS_MAGIC) {
        return -1; // Return an error if the magic number is incorrect
    }
    if (fs->sb.crc32 != _crc32(&fs->sb, sizeof(fs->sb) - sizeof(fs->sb.crc32))) {
        return -1; // Return an error if the CRC32 check fails
    }

    if (fs->sb.version != LFS_VERSION) {
        return -1; // Return an error if the version is incorrect
    }

    if (config->block_size != fs->sb.block_size || config->block_count != fs->sb.total_block_count) {
        return -1; // Return an error if the block size or block count does not match the super block
    }


    memcpy(&fs->sb, &fs->sb, sizeof(fs->sb)); // Copy the super block to the in-memory structure

    block_size = fs->sb.block_size;
    uint32_t total_block_count = fs->sb.total_block_count;

    // every block can contain block_size * 8 bits of bitmap
    uint32_t block_bmp_block_count = DIV_ROUND_UP(total_block_count, block_size * 8);

    fs->block_bmp = malloc(block_bmp_block_count * block_size);
    if (!fs->block_bmp) {
        goto cleanup; // Return an error if memory allocation fails
    }

    block_id = fs->sb.block_bmp_block_id;
    err = fs->read(fs->block_bmp, block_id); // Read the block bitmap from the second block
    if (err != 0) {
        goto cleanup; // Return the error code if reading the block bitmap fails
    }

    uint32_t inode_bmp_block_count = DIV_ROUND_UP(total_block_count, block_size * 8);
    fs->inode_bmp = malloc(inode_bmp_block_count * block_size);
    if (!fs->inode_bmp) {
        goto cleanup; // Return an error if memory allocation fails
    }

    block_id = fs->sb.inode_bmp_block_id; // The inode bitmap is stored in the third block
    err = fs->read(fs->inode_bmp, block_id); // Read the inode bitmap from the third block
    if (err != 0) {
        goto cleanup; // Return the error code if reading the inode bitmap fails
    }

    uint32_t inode_table_block_count = DIV_ROUND_UP(total_block_count * sizeof(inode_t), block_size);
    fs->inode_table = malloc(inode_table_block_count * block_size);
    if (!fs->inode_table) {
        goto cleanup; // Return an error if memory allocation fails
    }

    block_id = fs->sb.inode_table_block_id; // The inode table is stored after the inode bitmap
    err = fs->read(fs->inode_table, block_id); // Read the inode table from the appropriate block
    if (err != 0) {
        goto cleanup; // Return the error code if reading the inode table fails
    }
    // Implement the mount logic here

    uint32_t dir_table_block_count = DIV_ROUND_UP(total_block_count * sizeof(dir_entry_t), block_size);
    fs->dir_table = malloc(dir_table_block_count * block_size);
    if (!fs->dir_table) {
        goto cleanup; // Return an error if memory allocation fails
    }

    block_id = fs->sb.dir_table_block_id; // Update the block offset after reading the inode table
    err = fs->read(fs->dir_table, block_id); // Read the directory table from the appropriate block
    if (err != 0) {
        goto cleanup; // Return the error code if reading the directory table fails
    }

    return 0; // Return 0 on success

cleanup:
    if (fs->block_bmp) {
        free(fs->block_bmp);
        fs->block_bmp = NULL;
    }
    if (fs->inode_bmp) {
        free(fs->inode_bmp);
        fs->inode_bmp = NULL;
    }
    if (fs->inode_table) {
        free(fs->inode_table);
        fs->inode_table = NULL;
    }
    if (fs->dir_table) {
        free(fs->dir_table);
        fs->dir_table = NULL;
    }
    return -1;
}

static int _sync_lfs(lfs_t *fs)
{
    // Write the super block to the device
    fs->write(&fs->sb, 0);

    // Write the block bitmap to the device
    uint32_t block_bmp_block_count = DIV_ROUND_UP(fs->sb.total_block_count, fs->sb.block_size * 8);
    for (uint32_t i = 0; i < block_bmp_block_count; i++) {
        fs->write((void *)fs->block_bmp + i * fs->sb.block_size, fs->sb.block_bmp_block_id + i);
    }

    // Write the inode bitmap to the device
    uint32_t inode_bmp_block_count = DIV_ROUND_UP(fs->sb.total_inode_count, fs->sb.block_size * 8);
    for (uint32_t i = 0; i < inode_bmp_block_count; i++) {
        fs->write(fs->inode_bmp + i * fs->sb.block_size, fs->sb.inode_bmp_block_id + i);
    }

    // Write the inode table to the device
    uint32_t inode_table_block_count = DIV_ROUND_UP(fs->sb.total_inode_count * sizeof(inode_t), fs->sb.block_size);
    for (uint32_t i = 0; i < inode_table_block_count; i++) {
        fs->write(fs->inode_table + i * fs->sb.block_size, fs->sb.inode_table_block_id + i);
    }

    // Write the directory table to the device
    uint32_t dir_table_block_count = DIV_ROUND_UP(fs->sb.total_block_count * sizeof(dir_entry_t), fs->sb.block_size);
    for (uint32_t i = 0; i < dir_table_block_count; i++) {
        fs->write((void *)(fs->dir_table + i * fs->sb.block_size), fs->sb.dir_table_block_id + i);
    }

    return 0;
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
    fs->block_bmp = malloc(block_bmp_block_count * fs->sb.block_size);
    if (fs->block_bmp == NULL) {
        goto cleanup; // Failed to allocate memory for the block bitmap
    }
    memset(fs->block_bmp, 0, block_bmp_block_count * fs->sb.block_size);

    // inode bmp
    fs->sb.inode_bmp_block_id = block_offset; // The inode bitmap is stored in the third block
    uint32_t inode_bmp_block_count = DIV_ROUND_UP(fs->sb.total_inode_count, fs->sb.block_size * 8);
    block_offset += inode_bmp_block_count; // Move to the next block for the inode table
    fs->inode_bmp = malloc(inode_bmp_block_count * fs->sb.block_size);
    if (fs->inode_bmp == NULL) {
        goto cleanup; // Failed to allocate memory for the inode bitmap
    }
    memset(fs->inode_bmp, 0, inode_bmp_block_count * fs->sb.block_size);

    // inode table
    fs->sb.inode_table_block_id = block_offset; // The inode table is stored after the inode bitmap
    uint32_t inode_table_block_count = DIV_ROUND_UP(fs->sb.total_inode_count * sizeof(inode_t), fs->sb.block_size);
    block_offset += inode_table_block_count; // Move to the next block for the directory table
    fs->inode_table = malloc(inode_table_block_count * fs->sb.block_size);
    if (fs->inode_table == NULL) {
        goto cleanup; // Failed to allocate memory for the inode table
    }
    memset(fs->inode_table, 0, inode_table_block_count * fs->sb.block_size);

    // directory table
    fs->sb.dir_table_block_id = block_offset; // The directory table is stored after the inode table
    uint32_t dir_table_block_count = DIV_ROUND_UP(fs->sb.total_block_count * sizeof(dir_entry_t), fs->sb.block_size);
    block_offset += dir_table_block_count; // Move to the next block for the next section
    fs->dir_table = malloc(dir_table_block_count * fs->sb.block_size);
    if (fs->dir_table == NULL) {
        goto cleanup; // Failed to allocate memory for the directory table
    }
    memset(fs->dir_table, 0, dir_table_block_count * fs->sb.block_size);


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

    for (uint32_t i = 0; i <block_offset; i++) {
        _bmp_set(fs->block_bmp, i); // Mark the block as used
    }

    // fs->sb.free_block_count = fs->sb.total_block_count - block_offset;

    unsigned char *zero_block = calloc(1, fs->sb.block_size);
    if (zero_block == NULL) {
        goto cleanup;
    }
    for (uint32_t id = block_offset; id < fs->sb.total_block_count; id ++) {
        if (fs->write(zero_block, id) != 0) {
            free(zero_block);
            goto cleanup;
        }
    }
    // fs->dir_table = dir_entry; // No need to update fs->dir_table as it already points to the allocated memory

    // write all block to dev
    _sync_lfs(fs);


cleanup:
    if (fs->block_bmp) {
        free(fs->block_bmp);
        fs->block_bmp = NULL;
    }
    if (fs->inode_bmp) {
        free(fs->inode_bmp);
        fs->inode_bmp = NULL;
    }
    if (fs->inode_table) {
        free(fs->inode_table);
        fs->inode_table = NULL;
    }
    if (fs->dir_table) {
        free(fs->dir_table);
        fs->dir_table = NULL;
    }

    return 0; // Return 0 on success
}

static int _dump_printf(const char *format, ...)
{
    va_list args;
    va_start(args, format);
    int ret = vprintf(format, args);
    va_end(args);
    return ret < 0 ? -1 : 0;
}

int lfs_dump(lfs_t *fs)
{
    super_block_t *sb = &fs->sb;

    if (_dump_printf("super block:\n") != 0 ||
        _dump_printf("  magic: 0x%08" PRIx32 "\n", sb->magic) != 0 ||
        _dump_printf("  version: %" PRIu32 "\n", sb->version) != 0 ||
        _dump_printf("  block_size: %" PRIu32 "\n", sb->block_size) != 0 ||
        _dump_printf("  total_block_count: %" PRIu32 "\n",
                     sb->total_block_count) != 0 ||
        _dump_printf("  free_block_count: %" PRIu32 "\n",
                     sb->free_block_count) != 0 ||
        _dump_printf("  total_inode_count: %" PRIu32 "\n",
                     sb->total_inode_count) != 0 ||
        _dump_printf("  free_inode_count: %" PRIu32 "\n",
                     sb->free_inode_count) != 0 ||
        _dump_printf("  block_bmp_block_id: %" PRIu32 "\n",
                     sb->block_bmp_block_id) != 0 ||
        _dump_printf("  inode_bmp_block_id: %" PRIu32 "\n",
                     sb->inode_bmp_block_id) != 0 ||
        _dump_printf("  inode_table_block_id: %" PRIu32 "\n",
                     sb->inode_table_block_id) != 0 ||
        _dump_printf("  dir_table_block_id: %" PRIu32 "\n",
                     sb->dir_table_block_id) != 0 ||
        _dump_printf("  valid_dir_entry_id: %" PRIu32 "\n",
                     sb->valid_dir_entry_id) != 0 ||
        _dump_printf("  free_dir_entry_id: %" PRIu32 "\n",
                     sb->free_dir_entry_id) != 0 ||
        _dump_printf("  crc32: 0x%08" PRIx32 "\n", sb->crc32) != 0) {
        return -1;
    }

    if (_dump_printf("directory:\n") != 0) {
        return -1;
    }

    uint32_t dir_entry_id = sb->valid_dir_entry_id;
    uint32_t dir_entry_capacity = sb->total_block_count;
    uint32_t visited = 0;

    if (dir_entry_id == INVALID_ID) {
        if (_dump_printf("  (empty)\n") != 0) {
            return -1;
        }
        return fflush(stdout) == 0 ? 0 : -1;
    }

    while (dir_entry_id != INVALID_ID) {
        if (dir_entry_id >= dir_entry_capacity ||
            visited >= dir_entry_capacity) {
            return -1;  // Broken or cyclic directory chain
        }

        dir_entry_t *entry = &fs->dir_table[dir_entry_id];
        if (entry->inode_id >= sb->total_inode_count) {
            return -1;  // Directory entry points outside the inode table
        }
        inode_t *inode = &fs->inode_table[entry->inode_id];

        if (_dump_printf("  [%" PRIu32 "] name=\"%.*s\" inode_id=%" PRIu32
                         " size=%" PRIu32 " block_id=%" PRIu32
                         " next_inode_id=%" PRIu32 "\n",
                         dir_entry_id, (int)sizeof(entry->name), entry->name,
                         entry->inode_id, entry->length, inode->block_id,
                         inode->next_inode_id) != 0) {
            return -1;
        }

        dir_entry_id = entry->next_entry_id;
        visited++;
    }

    return fflush(stdout) == 0 ? 0 : -1;
}


static dir_entry_t *_find_dir_entry(lfs_t *fs, const char *path)
{
    uint32_t dir_entry_id = fs->sb.valid_dir_entry_id;
    uint32_t dir_entry_capacity = fs->sb.total_block_count;
    uint32_t visited = 0;

    while (dir_entry_id != INVALID_ID) {
        if (dir_entry_id >= dir_entry_capacity ||
            visited >= dir_entry_capacity) {
            return NULL;  // Broken or cyclic directory chain
        }

        dir_entry_t *dir_entry = &fs->dir_table[dir_entry_id];
        if (strncmp(dir_entry->name, path, sizeof(dir_entry->name)) == 0) {
            return dir_entry;  // Found the directory entry
        }

        dir_entry_id = dir_entry->next_entry_id;
        visited++;
    }

    return NULL;  // Directory entry not found
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
    // Take an entry from the free list without unlinking it yet.
    uint32_t free_dir_entry_id = fs->sb.free_dir_entry_id;
    if (free_dir_entry_id == INVALID_ID ||
        free_dir_entry_id >= fs->sb.total_block_count) {
        return -1;  // No free directory entry available
    }
    dir_entry_t *dir_entry = &fs->dir_table[free_dir_entry_id];
    uint32_t next_free_dir_entry_id = dir_entry->next_entry_id;
    if (next_free_dir_entry_id != INVALID_ID &&
        next_free_dir_entry_id >= fs->sb.total_block_count) {
        return -1;  // Broken free directory chain
    }

    size_t path_len = strlen(path);
    if (path_len == 0 || path_len >= sizeof(dir_entry->name)) {
        return -1;  // Name cannot be stored and found again
    }

    // Find an empty inode
    int free_inode_id = _alloc_inode(fs);
    if (free_inode_id == -1) {
        return -1;  // No free inode available
    }
    inode_t *inode = &fs->inode_table[free_inode_id];

    int block_id = _alloc_block(fs);
    if (block_id == -1) {
        _free_inode(fs, free_inode_id);  // Free the inode allocated above
        return -1;                       // No free block available
    }

    // Initialize the directory entry
    dir_entry->inode_id = (uint32_t)free_inode_id;
    dir_entry->length = 0;
    memcpy(dir_entry->name, path, path_len + 1);

    // Initialize the inode
    inode->block_id = (uint32_t)block_id;
    inode->next_inode_id = INVALID_ID;

    // Move the entry from the free list to the head of the valid list.
    dir_entry->next_entry_id = fs->sb.valid_dir_entry_id;
    fs->sb.valid_dir_entry_id = free_dir_entry_id;
    fs->sb.free_dir_entry_id = next_free_dir_entry_id;

    return 0;  // Return 0 on success
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

int lfs_unmount(lfs_t *fs)
{
    _sync_lfs(fs);

    if (fs->block_bmp) {
        free(fs->block_bmp);
        fs->block_bmp = NULL;
    }
    if (fs->inode_bmp) {
        free(fs->inode_bmp);
        fs->inode_bmp = NULL;
    }
    if (fs->inode_table) {
        free(fs->inode_table);
        fs->inode_table = NULL;
    }
    if (fs->dir_table) {
        free(fs->dir_table);
        fs->dir_table = NULL;
    }

    return 0;
}

int lfs_file_close(lfs_t *fs, lfs_file_t *file)
{
    // Write the file's block to the device
    _sync_lfs(fs);
    return 0;
}

int lfs_file_read(lfs_t *fs, lfs_file_t *file, void *buffer, uint32_t size)
{
    if (file->offset >= file->length) {
        return -1; // Offset is beyond the end of the file
    }
    uint32_t sz = min(size, file->length - file->offset);
    uint32_t start_block_id = file->block_id + file->offset / fs->sb.block_size;
    char *buf = malloc(DIV_ROUND_UP(sz, fs->sb.block_size) * fs->sb.block_size);
    if (!buf) {
        return -1; // Failed to allocate buffer
    }
    // Read the file's blocks into the buffer
    uint32_t file_block_count = DIV_ROUND_UP(sz, fs->sb.block_size);
    for (uint32_t i = 0; i < file_block_count; i++) {
        fs->read(buf + i * fs->sb.block_size, start_block_id + i);
    }
    memcpy(buffer, buf + file->offset % fs->sb.block_size, sz);
    free(buf);
    return 0;
}


int lfs_file_write(lfs_t *fs, lfs_file_t *file, const void *buffer, uint32_t size)
{
    if (file->offset >= file->length) {
        return -1; // Offset is beyond the end of the file
    }
    uint32_t sz = min(size, file->length - file->offset);
    uint32_t start_block_id = file->block_id + file->offset / fs->sb.block_size;
    char *buf = malloc(DIV_ROUND_UP(sz, fs->sb.block_size) * fs->sb.block_size);
    if (!buf) {
        return -1; // Failed to allocate buffer
    }
    memcpy(buf + file->offset % fs->sb.block_size, buffer, sz);
    file->length = size;
    uint32_t file_block_count = DIV_ROUND_UP(sz, fs->sb.block_size);
    for (uint32_t i = 0; i < file_block_count; i++) {
        fs->write(buf + i * fs->sb.block_size, start_block_id + i);
    }

    free(buf);
    return 0;
}


int lfs_file_rewind(lfs_t *fs, lfs_file_t *file)
{
    file->offset = 0; // Reset the file offset to the beginning of the file
    return 0;
}

int lfs_file_seek(lfs_t *fs, lfs_file_t *file, int32_t offset, uint32_t whence)
{
    int32_t new_offset;
    switch (whence) {
        case 0: // SEEK_SET
            new_offset = offset;
            break;
        case 1: // SEEK_CUR
            new_offset = file->offset + offset;
            break;
        case 2: // SEEK_END
            new_offset = file->length;
            break;
        default:
            return -1; // Invalid whence value
    }
    if (new_offset < 0 || new_offset > file->length) {
        return -1; // Offset out of range
    }
    file->offset = new_offset;
    return 0;
}

int lfs_file_dump(lfs_t *fs, lfs_file_t *file)
{
    printf("File: block_id=%u, length=%u, offset=%u\n", file->block_id, file->length, file->offset);
    return 0;
}

int lfs_file_remove(lfs_t *fs, const char *path)
{
    return _remove_file(fs, path);
}


int lfs_file_rename(lfs_t *fs, const char *oldpath, const char *newpath)
{
    dir_entry_t *dir_entry = _find_dir_entry(fs, oldpath);
    if (!dir_entry) {
        return -1; // Old path not found
    }
    memcpy(dir_entry->name, newpath, strlen(newpath) + 1);
    return 0;
}