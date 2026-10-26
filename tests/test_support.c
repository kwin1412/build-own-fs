#include "test_support.h"

#include <stdlib.h>
#include <string.h>

static uint8_t *g_device;
static uint32_t g_block_size;
static uint32_t g_block_count;
static uint32_t g_fail_read = UINT32_MAX;
static uint32_t g_fail_write = UINT32_MAX;

static int dev_read(void *buffer, uint32_t block_id)
{
    if (!g_device || !buffer || block_id >= g_block_count ||
        block_id == g_fail_read) {
        return -1;
    }
    memcpy(buffer, g_device + (size_t)block_id * g_block_size,
           g_block_size);
    return 0;
}

static int dev_write(const void *buffer, uint32_t block_id)
{
    if (!g_device || !buffer || block_id >= g_block_count ||
        block_id == g_fail_write) {
        return -1;
    }
    memcpy(g_device + (size_t)block_id * g_block_size, buffer,
           g_block_size);
    return 0;
}

int test_dev_init(uint32_t block_size, uint32_t block_count)
{
    test_dev_fini();
    if (block_size == 0 || block_count == 0) {
        return -1;
    }

    size_t size = (size_t)block_size * block_count;
    g_device = calloc(1, size);
    if (!g_device) {
        return -1;
    }
    g_block_size = block_size;
    g_block_count = block_count;
    test_dev_clear_faults();
    return 0;
}

void test_dev_fini(void)
{
    free(g_device);
    g_device = NULL;
    g_block_size = 0;
    g_block_count = 0;
    test_dev_clear_faults();
}

lfs_config_t test_dev_config(void)
{
    lfs_config_t config = {
        .read = dev_read,
        .write = dev_write,
        .block_size = g_block_size,
        .block_count = g_block_count,
    };
    return config;
}

void test_dev_fail_read(uint32_t block_id)
{
    g_fail_read = block_id;
}

void test_dev_fail_write(uint32_t block_id)
{
    g_fail_write = block_id;
}

void test_dev_clear_faults(void)
{
    g_fail_read = UINT32_MAX;
    g_fail_write = UINT32_MAX;
}

uint8_t *test_dev_block(uint32_t block_id)
{
    if (!g_device || block_id >= g_block_count) {
        return NULL;
    }
    return g_device + (size_t)block_id * g_block_size;
}

uint32_t test_crc32(const void *data, size_t size)
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
