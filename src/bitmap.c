#define _GNU_SOURCE
#include <string.h>
#include <errno.h>
#include "../include/fusefs.h"

int32_t bitmap_alloc(uint32_t bitmap_block, uint32_t total) {
    uint8_t buf[BLOCK_SIZE];
    if (cache_read(bitmap_block, buf) < 0) return -EIO;
    for (uint32_t i = 0; i < total; i++) {
        uint32_t byte = i / 8, bit = i % 8;
        if (!(buf[byte] & (1 << bit))) {
            buf[byte] |= (1 << bit);
            cache_write(bitmap_block, buf);
            return (int32_t)i;
        }
    }
    return -ENOSPC;
}

void bitmap_free(uint32_t bitmap_block, uint32_t bit) {
    uint8_t buf[BLOCK_SIZE];
    cache_read(bitmap_block, buf);
    buf[bit / 8] &= ~(1 << (bit % 8));
    cache_write(bitmap_block, buf);
}

int bitmap_test(uint32_t bitmap_block, uint32_t bit) {
    uint8_t buf[BLOCK_SIZE];
    cache_read(bitmap_block, buf);
    return (buf[bit / 8] >> (bit % 8)) & 1;
}

void bitmap_set(uint32_t bitmap_block, uint32_t bit) {
    uint8_t buf[BLOCK_SIZE];
    cache_read(bitmap_block, buf);
    buf[bit / 8] |= (1 << (bit % 8));
    cache_write(bitmap_block, buf);
}
