
#define _GNU_SOURCE
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <time.h>
#include "../include/fusefs.h"

/* Compute block number of inode inum inside inode table */
static uint32_t inode_block(uint32_t inum) {
    return INODE_TABLE_BLK + (inum - 1) / INODES_PER_BLOCK;
}
static uint32_t inode_offset(uint32_t inum) {
    return ((inum - 1) % INODES_PER_BLOCK) * sizeof(struct inode);
}

int inode_read(uint32_t inum, struct inode *ip) {
    if (inum == 0 || inum > MAX_INODES) return -EINVAL;
    uint8_t buf[BLOCK_SIZE];
    if (cache_read(inode_block(inum), buf) < 0) return -EIO;
    memcpy(ip, buf + inode_offset(inum), sizeof(struct inode));
    return 0;
}

int inode_write(uint32_t inum, const struct inode *ip) {
    if (inum == 0 || inum > MAX_INODES) return -EINVAL;
    uint8_t buf[BLOCK_SIZE];
    if (cache_read(inode_block(inum), buf) < 0) return -EIO;
    memcpy(buf + inode_offset(inum), ip, sizeof(struct inode));
    return cache_write(inode_block(inum), buf);
}

int32_t inode_alloc(uint16_t type, uint32_t mode, uint32_t uid, uint32_t gid) {
    int32_t inum = bitmap_alloc(INODE_BITMAP_BLK, MAX_INODES);
    if (inum < 0) return inum;
    inum++; /* bit 0 => inode 1 (root) */
    struct inode ip;
    memset(&ip, 0, sizeof ip);
    ip.type  = type;
    ip.mode  = mode;
    ip.uid   = uid;
    ip.gid   = gid;
    ip.nlink = 1;
    ip.atime = ip.mtime = ip.ctime = (int64_t)time(NULL);
    inode_write((uint32_t)inum, &ip);
    gfs.sb.free_inodes--;
    sb_write();
    return inum;
}

void inode_free(uint32_t inum) {
    struct inode ip;
    inode_read(inum, &ip);
    ip.type = 0;
    inode_write(inum, &ip);
    bitmap_free(INODE_BITMAP_BLK, inum - 1);
    gfs.sb.free_inodes++;
    sb_write();
}

/* Map file block index to disk block number; alloc=1 to allocate */
uint32_t inode_bmap(struct inode *ip, uint32_t fblock, int alloc) {
    /* Direct */
    if (fblock < DIRECT_BLOCKS) {
        if (ip->direct[fblock] == INVALID_BLOCK && alloc) {
            int32_t b = bitmap_alloc(DATA_BITMAP_BLK, gfs.sb.total_blocks - gfs.sb.data_start);
            if (b < 0) return INVALID_BLOCK;
            uint32_t bno = (uint32_t)b + gfs.sb.data_start;
            uint8_t zero[BLOCK_SIZE] = {0};
            cache_write(bno, zero);
            ip->direct[fblock] = bno;
            gfs.sb.free_blocks--;
            sb_write();
        }
        return ip->direct[fblock];
    }
    fblock -= DIRECT_BLOCKS;

    /* Single indirect */
    uint32_t ptrs = (uint32_t)PTRS_PER_BLOCK;
    if (fblock < ptrs) {
        if (ip->indirect == INVALID_BLOCK && alloc) {
            int32_t b = bitmap_alloc(DATA_BITMAP_BLK, gfs.sb.total_blocks - gfs.sb.data_start);
            if (b < 0) return INVALID_BLOCK;
            ip->indirect = (uint32_t)b + gfs.sb.data_start;
            uint8_t zero[BLOCK_SIZE] = {0};
            cache_write(ip->indirect, zero);
            gfs.sb.free_blocks--;
            sb_write();
        }
        if (ip->indirect == INVALID_BLOCK) return INVALID_BLOCK;
        uint32_t indir_buf[PTRS_PER_BLOCK];
        cache_read(ip->indirect, indir_buf);
        if (indir_buf[fblock] == INVALID_BLOCK && alloc) {
            int32_t b = bitmap_alloc(DATA_BITMAP_BLK, gfs.sb.total_blocks - gfs.sb.data_start);
            if (b < 0) return INVALID_BLOCK;
            indir_buf[fblock] = (uint32_t)b + gfs.sb.data_start;
            uint8_t zero[BLOCK_SIZE] = {0};
            cache_write(indir_buf[fblock], zero);
            cache_write(ip->indirect, indir_buf);
            gfs.sb.free_blocks--;
            sb_write();
        }
        return indir_buf[fblock];
    }
    fblock -= ptrs;

    /* Double indirect */
    if (fblock < ptrs * ptrs) {
        uint32_t di = fblock / ptrs;
        uint32_t si = fblock % ptrs;
        if (ip->dindirect == INVALID_BLOCK && alloc) {
            int32_t b = bitmap_alloc(DATA_BITMAP_BLK, gfs.sb.total_blocks - gfs.sb.data_start);
            if (b < 0) return INVALID_BLOCK;
            ip->dindirect = (uint32_t)b + gfs.sb.data_start;
            uint8_t zero[BLOCK_SIZE] = {0};
            cache_write(ip->dindirect, zero);
            gfs.sb.free_blocks--;
            sb_write();
        }
        if (ip->dindirect == INVALID_BLOCK) return INVALID_BLOCK;
        uint32_t dbl_buf[PTRS_PER_BLOCK];
        cache_read(ip->dindirect, dbl_buf);
        if (dbl_buf[di] == INVALID_BLOCK && alloc) {
            int32_t b = bitmap_alloc(DATA_BITMAP_BLK, gfs.sb.total_blocks - gfs.sb.data_start);
            if (b < 0) return INVALID_BLOCK;
            dbl_buf[di] = (uint32_t)b + gfs.sb.data_start;
            uint8_t zero[BLOCK_SIZE] = {0};
            cache_write(dbl_buf[di], zero);
            cache_write(ip->dindirect, dbl_buf);
            gfs.sb.free_blocks--;
            sb_write();
        }
        if (dbl_buf[di] == INVALID_BLOCK) return INVALID_BLOCK;
        uint32_t sgl_buf[PTRS_PER_BLOCK];
        cache_read(dbl_buf[di], sgl_buf);
        if (sgl_buf[si] == INVALID_BLOCK && alloc) {
            int32_t b = bitmap_alloc(DATA_BITMAP_BLK, gfs.sb.total_blocks - gfs.sb.data_start);
            if (b < 0) return INVALID_BLOCK;
            sgl_buf[si] = (uint32_t)b + gfs.sb.data_start;
            uint8_t zero[BLOCK_SIZE] = {0};
            cache_write(sgl_buf[si], zero);
            cache_write(dbl_buf[di], sgl_buf);
            gfs.sb.free_blocks--;
            sb_write();
        }
        return sgl_buf[si];
    }
    return INVALID_BLOCK; /* file too large */
}

