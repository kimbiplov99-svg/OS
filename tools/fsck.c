#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include "../include/fusefs.h"

static int errors = 0;
static int repaired = 0;
static int do_repair = 0;

#define ERROR(fmt, ...) do { fprintf(stderr, "ERROR: " fmt "\n", ##__VA_ARGS__); errors++; } while(0)
#define INFO(fmt, ...)  printf("  " fmt "\n", ##__VA_ARGS__)

static uint8_t block_ref[MAX_BLOCKS];

static void check_inode(uint32_t inum, uint32_t parent_inum, int depth);

static void check_block_ptr(uint32_t bno, uint32_t inum) {
    if (bno == INVALID_BLOCK) return;
    if (bno < gfs.sb.data_start || bno >= gfs.sb.total_blocks) {
        ERROR("inode %u: block %u out of valid range [%u, %u)",
              inum, bno, gfs.sb.data_start, gfs.sb.total_blocks);
        return;
    }
    uint32_t bit = bno - gfs.sb.data_start;
    if (!bitmap_test(DATA_BITMAP_BLK, bit)) {
        ERROR("inode %u: block %u referenced but not marked in bitmap", inum, bno);
        if (do_repair) {
            bitmap_set(DATA_BITMAP_BLK, bit);
            if (gfs.sb.free_blocks > 0) gfs.sb.free_blocks--;
            repaired++;
            printf("  [REPAIR] Marked block %u in data bitmap\n", bno);
        }
    }
    if (++block_ref[bit] > 1) {
        ERROR("inode %u: block %u referenced multiple times (shared block corruption)", inum, bno);
    }
}

static void check_indirect(uint32_t bno, uint32_t inum) {
    if (bno == INVALID_BLOCK) return;
    check_block_ptr(bno, inum);
    uint32_t buf[PTRS_PER_BLOCK];
    cache_read(bno, buf);
    for (uint32_t i = 0; i < (uint32_t)PTRS_PER_BLOCK; i++) {
        if (buf[i] != INVALID_BLOCK)
            check_block_ptr(buf[i], inum);
    }
}

static void check_dindirect(uint32_t bno, uint32_t inum) {
    if (bno == INVALID_BLOCK) return;
    check_block_ptr(bno, inum);
    uint32_t dbl[PTRS_PER_BLOCK];
    cache_read(bno, dbl);
    for (uint32_t i = 0; i < (uint32_t)PTRS_PER_BLOCK; i++) {
        if (dbl[i] != INVALID_BLOCK)
            check_indirect(dbl[i], inum);
    }
}

static void check_inode(uint32_t inum, uint32_t parent_inum, int depth) {
    if (depth > 64) { ERROR("inode %u: directory recursion depth exceeded", inum); return; }
    struct inode ip;
    if (inode_read(inum, &ip) < 0) { ERROR("cannot read inode %u", inum); return; }
    if (!ip.type) { ERROR("inode %u: inactive/zero type", inum); return; }

    INFO("inode %u: type=%d size=%llu nlink=%d blocks=%u",
         inum, ip.type, (unsigned long long)ip.size, ip.nlink, ip.blocks);

    for (int i = 0; i < DIRECT_BLOCKS; i++)
        check_block_ptr(ip.direct[i], inum);
    check_indirect(ip.indirect, inum);
    check_dindirect(ip.dindirect, inum);

    if (ip.type == T_DIR) {
        struct dirent de;
        uint64_t off = 0;
        int dot = 0, dotdot = 0;
        while (off < ip.size) {
            ssize_t n = inode_read_data(inum, &ip, &de, sizeof de, (off_t)off);
            if (n < (ssize_t)sizeof de) break;
            if (!de.inum) { off += sizeof de; continue; }
            if (strcmp(de.name, ".") == 0) {
                dot = 1;
                if (de.inum != inum) ERROR("inode %u: '.' points to %u", inum, de.inum);
            }
            if (strcmp(de.name, "..") == 0) {
                dotdot = 1;
                if (de.inum != parent_inum) ERROR("inode %u: '..' points to %u (expected %u)", inum, de.inum, parent_inum);
            }
            if (strcmp(de.name, ".") && strcmp(de.name, "..")) {
                if (!bitmap_test(INODE_BITMAP_BLK, de.inum - 1)) {
                    ERROR("inode %u: dirent '%s' points to unallocated inode %u", inum, de.name, de.inum);
                } else {
                    check_inode(de.inum, inum, depth + 1);
                }
            }
            off += sizeof de;
        }
        if (!dot)    ERROR("inode %u: missing '.' directory entry", inum);
        if (!dotdot) ERROR("inode %u: missing '..' directory entry", inum);
    }
}

int main(int argc, char *argv[]) {
    const char *img = NULL;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-r") == 0 || strcmp(argv[i], "--repair") == 0) {
            do_repair = 1;
        } else if (!img) {
            img = argv[i];
        }
    }

    if (!img) {
        fprintf(stderr, "Usage: %s [-r|--repair] <image>\n", argv[0]);
        return 1;
    }

    gfs.fd = open(img, O_RDWR);
    if (gfs.fd < 0) { perror("open image"); return 1; }

    sb_read();
    if (gfs.sb.magic != FS_MAGIC) {
        fprintf(stderr, "Bad magic 0x%08X\n", gfs.sb.magic);
        return 1;
    }
    pthread_mutex_init(&gfs.lock, NULL);
    pthread_mutex_init(&gfs.cache_lock, NULL);
    cache_init();

    printf("fsck: validating %s (%u blocks, %u inodes, repair=%s)\n",
           img, gfs.sb.total_blocks, gfs.sb.total_inodes, do_repair ? "ON" : "OFF");

    memset(block_ref, 0, sizeof block_ref);

    if (!bitmap_test(INODE_BITMAP_BLK, ROOT_INUM - 1)) {
        ERROR("root inode (inum 1) not marked in inode bitmap");
    } else {
        check_inode(ROOT_INUM, ROOT_INUM, 0);
    }

    /* Cross-check bitmap vs actual references */
    uint32_t data_blocks = gfs.sb.total_blocks - gfs.sb.data_start;
    for (uint32_t b = 0; b < data_blocks; b++) {
        int in_bitmap = bitmap_test(DATA_BITMAP_BLK, b);
        int has_ref   = block_ref[b] > 0;
        if (in_bitmap && !has_ref) {
            ERROR("data block %u marked used in bitmap but unreferenced (storage leak)",
                  b + gfs.sb.data_start);
            if (do_repair) {
                bitmap_free(DATA_BITMAP_BLK, b);
                gfs.sb.free_blocks++;
                repaired++;
                printf("  [REPAIR] Freed leaked block %u in bitmap\n", b + gfs.sb.data_start);
            }
        }
        if (!in_bitmap && has_ref) {
            ERROR("data block %u referenced but free in bitmap (data corruption)",
                  b + gfs.sb.data_start);
        }
    }

    if (do_repair && repaired > 0) {
        sb_write();
        cache_flush();
    }

    if (errors == 0) {
        printf("fsck: CLEAN — filesystem has zero errors.\n");
    } else {
        printf("fsck: Found %d error(s), repaired %d issue(s).\n", errors, repaired);
    }

    close(gfs.fd);
    return (errors > 0 && !do_repair) ? 1 : 0;
}
