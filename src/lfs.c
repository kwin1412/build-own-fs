#include "lfs.h"

#include <inttypes.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lfs_struct.h"

#define DIV_ROUND_UP(n, d) (((n) + (d) - 1) / (d))
#define INVALID_ID 0xFFFFFFFFu

typedef struct {
    uint32_t block_bmp_block_id;
    uint32_t block_bmp_blocks;
    uint32_t inode_bmp_block_id;
    uint32_t inode_bmp_blocks;
    uint32_t inode_table_block_id;
    uint32_t inode_table_blocks;
    uint32_t dir_table_block_id;
    uint32_t dir_table_blocks;
    uint32_t data_start;
} lfs_layout_t;

static uint32_t _crc32(const void *data, size_t size);
static int _compute_layout(uint32_t block_size, uint32_t total_blocks,
                           uint32_t total_inodes, lfs_layout_t *out);
static int _blocks_to_size(uint32_t blocks, uint32_t block_size, size_t *out);
static int _fs_is_mounted(const lfs_t *fs);
static int _validate_config(const lfs_config_t *config);
static int _get_data_start(const lfs_t *fs, uint32_t *out);
static int _bmp_set(bmp_t *bmp, uint32_t bit, uint32_t capacity);
static int _bmp_clear(bmp_t *bmp, uint32_t bit, uint32_t capacity);
static int _bmp_test(const bmp_t *bmp, uint32_t bit, uint32_t capacity);
static uint32_t _find_first_zero_bit(const bmp_t *bmp, uint32_t begin,
                                     uint32_t end);
static uint32_t _alloc_inode(lfs_t *fs);
static int _free_inode(lfs_t *fs, uint32_t inode_id);
static uint32_t _alloc_block(lfs_t *fs);
static int _free_block(lfs_t *fs, uint32_t block_id);
static int _read_region(lfs_t *fs, void *base, uint32_t first_block,
                        uint32_t block_count);
static int _write_region(lfs_t *fs, const void *base, uint32_t first_block,
                         uint32_t block_count);
static int _sync_lfs(lfs_t *fs);
static int _find_dir_entry(lfs_t *fs, const char *path, uint32_t *out_id);
static int _create_file(lfs_t *fs, const char *path, uint32_t *out_id);
static int _remove_file(lfs_t *fs, const char *path);
static int _get_file(lfs_t *fs, lfs_file_t *file, dir_entry_t **entry_out,
                     inode_t **inode_out);
static int _dump_printf(const char *format, ...);

static uint32_t _crc32(const void *data, size_t size)
{
    const unsigned char *ptr = (const unsigned char *)data;
    uint32_t crc = 0xFFFFFFFFu;

    for (size_t i = 0; i < size; i++) {
        crc ^= ptr[i];
        for (int j = 0; j < 8; j++) {
            if (crc & 1u) {
                crc = (crc >> 1) ^ 0xEDB88320u;
            } else {
                crc >>= 1;
            }
        }
    }
    return crc ^ 0xFFFFFFFFu;
}

static int _compute_layout(uint32_t block_size, uint32_t total_blocks,
                           uint32_t total_inodes, lfs_layout_t *out)
{
    if (!out || block_size == 0 || total_blocks == 0 || total_inodes == 0) {
        return -1;
    }

    uint64_t bits_per_block = (uint64_t)block_size * 8u;
    uint64_t block_bmp_blocks =
        DIV_ROUND_UP((uint64_t)total_blocks, bits_per_block);
    uint64_t inode_bmp_blocks =
        DIV_ROUND_UP((uint64_t)total_inodes, bits_per_block);
    uint64_t inode_table_blocks =
        DIV_ROUND_UP((uint64_t)total_inodes * sizeof(inode_t), block_size);
    uint64_t dir_table_blocks =
        DIV_ROUND_UP((uint64_t)total_blocks * sizeof(dir_entry_t), block_size);

    uint64_t inode_bmp_id = 1u + block_bmp_blocks;
    uint64_t inode_table_id = inode_bmp_id + inode_bmp_blocks;
    uint64_t dir_table_id = inode_table_id + inode_table_blocks;
    uint64_t data_start = dir_table_id + dir_table_blocks;

    if (block_bmp_blocks > UINT32_MAX || inode_bmp_blocks > UINT32_MAX ||
        inode_table_blocks > UINT32_MAX || dir_table_blocks > UINT32_MAX ||
        inode_bmp_id > UINT32_MAX || inode_table_id > UINT32_MAX ||
        dir_table_id > UINT32_MAX || data_start > UINT32_MAX ||
        data_start >= total_blocks) {
        return -1;
    }

    out->block_bmp_block_id = 1;
    out->block_bmp_blocks = (uint32_t)block_bmp_blocks;
    out->inode_bmp_block_id = (uint32_t)inode_bmp_id;
    out->inode_bmp_blocks = (uint32_t)inode_bmp_blocks;
    out->inode_table_block_id = (uint32_t)inode_table_id;
    out->inode_table_blocks = (uint32_t)inode_table_blocks;
    out->dir_table_block_id = (uint32_t)dir_table_id;
    out->dir_table_blocks = (uint32_t)dir_table_blocks;
    out->data_start = (uint32_t)data_start;
    return 0;
}

