#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "lfs.h"

#define TEST_BLOCK_SIZE (1024u)
#define TEST_BLOCK_COUNT_A (128u)
#define TEST_BLOCK_COUNT_B (512u)
#define TEST_DATA_START_A (14u)
#define TEST_DATA_START_B (47u)
#define TEST_INVALID_ID UINT32_MAX

int test_dev_init(uint32_t block_size, uint32_t block_count);
void test_dev_fini(void);
lfs_config_t test_dev_config(void);
void test_dev_fail_read(uint32_t block_id);
void test_dev_fail_write(uint32_t block_id);
void test_dev_clear_faults(void);
uint8_t *test_dev_block(uint32_t block_id);
uint32_t test_crc32(const void *data, size_t size);

#define CHECK(cond)                                                     \
    do {                                                                \
        if (!(cond)) {                                                  \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__,      \
                    __LINE__, #cond);                                   \
            return 1;                                                   \
        }                                                               \
    } while (0)