/* Truncate all blocks of an inode to zero */
int inode_trunc(uint32_t inum, struct inode *ip) {
    uint32_t ptrs = (uint32_t)PTRS_PER_BLOCK;

    for (int i = 0; i < DIRECT_BLOCKS; i++) {
        if (ip->direct[i]) {
            bitmap_free(DATA_BITMAP_BLK, ip->direct[i] - gfs.sb.data_start);
            gfs.sb.free_blocks++;
            ip->direct[i] = INVALID_BLOCK;
        }
    }
    if (ip->indirect) {
        uint32_t indir[PTRS_PER_BLOCK];
        cache_read(ip->indirect, indir);
        for (uint32_t i = 0; i < ptrs; i++) {
            if (indir[i]) {
                bitmap_free(DATA_BITMAP_BLK, indir[i] - gfs.sb.data_start);
                gfs.sb.free_blocks++;
            }
        }
        bitmap_free(DATA_BITMAP_BLK, ip->indirect - gfs.sb.data_start);
        gfs.sb.free_blocks++;
        ip->indirect = INVALID_BLOCK;
    }
    if (ip->dindirect) {
        uint32_t dbl[PTRS_PER_BLOCK];
        cache_read(ip->dindirect, dbl);
        for (uint32_t di = 0; di < ptrs; di++) {
            if (!dbl[di]) continue;
            uint32_t sgl[PTRS_PER_BLOCK];
            cache_read(dbl[di], sgl);
            for (uint32_t si = 0; si < ptrs; si++) {
                if (sgl[si]) {
                    bitmap_free(DATA_BITMAP_BLK, sgl[si] - gfs.sb.data_start);
                    gfs.sb.free_blocks++;
                }
            }
            bitmap_free(DATA_BITMAP_BLK, dbl[di] - gfs.sb.data_start);
            gfs.sb.free_blocks++;
        }
        bitmap_free(DATA_BITMAP_BLK, ip->dindirect - gfs.sb.data_start);
        gfs.sb.free_blocks++;
        ip->dindirect = INVALID_BLOCK;
    }
    ip->size = 0;
    ip->blocks = 0;
    ip->ctime = (int64_t)time(NULL);
    inode_write(inum, ip);
    sb_write();
    return 0;
}

/* Read up to `size` bytes from inode at offset `off` */
ssize_t inode_read_data(uint32_t inum, struct inode *ip,
                         void *buf, size_t size, off_t off) {
    if ((uint64_t)off >= ip->size) return 0;
    if ((uint64_t)off + size > ip->size)
        size = ip->size - (uint64_t)off;
    ssize_t total = 0;
    uint8_t block_buf[BLOCK_SIZE];
    while (size > 0) {
        uint32_t fblock = (uint32_t)(off / BLOCK_SIZE);
        uint32_t boff   = (uint32_t)(off % BLOCK_SIZE);
        uint32_t can    = BLOCK_SIZE - boff;
        if (can > size) can = (uint32_t)size;
        uint32_t bno = inode_bmap(ip, fblock, 0);
        if (bno == INVALID_BLOCK) break;
        cache_read(bno, block_buf);
        memcpy((char*)buf + total, block_buf + boff, can);
        total += can;
        off   += can;
        size  -= can;
    }
    ip->atime = (int64_t)time(NULL);
    inode_write(inum, ip);
    return total;
}

/* Write `size` bytes to inode at offset `off`; grows file as needed */
ssize_t inode_write_data(uint32_t inum, struct inode *ip,
                          const void *buf, size_t size, off_t off) {
    ssize_t total = 0;
    uint8_t block_buf[BLOCK_SIZE];
    while (size > 0) {
        uint32_t fblock = (uint32_t)(off / BLOCK_SIZE);
        uint32_t boff   = (uint32_t)(off % BLOCK_SIZE);
        uint32_t can    = BLOCK_SIZE - boff;
        if (can > size) can = (uint32_t)size;
        uint32_t bno = inode_bmap(ip, fblock, 1);
        if (bno == INVALID_BLOCK) break;
        cache_read(bno, block_buf);
        memcpy(block_buf + boff, (const char*)buf + total, can);
        cache_write(bno, block_buf);
        total += can;
        off   += can;
        size  -= can;
    }
    if ((uint64_t)off > ip->size) ip->size = (uint64_t)off;
    ip->mtime = ip->ctime = (int64_t)time(NULL);
    inode_write(inum, ip);
    return total;
}