static int _blocks_to_size(uint32_t blocks, uint32_t block_size, size_t *out)
{
    uint64_t size = (uint64_t)blocks * block_size;
    size_t converted = (size_t)size;

    if (!out || (uint64_t)converted != size) {
        return -1;
    }
    *out = converted;
    return 0;
}

static int _fs_is_mounted(const lfs_t *fs)
{
    return fs && fs->read && fs->write && fs->block_bmp && fs->inode_bmp &&
           fs->inode_table && fs->dir_table;
}

static int _validate_config(const lfs_config_t *config)
{
    if (!config || !config->read || !config->write) {
        return -1;
    }
    if (config->block_size < LFS_MIN_BLOCK_SIZE ||
        config->block_size > INT32_MAX || config->block_count == 0 ||
        config->block_count >= INVALID_ID) {
        return -1;
    }
    return 0;
}

static int _get_data_start(const lfs_t *fs, uint32_t *out)
{
    lfs_layout_t layout;

    if (!fs || !out) {
        return -1;
    }
    if (_compute_layout(fs->sb.block_size, fs->sb.total_block_count,
                        fs->sb.total_inode_count, &layout) != 0) {
        return -1;
    }
    *out = layout.data_start;
    return 0;
}

static int _bmp_set(bmp_t *bmp, uint32_t bit, uint32_t capacity)
{
    if (!bmp || bit >= capacity) {
        return -1;
    }
    bmp[bit / 8] |= (bmp_t)(1u << (bit % 8));
    return 0;
}

static int _bmp_clear(bmp_t *bmp, uint32_t bit, uint32_t capacity)
{
    if (!bmp || bit >= capacity) {
        return -1;
    }
    bmp[bit / 8] &= (bmp_t)~(1u << (bit % 8));
    return 0;
}

static int _bmp_test(const bmp_t *bmp, uint32_t bit, uint32_t capacity)
{
    if (!bmp || bit >= capacity) {
        return -1;
    }
    return (bmp[bit / 8] & (bmp_t)(1u << (bit % 8))) != 0 ? 1 : 0;
}

static uint32_t _find_first_zero_bit(const bmp_t *bmp, uint32_t begin,
                                     uint32_t end)
{
    if (!bmp || begin >= end) {
        return INVALID_ID;
    }
    for (uint32_t bit = begin; bit < end; bit++) {
        if ((bmp[bit / 8] & (bmp_t)(1u << (bit % 8))) == 0) {
            return bit;
        }
    }
    return INVALID_ID;
}

static uint32_t _alloc_inode(lfs_t *fs)
{
    if (!_fs_is_mounted(fs) || fs->sb.free_inode_count == 0) {
        return INVALID_ID;
    }

    uint32_t inode_id = _find_first_zero_bit(fs->inode_bmp, 0,
                                             fs->sb.total_inode_count);
    if (inode_id == INVALID_ID) {
        return INVALID_ID;
    }
    if (_bmp_set(fs->inode_bmp, inode_id, fs->sb.total_inode_count) != 0) {
        return INVALID_ID;
    }
    fs->sb.free_inode_count--;
    return inode_id;
}

static int _free_inode(lfs_t *fs, uint32_t inode_id)
{
    if (!_fs_is_mounted(fs)) {
        return -1;
    }
    if (_bmp_test(fs->inode_bmp, inode_id, fs->sb.total_inode_count) != 1) {
        return -1;
    }
    if (fs->sb.free_inode_count >= fs->sb.total_inode_count) {
        return -1;
    }
    if (_bmp_clear(fs->inode_bmp, inode_id, fs->sb.total_inode_count) != 0) {
        return -1;
    }
    fs->sb.free_inode_count++;
    return 0;
}

