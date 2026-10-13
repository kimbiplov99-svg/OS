#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>
#include "../include/fusefs.h"

int mkfs_create(const char *image_path, uint32_t total_blocks) {
    uint32_t inode_blocks = (MAX_INODES * sizeof(struct inode) + BLOCK_SIZE - 1) / BLOCK_SIZE;
    uint32_t data_start   = INODE_TABLE_BLK + inode_blocks;

    if (total_blocks <= data_start + 1) {
        fprintf(stderr, "Image too small (need > %u blocks)\n", data_start + 1);
        return -1;
    }

    gfs.fd = open(image_path, O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (gfs.fd < 0) { perror("open image"); return -1; }

    /* Allocate and zero image */
    uint8_t zero[BLOCK_SIZE] = {0};
    for (uint32_t i = 0; i < total_blocks; i++) {
        if (pwrite(gfs.fd, zero, BLOCK_SIZE, (off_t)i * BLOCK_SIZE) != BLOCK_SIZE) {
            perror("zero block"); return -1;
        }
    }

    /* Superblock */
    struct superblock sb;
    memset(&sb, 0, sizeof sb);
    sb.magic        = FS_MAGIC;
    sb.total_blocks = total_blocks;
    sb.total_inodes = MAX_INODES;
    sb.data_start   = data_start;
    sb.free_blocks  = total_blocks - data_start;
    sb.free_inodes  = MAX_INODES - 1; /* root uses inode 1 */
    sb.block_size   = BLOCK_SIZE;
    sb.version      = 1;
    if (pwrite(gfs.fd, &sb, sizeof sb, (off_t)SUPERBLOCK_BLK * BLOCK_SIZE) != (ssize_t)sizeof sb) {
        perror("write superblock"); return -1;
    }
    memcpy(&gfs.sb, &sb, sizeof sb);

    /* Inode bitmap: root is bit 0 */
    uint8_t ibm[BLOCK_SIZE] = {0};
    ibm[0] = 0x01;
    if (pwrite(gfs.fd, ibm, BLOCK_SIZE, (off_t)INODE_BITMAP_BLK * BLOCK_SIZE) != BLOCK_SIZE) {
        perror("write inode bitmap"); return -1;
    }

    /* Root inode (dir) */
    struct inode root;
    memset(&root, 0, sizeof root);
    root.type  = T_DIR;
    root.mode  = S_IFDIR | 0755;
    root.uid   = 0;
    root.gid   = 0;
    root.nlink = 2; /* . and .. */
    root.atime = root.mtime = root.ctime = (int64_t)time(NULL);

    pthread_mutex_init(&gfs.lock, NULL);
    pthread_mutex_init(&gfs.cache_lock, NULL);
    cache_init();

    inode_write(ROOT_INUM, &root);
    dir_add(ROOT_INUM, ".",  ROOT_INUM);
    dir_add(ROOT_INUM, "..", ROOT_INUM);

    cache_flush();
    close(gfs.fd);

    printf("mkfs: successfully formatted %s (%u blocks, %u MiB, data_start=%u)\n",
           image_path, total_blocks, (total_blocks * BLOCK_SIZE) / (1024 * 1024), data_start);
    return 0;
}

int main(int argc, char *argv[]) {
    if (argc < 2 || argc > 3) {
        fprintf(stderr, "Usage: %s <image> [blocks]\n", argv[0]);
        return 1;
    }
    uint32_t blocks = (argc == 3) ? (uint32_t)atoi(argv[2]) : 4096;
    return mkfs_create(argv[1], blocks) < 0 ? 1 : 0;
}
