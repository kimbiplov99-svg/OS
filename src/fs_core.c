#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <pthread.h>
#include "../include/fusefs.h"

struct fs_state gfs = {
    .fd = -1,
    .no_cache = 0,
    .cache_hits = 0,
    .cache_misses = 0
};

/* ---- Raw disk I/O ---- */
int disk_read(uint32_t block_no, void *buf) {
    off_t off = (off_t)block_no * BLOCK_SIZE;
    ssize_t n = pread(gfs.fd, buf, BLOCK_SIZE, off);
    if (n != BLOCK_SIZE) return -EIO;
    return 0;
}

int disk_write(uint32_t block_no, const void *buf) {
    off_t off = (off_t)block_no * BLOCK_SIZE;
    ssize_t n = pwrite(gfs.fd, buf, BLOCK_SIZE, off);
    if (n != BLOCK_SIZE) return -EIO;
    return 0;
}

void disk_flush(void) {
    fsync(gfs.fd);
}

/* ---- Block cache (write-back LRU) ---- */
static uint64_t cache_tick = 0;

void cache_init(void) {
    pthread_mutex_lock(&gfs.cache_lock);
    memset(gfs.cache, 0, sizeof(gfs.cache));
    pthread_mutex_unlock(&gfs.cache_lock);
}

static int cache_find(uint32_t block_no) {
    for (int i = 0; i < CACHE_BLOCKS; i++)
        if (gfs.cache[i].valid && gfs.cache[i].block_no == block_no)
            return i;
    return -1;
}

static int cache_evict(void) {
    int lru = 0;
    uint64_t min_t = UINT64_MAX;
    for (int i = 0; i < CACHE_BLOCKS; i++) {
        if (!gfs.cache[i].valid) return i;
        if (gfs.cache[i].access_time < min_t) {
            min_t = gfs.cache[i].access_time;
            lru = i;
        }
    }
    if (gfs.cache[lru].dirty) {
        disk_write(gfs.cache[lru].block_no, gfs.cache[lru].data);
        gfs.cache[lru].dirty = 0;
    }
    gfs.cache[lru].valid = 0;
    return lru;
}

int cache_read(uint32_t block_no, void *buf) {
    if (gfs.no_cache) {
        return disk_read(block_no, buf);
    }

    pthread_mutex_lock(&gfs.cache_lock);
    int idx = cache_find(block_no);
    if (idx >= 0) {
        gfs.cache[idx].access_time = ++cache_tick;
        memcpy(buf, gfs.cache[idx].data, BLOCK_SIZE);
        gfs.cache_hits++;
        pthread_mutex_unlock(&gfs.cache_lock);
        return 0;
    }
    gfs.cache_misses++;
    idx = cache_evict();
    int r = disk_read(block_no, gfs.cache[idx].data);
    if (r < 0) {
        pthread_mutex_unlock(&gfs.cache_lock);
        return r;
    }
    gfs.cache[idx].block_no = block_no;
    gfs.cache[idx].valid = 1;
    gfs.cache[idx].dirty = 0;
    gfs.cache[idx].access_time = ++cache_tick;
    memcpy(buf, gfs.cache[idx].data, BLOCK_SIZE);
    pthread_mutex_unlock(&gfs.cache_lock);
    return 0;
}

int cache_write(uint32_t block_no, const void *buf) {
    if (gfs.no_cache) {
        int r = disk_write(block_no, buf);
        disk_flush();
        return r;
    }

    pthread_mutex_lock(&gfs.cache_lock);
    int idx = cache_find(block_no);
    if (idx < 0) {
        idx = cache_evict();
        gfs.cache[idx].block_no = block_no;
        gfs.cache[idx].valid = 1;
    }
    memcpy(gfs.cache[idx].data, buf, BLOCK_SIZE);
    gfs.cache[idx].dirty = 1;
    gfs.cache[idx].access_time = ++cache_tick;
    pthread_mutex_unlock(&gfs.cache_lock);
    return 0;
}

void cache_flush(void) {
    if (gfs.no_cache) {
        disk_flush();
        return;
    }

    pthread_mutex_lock(&gfs.cache_lock);
    for (int i = 0; i < CACHE_BLOCKS; i++) {
        if (gfs.cache[i].valid && gfs.cache[i].dirty) {
            disk_write(gfs.cache[i].block_no, gfs.cache[i].data);
            gfs.cache[i].dirty = 0;
        }
    }
    pthread_mutex_unlock(&gfs.cache_lock);
    disk_flush();
}

/* ---- Superblock ---- */
void sb_read(void)  {
    disk_read(SUPERBLOCK_BLK, &gfs.sb);
}

void sb_write(void) {
    disk_write(SUPERBLOCK_BLK, &gfs.sb);
    disk_flush();
}