static uint32_t _alloc_block(lfs_t *fs)
{
    uint32_t data_start;

    if (!_fs_is_mounted(fs) || fs->sb.free_block_count == 0) {
        return INVALID_ID;
    }
    if (_get_data_start(fs, &data_start) != 0) {
        return INVALID_ID;
    }

    uint32_t block_id = _find_first_zero_bit(fs->block_bmp, data_start,
                                             fs->sb.total_block_count);
    if (block_id == INVALID_ID) {
        return INVALID_ID;
    }
    if (_bmp_set(fs->block_bmp, block_id, fs->sb.total_block_count) != 0) {
        return INVALID_ID;
    }
    fs->sb.free_block_count--;
    return block_id;
}

static int _free_block(lfs_t *fs, uint32_t block_id)
{
    uint32_t data_start;

    if (!_fs_is_mounted(fs)) {
        return -1;
    }
    if (_get_data_start(fs, &data_start) != 0) {
        return -1;
    }
    if (block_id < data_start || block_id >= fs->sb.total_block_count) {
        return -1;
    }
    if (_bmp_test(fs->block_bmp, block_id, fs->sb.total_block_count) != 1) {
        return -1;
    }
    if (fs->sb.free_block_count >= fs->sb.total_block_count - data_start) {
        return -1;
    }
    if (_bmp_clear(fs->block_bmp, block_id, fs->sb.total_block_count) != 0) {
        return -1;
    }
    fs->sb.free_block_count++;
    return 0;
}

static int _read_region(lfs_t *fs, void *base, uint32_t first_block,
                        uint32_t block_count)
{
    uint8_t *ptr = (uint8_t *)base;

    for (uint32_t i = 0; i < block_count; i++) {
        if (fs->read(ptr + (size_t)i * fs->sb.block_size,
                     first_block + i) != 0) {
            return -1;
        }
    }
    return 0;
}

static int _write_region(lfs_t *fs, const void *base, uint32_t first_block,
                         uint32_t block_count)
{
    const uint8_t *ptr = (const uint8_t *)base;

    for (uint32_t i = 0; i < block_count; i++) {
        if (fs->write(ptr + (size_t)i * fs->sb.block_size,
                      first_block + i) != 0) {
            return -1;
        }
    }
    return 0;
}

int lfs_mount(lfs_t *fs, const lfs_config_t *config)
{
    if (!fs || _validate_config(config) != 0) {
        return -1;
    }
    if (fs->block_bmp || fs->inode_bmp || fs->inode_table || fs->dir_table) {
        return -1;
    }

    memset(&fs->sb, 0, sizeof(fs->sb));
    fs->read = config->read;
    fs->write = config->write;

    uint8_t *super = malloc(config->block_size);
    if (!super) {
        return -1;
    }
    if (fs->read(super, 0) != 0) {
        free(super);
        return -1;
    }
    memcpy(&fs->sb, super, sizeof(fs->sb));
    free(super);

    if (fs->sb.magic != LFS_MAGIC || fs->sb.version != LFS_VERSION ||
        fs->sb.block_size != config->block_size ||
        fs->sb.total_block_count != config->block_count ||
        fs->sb.crc32 !=
            _crc32(&fs->sb, offsetof(super_block_t, crc32))) {
        return -1;
    }
    if (fs->sb.total_inode_count == 0 ||
        fs->sb.total_inode_count >= INVALID_ID) {
        return -1;
    }

    lfs_layout_t layout;
    if (_compute_layout(fs->sb.block_size, fs->sb.total_block_count,
                        fs->sb.total_inode_count, &layout) != 0) {
        return -1;
    }
    if (fs->sb.block_bmp_block_id != layout.block_bmp_block_id ||
        fs->sb.inode_bmp_block_id != layout.inode_bmp_block_id ||
        fs->sb.inode_table_block_id != layout.inode_table_block_id ||
        fs->sb.dir_table_block_id != layout.dir_table_block_id) {
        return -1;
    }
    if (fs->sb.valid_dir_entry_id != INVALID_ID &&
        fs->sb.valid_dir_entry_id >= fs->sb.total_block_count) {
        return -1;
    }
    if (fs->sb.free_dir_entry_id != INVALID_ID &&
        fs->sb.free_dir_entry_id >= fs->sb.total_block_count) {
        return -1;
    }

    size_t block_bmp_size;
    size_t inode_bmp_size;
    size_t inode_table_size;
    size_t dir_table_size;
    if (_blocks_to_size(layout.block_bmp_blocks, fs->sb.block_size,
                        &block_bmp_size) != 0 ||
        _blocks_to_size(layout.inode_bmp_blocks, fs->sb.block_size,
                        &inode_bmp_size) != 0 ||
        _blocks_to_size(layout.inode_table_blocks, fs->sb.block_size,
                        &inode_table_size) != 0 ||
        _blocks_to_size(layout.dir_table_blocks, fs->sb.block_size,
                        &dir_table_size) != 0) {
        return -1;
    }

    int result = -1;
    fs->block_bmp = calloc(1, block_bmp_size);
    if (!fs->block_bmp) {
        goto cleanup;
    }
    fs->inode_bmp = calloc(1, inode_bmp_size);
    if (!fs->inode_bmp) {
        goto cleanup;
    }
    fs->inode_table = calloc(1, inode_table_size);
    if (!fs->inode_table) {
        goto cleanup;
    }
    fs->dir_table = calloc(1, dir_table_size);
    if (!fs->dir_table) {
        goto cleanup;
    }

    if (_read_region(fs, fs->block_bmp, fs->sb.block_bmp_block_id,
                     layout.block_bmp_blocks) != 0 ||
        _read_region(fs, fs->inode_bmp, fs->sb.inode_bmp_block_id,
                     layout.inode_bmp_blocks) != 0 ||
        _read_region(fs, fs->inode_table, fs->sb.inode_table_block_id,
                     layout.inode_table_blocks) != 0 ||
        _read_region(fs, fs->dir_table, fs->sb.dir_table_block_id,
                     layout.dir_table_blocks) != 0) {
        goto cleanup;
    }

    uint32_t free_blocks = 0;
    for (uint32_t bit = layout.data_start; bit < fs->sb.total_block_count;
         bit++) {
        int used = _bmp_test(fs->block_bmp, bit, fs->sb.total_block_count);
        if (used < 0) {
            goto cleanup;
        }
        if (used == 0) {
            free_blocks++;
        }
    }
    uint32_t free_inodes = 0;
    for (uint32_t bit = 0; bit < fs->sb.total_inode_count; bit++) {
        int used = _bmp_test(fs->inode_bmp, bit, fs->sb.total_inode_count);
        if (used < 0) {
            goto cleanup;
        }
        if (used == 0) {
            free_inodes++;
        }
    }
    fs->sb.free_block_count = free_blocks;
    fs->sb.free_inode_count = free_inodes;
    result = 0;

cleanup:
    if (result != 0) {
        free(fs->block_bmp);
        free(fs->inode_bmp);
        free(fs->inode_table);
        free(fs->dir_table);
        fs->block_bmp = NULL;
        fs->inode_bmp = NULL;
        fs->inode_table = NULL;
        fs->dir_table = NULL;
    }
    return result;
}

int lfs_format(lfs_t *fs, const lfs_config_t *config)
{
    if (!fs || _validate_config(config) != 0) {
        return -1;
    }
    if (fs->block_bmp || fs->inode_bmp || fs->inode_table || fs->dir_table) {
        return -1;
    }

    memset(&fs->sb, 0, sizeof(fs->sb));
    fs->read = config->read;
    fs->write = config->write;

    lfs_layout_t layout;
    if (_compute_layout(config->block_size, config->block_count,
                        config->block_count, &layout) != 0) {
        return -1;
    }

    fs->sb.magic = LFS_MAGIC;
    fs->sb.version = LFS_VERSION;
    fs->sb.block_size = config->block_size;
    fs->sb.total_block_count = config->block_count;
    fs->sb.free_block_count =
        fs->sb.total_block_count - layout.data_start;
    fs->sb.total_inode_count = config->block_count;
    fs->sb.free_inode_count = fs->sb.total_inode_count;
    fs->sb.block_bmp_block_id = layout.block_bmp_block_id;
    fs->sb.inode_bmp_block_id = layout.inode_bmp_block_id;
    fs->sb.inode_table_block_id = layout.inode_table_block_id;
    fs->sb.dir_table_block_id = layout.dir_table_block_id;
    fs->sb.valid_dir_entry_id = INVALID_ID;
    fs->sb.free_dir_entry_id = 0;
    fs->sb.crc32 = 0;

    size_t block_bmp_size;
    size_t inode_bmp_size;
    size_t inode_table_size;
    size_t dir_table_size;
    if (_blocks_to_size(layout.block_bmp_blocks, fs->sb.block_size,
                        &block_bmp_size) != 0 ||
        _blocks_to_size(layout.inode_bmp_blocks, fs->sb.block_size,
                        &inode_bmp_size) != 0 ||
        _blocks_to_size(layout.inode_table_blocks, fs->sb.block_size,
                        &inode_table_size) != 0 ||
        _blocks_to_size(layout.dir_table_blocks, fs->sb.block_size,
                        &dir_table_size) != 0) {
        return -1;
    }

    int result = -1;
    uint8_t *zero_block = NULL;

    fs->block_bmp = calloc(1, block_bmp_size);
    if (!fs->block_bmp) {
        goto cleanup;
    }
    fs->inode_bmp = calloc(1, inode_bmp_size);
    if (!fs->inode_bmp) {
        goto cleanup;
    }
    fs->inode_table = calloc(1, inode_table_size);
    if (!fs->inode_table) {
        goto cleanup;
    }
    fs->dir_table = calloc(1, dir_table_size);
    if (!fs->dir_table) {
        goto cleanup;
    }

    for (uint32_t i = 0; i < fs->sb.total_block_count; i++) {
        dir_entry_t *entry = &fs->dir_table[i];
        entry->inode_id = INVALID_ID;
        entry->length = 0;
        entry->next_entry_id =
            (i + 1 < fs->sb.total_block_count) ? i + 1 : INVALID_ID;
        entry->name[0] = '\0';
    }

    for (uint32_t bit = 0; bit < layout.data_start; bit++) {
        if (_bmp_set(fs->block_bmp, bit, fs->sb.total_block_count) != 0) {
            goto cleanup;
        }
    }

    zero_block = calloc(1, fs->sb.block_size);
    if (!zero_block) {
        goto cleanup;
    }
    for (uint32_t block_id = layout.data_start;
         block_id < fs->sb.total_block_count; block_id++) {
        if (fs->write(zero_block, block_id) != 0) {
            goto cleanup;
        }
    }

    if (_sync_lfs(fs) != 0) {
        goto cleanup;
    }
    result = 0;

cleanup:
    free(zero_block);
    free(fs->block_bmp);
    free(fs->inode_bmp);
    free(fs->inode_table);
    free(fs->dir_table);
    fs->block_bmp = NULL;
    fs->inode_bmp = NULL;
    fs->inode_table = NULL;
    fs->dir_table = NULL;
    return result;
}

static int _sync_lfs(lfs_t *fs)
{
    if (!_fs_is_mounted(fs)) {
        return -1;
    }

    lfs_layout_t layout;
    if (_compute_layout(fs->sb.block_size, fs->sb.total_block_count,
                        fs->sb.total_inode_count, &layout) != 0) {
        return -1;
    }

    if (_write_region(fs, fs->block_bmp, fs->sb.block_bmp_block_id,
                      layout.block_bmp_blocks) != 0 ||
        _write_region(fs, fs->inode_bmp, fs->sb.inode_bmp_block_id,
                      layout.inode_bmp_blocks) != 0 ||
        _write_region(fs, fs->inode_table, fs->sb.inode_table_block_id,
                      layout.inode_table_blocks) != 0 ||
        _write_region(fs, fs->dir_table, fs->sb.dir_table_block_id,
                      layout.dir_table_blocks) != 0) {
        return -1;
    }

    fs->sb.crc32 = _crc32(&fs->sb, offsetof(super_block_t, crc32));

    uint8_t *super = calloc(1, fs->sb.block_size);
    if (!super) {
        return -1;
    }
    memcpy(super, &fs->sb, sizeof(fs->sb));
    int result = fs->write(super, 0);
    free(super);
    return result == 0 ? 0 : -1;
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
    if (!_fs_is_mounted(fs)) {
        return -1;
    }

    super_block_t *sb = &fs->sb;
    uint32_t data_start;
    if (_get_data_start(fs, &data_start) != 0) {
        return -1;
    }

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
    uint32_t capacity = sb->total_block_count;
    uint32_t visited = 0;

    if (dir_entry_id == INVALID_ID) {
        if (_dump_printf("  (empty)\n") != 0) {
            return -1;
        }
        return fflush(stdout) == 0 ? 0 : -1;
    }

    while (dir_entry_id != INVALID_ID) {
        if (dir_entry_id >= capacity || visited >= capacity) {
            return -1;
        }

        dir_entry_t *entry = &fs->dir_table[dir_entry_id];
        if (entry->inode_id >= sb->total_inode_count ||
            entry->length > sb->block_size) {
            return -1;
        }
        inode_t *inode = &fs->inode_table[entry->inode_id];
        if (inode->block_id < data_start ||
            inode->block_id >= sb->total_block_count) {
            return -1;
        }

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

static int _find_dir_entry(lfs_t *fs, const char *path, uint32_t *out_id)
{
    if (!_fs_is_mounted(fs) || !path || !out_id) {
        return -1;
    }

    uint32_t dir_entry_id = fs->sb.valid_dir_entry_id;
    uint32_t capacity = fs->sb.total_block_count;
    uint32_t visited = 0;

    while (dir_entry_id != INVALID_ID) {
        if (dir_entry_id >= capacity || visited >= capacity) {
            return -1;
        }

        dir_entry_t *entry = &fs->dir_table[dir_entry_id];
        if (strncmp(entry->name, path, sizeof(entry->name)) == 0) {
            *out_id = dir_entry_id;
            return 0;
        }

        dir_entry_id = entry->next_entry_id;
        visited++;
    }

    return 1;
}

static int _create_file(lfs_t *fs, const char *path, uint32_t *out_id)
{
    if (!_fs_is_mounted(fs) || !path || !out_id) {
        return -1;
    }

    size_t path_len = strlen(path);
    if (path_len == 0 || path_len > LFS_MAX_FILE_NAME_LEN) {
        return -1;
    }

    uint32_t existing_id;
    int found = _find_dir_entry(fs, path, &existing_id);
    if (found != 1) {
        return -1;
    }

    uint32_t capacity = fs->sb.total_block_count;
    uint32_t free_entry_id = fs->sb.free_dir_entry_id;
    if (free_entry_id == INVALID_ID || free_entry_id >= capacity) {
        return -1;
    }
    dir_entry_t *entry = &fs->dir_table[free_entry_id];
    uint32_t next_free_id = entry->next_entry_id;
    if (next_free_id != INVALID_ID && next_free_id >= capacity) {
        return -1;
    }

    uint32_t inode_id = _alloc_inode(fs);
    if (inode_id == INVALID_ID) {
        return -1;
    }
    uint32_t block_id = _alloc_block(fs);
    if (block_id == INVALID_ID) {
        _free_inode(fs, inode_id);
        return -1;
    }

    void *zero_block = calloc(1, fs->sb.block_size);
    if (!zero_block) {
        _free_block(fs, block_id);
        _free_inode(fs, inode_id);
        return -1;
    }
    if (fs->write(zero_block, block_id) != 0) {
        free(zero_block);
        _free_block(fs, block_id);
        _free_inode(fs, inode_id);
        return -1;
    }
    free(zero_block);

    inode_t *inode = &fs->inode_table[inode_id];
    inode->block_id = block_id;
    inode->next_inode_id = INVALID_ID;

    entry->inode_id = inode_id;
    entry->length = 0;
    memset(entry->name, 0, sizeof(entry->name));
    memcpy(entry->name, path, path_len + 1);

    entry->next_entry_id = fs->sb.valid_dir_entry_id;
    fs->sb.valid_dir_entry_id = free_entry_id;
    fs->sb.free_dir_entry_id = next_free_id;

    *out_id = free_entry_id;
    return 0;
}

static int _remove_file(lfs_t *fs, const char *path)
{
    if (!_fs_is_mounted(fs) || !path) {
        return -1;
    }

    uint32_t target_id;
    if (_find_dir_entry(fs, path, &target_id) != 0) {
        return -1;
    }

    dir_entry_t *entry = &fs->dir_table[target_id];
    if (entry->inode_id >= fs->sb.total_inode_count) {
        return -1;
    }
    inode_t *inode = &fs->inode_table[entry->inode_id];

    uint32_t data_start;
    if (_get_data_start(fs, &data_start) != 0) {
        return -1;
    }
    if (inode->block_id < data_start ||
        inode->block_id >= fs->sb.total_block_count) {
        return -1;
    }
    if (_bmp_test(fs->block_bmp, inode->block_id,
                  fs->sb.total_block_count) != 1) {
        return -1;
    }
    if (_bmp_test(fs->inode_bmp, entry->inode_id,
                  fs->sb.total_inode_count) != 1) {
        return -1;
    }

    uint32_t prev_id = INVALID_ID;
    uint32_t dir_entry_id = fs->sb.valid_dir_entry_id;
    uint32_t visited = 0;
    while (dir_entry_id != INVALID_ID && dir_entry_id != target_id) {
        if (dir_entry_id >= fs->sb.total_block_count ||
            visited >= fs->sb.total_block_count) {
            return -1;
        }
        prev_id = dir_entry_id;
        dir_entry_id = fs->dir_table[dir_entry_id].next_entry_id;
        visited++;
    }
    if (dir_entry_id != target_id) {
        return -1;
    }

    if (_free_block(fs, inode->block_id) != 0 ||
        _free_inode(fs, entry->inode_id) != 0) {
        return -1;
    }

    if (prev_id == INVALID_ID) {
        fs->sb.valid_dir_entry_id = entry->next_entry_id;
    } else {
        fs->dir_table[prev_id].next_entry_id = entry->next_entry_id;
    }

    entry->inode_id = INVALID_ID;
    entry->length = 0;
    entry->next_entry_id = fs->sb.free_dir_entry_id;
    memset(entry->name, 0, sizeof(entry->name));
    fs->sb.free_dir_entry_id = target_id;
    return 0;
}

static int _get_file(lfs_t *fs, lfs_file_t *file, dir_entry_t **entry_out,
                     inode_t **inode_out)
{
    if (!_fs_is_mounted(fs) || !file) {
        return -1;
    }
    if (file->dir_entry_id >= fs->sb.total_block_count) {
        return -1;
    }

    dir_entry_t *entry = &fs->dir_table[file->dir_entry_id];
    if (entry->inode_id >= fs->sb.total_inode_count) {
        return -1;
    }
    if (_bmp_test(fs->inode_bmp, entry->inode_id,
                  fs->sb.total_inode_count) != 1) {
        return -1;
    }
    inode_t *inode = &fs->inode_table[entry->inode_id];

    uint32_t data_start;
    if (_get_data_start(fs, &data_start) != 0) {
        return -1;
    }
    if (inode->block_id < data_start ||
        inode->block_id >= fs->sb.total_block_count) {
        return -1;
    }
    if (_bmp_test(fs->block_bmp, inode->block_id,
                  fs->sb.total_block_count) != 1) {
        return -1;
    }
    if (inode->block_id != file->block_id) {
        return -1;
    }
    if (file->offset > file->length ||
        file->length > fs->sb.block_size) {
        return -1;
    }

    if (entry_out) {
        *entry_out = entry;
    }
    if (inode_out) {
        *inode_out = inode;
    }
    return 0;
}

int lfs_unmount(lfs_t *fs)
{
    if (!fs) {
        return -1;
    }

    int result = -1;
    if (_fs_is_mounted(fs)) {
        result = _sync_lfs(fs);
    }

    free(fs->block_bmp);
    free(fs->inode_bmp);
    free(fs->inode_table);
    free(fs->dir_table);
    fs->block_bmp = NULL;
    fs->inode_bmp = NULL;
    fs->inode_table = NULL;
    fs->dir_table = NULL;
    return result;
}

int lfs_file_open(lfs_t *fs, lfs_file_t *file, const char *path)
{
    if (!_fs_is_mounted(fs) || !file || !path) {
        return -1;
    }

    size_t path_len = strlen(path);
    if (path_len == 0 || path_len > LFS_MAX_FILE_NAME_LEN) {
        return -1;
    }

    uint32_t entry_id;
    int found = _find_dir_entry(fs, path, &entry_id);
    if (found < 0) {
        return -1;
    }
    if (found == 1) {
        if (_create_file(fs, path, &entry_id) != 0) {
            return -1;
        }
    }

    dir_entry_t *entry = &fs->dir_table[entry_id];
    if (entry->inode_id >= fs->sb.total_inode_count) {
        return -1;
    }
    if (_bmp_test(fs->inode_bmp, entry->inode_id,
                  fs->sb.total_inode_count) != 1) {
        return -1;
    }
    inode_t *inode = &fs->inode_table[entry->inode_id];

    uint32_t data_start;
    if (_get_data_start(fs, &data_start) != 0) {
        return -1;
    }
    if (inode->block_id < data_start ||
        inode->block_id >= fs->sb.total_block_count) {
        return -1;
    }
    if (_bmp_test(fs->block_bmp, inode->block_id,
                  fs->sb.total_block_count) != 1) {
        return -1;
    }

    if (entry->length > fs->sb.block_size) {
        entry->length = fs->sb.block_size;
    }

    file->block_id = inode->block_id;
    file->length = entry->length;
    file->offset = 0;
    file->dir_entry_id = entry_id;
    return 0;
}

int lfs_file_close(lfs_t *fs, lfs_file_t *file)
{
    dir_entry_t *entry = NULL;
    inode_t *inode = NULL;

    if (_get_file(fs, file, &entry, &inode) != 0) {
        return -1;
    }

    entry->length = file->length;
    inode->block_id = file->block_id;
    return _sync_lfs(fs);
}

int lfs_file_read(lfs_t *fs, lfs_file_t *file, void *buffer, uint32_t size)
{
    if (_get_file(fs, file, NULL, NULL) != 0) {
        return -1;
    }
    if (size == 0) {
        return 0;
    }
    if (!buffer) {
        return -1;
    }
    if (file->offset >= file->length) {
        return 0;
    }

    uint32_t read_size = file->length - file->offset;
    if (read_size > size) {
        read_size = size;
    }

    uint8_t *block = malloc(fs->sb.block_size);
    if (!block) {
        return -1;
    }
    if (fs->read(block, file->block_id) != 0) {
        free(block);
        return -1;
    }
    memcpy(buffer, block + file->offset, read_size);
    free(block);

    file->offset += read_size;
    return (int)read_size;
}

int lfs_file_write(lfs_t *fs, lfs_file_t *file, const void *buffer,
                   uint32_t size)
{
    dir_entry_t *entry = NULL;

    if (_get_file(fs, file, &entry, NULL) != 0) {
        return -1;
    }
    if (size == 0) {
        return 0;
    }
    if (!buffer) {
        return -1;
    }
    if (file->offset >= fs->sb.block_size) {
        return -1;
    }

    uint32_t write_size = fs->sb.block_size - file->offset;
    if (write_size > size) {
        write_size = size;
    }

    uint8_t *block = calloc(1, fs->sb.block_size);
    if (!block) {
        return -1;
    }
    if (fs->read(block, file->block_id) != 0) {
        free(block);
        return -1;
    }
    memcpy(block + file->offset, buffer, write_size);
    if (fs->write(block, file->block_id) != 0) {
        free(block);
        return -1;
    }
    free(block);

    file->offset += write_size;
    if (file->offset > file->length) {
        file->length = file->offset;
    }
    entry->length = file->length;
    return (int)write_size;
}

int lfs_file_rewind(lfs_t *fs, lfs_file_t *file)
{
    if (_get_file(fs, file, NULL, NULL) != 0) {
        return -1;
    }
    file->offset = 0;
    return 0;
}

int lfs_file_seek(lfs_t *fs, lfs_file_t *file, int32_t offset,
                  uint32_t whence)
{
    if (_get_file(fs, file, NULL, NULL) != 0) {
        return -1;
    }

    int64_t base;
    switch (whence) {
        case 0:  // SEEK_SET
            base = 0;
            break;
        case 1:  // SEEK_CUR
            base = file->offset;
            break;
        case 2:  // SEEK_END
            base = file->length;
            break;
        default:
            return -1;
    }

    int64_t new_offset = base + offset;
    if (new_offset < 0 || new_offset > (int64_t)file->length) {
        return -1;
    }
    file->offset = (uint32_t)new_offset;
    return 0;
}

int lfs_file_dump(lfs_t *fs, lfs_file_t *file)
{
    if (_get_file(fs, file, NULL, NULL) != 0) {
        return -1;
    }
    if (printf("File: block_id=%" PRIu32 ", length=%" PRIu32
               ", offset=%" PRIu32 "\n",
               file->block_id, file->length, file->offset) < 0) {
        return -1;
    }
    return 0;
}

int lfs_file_remove(lfs_t *fs, const char *path)
{
    return _remove_file(fs, path);
}

int lfs_file_rename(lfs_t *fs, const char *oldpath, const char *newpath)
{
    if (!_fs_is_mounted(fs) || !oldpath || !newpath) {
        return -1;
    }

    size_t new_len = strlen(newpath);
    if (new_len == 0 || new_len > LFS_MAX_FILE_NAME_LEN) {
        return -1;
    }

    if (strcmp(oldpath, newpath) == 0) {
        uint32_t entry_id;
        return _find_dir_entry(fs, oldpath, &entry_id) == 0 ? 0 : -1;
    }

    uint32_t old_id;
    if (_find_dir_entry(fs, oldpath, &old_id) != 0) {
        return -1;
    }

    uint32_t new_id;
    if (_find_dir_entry(fs, newpath, &new_id) != 1) {
        return -1;
    }

    dir_entry_t *entry = &fs->dir_table[old_id];
    memset(entry->name, 0, sizeof(entry->name));
    memcpy(entry->name, newpath, new_len + 1);
    return 0;
}
